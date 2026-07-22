//! Verified Inference Set (VIS) — the signed, versioned allowlist that gates the
//! VERIFIED/slashable inference tier. Per docs/design/verified-inference-set.md.
//!
//! The open tier (docs/MODEL_REGISTRY_PROTOCOL.md) is untouched: an operator runs
//! whatever GGUF they want, un-refereed, un-slashable, HONE blesses nothing. The
//! verified tier is different — denier re-execution and the staked jury only work
//! if every referee runs the exact same content-hash-pinned engine+weights, so
//! that tier needs this allowlist. Membership is not a quality ranking or a
//! default; it only means a node MAY earn verified rewards serving this exact
//! engine+weights.
//!
//! A set is only usable for verified rewards once every member has a real
//! `weights_sha256` (never fabricated — populated at ingestion from the actual
//! GGUF bytes) AND Shin has signed that exact populated version. No agent may
//! sign or bump `set_version`. Anything short of that is `draft` and gates
//! nothing in on the verified tier — see [`VerifiedInferenceSet::is_usable`].

use serde::{Deserialize, Serialize};
use std::collections::HashSet;
use std::path::Path;

pub const PENDING_INGESTION: &str = "PENDING-INGESTION";

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct EngineInfo {
    pub engine_id: String,
    #[serde(default)]
    pub note: String,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct Signing {
    pub signed_by: String,
    pub signature: String,
    #[serde(default)]
    pub rule: String,
}

impl Signing {
    /// A signature is only real once it stops being the ingestion placeholder.
    fn is_signed(&self) -> bool {
        let sig = self.signature.trim();
        !sig.is_empty() && !sig.to_ascii_uppercase().contains("PENDING")
    }
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct VisModel {
    pub name: String,
    pub tier: String,
    #[serde(default)]
    pub role: String,
    pub architecture: String,
    #[serde(default)]
    pub kind: String,
    #[serde(default)]
    pub quant: String,
    #[serde(default)]
    pub context_target: Option<u64>,
    #[serde(default)]
    pub min_vram_mb: u64,
    #[serde(default)]
    pub min_ram_mb: u64,
    pub weights_sha256: String,
}

impl VisModel {
    /// A row only participates in a usable set once its hash has actually been
    /// filled in from real GGUF bytes — never fabricated, never assumed.
    pub fn is_hash_populated(&self) -> bool {
        let h = self.weights_sha256.trim();
        h.len() == 64
            && h.chars().all(|c| c.is_ascii_hexdigit())
            && !h.eq_ignore_ascii_case(PENDING_INGESTION)
    }
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct VerifiedInferenceSet {
    pub schema: String,
    pub set_version: u64,
    /// e.g. "draft-pending-ingestion" — informational; usability is derived, not
    /// trusted from this string, so a stale status field can't accidentally gate
    /// something in.
    #[serde(default)]
    pub status: String,
    pub engine: EngineInfo,
    pub signing: Signing,
    pub models: Vec<VisModel>,
}

impl VerifiedInferenceSet {
    pub fn load_from_file(path: &Path) -> anyhow::Result<Self> {
        let raw = std::fs::read_to_string(path)?;
        Ok(serde_json::from_str(&raw)?)
    }

    /// The set is only usable for verified-tier rewards when EVERY member has a
    /// real, non-placeholder hash AND Shin has signed this exact version. Fails
    /// closed: an empty model list, an unsigned set, or any pending hash means
    /// nothing gates in.
    pub fn is_usable(&self) -> bool {
        !self.models.is_empty()
            && self.signing.is_signed()
            && self.models.iter().all(|m| m.is_hash_populated())
    }

    /// Members eligible to gate the verified tier right now (empty unless the
    /// whole set is signed + fully hash-populated — see [`Self::is_usable`]).
    fn usable_members(&self) -> &[VisModel] {
        if self.is_usable() {
            &self.models
        } else {
            &[]
        }
    }

    /// Is `(engine_id, weights_sha256)` a member of a *usable* (signed,
    /// hash-complete) set? This is the actual verified-tier admission check — a
    /// job is checked by content hash, not by name, so a same-named but
    /// different-quant/different-build file cannot slip in.
    pub fn admits(&self, engine_id: &str, weights_sha256: &str) -> bool {
        self.usable_members()
            .iter()
            .any(|m| m.weights_sha256.eq_ignore_ascii_case(weights_sha256) && self.engine.engine_id == engine_id)
    }

    /// VIS member names that fit `hw`, scoped to `operator_set` (the operator's
    /// opt-in list) — the shuffler's rotation candidate pool. Returns nothing
    /// unless the set is usable: an unsigned or hash-incomplete set earns on
    /// nothing, so it offers no rotation candidates either.
    pub fn rotation_candidates(&self, hw: &DetectedHardware, operator_set: &HashSet<String>) -> Vec<&VisModel> {
        self.usable_members()
            .iter()
            .filter(|m| operator_set.contains(&m.name))
            .filter(|m| hw.vram_mb >= m.min_vram_mb && hw.ram_mb >= m.min_ram_mb)
            .collect()
    }
}

/// Local machine capacity relevant to the rotation hardware gate. Deliberately
/// minimal and dependency-free (mirrors the fields `hone_provision::Hardware`
/// checks — VRAM/RAM — without pulling that crate into hone-node's workspace,
/// which is a separate, standalone-crate build graph today).
#[derive(Debug, Clone, Copy, Default)]
pub struct DetectedHardware {
    pub vram_mb: u64,
    pub ram_mb: u64,
}

#[cfg(test)]
mod tests {
    use super::*;

    fn model(name: &str, hash: &str) -> VisModel {
        VisModel {
            name: name.into(),
            tier: "flagship".into(),
            role: "general-reasoning".into(),
            architecture: "qwen3".into(),
            kind: "dense".into(),
            quant: "Q4_K_M".into(),
            context_target: Some(32768),
            min_vram_mb: 22000,
            min_ram_mb: 20000,
            weights_sha256: hash.into(),
        }
    }

    fn signed(sig: &str) -> Signing {
        Signing { signed_by: "shin".into(), signature: sig.into(), rule: String::new() }
    }

    fn set(models: Vec<VisModel>, signature: &str) -> VerifiedInferenceSet {
        VerifiedInferenceSet {
            schema: "hone.verified-inference-set/v1".into(),
            set_version: 0,
            status: "draft-pending-ingestion".into(),
            engine: EngineInfo { engine_id: "candle-gguf".into(), note: String::new() },
            signing: signed(signature),
            models,
        }
    }

    const REAL_HASH: &str = "a3f5c9e1b2d4f6a8c0e2b4d6f8a0c2e4b6d8f0a2c4e6b8d0f2a4c6e8b0d2f4a6";
    const REAL_HASH_2: &str = "b4a6c8e0d2f4b6a8c0e2d4f6a8c0e2d4f6a8c0e2d4f6a8c0e2d4f6a8c0e2d4f6";

    #[test]
    fn unsigned_set_admits_nothing() {
        let s = set(vec![model("qwen3.6-27b", REAL_HASH)], "PENDING — set is unsigned");
        assert!(!s.is_usable());
        assert!(!s.admits("candle-gguf", REAL_HASH));
    }

    #[test]
    fn pending_hash_admits_nothing_even_if_signed() {
        let s = set(vec![model("qwen3.6-27b", PENDING_INGESTION)], "vault-sig-abc123");
        assert!(!s.is_usable(), "one unhashed row must block the whole set");
        assert!(!s.admits("candle-gguf", PENDING_INGESTION));
    }

    #[test]
    fn empty_model_list_is_never_usable() {
        let s = set(vec![], "vault-sig-abc123");
        assert!(!s.is_usable());
    }

    #[test]
    fn signed_and_fully_hashed_set_admits_by_hash() {
        let s = set(vec![model("qwen3.6-27b", REAL_HASH), model("gpt-oss-20b", REAL_HASH_2)], "vault-sig-abc123");
        assert!(s.is_usable());
        assert!(s.admits("candle-gguf", REAL_HASH));
        assert!(s.admits("candle-gguf", REAL_HASH_2));
    }

    #[test]
    fn admits_is_case_insensitive_on_hash_but_exact_on_engine() {
        let s = set(vec![model("qwen3.6-27b", REAL_HASH)], "vault-sig-abc123");
        assert!(s.admits("candle-gguf", &REAL_HASH.to_ascii_uppercase()));
        assert!(!s.admits("some-other-engine", REAL_HASH), "wrong engine must not admit");
    }

    #[test]
    fn wrong_hash_never_admits_even_with_matching_name() {
        // Guards the exact scenario the design doc calls out: a same-named but
        // different-build/different-quant file must not slip in under the label.
        let s = set(vec![model("qwen3.6-27b", REAL_HASH)], "vault-sig-abc123");
        let impostor_hash = "0".repeat(64);
        assert_ne!(impostor_hash, REAL_HASH);
        assert!(!s.admits("candle-gguf", &impostor_hash));
    }

    #[test]
    fn one_pending_row_blocks_admission_of_the_already_hashed_rows_too() {
        // A partially-ingested set is draft, full stop — it must not let the rows
        // that already have a real hash through while the rest are pending.
        let s = set(
            vec![model("qwen3.6-27b", REAL_HASH), model("gpt-oss-20b", PENDING_INGESTION)],
            "vault-sig-abc123",
        );
        assert!(!s.is_usable());
        assert!(!s.admits("candle-gguf", REAL_HASH));
    }

    #[test]
    fn rotation_candidates_respect_hardware_gate_and_operator_opt_in() {
        let s = set(
            vec![
                model("qwen3.6-27b", REAL_HASH),      // needs 22000 MB VRAM
                model("gpt-oss-20b", REAL_HASH_2),
            ],
            "vault-sig-abc123",
        );
        let beefy = DetectedHardware { vram_mb: 24000, ram_mb: 32000 };
        let potato = DetectedHardware { vram_mb: 4000, ram_mb: 8000 };

        let mut opted_in = HashSet::new();
        opted_in.insert("qwen3.6-27b".to_string());
        opted_in.insert("gpt-oss-20b".to_string());

        let on_beefy = s.rotation_candidates(&beefy, &opted_in);
        assert_eq!(on_beefy.len(), 2);

        let on_potato = s.rotation_candidates(&potato, &opted_in);
        assert!(on_potato.is_empty(), "neither flagship fits a potato");

        let mut only_one = HashSet::new();
        only_one.insert("qwen3.6-27b".to_string());
        let scoped = s.rotation_candidates(&beefy, &only_one);
        assert_eq!(scoped.len(), 1);
        assert_eq!(scoped[0].name, "qwen3.6-27b");
    }

    #[test]
    fn unsigned_set_offers_no_rotation_candidates_regardless_of_hardware_fit() {
        let s = set(vec![model("qwen3.6-27b", REAL_HASH)], "PENDING — set is unsigned");
        let beefy = DetectedHardware { vram_mb: 24000, ram_mb: 32000 };
        let mut opted_in = HashSet::new();
        opted_in.insert("qwen3.6-27b".to_string());
        assert!(s.rotation_candidates(&beefy, &opted_in).is_empty());
    }
}
