"use strict";

const crypto = require("crypto");
const https = require("https");
const http = require("http");

const TOKEN_DECIMALS = {
  USDC: 6,
  USDT: 6,
  DAI: 18,
};

const TOKEN_CONTRACTS = {
  ethereum: {
    USDC: "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48",
    USDT: "0xdAC17F958D2ee523a2206206994597C13D831ec7",
    DAI: "0x6B175474E89094C44Da98b954EedeAC495271d0F",
  },
};

const TRANSFER_TOPIC = "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a6df523b3ef";

function _trim(value) {
  return typeof value === "string" ? value.trim() : "";
}

function _normalizeHex(value) {
  const v = _trim(value).toLowerCase();
  return v.startsWith("0x") ? v : "0x" + v;
}

function _normalizeAddress(value) {
  const hex = _normalizeHex(value);
  if (!/^0x[a-f0-9]{40}$/.test(hex)) {
    throw new Error("invalid address: " + value);
  }
  return hex;
}

function _sha256Hex(text) {
  return crypto.createHash("sha256").update(String(text || ""), "utf8").digest("hex");
}

function _decimalToBaseUnits(value, decimals) {
  const raw = typeof value === "number" ? String(value) : String(value || "").trim();
  if (!raw) throw new Error("usd_amount required");
  if (!/^\d+(\.\d+)?$/.test(raw)) {
    throw new Error("usd_amount must be numeric");
  }
  const [whole, frac = ""] = raw.split(".");
  const padded = (frac + "0".repeat(decimals)).slice(0, decimals);
  return BigInt(whole || "0") * (10n ** BigInt(decimals)) + BigInt(padded || "0");
}

function _jsonRpc(rpcUrl, method, params) {
  return new Promise(function (resolve, reject) {
    const body = JSON.stringify({ jsonrpc: "2.0", id: 1, method: method, params: params || [] });
    const parsed = new URL(rpcUrl);
    const client = parsed.protocol === "https:" ? https : http;
    const req = client.request({
      hostname: parsed.hostname,
      port: parsed.port,
      path: parsed.pathname,
      method: "POST",
      headers: { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(body) },
    }, function (res) {
      let data = "";
      res.on("data", function (chunk) { data += chunk; });
      res.on("end", function () {
        try {
          const json = JSON.parse(data);
          if (json.error) reject(new Error(json.error.message || "json rpc error"));
          else resolve(json.result);
        } catch (err) {
          reject(err);
        }
      });
    });
    req.on("error", reject);
    req.write(body);
    req.end();
  });
}

async function verifyEvmStablecoinPayment(opts) {
  const proof = opts && typeof opts === "object" ? opts : {};
  const chain = _trim(proof.chain || "ethereum").toLowerCase();
  const token = _trim(proof.token).toUpperCase();
  const txHash = _trim(proof.tx_hash);
  const paymentAddress = _normalizeAddress(proof.payment_address);
  const rpcUrl = _trim(proof.rpc_url || process.env.BTCPC_TAKEOVER_RPC_URL || process.env.ETH_RPC_URL);
  const tokenContract = _normalizeAddress(
    proof.token_contract || (TOKEN_CONTRACTS[chain] && TOKEN_CONTRACTS[chain][token])
  );
  const mockReceipt = proof.mock_receipt || null;
  const mockTx = proof.mock_tx || null;

  if (!["ethereum"].includes(chain)) {
    throw new Error("unsupported stablecoin verification chain: " + chain);
  }
  if (!TOKEN_DECIMALS[token]) {
    throw new Error("unsupported stablecoin token: " + token);
  }
  if (!txHash) {
    throw new Error("tx_hash required");
  }

  let tx;
  let receipt;
  if (mockReceipt) {
    receipt = mockReceipt;
    tx = mockTx || { from: proof.sender || "0x" + "0".repeat(40), to: tokenContract, hash: txHash };
  } else {
    if (!rpcUrl) throw new Error("rpc_url required for stablecoin verification");
    tx = await _jsonRpc(rpcUrl, "eth_getTransactionByHash", [txHash]);
    receipt = await _jsonRpc(rpcUrl, "eth_getTransactionReceipt", [txHash]);
  }

  if (!tx || !receipt) {
    throw new Error("transaction not found");
  }
  if (!receipt.status || receipt.status === "0x0" || receipt.status === 0) {
    throw new Error("transaction failed");
  }
  if (_normalizeAddress(tx.to) !== tokenContract) {
    throw new Error("transaction did not target the expected stablecoin contract");
  }

  const transferLog = (receipt.logs || []).find(function (log) {
    return log && Array.isArray(log.topics) && log.topics[0] && _normalizeHex(log.topics[0]) === TRANSFER_TOPIC;
  });
  if (!transferLog) {
    throw new Error("stablecoin transfer event not found");
  }
  const toTopic = transferLog.topics[2] || "";
  const fromTopic = transferLog.topics[1] || "";
  const logTo = "0x" + _trim(toTopic).replace(/^0x/, "").slice(-40);
  const logFrom = "0x" + _trim(fromTopic).replace(/^0x/, "").slice(-40);
  if (_normalizeAddress(logTo) !== paymentAddress) {
    throw new Error("stablecoin transfer did not pay the expected address");
  }
  if (tx.from && _normalizeAddress(tx.from) !== _normalizeAddress(logFrom)) {
    throw new Error("stablecoin transfer sender mismatch");
  }

  const decimals = TOKEN_DECIMALS[token];
  const expectedUnits = _decimalToBaseUnits(proof.usd_amount, decimals);
  const rawValue = transferLog.data || "0x0";
  const actualUnits = BigInt(_trim(rawValue) ? _trim(rawValue) : "0x0");
  if (actualUnits < expectedUnits) {
    throw new Error("stablecoin transfer amount below required nominal fee");
  }

  return {
    ok: true,
    chain: chain,
    token: token,
    tx_hash: txHash,
    payment_address: paymentAddress,
    token_contract: tokenContract,
    usd_amount: proof.usd_amount,
    expected_units: expectedUnits.toString(),
    actual_units: actualUnits.toString(),
    receipt_hash: _sha256Hex(JSON.stringify(receipt)),
  };
}

async function verifyStablecoinPayment(proof) {
  return verifyEvmStablecoinPayment(proof);
}

module.exports = {
  verifyStablecoinPayment,
  verifyEvmStablecoinPayment,
  _decimalToBaseUnits,
  _normalizeAddress,
};
