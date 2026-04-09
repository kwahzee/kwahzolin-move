#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_ID="kwahzolin"
IMAGE_NAME="kwahzolin-builder"
REBUILD_DOCKER="${REBUILD_DOCKER:-0}"
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

if [ -z "$CROSS_PREFIX_OVERRIDE" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Kwahzolin Build (via Docker) ==="

    if [ "$REBUILD_DOCKER" = "1" ] || ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
    fi

    echo "Running cross-compilation inside container..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -e CROSS_PREFIX_OVERRIDE=1 \
        "$IMAGE_NAME"

    echo ""
    echo "=== Done ==="
    echo "Output: $REPO_ROOT/dist/${MODULE_ID}-module.tar.gz"
    exit 0
fi

cd "$REPO_ROOT"

if ! command -v "${CROSS_PREFIX}gcc" >/dev/null 2>&1; then
    echo "Error: cross-compiler '${CROSS_PREFIX}gcc' not found"
    echo "Hint: run  apt-get install gcc-aarch64-linux-gnu"
    exit 1
fi

echo "Cross-compiling for aarch64 with ${CROSS_PREFIX}gcc..."

mkdir -p "dist/$MODULE_ID"

"${CROSS_PREFIX}gcc" \
    -g -O3 -shared -fPIC \
    src/dsp/kwahzolin.c \
    -o "dist/$MODULE_ID/dsp.so" \
    -I src/dsp/include \
    -lm

echo "DSP compiled: dist/$MODULE_ID/dsp.so"

cp src/module.json "dist/$MODULE_ID/"
cp src/ui.js       "dist/$MODULE_ID/"

echo "Module files copied."

cd dist
tar -czf "${MODULE_ID}-module.tar.gz" "$MODULE_ID/"
cd ..

echo "Tarball: dist/${MODULE_ID}-module.tar.gz"
