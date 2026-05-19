#!/usr/bin/env bash
# Downloads a prebuilt wgpu-native release into renderer/wgpu-native/
# Usage: ./scripts/fetch_wgpu_native.sh [version]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

VERSION="${1:-v0.19.4.1}"
OUTDIR="$REPO_ROOT/wgpu-native"

OS="$(uname -s)"
ARCH="$(uname -m)"

case "$OS" in
    Linux)
        case "$ARCH" in
            x86_64)  PLATFORM="linux-x86_64" ;;
            aarch64) PLATFORM="linux-aarch64" ;;
            *)        echo "Unsupported arch: $ARCH"; exit 1 ;;
        esac
        ;;
    Darwin)
        case "$ARCH" in
            x86_64)  PLATFORM="macos-x86_64" ;;
            arm64)   PLATFORM="macos-aarch64" ;;
            *)        echo "Unsupported arch: $ARCH"; exit 1 ;;
        esac
        ;;
    *)
        echo "Unsupported OS: $OS"; exit 1 ;;
esac

ARCHIVE="wgpu-${PLATFORM}-release.zip"
URL="https://github.com/gfx-rs/wgpu-native/releases/download/${VERSION}/${ARCHIVE}"

echo "Downloading wgpu-native ${VERSION} for ${PLATFORM}..."
echo "  URL: $URL"
echo "  -> $OUTDIR"

mkdir -p "$OUTDIR/include/webgpu" "$OUTDIR/lib"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

curl -fL "$URL" -o "$TMP/$ARCHIVE"
unzip -q "$TMP/$ARCHIVE" -d "$TMP/extracted"

# The zip has flat layout: webgpu.h, wgpu.h, libwgpu_native.{a,so}
cp "$TMP/extracted/webgpu.h" "$OUTDIR/include/webgpu/" 2>/dev/null || true
cp "$TMP/extracted/wgpu.h"   "$OUTDIR/include/webgpu/" 2>/dev/null || true

# Static preferred over shared for portability
if ls "$TMP/extracted/libwgpu_native.a" 2>/dev/null; then
    cp "$TMP/extracted/libwgpu_native.a" "$OUTDIR/lib/"
fi
for ext in so dylib dll lib; do
    ls "$TMP/extracted/libwgpu_native.$ext" 2>/dev/null && \
        cp "$TMP/extracted/libwgpu_native.$ext" "$OUTDIR/lib/" || true
    ls "$TMP/extracted/wgpu_native.$ext" 2>/dev/null && \
        cp "$TMP/extracted/wgpu_native.$ext" "$OUTDIR/lib/" || true
done

echo ""
echo "Done. Contents of $OUTDIR:"
find "$OUTDIR" -type f | sort
