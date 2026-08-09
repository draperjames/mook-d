# aarch64 cross-compile toolchain for the Ableton Move (ARM64 Linux).
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc-aarch64-linux-gnu libc6-dev-arm64-cross ca-certificates file \
 && rm -rf /var/lib/apt/lists/*
COPY src/ /build/src/
WORKDIR /build
