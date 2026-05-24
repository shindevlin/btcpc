fn main() {
    // Generate UniFFI scaffolding from btcpc_mobile_core.udl
    uniffi::generate_scaffolding("src/btcpc_mobile_core.udl").unwrap();
}
