"use strict";

const { computeTestnetRewards } = require("../src/chain/testnetRewardEngine");

describe("BTCPCTEST testnet rewards", () => {
  test("active testnet nodes receive role-based BTCPCTEST and BTCPC bonus rewards", () => {
    const result = computeTestnetRewards({
      epochNumber: 123,
      blockReward: 100,
      testnetNodes: [
        { account: "btcpctest-a", node_types: ["btcpctest", "miner", "clock", "storage"], p2p_address: "ws://10.0.0.1:6942", last_seen_epoch: 123 },
        { account: "btcpctest-b", node_types: ["testnet", "clock", "verifier", "sensor", "service"], p2p_address: "ws://10.0.0.2:6942", last_seen_epoch: 123 },
      ],
    });

    const native = result.rewards.filter(r => r.token === "BTCPCTEST");
    const bonus = result.rewards.filter(r => r.token === "BTCPC" && r.reward_source === "btcpctest_bonus");

    expect(native).toHaveLength(7);
    expect(bonus).toHaveLength(2);
    expect(native.reduce((sum, r) => sum + r.amount, 0)).toBeCloseTo(98, 10);
    expect(bonus.reduce((sum, r) => sum + r.amount, 0)).toBeCloseTo(0.1, 10);
    expect(result.summary.btcpctest_nodes).toBe(2);
    expect(result.summary.native_reward).toBeCloseTo(100, 10);
    expect(result.summary.btcpc_bonus_reward).toBeCloseTo(0.1, 10);
    expect(result.summary.work_mode).toBe("report_only");
    expect(result.summary.developer_access_required).toBe(true);
    expect(result.summary.role_counts.miner).toBe(1);
    expect(result.summary.role_counts.clock).toBe(2);
    expect(result.summary.role_counts.storage).toBe(1);
    expect(result.summary.role_counts.verifier).toBe(1);
    expect(result.summary.role_counts.sensor).toBe(1);
    expect(result.summary.role_counts.service).toBe(1);
    expect(result.summary.total_distributed).toBeCloseTo(98.1, 10);
  });

  test("without testnet nodes, the pools recycle", () => {
    const result = computeTestnetRewards({
      epochNumber: 123,
      blockReward: 100,
      testnetNodes: [],
    });

    expect(result.rewards).toHaveLength(0);
    expect(result.summary.btcpctest_nodes).toBe(0);
    expect(result.summary.total_distributed).toBeCloseTo(0, 10);
    expect(result.summary.recycled).toBeCloseTo(100.1, 10);
  });

  test("developer allowlist flips only the allowed username into developer mode", () => {
    const result = computeTestnetRewards({
      epochNumber: 123,
      blockReward: 100,
      developerAccessUsers: ["btcpctest-a"],
      testnetNodes: [
        { account: "btcpctest-a", node_types: ["btcpctest", "miner"], p2p_address: "ws://10.0.0.1:6942", last_seen_epoch: 123 },
        { account: "btcpctest-b", node_types: ["btcpctest", "clock"], p2p_address: "ws://10.0.0.2:6942", last_seen_epoch: 123 },
      ],
    });

    const aRewards = result.rewards.filter(r => r.to === "btcpctest-a" && r.token === "BTCPCTEST");
    const bRewards = result.rewards.filter(r => r.to === "btcpctest-b" && r.token === "BTCPCTEST");

    expect(result.summary.work_mode).toBe("mixed");
    expect(result.summary.developer_access_enabled).toBe(true);
    expect(result.summary.developer_access_required).toBe(false);
    expect(result.summary.developer_access_allowlist_count).toBe(1);
    expect(result.summary.developer_access_allowed_nodes).toBe(1);
    expect(aRewards.every(r => r.meta.work_mode === "developer")).toBe(true);
    expect(bRewards.every(r => r.meta.work_mode === "report_only")).toBe(true);
  });
});
