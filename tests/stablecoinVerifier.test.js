"use strict";

const { verifyStablecoinPayment } = require("../src/services/stablecoinVerifier");

function topicAddress(addr) {
  return "0x" + "0".repeat(24) + addr.replace(/^0x/, "").toLowerCase();
}

describe("stablecoinVerifier", () => {
  it("verifies an ERC20 stablecoin transfer against a payment address", async () => {
    const paymentAddress = "0x" + "1".repeat(40);
    const tokenContract = "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48";
    const txHash = "0x" + "a".repeat(64);
    const result = await verifyStablecoinPayment({
      chain: "ethereum",
      token: "USDC",
      tx_hash: txHash,
      payment_address: paymentAddress,
      token_contract: tokenContract,
      usd_amount: 5,
      mock_tx: {
        from: "0x" + "2".repeat(40),
        to: tokenContract,
        hash: txHash,
      },
      mock_receipt: {
        status: "0x1",
        logs: [{
          topics: [
            "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a6df523b3ef",
            topicAddress("0x" + "2".repeat(40)),
            topicAddress(paymentAddress),
          ],
          data: "0x" + (5n * 10n ** 6n).toString(16),
        }],
      },
    });
    expect(result.ok).toBe(true);
    expect(result.token).toBe("USDC");
    expect(result.tx_hash).toBe(txHash);
  });

  it("rejects a transfer below the nominal amount", async () => {
    await expect(verifyStablecoinPayment({
      chain: "ethereum",
      token: "USDT",
      tx_hash: "0x" + "b".repeat(64),
      payment_address: "0x" + "3".repeat(40),
      token_contract: "0xdAC17F958D2ee523a2206206994597C13D831ec7",
      usd_amount: 5,
      mock_tx: {
        from: "0x" + "4".repeat(40),
        to: "0xdAC17F958D2ee523a2206206994597C13D831ec7",
        hash: "0x" + "b".repeat(64),
      },
      mock_receipt: {
        status: "0x1",
        logs: [{
          topics: [
            "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a6df523b3ef",
            topicAddress("0x" + "4".repeat(40)),
            topicAddress("0x" + "3".repeat(40)),
          ],
          data: "0x" + (4n * 10n ** 6n).toString(16),
        }],
      },
    })).rejects.toThrow(/below required nominal fee/);
  });
});
