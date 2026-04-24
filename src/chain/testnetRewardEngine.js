"use strict";

/**
 * BTCPCTEST Reward Engine
 *
 * Separate public-testnet reward path.
 * - BTCPCTEST is the native testnet token
 * - BTCPC is a small side bonus for keeping the testnet alive
 * - Public testnet runs in report-only mode by default:
 *   no real inference/storage proof work is required unless a
 *   developer explicitly enables work access.
 */

const { POOL } = require("./rewardEngine");

const BTCPCTEST_BONUS_PCT = 0.001; // 0.1% BTCPC side bonus
const REPORT_ONLY_MODE = "report_only";
const DEVELOPER_MODE = "developer";

function round(n) {
  return parseFloat(Number(n).toFixed(10));
}

function normalizeTypes(node) {
  return Array.isArray(node && node.node_types)
    ? node.node_types.map(t => String(t || "").trim().toLowerCase()).filter(Boolean)
    : [];
}

function isGenericTestnetNode(types) {
  return types.indexOf("btcpctest") !== -1 || types.indexOf("testnet") !== -1;
}

function hasRole(node, role) {
  const types = normalizeTypes(node);
  if (types.indexOf(role) !== -1) return true;
  if (role === "clock" && isGenericTestnetNode(types)) return true;
  return false;
}

function uniqueByAccount(nodes) {
  const map = new Map();
  for (const node of nodes || []) {
    if (!node || !node.account) continue;
    if (!map.has(node.account)) map.set(node.account, node);
  }
  return Array.from(map.values());
}

function parseAllowlist(value) {
  if (Array.isArray(value)) {
    return value.map(v => String(v || "").trim()).filter(Boolean);
  }
  return String(value || "")
    .split(/[\s,]+/)
    .map(v => String(v || "").trim())
    .filter(Boolean);
}

function getDeveloperAccessPolicy(stateStore, overrideUsers, forceAll) {
  const storePolicy = stateStore && typeof stateStore.getNetworkPolicy === "function"
    ? (stateStore.getNetworkPolicy() || {})
    : {};
  const explicitAllowlist = parseAllowlist(overrideUsers);
  if (forceAll) {
    return {
      enabled: true,
      allowlist: [],
      allowAll: true,
      source: "override",
    };
  }
  if (explicitAllowlist.length > 0) {
    return {
      enabled: true,
      allowlist: explicitAllowlist,
      allowAll: false,
      source: "override",
    };
  }
  return {
    enabled: !!storePolicy.btcpctestDeveloperEnabled,
    allowlist: parseAllowlist(storePolicy.btcpctestDeveloperAllowlist),
    allowAll: false,
    source: "policy",
  };
}

function isDeveloperAllowed(username, policy) {
  if (!policy || !policy.enabled) return false;
  if (policy.allowAll) return true;
  const name = String(username || "").trim();
  if (!name) return false;
  return policy.allowlist.indexOf(name) !== -1;
}

function extractTestnetNodes(stateStore, epochNumber) {
  const nodes = [];
  if (!stateStore || typeof stateStore.getAllAccounts !== "function" || typeof stateStore.getAccount !== "function") {
    return nodes;
  }

  const currentEpoch = typeof epochNumber === "number"
    ? epochNumber
    : (typeof stateStore.getChainHeight === "function" ? stateStore.getChainHeight() : 0);
  const cutoffEpoch = Math.max(0, currentEpoch - 100);

  for (const account of stateStore.getAllAccounts()) {
    if (!account || !account.username) continue;
    const acc = stateStore.getAccount(account.username);
    const nodeTypes = normalizeTypes(acc);
    if (nodeTypes.indexOf("btcpctest") === -1 && nodeTypes.indexOf("testnet") === -1) continue;
    const lastSeen = Math.max(
      acc && acc.heartbeat_epoch ? acc.heartbeat_epoch : 0,
      acc && acc.last_announced_epoch ? acc.last_announced_epoch : 0,
      acc && acc.last_registered_epoch ? acc.last_registered_epoch : 0,
      acc && acc.created_epoch ? acc.created_epoch : 0
    );
    if (lastSeen < cutoffEpoch) continue;
    nodes.push({
      account: account.username,
      node_types: nodeTypes,
      p2p_address: acc && acc.p2p_address ? acc.p2p_address : null,
      last_seen_epoch: lastSeen,
    });
  }

  return nodes;
}

function distributeRolePool(rewards, poolAmount, nodes, role, workMode) {
  const active = uniqueByAccount((nodes || []).filter(node => hasRole(node, role)));
  if (active.length === 0) return poolAmount;

  const share = round(poolAmount / active.length);
  if (share <= 0) return poolAmount;

  for (const node of active) {
    rewards.push({
      to: node.account,
      amount: share,
      type: "MINING_REWARD",
      token: "BTCPCTEST",
      reward_source: "btcpctest",
      meta: {
        role,
        node_types: node.node_types || [],
        p2p_address: node.p2p_address || null,
        last_seen_epoch: node.last_seen_epoch || 0,
        work_mode: node.work_mode || REPORT_ONLY_MODE,
      },
    });
  }

  return round(poolAmount - (share * active.length));
}

function computeTestnetRewards(input) {
  const {
    epochNumber = 0,
    blockReward = 0,
    btcpcBonusBase = blockReward,
    testnetNodes = null,
    developerAccess = false,
    developerAccessUsers = null,
    previewOnly = false,
  } = input || {};

  let stateStore = null;
  try { stateStore = input.stateStore || require("./stateStore"); } catch (_) {}

  const nodes = Array.isArray(testnetNodes) ? testnetNodes : extractTestnetNodes(stateStore, epochNumber);
  const rewards = [];
  let recycled = 0;

  const policy = previewOnly
    ? {
        enabled: false,
        allowlist: [],
        allowAll: false,
        source: "preview",
      }
    : getDeveloperAccessPolicy(stateStore, developerAccessUsers, developerAccess);
  const reportOnlyNodes = uniqueByAccount(nodes);

  const pools = [
    { poolKey: "mining", role: "miner", amount: round(blockReward * POOL.mining) },
    { poolKey: "verifier", role: "verifier", amount: round(blockReward * POOL.verifier) },
    { poolKey: "clock", role: "clock", amount: round(blockReward * POOL.clock) },
    { poolKey: "storage", role: "storage", amount: round(blockReward * POOL.storage) },
    { poolKey: "sensor", role: "sensor", amount: round(blockReward * POOL.sensor) },
    { poolKey: "service", role: "service", amount: round(blockReward * POOL.service) },
  ];

  for (const { role, amount: poolAmount } of pools) {
    recycled += distributeRolePool(
      rewards,
      poolAmount,
      reportOnlyNodes.map((node) => ({
        ...node,
        work_mode: isDeveloperAllowed(node.account, policy) ? DEVELOPER_MODE : REPORT_ONLY_MODE,
      })),
      role,
      null
    );
  }

  recycled += round(blockReward * POOL.reserve);

  const bonusPool = round(btcpcBonusBase * BTCPCTEST_BONUS_PCT);
  const bonusNodes = reportOnlyNodes.filter(node => normalizeTypes(node).length > 0);
  if (bonusNodes.length === 0) {
    recycled += bonusPool;
  } else {
    const bonusShare = round(bonusPool / bonusNodes.length);
    for (const node of bonusNodes) {
      rewards.push({
        to: node.account,
        amount: bonusShare,
        type: "MINING_REWARD",
        token: "BTCPC",
        reward_source: "btcpctest_bonus",
        meta: {
          node_types: node.node_types || [],
          p2p_address: node.p2p_address || null,
          last_seen_epoch: node.last_seen_epoch || 0,
          work_mode: isDeveloperAllowed(node.account, policy) ? DEVELOPER_MODE : REPORT_ONLY_MODE,
        },
      });
    }
  }

  const summary = {
    epoch: epochNumber,
    block_reward: blockReward,
    native_reward: round(blockReward),
    btcpc_bonus_reward: round(bonusPool),
    btcpctest_nodes: reportOnlyNodes.length,
    work_mode: policy.enabled ? "mixed" : REPORT_ONLY_MODE,
    developer_access_enabled: policy.enabled,
    developer_access_required: !policy.enabled,
    developer_access_allowlist_count: policy.allowAll ? reportOnlyNodes.length : policy.allowlist.length,
    developer_access_allowed_nodes: reportOnlyNodes.filter(node => isDeveloperAllowed(node.account, policy)).length,
    developer_access_source: policy.source,
    role_counts: {
      miner: reportOnlyNodes.filter(node => hasRole(node, "miner")).length,
      verifier: reportOnlyNodes.filter(node => hasRole(node, "verifier")).length,
      clock: reportOnlyNodes.filter(node => hasRole(node, "clock")).length,
      storage: reportOnlyNodes.filter(node => hasRole(node, "storage")).length,
      sensor: reportOnlyNodes.filter(node => hasRole(node, "sensor")).length,
      service: reportOnlyNodes.filter(node => hasRole(node, "service")).length,
    },
    total_distributed: round(rewards.reduce((sum, r) => sum + r.amount, 0)),
    recycled: round(recycled),
  };

  return { rewards, recycled, summary };
}

module.exports = {
  BTCPCTEST_BONUS_PCT,
  REPORT_ONLY_MODE,
  DEVELOPER_MODE,
  computeTestnetRewards,
  extractTestnetNodes,
  getDeveloperAccessPolicy,
  isDeveloperAllowed,
};
