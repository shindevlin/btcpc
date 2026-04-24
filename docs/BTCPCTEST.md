# BTCPCTEST Testnet Chain

`BTCPCTEST` is the native token for BTCPC's public testnet.

It is meant for real devices that stay online, publish a valid P2P address, and help keep the testnet honest when the network is flaky.

## What it does

- Runs on its own testnet chain, separate from BTCPC mainnet
- Announces a real peer address on chain
- Participates in the public testnet truth path
- Helps bootstrap the network when there are too few stable nodes

## How it earns

- `BTCPCTEST` earns the full testnet reward amount on the testnet chain
- Contributors also earn a small BTCPC bonus for helping the testnet stay alive
- If no active `btcpctest` nodes are online, the reward pools recycle back to the chain recycle account
- Eligible nodes split both pools equally

## Eligibility

- The account must register `node_types` including `btcpctest` or `testnet`
- The node must have recently announced or heartbeated on chain
- The node must still be online enough to be considered active

## Why it exists

Public testnet behavior is flaky by nature. BTCPC needs a small incentive to keep more than one real device participating so chain truth can survive reconnects, churn, and partial outages.

`BTCPCTEST` is a separate testnet token. It is not BTCPC, but helping the public testnet can earn both.

Live public preview: `/public/testnet/rewards`
