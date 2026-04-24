"use strict";

/**
 * BTCPC Service Node Routes
 * Shin Devlin
 *
 * Infrastructure operators stake BTCPC and run service nodes to earn
 * from the 8% SERVICE pool each epoch. Any node type (api, relay,
 * explorer, bridge) runs on top of the base service node infrastructure.
 *
 * POST /api/service-nodes/heartbeat   — staked node reports uptime (JWT required)
 * GET  /public/service-nodes          — list active service nodes (public)
 */

const express = require("express");
const router = express.Router();
const { authenticateToken } = require("../middlewares/auth");
const snReg = require("../services/serviceNodeRegistry");

function getEpochNumber() {
  try {
    var protocol = require("../p2p/protocol");
    if (typeof protocol.getCurrentEpoch === "function") return protocol.getCurrentEpoch();
  } catch (_) {}
  var genesis = parseInt(process.env.BTCPC_GENESIS_TIMESTAMP) || 1776236400000;
  var epochMs = 30000;
  return Math.floor((Date.now() - genesis) / epochMs);
}

/**
 * POST /api/service-nodes/heartbeat
 * Body: { types: ["api", "relay", ...], url?: string }
 */
router.post("/heartbeat", authenticateToken, (req, res) => {
  var account = req.user && req.user.username;
  if (!account) return res.status(401).json({ error: "auth required" });

  if (!snReg.hasMinStake(account)) {
    return res.status(403).json({
      error: "insufficient stake",
      min_stake: snReg.MIN_SERVICE_STAKE,
      detail: "Stake at least " + snReg.MIN_SERVICE_STAKE + " BTCPC to run a service node",
    });
  }

  var body = req.body || {};
  var types = body.types;
  if (!Array.isArray(types) || types.length === 0) {
    return res.status(400).json({
      error: "types required",
      valid_types: snReg.VALID_TYPES,
    });
  }

  var epoch = getEpochNumber();
  var url = typeof body.url === "string" ? body.url.slice(0, 256) : undefined;

  snReg.recordHeartbeat(account, types, epoch, url);

  return res.json({
    success: true,
    account: account,
    types: types,
    epoch: epoch,
    min_stake: snReg.MIN_SERVICE_STAKE,
  });
});

/**
 * GET /public/service-nodes
 */
router.get("/", (req, res) => {
  var epoch = getEpochNumber();
  var records = snReg.getActiveServiceNodeRecords(epoch, 10);
  return res.json({
    epoch: epoch,
    count: records.length,
    nodes: records,
  });
});

module.exports = router;
