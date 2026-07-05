//! Inference ENGINE — the model runner (distinct from `inference.rs`, which is
//! the inference-job *marketplace* state machine). This is the ONE place the
//! node executes a model to produce tokens.
//!
//! Before this, the node called an external Ollama daemon over HTTP
//! (`{OLLAMA_URL}/api/chat`) from ~6 scattered sites. That daemon being down is
//! exactly what 503'd bullship at launch. This module fixes both fragilities:
//!
//!   1. **Embedded backend (default):** a candle GGUF model runs IN-PROCESS
//!      (ported from the proven `btcpc-android/src/llm.rs`). If the node is
//!      alive, inference is alive — no external daemon, no port, no 503.
//!   2. **One module:** all callers go through `chat` / `available`; the backend
//!      choice lives here alone.
//!
//! Backend selection (once, at startup):
//!   - `INFERENCE_URL` (or legacy `OLLAMA_URL`) set → route to that external
//!     OpenAI/Ollama-compatible server (how a GPU node points at vLLM).
//!   - else, built with `inference-embedded` → embedded candle GGUF.
//!   - else → unavailable (relay-only node).
//!
//! See docs/INFERENCE_EMBED_SPEC.md and docs/INFERENCE_MIGRATION_NOTES.md.
#![allow(dead_code)]

use anyhow::{anyhow, Result};
use serde::{Deserialize, Serialize};
use std::sync::OnceLock;

/// A chat message (OpenAI/Ollama shape).
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Message {
    pub role: String,
    pub content: String,
}

/// A chat request. `model` is advisory for the embedded backend (it serves the
/// single loaded GGUF); it's forwarded verbatim to an external backend.
#[derive(Debug, Clone)]
pub struct ChatRequest {
    pub model: String,
    pub messages: Vec<Message>,
    pub max_tokens: usize,
}

/// A completed (non-streaming) chat response.
#[derive(Debug, Clone)]
pub struct ChatResponse {
    pub content: String,
}

/// How this node serves inference. Chosen once at startup.
enum Backend {
    /// External OpenAI/Ollama-compatible HTTP server (e.g. vLLM, or a legacy
    /// Ollama). `base` has no trailing slash.
    Http { base: String },
    /// In-process candle GGUF. Always available once the model is resident.
    #[cfg(feature = "inference-embedded")]
    Embedded,
    /// No inference on this node (relay-only).
    None,
}

static BACKEND: OnceLock<Backend> = OnceLock::new();

/// Resolve the backend once. Priority: explicit external URL → embedded → none.
fn backend() -> &'static Backend {
    BACKEND.get_or_init(|| {
        // Explicit external server wins (the vLLM / GPU-node path). Accept the
        // new INFERENCE_URL first, then the deprecated OLLAMA_URL alias.
        let ext = std::env::var("INFERENCE_URL")
            .ok()
            .or_else(|| std::env::var("OLLAMA_URL").ok())
            .map(|u| u.trim_end_matches('/').to_owned())
            .filter(|u| !u.is_empty());
        if let Some(base) = ext {
            tracing::info!("inference-engine: external backend at {base}");
            return Backend::Http { base };
        }
        #[cfg(feature = "inference-embedded")]
        {
            tracing::info!("inference-engine: embedded candle GGUF backend");
            return Backend::Embedded;
        }
        #[allow(unreachable_code)]
        {
            tracing::warn!("inference-engine: no backend (relay-only — no INFERENCE_URL, embedded feature off)");
            Backend::None
        }
    })
}

/// Is inference available on this node right now?
/// - Http: assumed available (the external server is the operator's concern).
/// - Embedded: true once the model is resident.
/// - None: false.
pub fn available() -> bool {
    match backend() {
        Backend::Http { .. } => true,
        #[cfg(feature = "inference-embedded")]
        Backend::Embedded => candle_backend::available(),
        Backend::None => false,
    }
}

/// Non-streaming chat completion. Returns the assistant content.
pub async fn chat(req: ChatRequest) -> Result<ChatResponse> {
    match backend() {
        Backend::Http { base } => http_chat(base, &req).await,
        #[cfg(feature = "inference-embedded")]
        Backend::Embedded => candle_backend::chat(&req).await,
        Backend::None => Err(anyhow!("inference not available on this node")),
    }
}

/// Startup hook — ensure the embedded model is present (downloads once, ~400 MB).
/// No-op for the HTTP/none backends. Background it at boot so it doesn't block.
pub async fn warm_up() {
    #[cfg(feature = "inference-embedded")]
    if matches!(backend(), Backend::Embedded) {
        let _ = candle_backend::ensure_ready().await;
    }
}

// ── External HTTP backend (vLLM / legacy Ollama) ─────────────────────────────
// Preserves the /api/chat request/response shape the old scattered call sites
// used against Ollama, so an Ollama-compatible server is a drop-in.

async fn http_chat(base: &str, req: &ChatRequest) -> Result<ChatResponse> {
    let body = serde_json::json!({
        "model": req.model,
        "messages": req.messages,
        "stream": false,
        "options": { "num_predict": req.max_tokens },
    });
    let http = reqwest::Client::new();
    let resp = http
        .post(format!("{base}/api/chat"))
        .json(&body)
        .send()
        .await?
        .json::<serde_json::Value>()
        .await?;
    // Ollama: { "message": { "content" } }. OpenAI: { "choices":[{"message":{"content"}}] }.
    let content = resp["message"]["content"]
        .as_str()
        .or_else(|| resp["choices"][0]["message"]["content"].as_str())
        .unwrap_or("")
        .to_owned();
    Ok(ChatResponse { content })
}

// ── Embedded candle GGUF backend ─────────────────────────────────────────────
// Ported from btcpc-android/src/llm.rs — the proven in-process candle path.

#[cfg(feature = "inference-embedded")]
mod candle_backend {
    use super::{ChatRequest, ChatResponse, Message};
    use anyhow::{anyhow, bail, Result};
    use std::path::{Path, PathBuf};
    use std::sync::OnceLock;

    // Default model — matches the phone tier (btcpc-android). Overridable via env.
    const HF_REPO: &str = "Qwen/Qwen2.5-0.5B-Instruct-GGUF";
    const GGUF_FILE: &str = "qwen2.5-0.5b-instruct-q4_k_m.gguf";
    const TOKENIZER_REPO: &str = "Qwen/Qwen2.5-0.5B-Instruct";

    static MODEL_PATH: OnceLock<PathBuf> = OnceLock::new();

    fn model_dir() -> PathBuf {
        std::env::var("HONE_MODEL_DIR")
            .map(PathBuf::from)
            .unwrap_or_else(|_| {
                dirs_next::data_dir()
                    .unwrap_or_else(|| PathBuf::from("."))
                    .join("btcpc")
                    .join("models")
            })
    }

    fn model_path() -> &'static PathBuf {
        MODEL_PATH.get_or_init(|| model_dir().join(GGUF_FILE))
    }

    /// True once the GGUF is on disk.
    pub fn available() -> bool {
        model_path().exists()
    }

    /// Download the model + tokenizer once, if missing. Idempotent.
    pub async fn ensure_ready() -> bool {
        if available() {
            return true;
        }
        let dir = model_dir();
        let _ = std::fs::create_dir_all(&dir);
        let res = tokio::task::spawn_blocking(move || -> Result<()> {
            let api = hf_hub::api::sync::ApiBuilder::new()
                .with_cache_dir(dir.clone())
                .build()?;
            let gguf = api.model(HF_REPO.to_owned()).get(GGUF_FILE)?;
            let _ = std::fs::copy(&gguf, dir.join(GGUF_FILE));
            let tok = api.model(TOKENIZER_REPO.to_owned()).get("tokenizer.json")?;
            let _ = std::fs::copy(&tok, dir.join("tokenizer.json"));
            Ok(())
        })
        .await;
        match res {
            Ok(Ok(())) => {
                tracing::info!("inference-engine: embedded model ready at {}", model_path().display());
                true
            }
            Ok(Err(e)) => {
                tracing::warn!("inference-engine: embedded model download failed: {e}");
                false
            }
            Err(e) => {
                tracing::warn!("inference-engine: embedded model download task failed: {e}");
                false
            }
        }
    }

    /// Flatten chat messages into a Qwen-template prompt.
    fn format_prompt(messages: &[Message]) -> String {
        let mut s = String::new();
        for m in messages {
            s.push_str(&format!("<|im_start|>{}\n{}<|im_end|>\n", m.role, m.content));
        }
        s.push_str("<|im_start|>assistant\n");
        s
    }

    pub async fn chat(req: &ChatRequest) -> Result<ChatResponse> {
        if !available() {
            bail!("embedded model not ready (still downloading?)");
        }
        let prompt = format_prompt(&req.messages);
        let max_tokens = req.max_tokens.max(1);
        let path = model_path().clone();
        let content =
            tokio::task::spawn_blocking(move || generate_sync(&path, &prompt, max_tokens)).await??;
        Ok(ChatResponse { content })
    }

    /// The candle generate loop — ported from btcpc-android/src/llm.rs. Greedy.
    fn generate_sync(model_path: &Path, prompt: &str, max_tokens: usize) -> Result<String> {
        use candle_core::quantized::gguf_file;
        use candle_core::{Device, Tensor};
        use candle_transformers::models::quantized_llama::{ModelWeights, MAX_SEQ_LEN};
        use tokenizers::Tokenizer;

        let device = Device::Cpu;

        let mut file = std::fs::File::open(model_path)?;
        let gguf = gguf_file::Content::read(&mut file)?;
        let mut model = ModelWeights::from_gguf(gguf, &mut file, &device)?;

        let tok_path = model_path.parent().unwrap().join("tokenizer.json");
        let tokenizer = if tok_path.exists() {
            Tokenizer::from_file(&tok_path).map_err(|e| anyhow!("tokenizer load: {e}"))?
        } else {
            let api = hf_hub::api::sync::Api::new()?;
            let f = api.model(TOKENIZER_REPO.to_owned()).get("tokenizer.json")?;
            let _ = std::fs::copy(&f, &tok_path);
            Tokenizer::from_file(&f).map_err(|e| anyhow!("tokenizer load: {e}"))?
        };

        let encoding = tokenizer.encode(prompt, true).map_err(|e| anyhow!("tokenize: {e}"))?;
        let mut tokens: Vec<u32> = encoding.get_ids().to_vec();
        let vocab = tokenizer.get_vocab(true);
        let eos = vocab
            .get("<|im_end|>")
            .or_else(|| vocab.get("<|endoftext|>"))
            .copied()
            .unwrap_or(151643);

        let mut out = String::new();
        for _ in 0..max_tokens {
            let input = Tensor::new(tokens.as_slice(), &device)?.unsqueeze(0)?;
            let logits = model.forward(&input, tokens.len().saturating_sub(1))?;
            let logits = logits.squeeze(0)?;
            let next = logits.argmax(candle_core::D::Minus1)?.to_scalar::<u32>()?;
            if next == eos {
                break;
            }
            let piece = tokenizer.decode(&[next], true).map_err(|e| anyhow!("decode: {e}"))?;
            out.push_str(&piece);
            tokens.push(next);
            if tokens.len() >= MAX_SEQ_LEN {
                break;
            }
        }
        Ok(out)
    }
}
