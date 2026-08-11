# HONE node image — podman-first, docker-compatible OCI build.
#
# Builds under either:
#   podman build -t localhost/hone-node:latest -f Containerfile .
#   docker build -t hone-node:latest -f Containerfile .
#
# Stage 1 compiles hone-node (in-process Candle inference, feature
# `inference-embedded` is on by default) and the hone CLI from source.
# Stage 2 is a slim Debian runtime with only the two binaries, genesis
# files and the self-healing entrypoint — no build toolchain, no source.
#
# Toolchain is pinned to match rust/rust-toolchain.toml (1.90.0): newer
# rustc (1.93+) ICEs on hone-node's api module — do not bump this without
# clearing that first (see rust/rust-toolchain.toml for the tracking note).

# ── Stage 1: build ────────────────────────────────────────────────────────
FROM docker.io/library/rust:1.90-bookworm AS builder

# build-essential: cc/g++ for secp256k1-sys, rocksdb-sys, onig-sys (tokenizers).
# clang + cmake: rocksdb-sys' bundled RocksDB C++ build.
# libssl-dev + pkg-config: hone-cli's reqwest (native-tls, not rustls).
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential clang cmake pkg-config libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY rust ./rust

# hone-node lives in the rust/ workspace; hone-cli is its own standalone
# workspace (see rust/hone-cli/Cargo.toml). Build only what each binary
# needs — `-p hone-node` skips unrelated workspace members (hone-p2p,
# hone-market, linkgit) that this image doesn't ship.
RUN cd rust && cargo build --release --locked -p hone-node
RUN cd rust/hone-cli && cargo build --release --locked

# ── Stage 2: runtime ──────────────────────────────────────────────────────
FROM docker.io/library/debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl libssl3 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10000 --create-home --shell /usr/sbin/nologin hone

COPY --from=builder /build/rust/target/release/hone-node /usr/local/bin/hone-node
COPY --from=builder /build/rust/hone-cli/target/release/hone /usr/local/bin/hone
COPY rust/hone-node/genesis.json          /app/genesis.json
COPY rust/hone-node/testnet-genesis.json  /app/testnet-genesis.json
COPY deploy/podman/entrypoint.sh          /usr/local/bin/hone-entrypoint.sh
RUN chmod +x /usr/local/bin/hone-entrypoint.sh

# Data volume: chain state/db, model cache, wallet/keys. Never baked into
# the image — mount a volume here in every deploy path (compose/pod/quadlet).
ENV HONE_DATA_DIR=/data \
    HONE_MODEL_DIR=/data/models \
    HONE_GENESIS_FILE=/app/genesis.json \
    HONE_API_PORT=4242 \
    HONE_P2P_PORT=6942

RUN mkdir -p /data/models && chown -R hone:hone /data /app
VOLUME ["/data"]
EXPOSE 4242 6942

USER hone
WORKDIR /app
ENTRYPOINT ["/usr/local/bin/hone-entrypoint.sh"]
CMD ["hone-node"]
