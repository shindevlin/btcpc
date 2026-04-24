"use strict";

/**
 * BTCPCTEST Reward Engine
 *
 * Separate public-testnet reward path.
 * - BTCPCTEST is the native testnet token
 * - BTCPC bonus is a small side incentive for helping the testnet stay alive
 */

const BTCPCTEST_BONUS_PCT = 0.001; // 0.1% BTCPC side bonus

function round(n) {
  return parseFloat(Number(n).toFixed(10));
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
    const nodeTypes = Array.isArray(acc && acc.node_types)
      ? acc.node_types.map(t => String(t || "").trim().toLowerCase())
      : [];
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

function computeTestnetRewards(input) {
  const {
    epochNumber = 0,
    blockReward = 0,
    btcpcBonusBase = blockReward,
    testnetNodes = null,
  } = input || {};

  let stateStore = null;
  try { stateStore = input.stateStore || require("./stateStore"); } catch (_) {}

  const nodes = Array.isArray(testnetNodes) ? testnetNodes : extractTestnetNodes(stateStore, epochNumber);
  const rewards = [];
  let recycled = 0;

  const testnetPool = round(blockReward);
  const btcpcBonusPool = round(btcpcBonusBase * BTCPCTEST_BONUS_PCT);

  if (nodes.length === 0) {
    recycled += testnetPool + btcpcBonusPool;
  } else {
    const nativeShare = round(testnetPool / nodes.length);
    const bonusShare = round(btcpcBonusPool / nodes.length);

    for (const node of nodes) {
      if (!node || !node.account) continue;
      if (nativeShare > 0) {
        rewards.push({
          to: node.account,
          amount: nativeShare,
          type: "MINING_REWARD",
          token: "BTCPCTEST",
          reward_source: "btcpctest",
          meta: {
            node_types: node.node_types || [],
            p2p_address: node.p2p_address || null,
            last_seen_epoch: node.last_seen_epoch || 0,
          },
        });
      }
      if (bonusShare > 0) {
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
          },
        });
      }
    }
  }

  const summary = {
    epoch: epochNumber,
    block_reward: blockReward,
    native_reward: round(testnetPool),
    btcpc_bonus_reward: round(btcpcBonusPool),
    btcpctest_nodes: nodes.length,
    total_distributed: round(rewards.reduce((sum, r) => sum + r.amount, 0)),
    recycled: round(recycled),
  };

  return { rewards, recycled, summary };
}

module.exports = {
  BTCPCTEST_BONUS_PCT,
  computeTestnetRewards,
  extractTestnetNodes,
};
