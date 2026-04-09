#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_ID="kwahzolin"
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

if ! command -v "${CROSS_PREFIX}gcc" >/dev/null 2>&1; then
    echo "Error: cross-compiler '${CROSS_PREFIX}gcc' not found"
    echo "Hint: apt-get install gcc-aarch64-linux-gnu"
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

echo "Done — dist/${MODULE_ID}-module.tar.gz"
