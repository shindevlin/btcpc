"use strict";

const crypto = require("crypto");
const axios = require("axios");
const User = require("../models/User");
const chainLink = require("./chainLink");

const enrollmentChallenges = new Map();
const transferChallenges = new Map();

const CHAIN_ALIASES = {
  eth: "evm",
  base: "evm",
  arbitrum: "evm",
  optimism: "evm",
  polygon: "evm",
  bsc: "evm",
  ln: "lightning",
  lnd: "lightning",
  lightning: "lightning",
  zk: "zkvm",
  sp1: "zkvm",
  noir: "zkvm",
  zkvm: "zkvm"
};

function normalizeAuthChain(chain) {
  const value = String(chain || "").trim().toLowerCase();
  return CHAIN_ALIASES[value] || value;
}

function isPrivateAuthEnabled() {
  return String(process.env.BTCPC_PRIVATE_AUTH_ENABLED || "").toLowerCase() === "true";
}

function supportedChain(chain) {
  return ["evm", "bitcoin", "solana", "ton", "lightning", "zkvm"].includes(normalizeAuthChain(chain));
}

function normalizeAddress(chain, address) {
  const family = normalizeAuthChain(chain);
  const value = String(address || "").trim();
  return family === "evm" || family === "bitcoin" || family === "ton"
    ? value.toLowerCase()
    : value;
}

function commitmentFor(chain, address) {
  return crypto.createHash("sha256").update(normalizeAuthChain(chain) + "|" + normalizeAddress(chain, address)).digest("hex");
}

function factorCommitment(chain, username, factorId) {
  return crypto.createHash("sha256").update([
    normalizeAuthChain(chain),
    String(username || "").trim(),
    String(factorId || "").trim()
  ].join("|")).digest("hex");
}

function getLightningProviderConfig() {
  const baseUrl = process.env.BTCPC_LIGHTNING_PROVIDER_URL || process.env.BTCPC_LIGHTNING_API_URL || "";
  return {
    baseUrl: baseUrl.replace(/\/+$/, ""),
    apiKey: process.env.BTCPC_LIGHTNING_PROVIDER_KEY || process.env.BTCPC_LIGHTNING_API_KEY || "",
    timeoutMs: Number(process.env.BTCPC_LIGHTNING_TIMEOUT_MS) || 10000,
    allowLocalFallback: String(process.env.BTCPC_PRIVATE_AUTH_ALLOW_LIGHTNING_FALLBACK || "").toLowerCase() === "true"
  };
}

function getZkVerifierConfig() {
  const baseUrl = process.env.BTCPC_ZK_VERIFIER_URL || "";
  return {
    baseUrl: baseUrl.replace(/\/+$/, ""),
    apiKey: process.env.BTCPC_ZK_VERIFIER_KEY || "",
    timeoutMs: Number(process.env.BTCPC_ZK_TIMEOUT_MS) || 10000,
    proofBackend: String(process.env.BTCPC_ZK_PROOF_BACKEND || "sp1").toLowerCase()
  };
}

async function createLightningInvoice(payload) {
  const cfg = getLightningProviderConfig();
  if (!cfg.baseUrl) {
    if (!cfg.allowLocalFallback) {
      throw new Error("Lightning provider not configured");
    }
    return {
      invoice: null,
      payment_hash: crypto.randomBytes(32).toString("hex"),
      provider: null
    };
  }
  const headers = {};
  if (cfg.apiKey) headers.Authorization = "Bearer " + cfg.apiKey;
  const res = await axios.post(
    cfg.baseUrl + "/invoices",
    payload,
    { headers, timeout: cfg.timeoutMs }
  );
  const data = res.data || {};
  return {
    invoice: data.invoice || data.payment_request || null,
    payment_hash: data.payment_hash || data.hash || payload.payment_hash || null,
    provider: cfg.baseUrl
  };
}

async function verifyLightningInvoice(paymentHash, expectedAmount, receipt) {
  const cfg = getLightningProviderConfig();
  if (!cfg.baseUrl) {
    if (!cfg.allowLocalFallback) {
      throw new Error("Lightning provider not configured");
    }
    if (receipt && receipt.payment_hash === paymentHash && receipt.settled === true) {
      return { settled: true, provider: null };
    }
    throw new Error("Lightning provider not configured");
  }
  const headers = {};
  if (cfg.apiKey) headers.Authorization = "Bearer " + cfg.apiKey;
  const res = await axios.get(
    cfg.baseUrl + "/payments/" + encodeURIComponent(paymentHash),
    { headers, timeout: cfg.timeoutMs }
  );
  const data = res.data || {};
  if (!data.settled && data.status !== "settled" && data.status !== "paid") {
    throw new Error("Lightning invoice not settled");
  }
  if (expectedAmount && Number(data.amount_sats || data.amount || 0) < Number(expectedAmount)) {
    throw new Error("Lightning settlement amount below requested amount");
  }
  return { settled: true, provider: cfg.baseUrl, payment_hash: paymentHash };
}

async function verifyZkReceipt(challenge, approval) {
  const cfg = getZkVerifierConfig();
  if (!cfg.baseUrl) {
    throw new Error("ZK verifier not configured");
  }
  const headers = {};
  if (cfg.apiKey) headers.Authorization = "Bearer " + cfg.apiKey;
  const res = await axios.post(
    cfg.baseUrl + "/verify",
    {
      backend: String((approval && approval.proof_backend) || (challenge && challenge.proofBackend) || cfg.proofBackend || "sp1").toLowerCase(),
      challenge,
      proof: approval.proof || approval.receipt || approval
    },
    { headers, timeout: cfg.timeoutMs }
  );
  const data = res.data || {};
  if (!data.valid && !data.success) {
    throw new Error(data.error || "ZK proof rejected");
  }
  return data;
}

function cleanLabel(label) {
  if (typeof label !== "string") return null;
  const trimmed = label.trim();
  return trimmed ? trimmed.slice(0, 80) : null;
}

function createChallengeId() {
  return crypto.randomBytes(16).toString("hex");
}

function pruneMap(map, ttlMs) {
  const now = Date.now();
  for (const [key, value] of map.entries()) {
    if (!value || value.expiresAt <= now || (ttlMs && now - value.createdAt > ttlMs)) {
      map.delete(key);
    }
  }
}

function resetState() {
  enrollmentChallenges.clear();
  transferChallenges.clear();
}

function recoverAddress(chain, message, signature, claimedAddress) {
  const family = normalizeAuthChain(chain);
  if (family === "evm") return chainLink.recoverEVMAddress(message, signature);
  if (family === "bitcoin") return chainLink.recoverBitcoinAddress(message, signature);
  if (family === "solana") return chainLink.recoverSolanaAddress(message, signature, claimedAddress);
  if (family === "ton") return chainLink.recoverTONAddress(message, signature, claimedAddress);
  throw new Error("Unsupported authorization chain: " + chain);
}

async function getUser(username) {
  const user = await User.findOne({ username: String(username || "").trim() });
  if (!user) throw new Error("User not found");
  if (!user.privateAuth) {
    user.privateAuth = { enabled: false, threshold: 1, factors: [], updatedAt: new Date() };
  }
  if (!Array.isArray(user.privateAuth.factors)) user.privateAuth.factors = [];
  if (!user.privateAuth.threshold || user.privateAuth.threshold < 1) user.privateAuth.threshold = 1;
  return user;
}

function getPolicySnapshot(user) {
  const privateAuth = user.privateAuth || {};
  const chains = [];
  if (Array.isArray(privateAuth.factors)) {
    for (const factor of privateAuth.factors) {
      const chain = normalizeAuthChain(factor.chain);
      if (chain && !chains.includes(chain)) chains.push(chain);
    }
  }
  return {
    enabled: !!privateAuth.enabled && isPrivateAuthEnabled(),
    configuredEnabled: !!privateAuth.enabled,
    runtimeEnabled: isPrivateAuthEnabled(),
    threshold: Math.max(1, Number(privateAuth.threshold) || 1),
    factorCount: Array.isArray(privateAuth.factors) ? privateAuth.factors.length : 0,
    chains,
    factors: Array.isArray(privateAuth.factors)
      ? privateAuth.factors.map((factor) => ({
          factorId: factor.factorId,
          chain: factor.chain,
          label: factor.label || null,
          createdAt: factor.createdAt || null
        }))
      : []
  };
}

function buildEnrollmentMessage(username, chain, factorId, challengeId) {
  return [
    "BTCPC-PRIVATE-AUTH:ENROLL",
    String(username),
    normalizeAuthChain(chain),
    String(factorId),
    String(challengeId)
  ].join(":");
}

function buildTransferMessage(request) {
  return [
    "BTCPC-PRIVATE-AUTH:TRANSFER",
    String(request.username),
    String(request.requestId),
    String(request.approvalChain || ""),
    String(request.from),
    String(request.to),
    String(request.amount),
    String(request.token || "BTCPC"),
    String(request.memo || ""),
    String(request.threshold)
  ].join(":");
}

function getChainPreviewCopy(chain) {
  const family = normalizeAuthChain(chain);
  if (family === "bitcoin") {
    return {
      title: "Bitcoin approval preview",
      summary: "Use a signed Bitcoin challenge as the approval receipt.",
      note: "This is the simplest existing-wallet path and stays readable to the user."
    };
  }
  if (family === "lightning") {
    return {
      title: "Lightning approval preview",
      summary: "Use a Lightning invoice payment as the approval receipt.",
      note: "The payer confirms a BOLT11-style invoice; BTCPC only stores the receipt."
    };
  }
  if (family === "zkvm") {
    return {
      title: "ZKVM approval preview",
      summary: "Use a portable proof backend to verify the hidden approval.",
      note: "The future verifier can swap between supported proof backends without changing policy."
    };
  }
  if (family === "evm") {
    return {
      title: "EVM approval preview",
      summary: "Use an existing EVM wallet signature as the approval receipt.",
      note: "This is bridge support while the hidden-approval stack matures."
    };
  }
  if (family === "solana") {
    return {
      title: "Solana approval preview",
      summary: "Use an existing Solana wallet signature as the approval receipt.",
      note: "This follows the same staged flow and stays chain-neutral at the policy layer."
    };
  }
  if (family === "ton") {
    return {
      title: "TON approval preview",
      summary: "Use a TON wallet signature as the approval receipt.",
      note: "The preview remains read-only until the future rollout is enabled."
    };
  }
  return {
    title: "Private authorization preview",
    summary: "Read-only staged preview.",
    note: "The future approval flow is visible in code but disabled by default."
  };
}

function buildEnrollmentPreview(username, chain, label) {
  const family = normalizeAuthChain(chain);
  const challengeId = createChallengeId();
  const factorId = createChallengeId();
  const copy = getChainPreviewCopy(family);
  return {
    chain: family,
    challengeId,
    factorId,
    message: buildEnrollmentMessage(username, family, factorId, challengeId),
    label: cleanLabel(label),
    copy,
    approvalKind: family === "lightning" ? "invoice" : family === "zkvm" ? "proof" : "signature",
    samplePayload: family === "lightning"
      ? {
          receipt: {
            payment_hash: "future-" + challengeId,
            settled: true
          }
        }
      : family === "zkvm"
        ? {
            proof_backend: getZkVerifierConfig().proofBackend,
            proof: {
              public_inputs: { challenge_id: challengeId },
              proof_bytes: "0x"
            }
          }
        : {
            signature: "<approval-signature>"
          }
  };
}

function buildTransferPreview(username, transfer) {
  const approvalChain = normalizeAuthChain(transfer && (transfer.approval_chain || transfer.chain));
  const requestId = createChallengeId();
  const from = String((transfer && transfer.from) || username || "").trim();
  const to = String((transfer && transfer.to) || "").trim();
  const amount = Number(transfer && transfer.amount) || 0;
  const token = String((transfer && transfer.token) || "BTCPC");
  const memo = String((transfer && transfer.memo) || "");
  const threshold = Math.max(1, Number(transfer && transfer.threshold) || 1);
  const proofBackend = transfer && transfer.proof_backend ? String(transfer.proof_backend).trim().toLowerCase() : null;
  const challenge = {
    requestId,
    username: String(username || "").trim(),
    from,
    to,
    amount,
    token,
    memo,
    threshold,
    approvalChain,
    proofBackend,
    message: buildTransferMessage({
      username,
      requestId,
      approvalChain,
      from,
      to,
      amount,
      token,
      memo,
      threshold
    })
  };
  const family = normalizeAuthChain(approvalChain);
  const copy = getChainPreviewCopy(family);
  const samplePayload = family === "lightning"
    ? {
        receipt: {
          payment_hash: "future-" + requestId,
          settled: true
        }
      }
    : family === "zkvm"
      ? {
          proof_backend: proofBackend || getZkVerifierConfig().proofBackend,
          proof: {
            public_inputs: { request_id: requestId },
            proof_bytes: "0x"
          }
        }
      : {
          signature: "<approval-signature>"
        };
  return {
    ...challenge,
    copy,
    approvalKind: family === "lightning" ? "invoice" : family === "zkvm" ? "proof" : "signature",
    samplePayload
  };
}

function coerceVerificationPayload(input) {
  if (!input) return null;
  if (typeof input === "string") return input;
  if (typeof input !== "object") return null;
  if (input.signature) return input.signature;
  if (input.receipt) return input.receipt;
  if (input.proof) return input.proof;
  if (input.invoice) return input.invoice;
  return input;
}

function previewEnrollment(username, chain, label) {
  return buildEnrollmentPreview(username, chain, label);
}

function previewTransferAuthorization(username, transfer) {
  return buildTransferPreview(username, transfer);
}

function getPrivateAuthBanner() {
  return {
    staged: true,
    runtimeEnabled: isPrivateAuthEnabled(),
    title: "Private authorization is staged in code and disabled by default.",
    summary: "Future chain-based 2FA, Bitcoin verification, Lightning verification, and zkVM hooks are documented for later rollout.",
    docsPath: "/docs/PRIVATE_AUTH_FUTURE.md"
  };
}

function getPrivateAuthRouteSummary(basePath = "/api/wallet/private-auth") {
  const prefix = String(basePath || "/api/wallet/private-auth").replace(/\/+$/, "");
  return [
    {
      path: prefix,
      method: "GET",
      status: "staged",
      purpose: "Read the current private-auth policy."
    },
    {
      path: prefix + "/preview",
      method: "GET",
      status: "staged",
      purpose: "Render the future approval shape without activating it."
    },
    {
      path: prefix + "/policy",
      method: "POST",
      status: "feature-flagged",
      purpose: "Write policy when the future rollout is enabled."
    },
    {
      path: prefix + "/enroll/request",
      method: "POST",
      status: "feature-flagged",
      purpose: "Request an approval-chain enrollment challenge."
    },
    {
      path: prefix + "/enroll/verify",
      method: "POST",
      status: "feature-flagged",
      purpose: "Verify an enrollment receipt or proof."
    },
    {
      path: prefix + "/transfer/request",
      method: "POST",
      status: "feature-flagged",
      purpose: "Request a spend authorization challenge."
    },
    {
      path: prefix + "/transfer/verify",
      method: "POST",
      status: "feature-flagged",
      purpose: "Verify a spend approval receipt or proof."
    }
  ];
}

async function requestEnrollment(username, chain, label, address) {
  if (!isPrivateAuthEnabled()) {
    throw new Error("Private authorization is staged but disabled by feature flag");
  }
  if (!supportedChain(chain)) {
    throw new Error("Unsupported private authorization chain: " + chain);
  }

  const family = normalizeAuthChain(chain);
  const claimedAddress = typeof address === "string" ? address.trim() : "";
  if (family === "lightning" || family === "zkvm") {
    // No wallet address needed. These chains verify control through a paid invoice
    // or an external proof receipt, so the hidden factor stays opaque.
  } else if ((family === "solana" || family === "ton") && !claimedAddress) {
    throw new Error("address is required for " + family + " enrollment");
  }

  const factorId = createChallengeId();
  const challengeId = createChallengeId();
  const paymentHash = family === "lightning" ? crypto.randomBytes(32).toString("hex") : null;
  const challenge = {
    challengeId,
    factorId,
    username: String(username).trim(),
    chain: family,
    address: claimedAddress || null,
    label: cleanLabel(label),
    message: buildEnrollmentMessage(username, chain, factorId, challengeId),
    paymentHash,
    createdAt: Date.now(),
    expiresAt: Date.now() + 10 * 60 * 1000
  };
  if (family === "lightning") {
    const invoicePayload = {
      amount_sats: Number(process.env.BTCPC_LIGHTNING_AUTH_SATS) || 1,
      memo: "BTCPC private auth enrollment",
      payment_hash: paymentHash,
      metadata: {
        username: challenge.username,
        factor_id: factorId,
        challenge_id: challengeId
      }
    };
    try {
      const invoice = await createLightningInvoice(invoicePayload);
      challenge.invoice = invoice.invoice;
      challenge.provider = invoice.provider;
      challenge.paymentHash = invoice.payment_hash || paymentHash;
    } catch (err) {
      throw new Error("Lightning enrollment invoice could not be created: " + err.message);
    }
  }
  enrollmentChallenges.set(challengeId, challenge);
  pruneMap(enrollmentChallenges);
  return {
    challengeId: challenge.challengeId,
    factorId: challenge.factorId,
    message: challenge.message,
    chain: challenge.chain,
    invoice: challenge.invoice || null,
    paymentHash: challenge.paymentHash || null,
    amountSats: family === "lightning" ? (Number(process.env.BTCPC_LIGHTNING_AUTH_SATS) || 1) : null,
    expiresIn: 600
  };
}

async function verifyEnrollment(challengeId, signature) {
  if (!isPrivateAuthEnabled()) {
    throw new Error("Private authorization is staged but disabled by feature flag");
  }
  pruneMap(enrollmentChallenges);
  const challenge = enrollmentChallenges.get(String(challengeId || ""));
  if (!challenge) {
    return { success: false, error: "Enrollment challenge not found or expired" };
  }

  let recoveredAddress;
  try {
    if (challenge.chain === "lightning") {
      const receipt = coerceVerificationPayload(signature);
      await verifyLightningInvoice(
        challenge.paymentHash,
        Number(process.env.BTCPC_LIGHTNING_AUTH_SATS) || 1,
        receipt
      );
      recoveredAddress = "lightning:" + challenge.paymentHash;
    } else if (challenge.chain === "zkvm") {
      await verifyZkReceipt(challenge.message, typeof signature === "object" ? signature : { proof: signature });
      recoveredAddress = "zkvm:" + challenge.challengeId;
    } else {
      recoveredAddress = recoverAddress(challenge.chain, challenge.message, signature, challenge.address);
    }
  } catch (err) {
    return { success: false, error: "Signature verification failed: " + err.message };
  }

  const user = await getUser(challenge.username);
  const commitment = challenge.chain === "lightning"
    ? factorCommitment(challenge.chain, challenge.username, challenge.factorId)
    : challenge.chain === "zkvm"
      ? factorCommitment(challenge.chain, challenge.username, challenge.factorId)
      : commitmentFor(challenge.chain, recoveredAddress);
  const factors = user.privateAuth.factors;
  if (!factors.find((factor) => factor.factorId === challenge.factorId)) {
    factors.push({
      factorId: challenge.factorId,
      chain: challenge.chain,
      commitment,
      label: challenge.label,
      createdAt: new Date()
    });
  }
  user.privateAuth.enabled = true;
  user.privateAuth.updatedAt = new Date();
  await user.save();

  enrollmentChallenges.delete(challengeId);
  return {
    success: true,
    username: challenge.username,
    chain: challenge.chain,
    factorId: challenge.factorId,
    label: challenge.label,
    hidden: true
  };
}

async function setPolicy(username, updates) {
  const user = await getUser(username);
  const threshold = Number(updates && updates.threshold);
  if (Number.isInteger(threshold) && threshold > 0) {
    user.privateAuth.threshold = threshold;
  }
  if (updates && Object.prototype.hasOwnProperty.call(updates, "enabled")) {
    user.privateAuth.enabled = !!updates.enabled;
  } else {
    user.privateAuth.enabled = true;
  }
  user.privateAuth.updatedAt = new Date();
  await user.save();
  return getPolicySnapshot(user);
}

async function getPolicy(username) {
  const user = await getUser(username);
  return getPolicySnapshot(user);
}

async function requestTransferAuthorization(username, transfer) {
  if (!isPrivateAuthEnabled()) {
    throw new Error("Private authorization is staged but disabled by feature flag");
  }
  const user = await getUser(username);
  const policy = getPolicySnapshot(user);
  if (!policy.enabled) {
    throw new Error("Private authorization is not enabled for this account");
  }

  if (!transfer || typeof transfer !== "object") {
    throw new Error("Transfer details required");
  }
  if (!transfer.to) throw new Error("Transfer recipient required");
  if (!(Number(transfer.amount) > 0)) throw new Error("Transfer amount must be positive");
  const approvalChain = normalizeAuthChain(transfer.approval_chain || transfer.chain || "");
  if (!approvalChain) {
    throw new Error("approval_chain required");
  }
  if (!supportedChain(approvalChain)) {
    throw new Error("Unsupported approval chain: " + approvalChain);
  }

  if (!policy.factors.find((factor) => normalizeAuthChain(factor.chain) === approvalChain)) {
    throw new Error("Approval chain not enrolled: " + approvalChain);
  }

  const requestId = createChallengeId();
  const amount = Number(transfer.amount);
  const challenge = {
    requestId,
    username: String(username).trim(),
    from: String(transfer.from || username).trim(),
    to: String(transfer.to).trim(),
    amount,
    token: String(transfer.token || "BTCPC"),
    memo: transfer.memo ? String(transfer.memo).slice(0, 500) : "",
    threshold: Math.max(1, Number(transfer.threshold) || policy.threshold || 1),
    approvalChain,
    proofBackend: transfer.proof_backend ? String(transfer.proof_backend).trim().toLowerCase() : null,
    createdAt: Date.now(),
    expiresAt: Date.now() + 10 * 60 * 1000
  };
  challenge.message = buildTransferMessage(challenge);
  if (approvalChain === "lightning") {
    const paymentHash = crypto.randomBytes(32).toString("hex");
    challenge.paymentHash = paymentHash;
    try {
      const invoice = await createLightningInvoice({
        amount_sats: Number(process.env.BTCPC_LIGHTNING_AUTH_SATS) || 1,
        memo: "BTCPC private auth transfer",
        payment_hash: paymentHash,
        metadata: {
          username: challenge.username,
          request_id: requestId,
          transfer_message: challenge.message
        }
      });
      challenge.invoice = invoice.invoice;
      challenge.provider = invoice.provider;
      challenge.paymentHash = invoice.payment_hash || paymentHash;
    } catch (err) {
      throw new Error("Lightning approval invoice could not be created: " + err.message);
    }
  }
  transferChallenges.set(requestId, challenge);
  pruneMap(transferChallenges);
  return {
    requestId: challenge.requestId,
    message: challenge.message,
    approvalChain: challenge.approvalChain,
    proofBackend: challenge.proofBackend,
    threshold: challenge.threshold,
    invoice: challenge.invoice || null,
    paymentHash: challenge.paymentHash || null,
    amountSats: approvalChain === "lightning" ? (Number(process.env.BTCPC_LIGHTNING_AUTH_SATS) || 1) : null,
    expiresIn: 600,
    chain_options: ["evm", "bitcoin", "solana", "ton", "lightning", "zkvm"]
  };
}

async function verifyTransferAuthorization(username, transfer, privateAuth) {
  if (!isPrivateAuthEnabled()) {
    throw new Error("Private authorization is staged but disabled by feature flag");
  }
  pruneMap(transferChallenges);
  if (!privateAuth || typeof privateAuth !== "object") {
    throw new Error("Private authorization payload required");
  }

  const requestId = String(privateAuth.requestId || "").trim();
  if (!requestId) throw new Error("private_auth.requestId required");

  const challenge = transferChallenges.get(requestId);
  if (!challenge) throw new Error("Authorization request not found or expired");
  if (challenge.username !== String(username).trim()) throw new Error("Authorization request does not belong to this user");
  if (challenge.approvalChain !== normalizeAuthChain(privateAuth.approvalChain || privateAuth.chain || challenge.approvalChain)) {
    throw new Error("Authorization chain mismatch");
  }
  if (challenge.from !== String(transfer.from || username).trim()) throw new Error("Authorization sender mismatch");
  if (challenge.to !== String(transfer.to).trim()) throw new Error("Authorization recipient mismatch");
  if (Number(challenge.amount) !== Number(transfer.amount)) throw new Error("Authorization amount mismatch");
  if (String(challenge.token || "BTCPC") !== String(transfer.token || "BTCPC")) throw new Error("Authorization token mismatch");
  if ((challenge.memo || "") !== String(transfer.memo || "")) throw new Error("Authorization memo mismatch");
  if (Date.now() > challenge.expiresAt) throw new Error("Authorization request expired");

  const user = await getUser(username);
  const factors = (user.privateAuth && Array.isArray(user.privateAuth.factors)) ? user.privateAuth.factors : [];
  const approvals = Array.isArray(privateAuth.approvals) ? privateAuth.approvals : [];
  if (approvals.length === 0) throw new Error("At least one approval is required");

  const seen = new Set();
  const verifiedFactors = [];

  for (const approval of approvals) {
    if (!approval || typeof approval !== "object") continue;
    const factorId = String(approval.factorId || "").trim();
    const chain = normalizeAuthChain(approval.chain);
    const signature = String(approval.signature || "").trim();
    if (!factorId || !chain) {
      throw new Error("Each approval requires factorId and chain");
    }
    if (chain === "lightning" || chain === "zkvm") {
      if (!approval.receipt && !approval.proof && !approval.invoice && !signature) {
        throw new Error("Each " + chain + " approval requires a receipt or proof");
      }
    } else if (!signature) {
      throw new Error("Each approval requires factorId, chain, and signature");
    }
    if (seen.has(factorId)) {
      throw new Error("Duplicate approval factor: " + factorId);
    }

    const factor = factors.find((item) => item.factorId === factorId && normalizeAuthChain(item.chain) === chain);
    if (!factor) {
      throw new Error("Unknown approval factor: " + factorId);
    }

    let recoveredAddress;
    try {
      if (chain === "lightning") {
        await verifyLightningInvoice(
          challenge.paymentHash,
          Number(process.env.BTCPC_LIGHTNING_AUTH_SATS) || 1,
          coerceVerificationPayload(approval)
        );
        recoveredAddress = "lightning:" + challenge.paymentHash;
      } else if (chain === "zkvm") {
        await verifyZkReceipt(challenge.message, approval);
        recoveredAddress = "zkvm:" + challenge.challengeId;
      } else {
        recoveredAddress = recoverAddress(chain, challenge.message, signature, approval.address);
      }
    } catch (err) {
      throw new Error("Approval signature verification failed: " + err.message);
    }

    const expectedCommitment = (chain === "lightning" || chain === "zkvm")
      ? factorCommitment(chain, challenge.username, factor.factorId)
      : commitmentFor(chain, recoveredAddress);
    if (expectedCommitment !== factor.commitment) {
      throw new Error("Approval factor does not match the committed hidden wallet");
    }

    seen.add(factorId);
    verifiedFactors.push({
      factorId,
      chain,
      commitment: factor.commitment
    });
  }

  if (verifiedFactors.length < challenge.threshold) {
    throw new Error("Insufficient approvals: " + verifiedFactors.length + "/" + challenge.threshold);
  }

  transferChallenges.delete(requestId);

  return {
    requestId,
    threshold: challenge.threshold,
    approvalCount: verifiedFactors.length,
    factors: verifiedFactors,
    hidden: true
  };
}

module.exports = {
  normalizeAuthChain,
  supportedChain,
  requestEnrollment,
  verifyEnrollment,
  getPolicy,
  setPolicy,
  requestTransferAuthorization,
  verifyTransferAuthorization,
  isPrivateAuthEnabled,
  previewEnrollment,
  previewTransferAuthorization,
  getChainPreviewCopy,
  getPrivateAuthBanner,
  getPrivateAuthRouteSummary,
  coerceVerificationPayload,
  resetState
};
