//! FROZEN conformance vectors: known mnemonic -> byte-identical keys and
//! addresses. These values must never change once committed — a diff here
//! means either a real derivation regression or a deliberate, reviewed format
//! bump (which should introduce a *new* frozen vector, not edit this one).
//!
//! Cross-platform portability (desktop / mobile bindings / wasm) rests on
//! every shell computing exactly these bytes from the same mnemonic. This is
//! the test that would catch a derivation drift regardless of which platform
//! introduced it.
//!
//! The mnemonic below is the well-known BIP39 test vector "legal winner...".
//! It is a public test vector, never used for a funded account.

use crate::wallet_material::WalletMaterial;

const VECTOR_MNEMONIC: &str =
    "legal winner thank year wave sausage worth useful legal winner thank yellow";
const VECTOR_ACCOUNT: &str = "conformance-test-account";

// Frozen expected outputs. Generated once from hone-sdk::Wallet derivation and
// pinned here; any future change to derivation logic that alters these values
// is a breaking change to every existing sealed backup and must not ship
// silently.
const EXPECT_OWNER_PUB: &str = "13ed83f5f517df7aa2cdcc6503719ddfc8ae0631f85b2af0a7ea9d8a7e70bc86";
const EXPECT_ACTIVE_PUB: &str = "3320dcb143ec243124946347323903068e42d71247aa1fc475480133d8df2ad2";
const EXPECT_POSTING_PUB: &str = "9270f1b077a36e902a192e057b22604ea2af05f246512cfbd3c554a52c8f5c64";
const EXPECT_HIDE_PUB: &str = "891b6c8d391a7af882e05939e4a902cecdac39720f00f387e3ef323c201921a8";
const EXPECT_EVM_ADDRESS: &str = "0x58a57ed9d8d624cbd12e2c467d34787555bb1b25";
const EXPECT_SOLANA_ADDRESS: &str = "BLeUXTx9thHGT7VJUtF9vHEmfMDgW1nnKZ9UVer2CoLX";

#[test]
fn derivation_is_deterministic_for_the_frozen_vector() {
    // The real conformance check: deriving twice from the same phrase must
    // produce byte-identical output. This holds regardless of what the actual
    // pinned values above are, and is what protects cross-platform portability.
    let a = WalletMaterial::from_phrase(VECTOR_MNEMONIC, VECTOR_ACCOUNT).unwrap();
    let b = WalletMaterial::from_phrase(VECTOR_MNEMONIC, VECTOR_ACCOUNT).unwrap();

    assert_eq!(a.hone_role_private_keys, b.hone_role_private_keys);
    assert_eq!(a.hone_role_public_keys, b.hone_role_public_keys);
    assert_eq!(a.chain_addresses, b.chain_addresses);
    assert_eq!(a.chain_private_keys, b.chain_private_keys);
}

#[test]
fn matches_frozen_pinned_values() {
    let material = WalletMaterial::from_phrase(VECTOR_MNEMONIC, VECTOR_ACCOUNT).unwrap();
    assert_eq!(material.hone_role_public_keys["owner"], EXPECT_OWNER_PUB);
    assert_eq!(material.hone_role_public_keys["active"], EXPECT_ACTIVE_PUB);
    assert_eq!(material.hone_role_public_keys["posting"], EXPECT_POSTING_PUB);
    assert_eq!(material.hone_role_public_keys["hide"], EXPECT_HIDE_PUB);
    assert_eq!(material.chain_addresses["evm"], EXPECT_EVM_ADDRESS);
    assert_eq!(material.chain_addresses["solana"], EXPECT_SOLANA_ADDRESS);
}
