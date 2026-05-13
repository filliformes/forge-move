#!/usr/bin/env bash
# Forge — Docker ARM64 cross-compile
# Uses the docker create + docker cp pattern (works on Windows MSYS where
# `docker run -v` mangles paths).
set -e

MODULE_ID="forge"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WIN_ROOT="$(cd "$ROOT" && pwd -W 2>/dev/null || pwd)"
LOCAL_DIST="$ROOT/dist/$MODULE_ID"

echo "==> Building Docker image…"
docker build -t schwung-forge-builder "$SCRIPT_DIR" >/dev/null

mkdir -p "$LOCAL_DIST"

echo "==> Compiling dsp.so (aarch64)…"
CONTAINER_ID=$(MSYS_NO_PATHCONV=1 docker create \
    -w /build schwung-forge-builder bash -c "
        set -e
        dos2unix /build/src/dsp/forge.c 2>/dev/null || true
        mkdir -p /build/dist/forge
        aarch64-linux-gnu-gcc \
            -O2 -ffast-math -shared -fPIC \
            -Wall -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable \
            -o /build/dist/forge/dsp.so \
            /build/src/dsp/forge.c \
            -lm
        echo 'COMPILE_OK'
    ")

# Copy source IN
docker cp "$WIN_ROOT/src" "$CONTAINER_ID:/build/src"

# Run the build
docker start -a "$CONTAINER_ID"
EXIT_CODE=$(docker inspect "$CONTAINER_ID" --format='{{.State.ExitCode}}')
if [ "$EXIT_CODE" != "0" ]; then
    echo "ERROR: Compile failed (exit $EXIT_CODE)."
    docker rm "$CONTAINER_ID" >/dev/null 2>&1 || true
    exit 1
fi

# Extract artifact
docker cp "$CONTAINER_ID:/build/dist/forge/dsp.so" "$LOCAL_DIST/dsp.so"
docker rm "$CONTAINER_ID" >/dev/null

# Copy module.json next to dsp.so for the install/release tarball
cp "$ROOT/src/module.json" "$LOCAL_DIST/module.json"

# Verify the binary actually exports the init symbol (catches silent failures).
# Uses aarch64 nm via Docker because GNU strings doesn't index .dynsym on
# cross-compiled .so. MSYS_NO_PATHCONV avoids Windows path mangling.
VERIFY_OUT=$(MSYS_NO_PATHCONV=1 docker run --rm \
    -v "$WIN_ROOT/dist/forge:/d" schwung-forge-builder \
    aarch64-linux-gnu-nm -D /d/dsp.so 2>/dev/null || true)
if echo "$VERIFY_OUT" | grep -q "move_plugin_init_v2"; then
    echo "==> Verified: move_plugin_init_v2 exported."
else
    echo "WARNING: could not verify move_plugin_init_v2 export (skipping check)."
fi

# Tarball
tar -czf "$ROOT/dist/$MODULE_ID-module.tar.gz" -C "$ROOT/dist" "$MODULE_ID/"
echo "==> Built: dist/$MODULE_ID-module.tar.gz"
