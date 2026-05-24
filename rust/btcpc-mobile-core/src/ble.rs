//! BLE frame parsing for Flipper Zero sensor data.
//!
//! Wire format is defined in clients/btcpc-flipper/protocol/btcpc_protocol.h:
//!
//!   [BtcpcFrameHeader (71 bytes)] [payload bytes]
//!
//! Header layout (packed, little-endian multi-byte integers):
//!   [0..4]   magic:       'B','T','P','C'
//!   [4]      msg_type:    BtcpcMsgType (u8)
//!   [5..7]   payload_len: u16 little-endian
//!   [7..71]  sig:         ed25519 signature (64 bytes) over payload bytes
//!
//! Signature covers the payload bytes only — NOT the header.
//! The phone verifies the signature against the Flipper's on-chain public key.
//!
//! Known message types (do not change these without coordinating with the
//! firmware agent — they are compiled into Flipper firmware):
//!   Flipper → phone: 0x01=SubGhzObs, 0x02=RfidScan, 0x03=NfcScan,
//!                    0x04=IButton, 0x05=Heartbeat, 0x06=IrCapture
//!   Phone → Flipper: 0x10=EntryHash, 0x11=ClockSync, 0x12=Gps
//!
//! Planned (see ARCHITECTURE.md for the protocol extension spec):
//!   Phone → Flipper: 0x13=SignReq
//!   Flipper → phone: 0x07=SignResp

use ed25519_dalek::{Signature, VerifyingKey};

use crate::MobileCoreError;

/// Magic bytes — ASCII "BTPC"
const MAGIC: [u8; 4] = [b'B', b'T', b'P', b'C'];

/// Header size in bytes (matches sizeof(BtcpcFrameHeader) = 71).
const HEADER_SIZE: usize = 71;

/// Signature offset and length within the header.
const SIG_OFFSET: usize = 7;
const SIG_LEN:    usize = 64;

/// Payload length field offset (u16 LE at byte 5).
const PAYLOAD_LEN_OFFSET: usize = 5;

/// msg_type offset (u8 at byte 4).
const MSG_TYPE_OFFSET: usize = 4;

/// Decoded BLE frame. Returned by parse_ble_frame().
pub struct BleFrame {
    /// Raw BtcpcMsgType value (u8). See UDL and btcpc_protocol.h for values.
    pub msg_type: u8,
    /// Decoded payload bytes (payload_len bytes immediately after the header).
    pub payload: Vec<u8>,
}

/// Parse a raw BLE frame received from the Flipper Zero.
///
/// Validates magic, checks length, returns msg_type + payload bytes.
/// Does NOT verify the signature — call verify_ble_frame_sig() separately.
pub fn parse_ble_frame(raw_frame: Vec<u8>) -> Result<BleFrame, MobileCoreError> {
    if raw_frame.len() < HEADER_SIZE {
        return Err(MobileCoreError::InvalidFrame(format!(
            "frame too short: {} bytes, need at least {}",
            raw_frame.len(),
            HEADER_SIZE
        )));
    }

    // Check magic
    if raw_frame[..4] != MAGIC {
        return Err(MobileCoreError::InvalidFrame(format!(
            "bad magic: {:02x?}, expected 42545043",
            &raw_frame[..4]
        )));
    }

    let msg_type = raw_frame[MSG_TYPE_OFFSET];

    // payload_len: u16 little-endian at offset 5
    let payload_len = u16::from_le_bytes([
        raw_frame[PAYLOAD_LEN_OFFSET],
        raw_frame[PAYLOAD_LEN_OFFSET + 1],
    ]) as usize;

    let expected_total = HEADER_SIZE + payload_len;
    if raw_frame.len() < expected_total {
        return Err(MobileCoreError::InvalidFrame(format!(
            "frame truncated: have {} bytes, header declares {} payload bytes (need {})",
            raw_frame.len(),
            payload_len,
            expected_total
        )));
    }

    let payload = raw_frame[HEADER_SIZE..HEADER_SIZE + payload_len].to_vec();
    Ok(BleFrame { msg_type, payload })
}

/// Verify the ed25519 signature in a BLE frame header.
///
/// The signature at header bytes [7..71] covers the payload bytes only.
/// `flipper_pubkey_hex`: 64-char lowercase hex of the Flipper's ed25519 pubkey,
/// which must be registered on-chain for the Flipper's device identity.
pub fn verify_ble_frame_sig(
    raw_frame: Vec<u8>,
    flipper_pubkey_hex: String,
) -> Result<bool, MobileCoreError> {
    if raw_frame.len() < HEADER_SIZE {
        return Err(MobileCoreError::InvalidFrame("frame too short".into()));
    }

    // Parse payload_len to find payload slice
    let payload_len = u16::from_le_bytes([
        raw_frame[PAYLOAD_LEN_OFFSET],
        raw_frame[PAYLOAD_LEN_OFFSET + 1],
    ]) as usize;
    if raw_frame.len() < HEADER_SIZE + payload_len {
        return Err(MobileCoreError::InvalidFrame("frame truncated".into()));
    }
    let payload = &raw_frame[HEADER_SIZE..HEADER_SIZE + payload_len];

    // Extract the 64-byte signature from the header
    let sig_bytes: [u8; 64] = raw_frame[SIG_OFFSET..SIG_OFFSET + SIG_LEN]
        .try_into()
        .map_err(|_| MobileCoreError::InvalidFrame("sig slice wrong length".into()))?;
    let signature = Signature::from_bytes(&sig_bytes);

    // Decode the Flipper's public key
    let pubkey_bytes = hex::decode(&flipper_pubkey_hex)
        .map_err(|e| MobileCoreError::ParseError(format!("bad pubkey hex: {}", e)))?;
    let pubkey_arr: [u8; 32] = pubkey_bytes
        .try_into()
        .map_err(|_| MobileCoreError::ParseError("pubkey must be 32 bytes".into()))?;
    let verifying_key = VerifyingKey::from_bytes(&pubkey_arr)
        .map_err(|e| MobileCoreError::ParseError(format!("invalid pubkey: {}", e)))?;

    // Verify: signature covers payload bytes only
    match verifying_key.verify_strict(payload, &signature) {
        Ok(_) => Ok(true),
        Err(_) => Ok(false),
    }
}
