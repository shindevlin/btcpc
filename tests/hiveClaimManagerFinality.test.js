"use strict";

jest.mock("../src/services/crossChainFinalityConsumer", () => ({
  assertEpochFinalized: jest.fn(() => {
    throw new Error("epoch 101 exceeds finality cutoff 100 for hive");
  }),
}));

jest.mock("mongoose", () => {
  function Schema() {
    this.index = jest.fn();
  }
  return {
    Schema,
    model: jest.fn(() => {
      function HiveClaim(doc) {
        Object.assign(this, doc);
        this.save = jest.fn().mockResolvedValue(this);
      }
      HiveClaim.findOne = jest.fn();
      HiveClaim.find = jest.fn();
      return HiveClaim;
    }),
  };
});

const mockBroadcastJson = jest.fn();

jest.mock("@hiveio/dhive", () => ({
  Client: jest.fn(() => ({
    broadcast: {
      json: mockBroadcastJson,
    },
  })),
  PrivateKey: {
    fromString: jest.fn(() => ({})),
  },
}));

const { postClaimToHive } = require("../src/claims/hiveClaimManager");

describe("hive claim finality gating", () => {
  beforeEach(() => {
    jest.clearAllMocks();
  });

  test("rejects claims before Hive finality cutoff", async () => {
    await expect(
      postClaimToHive(
        {
          miner: "miner-1",
          epoch: 101,
          amount: "1.0",
          period: 1,
          cross_chain_ratio: "0.9",
          proof_signature: "0x" + "1".repeat(130),
          timestamp: Date.now(),
        },
        "alice",
        "posting-key"
      )
    ).rejects.toThrow("exceeds finality cutoff");

    expect(mockBroadcastJson).not.toHaveBeenCalled();
  });
});
