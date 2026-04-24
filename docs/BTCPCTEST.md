# BTCPCTEST Testnet Chain

`BTCPCTEST` is the native token for BTCPC's public testnet chain.

It is meant for real devices that stay online, publish a valid P2P address, and keep the testnet honest when the network is flaky.

## What it does

- Runs on its own testnet chain, separate from BTCPC mainnet
- Announces a real peer address on chain
- Keeps clock time and report-only role presence by default
- Mirrors the mainnet-style reward allotment on the testnet chain
- Helps bootstrap the network when there are too few stable nodes
- Real inference/storage work stays off by default unless developer access is enabled

## How it earns

- `BTCPCTEST` earns the same role-based allotment the equivalent role would receive on BTCPC mainnet, but on the separate testnet chain
- Contributors also earn a small BTCPC bonus for helping the testnet stay alive
- Public testnet rewards are report-only by default: no real inference or storage work is required unless developer access is enabled
- If no active `btcpctest` nodes are online, the reward pools recycle back to the chain recycle account
- Eligible nodes split each role pool equally by report presence

## Eligibility

- The account must register `node_types` including `btcpctest` or `testnet`
- The node must have recently announced or heartbeated on chain
- The node must still be online enough to be considered active
- If the node declares role types like `miner`, `clock`, `storage`, `sensor`, `verifier`, or `service`, it can collect the matching role pool
- Generic `btcpctest` / `testnet` nodes count as clock participants so they can help keep testnet time moving

## Why it exists

Public testnet behavior is flaky by nature. BTCPC needs a small incentive to keep more than one real device participating so chain truth can survive reconnects, churn, and partial outages.

`BTCPCTEST` is a separate testnet token. It is not BTCPC, but helping the public testnet can earn both.

By default, the public testnet is report-only:

- no real inference work
- no real storage work
- clocks keep time
- nodes report their roles and stay reachable

Developer access can turn real work back on for testing purposes.

Live public preview: `/public/testnet/rewards`
