//! Model / job shuffler — the node-side scheduler that keeps a node from being
//! pinned to a single model.
//!
//! A worker node can serve many kinds of marketplace jobs (LLM inference, LoRA
//! fine-tune / training, embeddings, …) across a set of manifest-approved
//! engine+model+adapter combos. Naively bidding on "the one model I loaded" wastes
//! the node whenever that model has no work but a resident-adjacent model does.
//!
//! The shuffler continuously:
//!   1. pulls the open jobs it is *eligible* to serve (mode + manifest allowlist),
//!   2. scores each by expected value per unit of cost,
//!         value = reward * urgency(age) / (compute_cost + swap_cost)
//!      where `swap_cost` is ~0 when the job's BASE model is already resident, a
//!      small constant for a LoRA-adapter hot-swap on a resident base, and an
//!      expensive constant for loading a DIFFERENT base model,
//!   3. picks the top job, preferring the resident base and draining a batch of
//!      resident work before paying to swap a base model,
//!   4. resists thrashing: a base-model swap only happens when the pending value
//!      beats the load cost by a margin AND not more often than a cooldown.
//!
//! Memory is never oversubscribed: a job is only schedulable if its resident set
//! fits the VRAM budget. A `Hybrid` node reserves a latency slice for inference and
//! runs training in the remaining budget. Training jobs are *yieldable* — they
//! checkpoint so the node can pause one, grab a burst of high-value inference, and
//! resume without losing work.
//!
//! ## Scope / boundaries
//! This module is pure scheduling logic. It touches no consensus, clock, genesis,
//! chain-id, launch, or gate code. The real `candle` runtime is a follow-up; here
//! the runtime is abstracted behind [`ModelRuntime`] and a [`MockRuntime`] drives
//! every test with NO GPU.
//!
//! The public surface (trait, config, scheduler) is intentionally ahead of its
//! callers — the daemon wiring lands in a follow-up — so unused-warnings on the
//! not-yet-called API are expected until then.
#![allow(dead_code)]

use std::collections::HashMap;

// ── Config knobs ───────────────────────────────────────────────────────────────

/// Tunable scheduler parameters. All costs are in abstract "compute units" and all
/// VRAM figures in MiB so the mock and the real runtime share one currency.
#[derive(Debug, Clone)]
pub struct ShufflerConfig {
    /// A base-model swap only fires when the best pending job's raw value exceeds the
    /// value achievable on the *resident* base by at least this multiplicative margin.
    /// 1.0 = swap on any tie; 1.5 = require the newcomer to be 50% better.
    pub swap_value_margin: f64,
    /// Minimum number of scheduler ticks between two base-model swaps (anti-thrash
    /// cooldown). A swap attempt inside the cooldown is refused even if it clears the
    /// margin, so a noisy job stream cannot flap the base every tick.
    pub swap_cooldown_ticks: u64,
    /// How many resident-base jobs to drain before even *considering* a base swap,
    /// so a swap never preempts cheap already-loaded work that is still queued.
    pub drain_batch: usize,
    /// Compute-unit cost charged for loading a fresh BASE model (expensive).
    pub base_swap_cost: f64,
    /// Compute-unit cost charged for hot-swapping a LoRA adapter on a resident base
    /// (near-free relative to a base swap).
    pub adapter_swap_cost: f64,
    /// Fraction of the VRAM budget a `Hybrid` node reserves for latency-sensitive
    /// inference; training may only use the remainder. 0.0..=1.0.
    pub hybrid_inference_reserve: f64,
    /// Urgency half-life in epochs: a job's urgency multiplier climbs from 1.0 toward
    /// `urgency_max` as it ages, reaching the midpoint after this many epochs.
    pub urgency_halflife_epochs: f64,
    /// Ceiling on the age-urgency multiplier so an ancient job cannot dominate purely
    /// by starving — starvation is handled separately by an aging floor.
    pub urgency_max: f64,
}

impl Default for ShufflerConfig {
    fn default() -> Self {
        Self {
            swap_value_margin: 1.5,
            swap_cooldown_ticks: 4,
            drain_batch: 3,
            base_swap_cost: 100.0,
            adapter_swap_cost: 5.0,
            hybrid_inference_reserve: 0.5,
            urgency_halflife_epochs: 20.0,
            urgency_max: 4.0,
        }
    }
}

// ── Node capability / mode ─────────────────────────────────────────────────────

/// What a node is willing to schedule.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    /// Only latency-sensitive inference / embeddings work.
    Inference,
    /// Only background training / fine-tune work.
    Training,
    /// Both: reserve an inference slice, run training in the remainder.
    Hybrid,
}

/// A manifest-approved combo this node may serve. Mirrors the design doc's manifest
/// key at the granularity the scheduler needs: which base model, and (optionally)
/// which LoRA adapter, under which engine.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct ApprovedCombo {
    pub engine_id: String,
    pub base_model: String,
    /// `None` = the bare base model; `Some(id)` = base + this LoRA adapter.
    pub adapter: Option<String>,
}

impl ApprovedCombo {
    pub fn base(engine: &str, model: &str) -> Self {
        Self { engine_id: engine.into(), base_model: model.into(), adapter: None }
    }
    pub fn with_adapter(engine: &str, model: &str, adapter: &str) -> Self {
        Self { engine_id: engine.into(), base_model: model.into(), adapter: Some(adapter.into()) }
    }
}

/// The kind of work a job asks for. Gates against [`Mode`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum JobKind {
    Inference,
    Embeddings,
    Training,
}

impl JobKind {
    /// Whether a node in `mode` is allowed to run this kind of job at all.
    fn allowed_by(self, mode: Mode) -> bool {
        match (mode, self) {
            (Mode::Inference, JobKind::Inference | JobKind::Embeddings) => true,
            (Mode::Training, JobKind::Training) => true,
            (Mode::Hybrid, _) => true,
            _ => false,
        }
    }

    /// Latency-sensitive kinds draw from the reserved inference slice on a Hybrid node.
    fn is_latency_sensitive(self) -> bool {
        matches!(self, JobKind::Inference | JobKind::Embeddings)
    }
}

/// Static description of what this node can do.
#[derive(Debug, Clone)]
pub struct CapabilityProfile {
    pub node_id: String,
    pub mode: Mode,
    /// Total VRAM budget in MiB the scheduler may commit to resident models.
    pub vram_budget_mib: u64,
    /// The manifest-approved combos this node is permitted to serve.
    pub approved: Vec<ApprovedCombo>,
}

impl CapabilityProfile {
    /// True if this node may serve `job` (mode gate + manifest allowlist).
    pub fn can_serve(&self, job: &OpenJob) -> bool {
        if !job.kind.allowed_by(self.mode) {
            return false;
        }
        self.approved.iter().any(|c| c.matches(job))
    }
}

impl ApprovedCombo {
    fn matches(&self, job: &OpenJob) -> bool {
        self.engine_id == job.engine_id
            && self.base_model == job.base_model
            && self.adapter.as_deref() == job.adapter.as_deref()
    }
}

// ── Jobs ───────────────────────────────────────────────────────────────────────

/// A marketplace job the shuffler could bid on, projected down to just the fields
/// scheduling needs. The daemon builds these from on-chain `JobState`; the scheduler
/// never touches the chain, which is what makes it unit-testable with no GPU.
#[derive(Debug, Clone)]
pub struct OpenJob {
    pub job_id: String,
    pub kind: JobKind,
    pub engine_id: String,
    pub base_model: String,
    pub adapter: Option<String>,
    /// Reward in hunits (the max fee we could earn).
    pub reward: u64,
    /// Epoch the job was posted; combined with `now_epoch` to compute urgency.
    pub posted_epoch: u64,
    /// Abstract compute cost to actually run the job (tokens, steps…). Never zero.
    pub compute_cost: f64,
    /// VRAM the job's model needs resident, in MiB.
    pub vram_mib: u64,
}

/// A scored, schedulable job with its cost breakdown, ready for the picker.
#[derive(Debug, Clone)]
pub struct ScoredJob {
    pub job: OpenJob,
    /// Final value = reward * urgency / (compute_cost + swap_cost).
    pub value: f64,
    /// The swap cost that would be paid to run this job from the CURRENT resident set.
    pub swap_cost: f64,
    /// True when the job's base model is already resident (adapter may still swap).
    pub base_resident: bool,
    /// True when running this job requires loading a different BASE model.
    pub needs_base_swap: bool,
}

// ── Runtime abstraction ────────────────────────────────────────────────────────

/// Result of running a job on the runtime.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RunOutcome {
    pub job_id: String,
    /// Opaque result payload (a hash / text in the real impl).
    pub output: String,
    /// True if a yieldable job was paused at a checkpoint rather than finished.
    pub yielded: bool,
}

/// The model-execution backend. The scheduler drives it through this trait so the
/// real candle engine and the test mock are interchangeable.
pub trait ModelRuntime {
    /// Load a base model, making it the resident base. Returns its VRAM footprint (MiB).
    fn load_base(&mut self, model_id: &str, vram_mib: u64);
    /// Hot-swap a LoRA adapter onto the resident base. `None` clears any adapter.
    fn swap_adapter(&mut self, adapter_id: Option<&str>);
    /// Run (or resume) a job to completion or to its next checkpoint.
    fn run(&mut self, job: &OpenJob) -> RunOutcome;
    /// Currently resident base model id, if any.
    fn resident_base(&self) -> Option<&str>;
    /// Currently resident adapter id, if any.
    fn resident_adapter(&self) -> Option<&str>;
    /// VRAM committed to resident models right now, in MiB.
    fn vram_used_mib(&self) -> u64;
}

// ── Scheduler ──────────────────────────────────────────────────────────────────

/// A pending training checkpoint so a yielded job can resume where it left off.
#[derive(Debug, Clone)]
pub struct TrainingCheckpoint {
    pub job_id: String,
    pub base_model: String,
    pub adapter: Option<String>,
    /// Progress in [0.0, 1.0]. `run` advances it; 1.0 = done.
    pub progress: f64,
}

/// Reason a scheduler tick did what it did — surfaced for logging and asserted in tests.
#[derive(Debug, Clone, PartialEq)]
pub enum TickAction {
    /// Ran `job_id`; `swapped_base` indicates a base model was loaded this tick.
    Ran { job_id: String, swapped_base: bool },
    /// A base swap was warranted by value but refused by the cooldown.
    HeldByCooldown { job_id: String },
    /// Nothing eligible / schedulable this tick.
    Idle,
}

/// The node-side scheduler. Holds the runtime, capability profile, config, and the
/// anti-thrash / checkpoint state that must persist across ticks.
pub struct Shuffler<R: ModelRuntime> {
    pub runtime: R,
    pub profile: CapabilityProfile,
    pub cfg: ShufflerConfig,
    /// Monotonic tick counter (also used as the cooldown clock).
    tick: u64,
    /// Tick at which the last base swap happened (`None` = never).
    last_base_swap_tick: Option<u64>,
    /// How many resident-base jobs have run since the last base swap.
    resident_run_streak: usize,
    /// Paused training jobs keyed by job_id, awaiting resume.
    checkpoints: HashMap<String, TrainingCheckpoint>,
    /// Count of base swaps performed over this shuffler's life (test observability).
    pub base_swap_count: u64,
}

impl<R: ModelRuntime> Shuffler<R> {
    pub fn new(runtime: R, profile: CapabilityProfile, cfg: ShufflerConfig) -> Self {
        Self {
            runtime,
            profile,
            cfg,
            tick: 0,
            last_base_swap_tick: None,
            resident_run_streak: 0,
            checkpoints: HashMap::new(),
            base_swap_count: 0,
        }
    }

    /// Age → urgency multiplier: 1.0 for a fresh job, asymptotically approaching
    /// `urgency_max`, reaching the midpoint after `urgency_halflife_epochs`.
    fn urgency(&self, posted_epoch: u64, now_epoch: u64) -> f64 {
        let age = now_epoch.saturating_sub(posted_epoch) as f64;
        let h = self.cfg.urgency_halflife_epochs.max(1.0);
        // 1.0 at age 0, → urgency_max as age → ∞.
        let frac = age / (age + h); // 0 → 1
        1.0 + (self.cfg.urgency_max - 1.0) * frac
    }

    /// The VRAM ceiling this job may draw on given the node's mode. On a Hybrid node,
    /// background training is capped to the non-reserved remainder; latency-sensitive
    /// work may use the whole budget.
    fn vram_ceiling_for(&self, kind: JobKind) -> u64 {
        match self.profile.mode {
            Mode::Hybrid if !kind.is_latency_sensitive() => {
                let reserve = self.cfg.hybrid_inference_reserve.clamp(0.0, 1.0);
                ((self.profile.vram_budget_mib as f64) * (1.0 - reserve)) as u64
            }
            _ => self.profile.vram_budget_mib,
        }
    }

    /// Compute the swap cost of running `job` from the current resident set.
    fn swap_cost_for(&self, job: &OpenJob) -> (f64, bool, bool) {
        let base_resident = self.runtime.resident_base() == Some(job.base_model.as_str());
        if !base_resident {
            // Loading a different base model — expensive.
            (self.cfg.base_swap_cost, false, true)
        } else if self.runtime.resident_adapter() != job.adapter.as_deref() {
            // Base is resident, adapter differs — cheap hot-swap.
            (self.cfg.adapter_swap_cost, true, false)
        } else {
            // Fully resident — free.
            (0.0, true, false)
        }
    }

    /// Score one job, or `None` if it can't be scheduled (not eligible, or its model
    /// doesn't fit the VRAM ceiling for its kind).
    pub fn score(&self, job: &OpenJob, now_epoch: u64) -> Option<ScoredJob> {
        if !self.profile.can_serve(job) {
            return None;
        }
        if job.vram_mib > self.vram_ceiling_for(job.kind) {
            return None; // never oversubscribe memory
        }
        let (swap_cost, base_resident, needs_base_swap) = self.swap_cost_for(job);
        let urgency = self.urgency(job.posted_epoch, now_epoch);
        let denom = job.compute_cost.max(f64::MIN_POSITIVE) + swap_cost;
        let value = (job.reward as f64) * urgency / denom;
        Some(ScoredJob { job: job.clone(), value, swap_cost, base_resident, needs_base_swap })
    }

    /// Rank all schedulable jobs best-first.
    pub fn rank(&self, jobs: &[OpenJob], now_epoch: u64) -> Vec<ScoredJob> {
        let mut scored: Vec<ScoredJob> = jobs
            .iter()
            .filter_map(|j| self.score(j, now_epoch))
            .collect();
        // Sort by value desc; break ties deterministically by job_id so behaviour is
        // reproducible across machines (no float NaN sneaks in — value is finite).
        scored.sort_by(|a, b| {
            b.value
                .partial_cmp(&a.value)
                .unwrap_or(std::cmp::Ordering::Equal)
                .then_with(|| a.job.job_id.cmp(&b.job.job_id))
        });
        scored
    }

    /// Pick the job to run this tick, applying the drain-then-swap preference and the
    /// anti-thrash margin+cooldown. Returns the chosen scored job, or `None` if idle,
    /// alongside whether the pick is being blocked purely by the cooldown.
    fn choose<'a>(&self, ranked: &'a [ScoredJob]) -> (Option<&'a ScoredJob>, bool) {
        if ranked.is_empty() {
            return (None, false);
        }
        let best = &ranked[0];

        // If the top job needs a base swap, see whether we should instead drain a
        // resident job first, and whether the swap clears the anti-thrash gate.
        if best.needs_base_swap {
            let best_resident = ranked.iter().find(|s| !s.needs_base_swap);

            // Drain preference: keep serving resident work until the batch is spent,
            // as long as a resident job exists at all.
            if let Some(res) = best_resident {
                if self.resident_run_streak < self.cfg.drain_batch {
                    return (Some(res), false);
                }
                // Margin gate: the swap must beat the best resident value by the margin.
                if best.value < res.value * self.cfg.swap_value_margin {
                    return (Some(res), false);
                }
            }

            // Cooldown gate: even a warranted swap waits out the cooldown.
            if let Some(last) = self.last_base_swap_tick {
                if self.tick.saturating_sub(last) < self.cfg.swap_cooldown_ticks {
                    // Prefer a resident job if one exists; otherwise we're held.
                    if let Some(res) = best_resident {
                        return (Some(res), false);
                    }
                    return (None, true);
                }
            }
        }

        (Some(best), false)
    }

    /// Run one scheduler tick over the currently-open jobs. Returns what it did.
    ///
    /// This is the heart of the shuffler: rank → choose (with drain + hysteresis) →
    /// (swap if needed) → run. A yielded training job is checkpointed for resume.
    pub fn tick(&mut self, open_jobs: &[OpenJob], now_epoch: u64) -> TickAction {
        self.tick += 1;
        let ranked = self.rank(open_jobs, now_epoch);
        let (choice, held) = self.choose(&ranked);

        let chosen = match choice {
            Some(c) => c.clone(),
            None => {
                if held {
                    // We know there is a best job even though it's cooldown-blocked.
                    let jid = ranked.first().map(|s| s.job.job_id.clone()).unwrap_or_default();
                    return TickAction::HeldByCooldown { job_id: jid };
                }
                return TickAction::Idle;
            }
        };

        let swapped_base = chosen.needs_base_swap;
        if swapped_base {
            self.runtime.load_base(&chosen.job.base_model, chosen.job.vram_mib);
            self.runtime.swap_adapter(chosen.job.adapter.as_deref());
            self.last_base_swap_tick = Some(self.tick);
            self.base_swap_count += 1;
            self.resident_run_streak = 0;
        } else if self.runtime.resident_adapter() != chosen.job.adapter.as_deref() {
            self.runtime.swap_adapter(chosen.job.adapter.as_deref());
        }

        let outcome = self.runtime.run(&chosen.job);

        // Track resident-run streak for the drain preference.
        if !swapped_base {
            self.resident_run_streak = self.resident_run_streak.saturating_add(1);
        }

        // Yieldable training: persist a checkpoint so the job can resume later.
        if chosen.job.kind == JobKind::Training {
            let entry = self
                .checkpoints
                .entry(chosen.job.job_id.clone())
                .or_insert_with(|| TrainingCheckpoint {
                    job_id: chosen.job.job_id.clone(),
                    base_model: chosen.job.base_model.clone(),
                    adapter: chosen.job.adapter.clone(),
                    progress: 0.0,
                });
            if outcome.yielded {
                // advance progress toward completion but keep the checkpoint alive
                entry.progress = (entry.progress + 0.25).min(0.99);
            } else {
                entry.progress = 1.0;
                self.checkpoints.remove(&chosen.job.job_id);
            }
        }

        TickAction::Ran { job_id: chosen.job.job_id, swapped_base }
    }

    /// Live checkpoints (paused training jobs) — for observability / resume wiring.
    pub fn checkpoints(&self) -> &HashMap<String, TrainingCheckpoint> {
        &self.checkpoints
    }
}

// ── Mock runtime (tests only) ──────────────────────────────────────────────────

/// A no-GPU mock of [`ModelRuntime`] that records resident state and, for training
/// jobs, yields at a checkpoint until it has been run enough times to "finish".
#[cfg(test)]
pub struct MockRuntime {
    base: Option<String>,
    adapter: Option<String>,
    vram_mib: u64,
    /// How many times each training job has been run (drives the yield/finish flip).
    train_runs: HashMap<String, u32>,
    /// Runs required before a training job reports finished (yields until then).
    pub train_runs_to_finish: u32,
    /// Every job id ever run, in order — asserted in tests.
    pub run_log: Vec<String>,
}

#[cfg(test)]
impl MockRuntime {
    pub fn new() -> Self {
        Self {
            base: None,
            adapter: None,
            vram_mib: 0,
            train_runs: HashMap::new(),
            train_runs_to_finish: 2,
            run_log: Vec::new(),
        }
    }
}

#[cfg(test)]
impl ModelRuntime for MockRuntime {
    fn load_base(&mut self, model_id: &str, vram_mib: u64) {
        self.base = Some(model_id.to_owned());
        self.adapter = None; // loading a base clears the adapter
        self.vram_mib = vram_mib;
    }
    fn swap_adapter(&mut self, adapter_id: Option<&str>) {
        self.adapter = adapter_id.map(|s| s.to_owned());
    }
    fn run(&mut self, job: &OpenJob) -> RunOutcome {
        self.run_log.push(job.job_id.clone());
        let yielded = if job.kind == JobKind::Training {
            let n = self.train_runs.entry(job.job_id.clone()).or_insert(0);
            *n += 1;
            *n < self.train_runs_to_finish
        } else {
            false
        };
        RunOutcome { job_id: job.job_id.clone(), output: format!("out:{}", job.job_id), yielded }
    }
    fn resident_base(&self) -> Option<&str> {
        self.base.as_deref()
    }
    fn resident_adapter(&self) -> Option<&str> {
        self.adapter.as_deref()
    }
    fn vram_used_mib(&self) -> u64 {
        self.vram_mib
    }
}

// ── Tests ──────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn engine() -> &'static str { "hone-candle" }

    fn profile_hybrid(vram: u64, approved: Vec<ApprovedCombo>) -> CapabilityProfile {
        CapabilityProfile { node_id: "node-a".into(), mode: Mode::Hybrid, vram_budget_mib: vram, approved }
    }

    fn job(id: &str, kind: JobKind, model: &str, adapter: Option<&str>, reward: u64, vram: u64) -> OpenJob {
        OpenJob {
            job_id: id.into(),
            kind,
            engine_id: engine().into(),
            base_model: model.into(),
            adapter: adapter.map(|s| s.into()),
            reward,
            posted_epoch: 0,
            compute_cost: 10.0,
            vram_mib: vram,
        }
    }

    fn shuffler_with(profile: CapabilityProfile, cfg: ShufflerConfig) -> Shuffler<MockRuntime> {
        Shuffler::new(MockRuntime::new(), profile, cfg)
    }

    // ── scoring ────────────────────────────────────────────────────────────────

    #[test]
    fn scoring_prefers_resident_base_over_base_swap() {
        // Two equal-reward jobs: one on the resident base, one needing a base swap.
        let approved = vec![
            ApprovedCombo::base(engine(), "llama"),
            ApprovedCombo::base(engine(), "qwen"),
        ];
        let mut s = shuffler_with(profile_hybrid(10_000, approved), ShufflerConfig::default());
        s.runtime.load_base("llama", 2_000); // llama resident

        let resident = job("j-res", JobKind::Inference, "llama", None, 1_000, 2_000);
        let swap = job("j-swap", JobKind::Inference, "qwen", None, 1_000, 2_000);

        let ranked = s.rank(&[swap, resident], 0);
        assert_eq!(ranked[0].job.job_id, "j-res", "resident base must outrank a base swap at equal reward");
        assert!(ranked[0].base_resident);
        assert!(ranked[1].needs_base_swap);
        // The swap-cost penalty is exactly what flips them.
        assert!(ranked[0].value > ranked[1].value);
    }

    #[test]
    fn scoring_prefers_lora_hotswap_over_base_swap() {
        // Resident base "llama". A LoRA-adapter job on llama should beat an equal-reward
        // job that needs a whole different base loaded.
        let approved = vec![
            ApprovedCombo::with_adapter(engine(), "llama", "sql-lora"),
            ApprovedCombo::base(engine(), "qwen"),
        ];
        let mut s = shuffler_with(profile_hybrid(10_000, approved), ShufflerConfig::default());
        s.runtime.load_base("llama", 2_000);

        let lora = job("j-lora", JobKind::Inference, "llama", Some("sql-lora"), 1_000, 2_000);
        let base_swap = job("j-base", JobKind::Inference, "qwen", None, 1_000, 2_000);

        let ranked = s.rank(&[base_swap, lora], 0);
        assert_eq!(ranked[0].job.job_id, "j-lora", "LoRA hot-swap must be chosen over a base swap");
        assert_eq!(ranked[0].swap_cost, ShufflerConfig::default().adapter_swap_cost);
        assert_eq!(ranked[1].swap_cost, ShufflerConfig::default().base_swap_cost);
    }

    #[test]
    fn scoring_urgency_lifts_an_aging_job() {
        let approved = vec![ApprovedCombo::base(engine(), "llama")];
        let s = shuffler_with(profile_hybrid(10_000, approved), ShufflerConfig::default());

        let fresh = job("fresh", JobKind::Inference, "llama", None, 1_000, 2_000);
        let mut old = job("old", JobKind::Inference, "llama", None, 1_000, 2_000);
        old.posted_epoch = 0;

        // now_epoch far ahead → "old" has accrued urgency, "fresh" (posted this epoch) has not.
        let mut fresh_now = fresh.clone();
        fresh_now.posted_epoch = 100;
        let ranked = s.rank(&[fresh_now, old], 100);
        assert_eq!(ranked[0].job.job_id, "old", "the aged job must be lifted above an identical fresh one");
    }

    // ── eligibility / manifest gate ──────────────────────────────────────────────

    #[test]
    fn ineligible_jobs_are_not_scheduled() {
        // Node only approved for llama inference; qwen and unapproved adapters are filtered.
        let approved = vec![ApprovedCombo::base(engine(), "llama")];
        let s = shuffler_with(profile_hybrid(10_000, approved), ShufflerConfig::default());

        let ok = job("ok", JobKind::Inference, "llama", None, 1_000, 2_000);
        let wrong_model = job("wrong-model", JobKind::Inference, "qwen", None, 9_999, 2_000);
        let wrong_adapter = job("wrong-adapter", JobKind::Inference, "llama", Some("x"), 9_999, 2_000);

        let ranked = s.rank(&[ok, wrong_model, wrong_adapter], 0);
        assert_eq!(ranked.len(), 1);
        assert_eq!(ranked[0].job.job_id, "ok");
    }

    #[test]
    fn mode_gate_filters_training_on_inference_node() {
        let approved = vec![ApprovedCombo::base(engine(), "llama")];
        let mut prof = profile_hybrid(10_000, approved);
        prof.mode = Mode::Inference;
        let s = shuffler_with(prof, ShufflerConfig::default());

        let infer = job("infer", JobKind::Inference, "llama", None, 1_000, 2_000);
        let train = job("train", JobKind::Training, "llama", None, 5_000, 2_000);
        let ranked = s.rank(&[infer, train], 0);
        assert_eq!(ranked.len(), 1, "an Inference-mode node must not schedule training");
        assert_eq!(ranked[0].job.job_id, "infer");
    }

    // ── VRAM budget ──────────────────────────────────────────────────────────────

    #[test]
    fn vram_budget_never_oversubscribed() {
        let approved = vec![ApprovedCombo::base(engine(), "big")];
        let s = shuffler_with(profile_hybrid(4_000, approved), ShufflerConfig::default());
        // Job needs 8 GiB, budget is 4 GiB → not schedulable.
        let too_big = job("too-big", JobKind::Inference, "big", None, 1_000_000, 8_000);
        assert!(s.score(&too_big, 0).is_none(), "a model larger than the VRAM budget must never be scheduled");
    }

    #[test]
    fn hybrid_reserves_inference_slice_for_training_budget() {
        // 10 GiB budget, 50% reserved for inference → training may use at most 5 GiB.
        let approved = vec![
            ApprovedCombo::base(engine(), "m"),
        ];
        let mut cfg = ShufflerConfig::default();
        cfg.hybrid_inference_reserve = 0.5;
        let s = shuffler_with(profile_hybrid(10_000, approved), cfg);

        // A training job needing 6 GiB exceeds the 5 GiB training remainder → rejected...
        let train_big = job("train-big", JobKind::Training, "m", None, 1_000, 6_000);
        assert!(s.score(&train_big, 0).is_none(), "Hybrid training must honor the reserved inference slice");

        // ...but a latency inference job of the same size may use the whole budget.
        let infer_big = job("infer-big", JobKind::Inference, "m", None, 1_000, 6_000);
        assert!(s.score(&infer_big, 0).is_some(), "latency inference may draw on the full budget");
    }

    // ── anti-thrash ──────────────────────────────────────────────────────────────

    #[test]
    fn mixed_stream_does_not_flap_base_models() {
        // A stream that alternates which model has the highest raw reward must NOT
        // swap the base every tick. With drain_batch + cooldown, swaps stay bounded.
        let approved = vec![
            ApprovedCombo::base(engine(), "llama"),
            ApprovedCombo::base(engine(), "qwen"),
        ];
        let mut cfg = ShufflerConfig::default();
        cfg.drain_batch = 2;
        cfg.swap_cooldown_ticks = 3;
        cfg.swap_value_margin = 1.5;
        let mut s = shuffler_with(profile_hybrid(10_000, approved), cfg);
        s.runtime.load_base("llama", 2_000);

        // Both models always have work available; qwen's reward jitters just above llama.
        let ticks = 30;
        for t in 0..ticks {
            let llama = job(&format!("l{t}"), JobKind::Inference, "llama", None, 1_000, 2_000);
            // qwen only marginally better — below the 1.5x margin, so it should NOT justify a swap.
            let qwen = job(&format!("q{t}"), JobKind::Inference, "qwen", None, 1_100, 2_000);
            s.tick(&[llama, qwen], t);
        }
        assert!(
            s.base_swap_count <= 1,
            "marginal jitter must not flap the base model every tick (swaps={})",
            s.base_swap_count
        );
    }

    #[test]
    fn worthwhile_swap_eventually_happens() {
        // If the other model is *decisively* more valuable, a swap must occur (once),
        // otherwise the node would ignore real money to avoid thrash.
        let approved = vec![
            ApprovedCombo::base(engine(), "llama"),
            ApprovedCombo::base(engine(), "qwen"),
        ];
        let mut s = shuffler_with(profile_hybrid(10_000, approved), ShufflerConfig::default());
        s.runtime.load_base("llama", 2_000);

        let mut swapped = false;
        for t in 0..20 {
            let llama = job(&format!("l{t}"), JobKind::Inference, "llama", None, 100, 2_000);
            let qwen = job(&format!("q{t}"), JobKind::Inference, "qwen", None, 100_000, 2_000);
            if let TickAction::Ran { swapped_base: true, .. } = s.tick(&[llama, qwen], t) {
                swapped = true;
            }
        }
        assert!(swapped, "a decisively higher-value model must eventually win a base swap");
        assert!(s.base_swap_count >= 1);
    }

    #[test]
    fn starving_lora_queue_is_eventually_served() {
        // A LoRA queue that is never the single top job must still get served — no starvation.
        // Base resident is "llama". A LoRA on llama is always present but lower reward than
        // fresh base-llama inference; urgency must eventually lift it.
        let approved = vec![
            ApprovedCombo::base(engine(), "llama"),
            ApprovedCombo::with_adapter(engine(), "llama", "sql-lora"),
        ];
        let s_cfg = ShufflerConfig { urgency_halflife_epochs: 5.0, urgency_max: 10.0, ..Default::default() };
        let mut s = shuffler_with(profile_hybrid(10_000, approved), s_cfg);
        s.runtime.load_base("llama", 2_000);

        // The LoRA job is posted at epoch 0 and never re-posted; base jobs are always fresh.
        let lora = job("lora-starving", JobKind::Inference, "llama", Some("sql-lora"), 500, 2_000);
        let mut served_lora = false;
        for t in 0..60u64 {
            let mut fresh_base = job(&format!("b{t}"), JobKind::Inference, "llama", None, 600, 2_000);
            fresh_base.posted_epoch = t; // genuinely fresh each tick → no accrued urgency
            let lora_t = lora.clone(); // posted_epoch stays 0 → ages
            if let TickAction::Ran { job_id, .. } = s.tick(&[fresh_base, lora_t], t) {
                if job_id == "lora-starving" {
                    served_lora = true;
                    break;
                }
            }
        }
        assert!(served_lora, "an aging LoRA job must eventually be served (no starvation)");
    }

    // ── yieldable training ───────────────────────────────────────────────────────

    #[test]
    fn training_job_checkpoints_and_can_be_preempted() {
        // Start a training job (yields after 1 run), then a burst of high-value inference
        // preempts it; the training checkpoint must survive for resume.
        let approved = vec![
            ApprovedCombo::base(engine(), "llama"),
        ];
        let mut cfg = ShufflerConfig::default();
        cfg.drain_batch = 0; // don't force draining for this test
        let mut s = shuffler_with(profile_hybrid(10_000, approved), cfg);
        s.runtime.train_runs_to_finish = 3; // needs 3 runs to finish
        s.runtime.load_base("llama", 2_000);

        // Tick 1: only a training job available → it runs and yields (checkpoint created).
        let train = job("train-1", JobKind::Training, "llama", None, 100, 2_000);
        s.tick(&[train.clone()], 0);
        assert!(s.checkpoints().contains_key("train-1"), "training must checkpoint on yield");
        let p1 = s.checkpoints()["train-1"].progress;
        assert!(p1 > 0.0 && p1 < 1.0);

        // Tick 2: a high-value inference job appears → it preempts; checkpoint stays.
        let hot = job("hot", JobKind::Inference, "llama", None, 50_000, 2_000);
        let action = s.tick(&[hot, train.clone()], 1);
        assert_eq!(action, TickAction::Ran { job_id: "hot".into(), swapped_base: false });
        assert!(s.checkpoints().contains_key("train-1"), "checkpoint must survive preemption");

        // Resume training across two more ticks → it finishes and the checkpoint clears.
        s.tick(&[train.clone()], 2);
        s.tick(&[train.clone()], 3);
        assert!(!s.checkpoints().contains_key("train-1"), "finished training clears its checkpoint");
    }

    #[test]
    fn idle_when_no_eligible_jobs() {
        let approved = vec![ApprovedCombo::base(engine(), "llama")];
        let mut s = shuffler_with(profile_hybrid(10_000, approved), ShufflerConfig::default());
        // Only an unapproved job on offer.
        let wrong = job("wrong", JobKind::Inference, "qwen", None, 1_000, 2_000);
        assert_eq!(s.tick(&[wrong], 0), TickAction::Idle);
    }
}
