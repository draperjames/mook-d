#!/bin/bash
# Cross-compile mookd.c -> ARM64 dsp.so via Docker, package as tar.gz.
# Mirrors the aphex-move build pattern (COPY src/, docker create + cp) so it
# works on Windows/macOS/Linux Docker alike.
set -e
MODULE_ID="mook-d"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."

echo "Building $MODULE_ID for ARM64 (aarch64)..."
docker build -t ${MODULE_ID}-builder -f "$ROOT/scripts/Dockerfile" "$ROOT"

CONTAINER_ID=$(docker create ${MODULE_ID}-builder bash -c "
    set -e
    mkdir -p /build/dist/$MODULE_ID
    aarch64-linux-gnu-gcc \
      -O2 -shared -fPIC -ffast-math \
      -Wall -Wno-unused -Wno-format \
      -I /build/src/dsp \
      -o /build/dist/$MODULE_ID/dsp.so \
      /build/src/dsp/mookd.c \
      -lm
    cp /build/src/module.json /build/dist/$MODULE_ID/
    [ -f /build/src/help.json ] && cp /build/src/help.json /build/dist/$MODULE_ID/ || true
    echo '=== Build complete ==='
    ls -la /build/dist/$MODULE_ID/
    echo '=== GLIBC symbols (want <= 2.27) ==='
    aarch64-linux-gnu-strings /build/dist/$MODULE_ID/dsp.so 2>/dev/null | grep GLIBC_ | sort -uV || true
")

docker start -a "$CONTAINER_ID"
EXIT_CODE=$(docker inspect "$CONTAINER_ID" --format='{{.State.ExitCode}}')
if [ "$EXIT_CODE" != "0" ]; then
    echo "ERROR: Compile failed (exit $EXIT_CODE)."; docker rm "$CONTAINER_ID" >/dev/null; exit 1
fi

mkdir -p "$ROOT/dist/$MODULE_ID"
docker cp "$CONTAINER_ID:/build/dist/$MODULE_ID/dsp.so"      "$ROOT/dist/$MODULE_ID/dsp.so"
docker cp "$CONTAINER_ID:/build/dist/$MODULE_ID/module.json" "$ROOT/dist/$MODULE_ID/module.json"
docker cp "$CONTAINER_ID:/build/dist/$MODULE_ID/help.json"   "$ROOT/dist/$MODULE_ID/help.json" 2>/dev/null || true
docker rm "$CONTAINER_ID" >/dev/null

cd "$ROOT/dist"
tar -czf ${MODULE_ID}-module.tar.gz ${MODULE_ID}/
echo "Packaged: dist/${MODULE_ID}-module.tar.gz"
