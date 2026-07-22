# HONE Sealed Backup

**Draft — July 2026.** An encrypted wallet backup that unlocks with a password, another
wallet's signature, or both — built entirely from standard, audited primitives.

---

## Principle: unique in composition, never in cryptography

The distinctive part of this design is *how existing pieces are wired together* — the
HONE-native wallet recipient, the policy options, the vault/agent tie-in. The crypto
itself is boring on purpose: AES-256-GCM, X25519, Ed25519, Argon2id, HKDF, Shamir. All
have decades of review and audited libraries. **We never invent a cipher or a KDF.** A
"clever" novel primitive guarding someone's life savings is a liability, not a feature.

## The envelope

One random **Data Encryption Key (DEK)**, 32 bytes, encrypts the payload once:

```
payload_ct = AES-256-GCM(DEK, wallet_material)
```

`wallet_material` = the recovery phrase + every derived private key + every public
key/address (per the "the seed is everything anyway" call — the phrase already grants
all keys, so storing the derived keys adds robustness against derivation drift and lets
any key import into other wallets, at no extra exposure).

The DEK is then **wrapped separately for each way you're allowed to unlock** — the
`age`/PGP envelope pattern. Any wrap that can be undone recovers the DEK, which decrypts
the payload. Adding an unlock method never re-encrypts the payload; it just adds a small
wrapped-DEK stub.

```
.hone-backup  =  { magic, version, cipher, payload_ct, nonce,
                   recipients: [ <wrap stub>, ... ],
                   policy,               // any | all | k-of-n
                   public_index }        // addresses, plaintext, outside the encryption
```

`public_index` is the already-public address list, readable without unlocking anything
(also written beside the file as `…-addresses.txt`).

## Recipient (unlock) types

**1. Password** — `KEK = Argon2id(password, salt, hard params)`, `wrap = AES-256-GCM(KEK, DEK)`.
Standard. The Argon2id cost is tuned so a weak password still costs real time to attack.

**2. HONE wallet — encrypt-to-public-key (the native path).** This is the clean one and
it's why HONE gets this nearly for free: the HONE wallet already derives **hide/seek**
keys expressly for encryption. Encrypt the DEK *to the recipient wallet's hide public
key*:

```
ephemeral X25519 keypair (e_pk, e_sk)
shared = X25519(e_sk, recipient_hide_pub→x25519)
KEK    = HKDF(shared)
wrap   = AES-256-GCM(KEK, DEK);  store e_pk
```

To open: the recipient wallet does `X25519(hide_sk, e_pk)` → same `KEK` → DEK. This is a
NaCl sealed-box / age-X25519 recipient. No password, no signature determinism to worry
about — proper asymmetric encryption, because HONE controls its own key stack and can
expose the ECDH. Ed25519→X25519 conversion is standard (RFC 7748 / libsodium).

**3. External wallet — sign-to-derive (a first-class path — this is the one people will
use most).** Almost every wallet a person already owns — MetaMask, Phantom, a Ledger —
can *sign* but cannot do ECDH. So the DEK is wrapped with a key derived from a
*deterministic signature* over a unique, domain-bound challenge stored in the file:

```
challenge  = random 32 bytes, embedded in a human-readable, domain-separated message:
             "HONE Backup Unlock — do not sign this anywhere else.
              Backup ID: <id>   Challenge: <hex>"
KEK        = HKDF( wallet.sign(message) )
wrap       = AES-256-GCM(KEK, DEK)
```

To reopen: connect the same wallet, sign the same stored message, re-derive the KEK, and
the AES-GCM auth tag confirms it decrypted correctly.

**Which wallets this actually works with:**

| Wallet | Signs with | Deterministic? | Verdict |
|---|---|---|---|
| MetaMask / EVM (software) | secp256k1, EIP-712 | yes (RFC-6979) | works |
| Ledger / Trezor (EVM app) | secp256k1 on-device | yes | works — and strongest (key never leaves device) |
| Phantom / Solana | Ed25519 | always | works |
| Bitcoin (BIP-137 signmessage) | secp256k1 | usually (RFC-6979) | works on most |
| Any randomized signer | — | no | refused at seal time (below) |

**The reliability guarantee — a seal-time determinism check.** The one real risk is a
wallet whose signatures aren't reproducible; a naive design would happily create a backup
that can never be reopened. We close that hole: at seal time the app asks the wallet to
sign the message **twice** and only accepts the wallet if both signatures (and the
re-derived KEK) match. A non-deterministic wallet is rejected on the spot — *"this wallet
produces random signatures and can't lock a backup; use another wallet or a password"* —
so you never end up with an unopenable file. Honest failure at create, never a surprise
at restore.

**Anti-phishing.** The signature *is* the key, so the message is domain-separated and
carries an explicit "do not sign this anywhere else" line, and the challenge is random per
backup and lives *inside the file* — an attacker who can't see your backup file doesn't
even know which message to trick you into signing. For EVM, use EIP-712 typed data so the
wallet shows structured fields ("HONE Backup Unlock, Backup ID …") instead of an opaque
blob. Prefer hardware wallets here: the approval happens on the device screen.

**Multiple external recipients.** Seal to your MetaMask *and* your Ledger with policy
`any` and either one reopens it — cheap redundancy against losing access to one wallet.

## Policy: one file, your choice of AND / OR / quorum

- **any** (OR) — any single recipient opens it. This is "password **or** wallet": lose
  one, the other still works. No new single point of failure. Default for "Both" off.
- **all** (AND) — every listed recipient required. This is the "Both" option: password
  **and** wallet, for higher security.
- **k-of-n** (quorum) — Shamir-split the DEK across *n* recipient wallets; any *k* rebuild
  it. This is guardian / social recovery: seal your backup to five people you trust, need
  any three to help you restore. Ships later; the envelope already supports it.

## The HONE-native twist (why this is more than a generic backup)

Because HONE wallets are a *role hierarchy* (vault, agent, hide, seek…), the unlocking
authority can be **another of your own keys** — which composes with the vault/agent model:

- Seal the **agent wallet's** backup to the **vault key**. The everyday/hot wallet's
  backup can only be reopened by the cold vault. The agent never holds what reopens it.
- Seal the **vault's** backup to a **guardian quorum** (k-of-n) — your own cold key plus
  people you trust — so even total device loss is recoverable without any password written
  down anywhere.

"Your keys guard your other keys" falls out of HONE already having the multi-key
hierarchy; a single-key wallet can't express it. That's the unique, functioning bit.

## Restore

Symmetric and one-action: open the app → **Restore** → pick the `.hone-backup` → the app
reads the recipient stubs and asks only for what the policy needs — a password field, a
"approve in your wallet" prompt, or both — reconstructs the DEK, decrypts, done. Fully
offline; the chain is never contacted to restore.

## Cross-platform: one core, many shells

The whole thing has to run on desktop and phones and produce identical results — a wallet
made on desktop must restore byte-for-byte on a phone, and open the same `.hone-backup`.
That forces one rule: **all crypto lives in a single portable Rust core** — the derivation
(with its frozen conformance vectors), the envelope, the address generation. That core
compiles native for desktop, as bindings for mobile, and to WebAssembly for a local-web
build. Each platform is only a thin UI shell over the *same* brain; the cryptography never
forks per platform, so backups stay portable by construction.

Phasing: desktop first (easiest to run verifiably offline, and where the fleet already
works), then Android (the Capacitor app exists as a starting point), then iOS.

Offline vs. external-wallet unlock, across platforms: creating the wallet and showing the
phrase is **fully offline on every platform** — the app requests no network permission at
all, which is auditable. External-wallet unlock rides that wallet's own transport (browser
extension or USB on desktop; WalletConnect or Bluetooth on a phone), which can touch the
network — so strict air-gap stays with the password and HONE-native methods, and the
air-gapped bridge for external/cross-device flows is **QR handshakes** (animated QR, the
Keystone pattern — no radio involved).

## Unlock = recovery (not signing), and it's ephemeral

**Scope.** This tool *creates* a wallet + backup and *recovers* from one. It is **not** the
wallet. Signing, transactions, marketplace and agent/vault operations — all the day-to-day
"other stuff" — live in the HONE wallet app, a separate component. So when a sealed backup
unlocks, the operation is **recovery**: reconstitute the wallet onto a device, then hand
off to the wallet, which owns signing from there. This tool never signs anything.

Recovery is still handled ephemerally — the plaintext appears only for the hand-off and is
then gone:

- **Decrypt into locked, self-zeroizing memory.** The DEK and the recovered phrase/keys
  live in memory pinned non-swappable (`mlock` / `VirtualLock`) and wrapped in zeroizing
  types (Rust `zeroize` / `secrecy`) that overwrite the bytes the instant they drop.
  Plaintext never touches disk, clipboard, logs, or crash dumps.
- **Write straight into the wallet's own at-rest store, re-sealed.** Recovered material is
  handed to the HONE wallet's encrypted keystore and the plaintext is wiped immediately
  after. It is never left sitting in the clear between "unlocked" and "installed."
- **Verify-only recovery is display-only.** If you're just checking a backup ("do these
  words still open it?"), the phrase is rendered from memory, screenshot-blocked
  (`FLAG_SECURE` on Android), cleared on confirm, and nothing is persisted at all.
- **Show the phrase and every private key — copy or write down.** Create and recovery both
  display the recovery phrase and all private keys, so you can copy-paste them (into another
  wallet, for instance) or write them down. Copy is always an explicit tap, never
  automatic; it puts the value on the clipboard behind a short auto-clear timer (~45s) and
  is flagged sensitive to the OS (Android `EXTRA_IS_SENSITIVE`, kept out of clipboard
  previews/history). Writing it down avoids the clipboard entirely. Everything shown comes
  from the same locked, zeroizing memory and is cleared when you leave the screen — nothing
  reaches disk.
- **The unlock proof is a secret too.** When an external wallet's signature derives the
  KEK, that signature is itself key material and is zeroized the moment the KEK is derived.

From then on, the running wallet does the signing — and *it* applies its own just-in-time
ephemeral unlock per operation, plus the vault/agent split. That per-signature discipline
is the wallet's job, documented with the wallet, not here. Here, unlock happens once, to
recover.

## What functions today vs. later

- **Now, low effort:** envelope + password recipient + HONE-wallet (hide-key ECDH)
  recipient + **external sign-to-derive for the wallets people actually own — MetaMask/EVM,
  Phantom/Solana, and Ledger/Trezor hardware** — with the seal-time determinism check +
  any/all policy. All primitives are in libsodium/RustCrypto; the wallet connections use
  the standard `personal_sign`/EIP-712/`signMessage` each wallet already exposes. This is
  the whole "password · another wallet · both" chooser, external wallets included.
- **Later:** k-of-n guardian recovery (Shamir over the DEK) and the long tail of niche or
  non-deterministic wallets, added as more recipient types without touching the envelope
  or re-encrypting anything.

No new cryptography at any stage — only new *recipients* on a standard envelope.
