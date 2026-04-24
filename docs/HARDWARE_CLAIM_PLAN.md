# Hardware Claim Plan

## Goal
Bind each device hardware hash to the owning account's posting key hash, and allow a different account to take over that hardware only through a nominal stablecoin payment.

## Scope
- Sensors
- LoRa gateways
- Future device classes that need the same anti-zombie identity rule

## Rules
- One active hardware hash per active owner claim.
- The active claim is tied to the owner's posting key hash.
- A new owner can only rebind the hardware hash with a documented takeover event.
- Takeover payment tokens are limited to `USDC`, `USDT`, or `DAI`.
- The chain must be able to record bad actor revocations and inspect claim history.

## Implementation Steps
1. Add chain-level ledger entries for hardware claim and hardware takeover.
2. Add a stablecoin receipt verifier for takeover payments.
3. Mirror those entries in the state store so claim state survives replay.
4. Expose the claim/takeover state in the sensor and gateway registries.
5. Add tests for:
   - owner binding
   - duplicate claim rejection
   - paid takeover
   - revocation / rebind after retirement

## Verification
- IoT registry tests
- Ledger/state replay tests
- Full Node test suite
