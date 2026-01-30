#!/bin/bash
#
# Build all platforms for Unity
#
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=============================================="
echo "Building libpeer for all Unity platforms"
echo "=============================================="

# macOS (always builds on macOS)
if [[ "$OSTYPE" == "darwin"* ]]; then
    echo ""
    echo "[1/3] Building macOS..."
    bash "${SCRIPT_DIR}/build_macos.sh"

    echo ""
    echo "[2/3] Building iOS..."
    bash "${SCRIPT_DIR}/build_ios.sh"
fi

# Android (requires NDK)
if [ -n "$ANDROID_NDK_HOME" ] || [ -n "$NDK_ROOT" ] || [ -d "$HOME/Library/Android/sdk/ndk" ]; then
    echo ""
    echo "[3/3] Building Android..."
    bash "${SCRIPT_DIR}/build_android.sh"
else
    echo ""
    echo "[3/3] Skipping Android (NDK not found)"
fi

echo ""
echo "=============================================="
echo "All builds complete!"
echo "=============================================="
echo ""
echo "UPM package location:"
echo "  $(cd "${SCRIPT_DIR}/../upm/jp.co.mixi.libpeer" && pwd)"
