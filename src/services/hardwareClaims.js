"use strict";

const crypto = require("crypto");
const stateStore = require("../chain/stateStore");

const SUPPORTED_TAKEOVER_TOKENS = new Set(["USDC", "USDT", "DAI"]);
const DEFAULT_TAKEOVER_USD = Number(process.env.BTCPC_HARDWARE_TAKEOVER_USD || 5);

const claims = new Map();

function _trim(value) {
  return typeof value === "string" ? value.trim() : "";
}

function _normalizeHash(hash) {
  const value = _trim(hash).toLowerCase();
  if (!value) return "";
  if (!/^[a-f0-9]{64}$/.test(value)) {
    throw new Error("hardware_hash must be 64-char hex");
  }
  return value;
}

function _sha256Hex(text) {
  return crypto.createHash("sha256").update(String(text || ""), "utf8").digest("hex");
}

function _currentPostingKey(owner) {
  const acct = stateStore.getAccount(owner);
  return acct && acct.public_keys && acct.public_keys.posting ? String(acct.public_keys.posting) : "";
}

function _postingKeyHash(postingKey) {
  const key = _trim(postingKey);
  if (!key) return null;
  return _sha256Hex("posting_key:" + key.toLowerCase());
}

function _validateTakeover(takeover) {
  const txHash = _trim(takeover && takeover.tx_hash);
  const token = _trim(takeover && takeover.token).toUpperCase();
  const usdAmount = Number(takeover && takeover.usd_amount);

  if (!txHash) throw new Error("hardware takeover tx_hash required");
  if (!SUPPORTED_TAKEOVER_TOKENS.has(token)) {
    throw new Error("hardware takeover token must be one of: USDC, USDT, DAI");
  }
  if (!Number.isFinite(usdAmount) || usdAmount < DEFAULT_TAKEOVER_USD) {
    throw new Error("hardware takeover usd_amount must be at least " + DEFAULT_TAKEOVER_USD);
  }

  return {
    tx_hash: txHash,
    token: token,
    usd_amount: usdAmount,
  };
}

function getHardwareClaim(hardwareHash) {
  if (!hardwareHash) return null;
  const normalized = _normalizeHash(hardwareHash);
  return claims.get(normalized) || null;
}

function getHardwareClaimByPostingKey(postingKey) {
  const pkHash = _postingKeyHash(postingKey);
  if (!pkHash) return null;
  for (const claim of claims.values()) {
    if (claim.posting_key_hash === pkHash) return claim;
  }
  return null;
}

function claimHardware(owner, hardwareHash, options) {
  if (!owner) throw new Error("owner required");
  const normalizedHash = _normalizeHash(hardwareHash);
  if (!normalizedHash) {
    return {
      hardware_hash: null,
      owner: owner,
      posting_key: null,
      posting_key_hash: null,
      hardware_id_kind: (options && options.hardware_id_kind) || null,
      hardware_id: (options && options.hardware_id) || null,
      claim_epoch: Number(options && options.epoch) || 0,
      last_updated_epoch: Number(options && options.epoch) || 0,
      status: "unbound",
      takeover_token: null,
      takeover_usd: null,
      takeover_tx_hash: null,
    };
  }
  const opts = options && typeof options === "object" ? options : {};
  const currentPostingKey = _trim(opts.posting_key) || _currentPostingKey(owner);
  const currentPostingKeyHash = _postingKeyHash(currentPostingKey);
  const takeover = opts.takeover ? _validateTakeover(opts.takeover) : null;
  const epoch = Number(opts.epoch) || 0;

  const existing = claims.get(normalizedHash);
  if (!existing) {
    const created = {
      hardware_hash: normalizedHash,
      owner: owner,
      posting_key: currentPostingKey || null,
      posting_key_hash: currentPostingKeyHash || null,
      hardware_id_kind: opts.hardware_id_kind || null,
      hardware_id: opts.hardware_id || null,
      claim_epoch: epoch,
      last_updated_epoch: epoch,
      status: "active",
      takeover_token: null,
      takeover_usd: null,
      takeover_tx_hash: null,
    };
    claims.set(normalizedHash, created);
    return created;
  }

  if (existing.owner === owner) {
    if (currentPostingKeyHash && existing.posting_key_hash && existing.posting_key_hash !== currentPostingKeyHash) {
      existing.posting_key = currentPostingKey || existing.posting_key || null;
      existing.posting_key_hash = currentPostingKeyHash;
    } else if (currentPostingKeyHash && !existing.posting_key_hash) {
      existing.posting_key = currentPostingKey || null;
      existing.posting_key_hash = currentPostingKeyHash;
    }
    if (opts.hardware_id_kind) existing.hardware_id_kind = opts.hardware_id_kind;
    if (opts.hardware_id) existing.hardware_id = opts.hardware_id;
    existing.last_updated_epoch = epoch;
    claims.set(normalizedHash, existing);
    return existing;
  }

  if (!takeover) {
    throw new Error("hardware_hash already claimed by " + existing.owner + " and requires a stablecoin takeover");
  }

  existing.owner = owner;
  existing.posting_key = currentPostingKey || existing.posting_key || null;
  existing.posting_key_hash = currentPostingKeyHash || existing.posting_key_hash || null;
  if (opts.hardware_id_kind) existing.hardware_id_kind = opts.hardware_id_kind;
  if (opts.hardware_id) existing.hardware_id = opts.hardware_id;
  existing.takeover_token = takeover.token;
  existing.takeover_usd = takeover.usd_amount;
  existing.takeover_tx_hash = takeover.tx_hash;
  existing.takeover_epoch = epoch;
  existing.last_updated_epoch = epoch;
  claims.set(normalizedHash, existing);
  return existing;
}

function resetForTests() {
  claims.clear();
}

module.exports = {
  claimHardware,
  getHardwareClaim,
  getHardwareClaimByPostingKey,
  resetForTests,
  supportedTakeoverTokens: Array.from(SUPPORTED_TAKEOVER_TOKENS),
  defaultTakeoverUsd: DEFAULT_TAKEOVER_USD,
};
