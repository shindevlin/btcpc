"use strict";

const { computeTestnetRewards } = require("../src/chain/testnetRewardEngine");

describe("BTCPCTEST testnet rewards", () => {
  test("active testnet nodes receive BTCPCTEST and BTCPC bonus rewards", () => {
    const result = computeTestnetRewards({
      epochNumber: 123,
      blockReward: 100,
      testnetNodes: [
        { account: "btcpctest-a", node_types: ["btcpctest"], p2p_address: "ws://10.0.0.1:6942", last_seen_epoch: 123 },
        { account: "btcpctest-b", node_types: ["testnet"], p2p_address: "ws://10.0.0.2:6942", last_seen_epoch: 123 },
      ],
    });

    const native = result.rewards.filter(r => r.token === "BTCPCTEST");
    const bonus = result.rewards.filter(r => r.token === "BTCPC" && r.reward_source === "btcpctest_bonus");

    expect(native).toHaveLength(2);
    expect(bonus).toHaveLength(2);
    expect(native.reduce((sum, r) => sum + r.amount, 0)).toBeCloseTo(100, 10);
    expect(bonus.reduce((sum, r) => sum + r.amount, 0)).toBeCloseTo(0.1, 10);
    expect(result.summary.btcpctest_nodes).toBe(2);
    expect(result.summary.native_reward).toBeCloseTo(100, 10);
    expect(result.summary.btcpc_bonus_reward).toBeCloseTo(0.1, 10);
    expect(result.summary.total_distributed).toBeCloseTo(100.1, 10);
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
});
