//! flipper_rx.rs — receive signed sensor frames from a paired Flipper Zero.
//!
//! Closes the Flipper -> phone -> chain pipeline (Option B in
//! `clients/btcpc-flipper/docs/SIGNING_INTEGRATION.md`). The Flipper captures a
//! reading, packs it into a BtcpcFrame, and signs the payload with its device
//! key over BLE. This module:
//!   1. parses the raw BLE bytes into a frame (magic + header + payload),
//!   2. verifies the ed25519 device signature against the Flipper's registered
//!      public key (the anti-spoof boundary — a third party cannot inject fake
//!      Flipper data into someone else's relay),
//!   3. maps the payload to a phone `SensorReading` owned by this phone's
//!      account, and hands it to `sensors::submit`, which re-signs it with the
//!      owner posting key and broadcasts it to the network.
//!
//! Wire format (must match clients/btcpc-flipper/protocol/btcpc_protocol.h,
//! all little-endian, header is __attribute__((packed))):
//!   [0..4]   magic "BTPC"
//!   [4]      msg_type (u8)
//!   [5..7]   payload_len (u16 LE)
//!   [7..71]  sig (64-byte ed25519 over payload bytes)
//!   [71..]   payload
//!
//! Shin Devlin — btcpc.network

use std::sync::Arc;
use ed25519_dalek::{Verifier, VerifyingKey, Signature};
use sha2::{Digest, Sha256};
use tracing::{warn, info};

use crate::chain::Chain;
use crate::net::NetCmd;
use crate::sensors::{self, SensorReading};

const FRAME_MAGIC: [u8; 4] = *b"BTPC";
const SIG_LEN: usize = 64;
const HEADER_LEN: usize = 4 + 1 + 2 + SIG_LEN; // = 71

// Message types (from btcpc_protocol.h). Only Flipper->phone types handled.
const MSG_SUBGHZ_OBS: u8 = 0x01;
const MSG_RFID_SCAN: u8 = 0x02;
const MSG_NFC_SCAN: u8 = 0x03;
const MSG_IBUTTON: u8 = 0x04;
const MSG_HEARTBEAT: u8 = 0x05;
const MSG_IR_CAPTURE: u8 = 0x06;

/// A parsed, signature-verified frame ready to convert to a SensorReading.
struct Frame<'a> {
    msg_type: u8,
    payload: &'a [u8],
}

/// Parse and verify a raw BLE buffer against the Flipper's public key.
/// Returns None (and logs) on any malformed frame, bad magic, length mismatch,
/// or signature failure — the caller drops it silently, which is the anti-spoof
/// behaviour we want.
fn parse_and_verify<'a>(buf: &'a [u8], flipper_pubkey: &VerifyingKey) -> Option<Frame<'a>> {
    if buf.len() < HEADER_LEN {
        warn!("flipper_rx: frame too short ({} < {})", buf.len(), HEADER_LEN);
        return None;
    }
    if buf[0..4] != FRAME_MAGIC {
        warn!("flipper_rx: bad magic");
        return None;
    }
    let msg_type = buf[4];
    let payload_len = u16::from_le_bytes([buf[5], buf[6]]) as usize;
    let sig_bytes = &buf[7..7 + SIG_LEN];

    let payload_start = HEADER_LEN;
    let payload_end = payload_start.checked_add(payload_len)?;
    if buf.len() < payload_end {
        warn!(
            "flipper_rx: payload_len {} exceeds buffer ({} available)",
            payload_len,
            buf.len() - payload_start
        );
        return None;
    }
    let payload = &buf[payload_start..payload_end];

    // The Flipper signs the payload bytes only (see frame_sign in the C).
    let sig_arr: [u8; SIG_LEN] = sig_bytes.try_into().ok()?;
    let signature = Signature::from_bytes(&sig_arr);
    if flipper_pubkey.verify(payload, &signature).is_err() {
        warn!("flipper_rx: device signature verification FAILED — dropping");
        return None;
    }

    Some(Frame { msg_type, payload })
}

/// Deterministic data_hash over the raw payload bytes. The phone commits to
/// exactly what the Flipper sent (which is what the device signed), so the
/// hash is reproducible and tamper-evident.
fn payload_data_hash(msg_type: u8, payload: &[u8]) -> String {
    let mut h = Sha256::new();
    h.update([msg_type]);
    h.update(payload);
    hex::encode(h.finalize())
}

/// Map a verified frame to a phone SensorReading owned by `owner`.
/// Returns None for message types that are not sensor readings (e.g. IR
/// capture is not yet a rewarded reading type; heartbeat is housekeeping).
fn frame_to_reading(frame: &Frame, owner: &str, flipper_pk_hex: &str, epoch: u64) -> Option<SensorReading> {
    let data_hash = payload_data_hash(frame.msg_type, frame.payload);
    // sensor_id ties the reading to this specific Flipper device so the chain /
    // aggregation layer can group by physical device.
    let short_id = &flipper_pk_hex[..flipper_pk_hex.len().min(16)];

    let (sensor_type, primary_value, values_json, unit, kind_tag) = match frame.msg_type {
        MSG_SUBGHZ_OBS => {
            // BtcpcSubGhzObs: freq_hz(u32 LE), rssi_dbm(i8), modulation(u8), bandwidth(u8)
            if frame.payload.len() < 7 { return None; }
            let freq = u32::from_le_bytes(frame.payload[0..4].try_into().ok()?);
            let rssi = frame.payload[4] as i8;
            let modulation = frame.payload[5];
            let bandwidth = frame.payload[6];
            (
                "continuous",
                rssi as f64,
                format!(
                    "{{\"freq_hz\":{},\"rssi_dbm\":{},\"modulation\":{},\"bandwidth\":{}}}",
                    freq, rssi, modulation, bandwidth
                ),
                "dBm",
                "subghz",
            )
        }
        MSG_NFC_SCAN | MSG_RFID_SCAN | MSG_IBUTTON => {
            // Event-class: a distinct discovery. primary_value = 1 (a hit).
            // Keep the raw payload as hex in values_json for the aggregation layer.
            let tag = match frame.msg_type {
                MSG_NFC_SCAN => "nfc",
                MSG_RFID_SCAN => "rfid",
                _ => "ibutton",
            };
            (
                "event",
                1.0,
                format!("{{\"raw\":\"{}\"}}", hex::encode(frame.payload)),
                "count",
                tag,
            )
        }
        MSG_HEARTBEAT | MSG_IR_CAPTURE => {
            // Housekeeping / not-yet-rewarded — do not submit as a reading.
            return None;
        }
        other => {
            warn!("flipper_rx: unknown msg_type 0x{:02x}", other);
            return None;
        }
    };

    Some(SensorReading {
        sensor_id: format!("{}/flipper-{}-{}", owner, short_id, kind_tag),
        sensor_type: sensor_type.to_string(),
        primary_value,
        values_json,
        unit: unit.to_string(),
        owner: owner.to_string(),
        epoch,
    })
}

/// Handle one raw BLE frame received from the paired Flipper.
///
/// `flipper_pk_hex` is the Flipper's registered device public key (64 hex
/// chars). `owner` is this phone's account; the resulting reading is attributed
/// to and re-signed by the owner (Option B). This is the single entry point the
/// JNI BLE-receive callback calls.
pub async fn handle_ble_frame(
    buf: &[u8],
    flipper_pk_hex: &str,
    owner: &str,
    posting_key: String,
    chain: Arc<Chain>,
    cmd_tx: tokio::sync::mpsc::Sender<NetCmd>,
) {
    let pk_bytes = match hex::decode(flipper_pk_hex.trim()) {
        Ok(b) => b,
        Err(_) => { warn!("flipper_rx: flipper pubkey not valid hex"); return; }
    };
    let pk_arr: [u8; 32] = match pk_bytes.try_into() {
        Ok(a) => a,
        Err(_) => { warn!("flipper_rx: flipper pubkey must be 32 bytes"); return; }
    };
    let flipper_pubkey = match VerifyingKey::from_bytes(&pk_arr) {
        Ok(k) => k,
        Err(e) => { warn!("flipper_rx: bad flipper pubkey: {}", e); return; }
    };

    let frame = match parse_and_verify(buf, &flipper_pubkey) {
        Some(f) => f,
        None => return, // already logged; drop silently (anti-spoof)
    };

    let epoch = chain.current_epoch();
    let reading = match frame_to_reading(&frame, owner, flipper_pk_hex, epoch) {
        Some(r) => r,
        None => return, // housekeeping / non-reading frame
    };

    info!(
        "flipper_rx: verified {} reading from device {} -> submitting",
        reading.sensor_type, flipper_pk_hex.get(..8).unwrap_or("")
    );
    sensors::submit(reading, chain, cmd_tx, posting_key).await;
}

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_dalek::Signer;

    /// Build a valid signed frame the way the Flipper would.
    fn make_frame(sk: &ed25519_dalek::SigningKey, msg_type: u8, payload: &[u8]) -> Vec<u8> {
        let sig = sk.sign(payload);
        let mut buf = Vec::with_capacity(HEADER_LEN + payload.len());
        buf.extend_from_slice(&FRAME_MAGIC);
        buf.push(msg_type);
        buf.extend_from_slice(&(payload.len() as u16).to_le_bytes());
        buf.extend_from_slice(&sig.to_bytes());
        buf.extend_from_slice(payload);
        buf
    }

    fn subghz_payload(freq: u32, rssi: i8) -> Vec<u8> {
        let mut p = Vec::new();
        p.extend_from_slice(&freq.to_le_bytes());
        p.push(rssi as u8);
        p.push(2); // modulation OOK
        p.push(0); // bandwidth
        p
    }

    #[test]
    fn valid_frame_parses_and_verifies() {
        let sk = ed25519_dalek::SigningKey::from_bytes(&[7u8; 32]);
        let pk = sk.verifying_key();
        let payload = subghz_payload(433_920_000, -72);
        let frame = make_frame(&sk, MSG_SUBGHZ_OBS, &payload);

        let parsed = parse_and_verify(&frame, &pk).expect("valid frame must parse");
        assert_eq!(parsed.msg_type, MSG_SUBGHZ_OBS);
        assert_eq!(parsed.payload, &payload[..]);
    }

    #[test]
    fn wrong_key_signature_rejected() {
        let flipper_sk = ed25519_dalek::SigningKey::from_bytes(&[7u8; 32]);
        let attacker_pk = ed25519_dalek::SigningKey::from_bytes(&[9u8; 32]).verifying_key();
        let payload = subghz_payload(433_920_000, -72);
        let frame = make_frame(&flipper_sk, MSG_SUBGHZ_OBS, &payload);
        // Verify against the WRONG key — must fail (anti-spoof).
        assert!(parse_and_verify(&frame, &attacker_pk).is_none());
    }

    #[test]
    fn tampered_payload_rejected() {
        let sk = ed25519_dalek::SigningKey::from_bytes(&[7u8; 32]);
        let pk = sk.verifying_key();
        let payload = subghz_payload(433_920_000, -72);
        let mut frame = make_frame(&sk, MSG_SUBGHZ_OBS, &payload);
        // Flip a byte in the payload after signing.
        let last = frame.len() - 1;
        frame[last] ^= 0xFF;
        assert!(parse_and_verify(&frame, &pk).is_none());
    }

    #[test]
    fn bad_magic_rejected() {
        let sk = ed25519_dalek::SigningKey::from_bytes(&[7u8; 32]);
        let pk = sk.verifying_key();
        let payload = subghz_payload(433_920_000, -72);
        let mut frame = make_frame(&sk, MSG_SUBGHZ_OBS, &payload);
        frame[0] = b'X';
        assert!(parse_and_verify(&frame, &pk).is_none());
    }

    #[test]
    fn truncated_frame_rejected() {
        let sk = ed25519_dalek::SigningKey::from_bytes(&[7u8; 32]);
        let pk = sk.verifying_key();
        let payload = subghz_payload(433_920_000, -72);
        let frame = make_frame(&sk, MSG_SUBGHZ_OBS, &payload);
        assert!(parse_and_verify(&frame[..HEADER_LEN - 1], &pk).is_none());
    }

    #[test]
    fn subghz_frame_maps_to_continuous_reading() {
        let sk = ed25519_dalek::SigningKey::from_bytes(&[7u8; 32]);
        let pk_hex = hex::encode(sk.verifying_key().to_bytes());
        let payload = subghz_payload(433_920_000, -55);
        let frame = Frame { msg_type: MSG_SUBGHZ_OBS, payload: &payload };
        let r = frame_to_reading(&frame, "alice", &pk_hex, 42).expect("subghz maps to a reading");
        assert_eq!(r.sensor_type, "continuous");
        assert_eq!(r.primary_value, -55.0);
        assert_eq!(r.owner, "alice");
        assert!(r.sensor_id.starts_with("alice/flipper-"));
        assert!(r.sensor_id.ends_with("-subghz"));
    }

    #[test]
    fn nfc_frame_maps_to_event_reading() {
        let uid = [0x04u8, 0x1a, 0x2b, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00];
        let mut payload = vec![0u8]; // tech = A
        payload.extend_from_slice(&uid);
        payload.push(4); // uid_len
        payload.extend_from_slice(&[0x00, 0x44]); // atqa
        payload.push(0x08); // sak
        let sk = ed25519_dalek::SigningKey::from_bytes(&[7u8; 32]);
        let pk_hex = hex::encode(sk.verifying_key().to_bytes());
        let frame = Frame { msg_type: MSG_NFC_SCAN, payload: &payload };
        let r = frame_to_reading(&frame, "bob", &pk_hex, 1).expect("nfc maps to a reading");
        assert_eq!(r.sensor_type, "event");
        assert_eq!(r.primary_value, 1.0);
        assert!(r.sensor_id.ends_with("-nfc"));
    }

    #[test]
    fn heartbeat_frame_is_not_a_reading() {
        let sk = ed25519_dalek::SigningKey::from_bytes(&[7u8; 32]);
        let pk_hex = hex::encode(sk.verifying_key().to_bytes());
        let payload = vec![0u8; 13];
        let frame = Frame { msg_type: MSG_HEARTBEAT, payload: &payload };
        assert!(frame_to_reading(&frame, "carol", &pk_hex, 1).is_none());
    }
}
