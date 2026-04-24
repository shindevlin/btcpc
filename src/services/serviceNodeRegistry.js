"use strict";

/**
 * BTCPC Service Node Registry
 * Shin Devlin
 *
 * Tracks active service infrastructure nodes. Anyone may stake BTCPC and
 * run a service node to earn from the 8% SERVICE pool each epoch.
 *
 * Service types (extensible):
 *   api      — public RPC / REST gateway
 *   relay    — P2P relay / bridge node
 *   explorer — block explorer endpoint
 *   bridge   — cross-chain bridge relay
 *
 * A single account may declare multiple types. Heartbeat window is 10 epochs.
 * Minimum stake is read from BTCPC_MIN_SERVICE_STAKE (default 50 BTCPC).
 */

var _snEnv = parseInt(process.env.BTCPC_MIN_SERVICE_STAKE);
var MIN_SERVICE_STAKE = isNaN(_snEnv) ? 50 : _snEnv;

var VALID_TYPES = new Set(["api", "relay", "explorer", "bridge"]);

// Map<account, { types: string[], epoch: number, url?: string, registered_at: number }>
var registry = new Map();

function sanitizeTypes(raw) {
  if (!Array.isArray(raw)) return [];
  return raw.filter(function (t) { return typeof t === "string" && VALID_TYPES.has(t); });
}

/**
 * Record a heartbeat from a service node.
 * @param {string} account   — BTCPC account name
 * @param {string[]} types   — service types this node provides
 * @param {number}  epoch    — current epoch number
 * @param {string}  [url]    — optional public endpoint URL
 */
function recordHeartbeat(account, types, epoch, url) {
  if (!account || typeof account !== "string") return;
  if (typeof epoch !== "number" || epoch < 0) return;
  var cleanTypes = sanitizeTypes(types);
  if (cleanTypes.length === 0) return;

  var existing = registry.get(account);
  if (existing) {
    existing.types = cleanTypes;
    existing.epoch = epoch;
    if (url) existing.url = url;
  } else {
    registry.set(account, {
      account: account,
      types: cleanTypes,
      epoch: epoch,
      url: url || null,
      registered_at: Date.now(),
    });
  }
}

/**
 * Return accounts that heartbeated within [epoch - window, epoch].
 */
function getActiveServiceNodes(epoch, window) {
  var w = typeof window === "number" ? window : 10;
  var result = [];
  for (var rec of registry.values()) {
    if (rec.epoch >= epoch - w && rec.epoch <= epoch) {
      result.push(rec.account);
    }
  }
  return result;
}

/**
 * Return full records of active service nodes (for API responses).
 */
function getActiveServiceNodeRecords(epoch, window) {
  var w = typeof window === "number" ? window : 10;
  var result = [];
  for (var rec of registry.values()) {
    if (rec.epoch >= epoch - w && rec.epoch <= epoch) {
      result.push(Object.assign({}, rec));
    }
  }
  return result;
}

/**
 * Check if an account has the minimum required stake to be a service node.
 * Uses nodeRegistry stake (updated by STAKE/UNSTAKE ledger entries) or
 * stateStore stake pool — whichever is higher.
 */
function hasMinStake(account) {
  if (MIN_SERVICE_STAKE === 0) return true;
  try {
    var nodeRegistry = require("../chain/nodeRegistry");
    var node = nodeRegistry.getNode(account);
    if (node && node.permissioned) return true;
    var staked = (node && node.stake) || 0;

    var stateStore = require("../chain/stateStore");
    var pool = stateStore.getStakePool ? stateStore.getStakePool(account) : null;
    var poolStaked = (pool && pool.total_staked) || 0;
    if (poolStaked > staked) staked = poolStaked;

    return staked >= MIN_SERVICE_STAKE;
  } catch (_) {
    return false;
  }
}

module.exports = {
  MIN_SERVICE_STAKE: MIN_SERVICE_STAKE,
  VALID_TYPES: Array.from(VALID_TYPES),
  recordHeartbeat: recordHeartbeat,
  getActiveServiceNodes: getActiveServiceNodes,
  getActiveServiceNodeRecords: getActiveServiceNodeRecords,
  hasMinStake: hasMinStake,
};
