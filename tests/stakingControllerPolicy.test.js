"use strict";

const ledger = require("../src/services/ledger");
const { setStakePolicy } = require("../src/controllers/stakingController");

jest.mock("../src/services/ledger", () => ({
  getCurrentEpoch: jest.fn(async () => 123),
  recordNetworkPolicy: jest.fn(async () => ({})),
}));

describe("stakingController setStakePolicy", () => {
  beforeEach(() => {
    jest.clearAllMocks();
  });

  test("authority accounts can set stake floor and btcpctest developer allowlist", async () => {
    const req = {
      user: { username: "shindevlin" },
      body: {
        freeUntilStakers: 2000,
        btcpctestDeveloperEnabled: true,
        btcpctestDeveloperAllowlist: ["alice", "bob"],
      },
    };
    const res = {
      json: jest.fn(),
      status: jest.fn(function (code) {
        this.statusCode = code;
        return this;
      }),
    };

    await setStakePolicy(req, res);

    expect(ledger.recordNetworkPolicy).toHaveBeenCalledWith(
      "shindevlin",
      {
        stake_free_until_stakers: 2000,
        btcpctest_developer_enabled: true,
        btcpctest_developer_allowlist: ["alice", "bob"],
      },
      123,
    );
    expect(res.json).toHaveBeenCalledWith({
      success: true,
      freeUntilStakers: 2000,
      btcpctestDeveloperEnabled: true,
      btcpctestDeveloperAllowlist: ["alice", "bob"],
    });
  });
});
