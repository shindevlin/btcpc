"use strict";

const fs = require("fs");
const os = require("os");
const path = require("path");
const crypto = require("crypto");
const secp256k1 = require("secp256k1");

function makeTempDir() {
  const dir = path.join(os.tmpdir(), "btcpc-cross-chain-finality-" + process.pid + "-" + Math.random().toString(36).slice(2));
  fs.mkdirSync(dir, { recursive: true });
  return dir;
}

function removeTempDir(dir) {
  try {
    fs.rmSync(dir, { recursive: true, force: true });
  } catch (_) {}
}

function makeSigningKey() {
  let priv;
  do {
    priv = crypto.randomBytes(32);
  } while (!secp256k1.privateKeyVerify(priv));
  return priv.toString("hex");
}

describe("cross-chain finality consumer", () => {
  let tempDir;
  let prevDataDir;
  let prevBaseContract;

  beforeEach(() => {
    tempDir = makeTempDir();
    prevDataDir = process.env.BTCPC_DATA_DIR;
    prevBaseContract = process.env.WBTCPC_BASE_CONTRACT;
    process.env.BTCPC_DATA_DIR = tempDir;
    process.env.WBTCPC_BASE_CONTRACT = "0x" + "1".repeat(40);
    jest.resetModules();
    jest.unmock("../src/claims/claimProofGenerator");
  });

  afterEach(() => {
    if (prevDataDir === undefined) delete process.env.BTCPC_DATA_DIR;
    else process.env.BTCPC_DATA_DIR = prevDataDir;

    if (prevBaseContract === undefined) delete process.env.WBTCPC_BASE_CONTRACT;
    else process.env.WBTCPC_BASE_CONTRACT = prevBaseContract;

    removeTempDir(tempDir);
  });

  test("loads the latest chain announcement and enforces the cutoff", () => {
    const consumer = require("../src/services/crossChainFinalityConsumer");
    const crossChainDir = consumer.getCrossChainDir();
    const baseDir = path.join(crossChainDir, "base");
    fs.mkdirSync(baseDir, { recursive: true });
    fs.writeFileSync(path.join(baseDir, "finality-00000100.json"), JSON.stringify({
      target_chain: "base",
      finality_epoch: 100,
      cutoff_epoch: 100,
      finalized_job_count: 1,
      finalized_jobs: [{ job_id: "job-1" }],
      announcement_hash: "a".repeat(64),
    }, null, 2));

    const announcement = consumer.loadLatestAnnouncement("base");
    expect(announcement.cutoff_epoch).toBe(100);
    expect(consumer.isEpochFinalized("base", 100)).toBe(true);
    expect(consumer.isEpochFinalized("base", 101)).toBe(false);
    expect(() => consumer.assertEpochFinalized("base", 101)).toThrow("exceeds finality cutoff");
  });

  test("submitAllClaims skips epochs beyond the announced cutoff", async () => {
    const consumer = require("../src/services/crossChainFinalityConsumer");
    const crossChainDir = consumer.getCrossChainDir();
    const baseDir = path.join(crossChainDir, "base");
    fs.mkdirSync(baseDir, { recursive: true });
    fs.writeFileSync(path.join(baseDir, "finality-00000100.json"), JSON.stringify({
      target_chain: "base",
      finality_epoch: 100,
      cutoff_epoch: 100,
      finalized_job_count: 1,
      finalized_jobs: [{ job_id: "job-1" }],
      announcement_hash: "b".repeat(64),
    }, null, 2));

    jest.doMock("../src/claims/claimProofGenerator", () => ({
      generateClaimProof: jest.fn(() => ({
        miner: "miner-1",
        epoch: 101,
        target_wallet: "0x" + "2".repeat(40),
        direct_amount: "1.0",
        amount: "1.0",
        lp_amount: "0",
        period: 0,
        cross_chain_ratio: 1,
      })),
    }));

    const submitter = require("../src/claims/evmClaimSubmitter");
    const results = await submitter.submitAllClaims(
      "miner-1",
      101,
      1,
      { base: "0x" + "2".repeat(40) },
      makeSigningKey()
    );

    expect(results).toEqual(expect.arrayContaining([
      expect.objectContaining({ chain: "base", status: "skipped_unfinalized", epoch: 101 }),
    ]));

    jest.dontMock("../src/claims/claimProofGenerator");
    jest.resetModules();
  });

  test("generateClaimProof rejects unfinalized epochs", () => {
    const consumer = require("../src/services/crossChainFinalityConsumer");
    const crossChainDir = consumer.getCrossChainDir();
    const baseDir = path.join(crossChainDir, "base");
    fs.mkdirSync(baseDir, { recursive: true });
    fs.writeFileSync(path.join(baseDir, "finality-00000100.json"), JSON.stringify({
      target_chain: "base",
      finality_epoch: 100,
      cutoff_epoch: 100,
      finalized_job_count: 1,
      finalized_jobs: [{ job_id: "job-1" }],
      announcement_hash: "c".repeat(64),
    }, null, 2));

    const { generateClaimProof } = require("../src/claims/claimProofGenerator");
    expect(() => generateClaimProof({
      btcpcAccount: "miner-1",
      chain: "base",
      targetWallet: "0x" + "2".repeat(40),
      amount: 1,
      nonce: "0x" + "4".repeat(64),
      epoch: 101,
      oraclePrivKey: makeSigningKey(),
    })).toThrow("exceeds finality cutoff");
  });
});
