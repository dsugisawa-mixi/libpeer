#!/bin/bash
#
# Android Build Script for libpeer Unity Plugin
#
# Requirements:
#   - Android NDK (set ANDROID_NDK_HOME or NDK_ROOT)
#   - CMake 3.16+
#
# Builds for:
#   - arm64-v8a (64-bit ARM, most modern devices)
#   - armeabi-v7a (32-bit ARM, legacy devices)
#   - x86_64 (Emulator)
#
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_ROOT="${PROJECT_ROOT}/_build/android"
OUTPUT_DIR="${PROJECT_ROOT}/upm/jp.co.mixi.libpeer/Plugins/Android"

# Find NDK
if [ -n "$ANDROID_NDK_HOME" ]; then
    NDK_ROOT="$ANDROID_NDK_HOME"
elif [ -n "$NDK_ROOT" ]; then
    NDK_ROOT="$NDK_ROOT"
elif [ -d "$HOME/Android/Sdk/ndk" ]; then
    # Find latest NDK version
    NDK_ROOT=$(ls -d "$HOME/Android/Sdk/ndk"/*/ 2>/dev/null | sort -V | tail -1)
elif [ -d "$HOME/Library/Android/sdk/ndk" ]; then
    # macOS Android Studio location
    NDK_ROOT=$(ls -d "$HOME/Library/Android/sdk/ndk"/*/ 2>/dev/null | sort -V | tail -1)
else
    echo "Error: Android NDK not found"
    echo "Set ANDROID_NDK_HOME or NDK_ROOT environment variable"
    exit 1
fi

# Remove trailing slash
NDK_ROOT="${NDK_ROOT%/}"

echo "Using NDK: ${NDK_ROOT}"

# NDK toolchain file
TOOLCHAIN="${NDK_ROOT}/build/cmake/android.toolchain.cmake"
if [ ! -f "$TOOLCHAIN" ]; then
    echo "Error: NDK toolchain not found at: ${TOOLCHAIN}"
    exit 1
fi

# Android API level (API 24+ required for getifaddrs in usrsctp)
ANDROID_API=24

# ABIs to build
ABIS=("arm64-v8a" "armeabi-v7a" "x86_64")

# Clean previous builds
rm -rf "${BUILD_ROOT}"
mkdir -p "${BUILD_ROOT}"

echo "================================================"
echo "Building libpeer for Android"
echo "================================================"

build_abi() {
    local ABI=$1
    local BUILD_DIR="${BUILD_ROOT}/${ABI}"

    echo ""
    echo "Building for ${ABI}..."
    echo "----------------------------------------"

    mkdir -p "${BUILD_DIR}"

    # Configure
    cmake -S "${PROJECT_ROOT}/cmake/unity" -B "${BUILD_DIR}" \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
        -DANDROID_ABI="${ABI}" \
        -DANDROID_PLATFORM="android-${ANDROID_API}" \
        -DANDROID_STL=c++_static \
        -DCMAKE_BUILD_TYPE=Release \
        -DUNITY_PLATFORM=Android

    # Build
    cmake --build "${BUILD_DIR}" --config Release --parallel

    echo "Built: ${BUILD_DIR}/libpeer.so"
}

# Build for each ABI
for abi in "${ABIS[@]}"; do
    build_abi "$abi"
done

echo ""
echo "================================================"
echo "Installing to UPM Package"
echo "================================================"

rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}/libs"

for abi in "${ABIS[@]}"; do
    mkdir -p "${OUTPUT_DIR}/libs/${abi}"

    # Find and copy the .so file
    SO_FILE=$(find "${BUILD_ROOT}/${abi}" -name "libpeer.so" | head -1)
    if [ -n "$SO_FILE" ]; then
        cp "$SO_FILE" "${OUTPUT_DIR}/libs/${abi}/"
        echo "Installed: ${OUTPUT_DIR}/libs/${abi}/libpeer.so"

        # Strip debug symbols for smaller size
        STRIP="${NDK_ROOT}/toolchains/llvm/prebuilt/*/bin/llvm-strip"
        STRIP_CMD=$(echo $STRIP)
        if [ -x "$STRIP_CMD" ]; then
            "$STRIP_CMD" "${OUTPUT_DIR}/libs/${abi}/libpeer.so"
            echo "Stripped debug symbols"
        fi
    fi
done

# Create AndroidManifest.xml for the plugin
cat > "${OUTPUT_DIR}/AndroidManifest.xml" << 'EOF'
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="jp.co.mixi.libpeer">

    <!-- Required for WebRTC networking -->
    <uses-permission android:name="android.permission.INTERNET" />
    <uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />

</manifest>
EOF

# Create Unity meta files
cat > "${OUTPUT_DIR}/libs.meta" << 'EOF'
fileFormatVersion: 2
guid: GENERATE_NEW_GUID_HERE
folderAsset: yes
DefaultImporter:
  externalObjects: {}
  userData:
  assetBundleName:
  assetBundleVariant:
EOF

# Meta file for each ABI folder and .so
for abi in "${ABIS[@]}"; do
    cat > "${OUTPUT_DIR}/libs/${abi}.meta" << EOF
fileFormatVersion: 2
guid: GENERATE_NEW_GUID_HERE
folderAsset: yes
DefaultImporter:
  externalObjects: {}
  userData:
  assetBundleName:
  assetBundleVariant:
EOF

    cat > "${OUTPUT_DIR}/libs/${abi}/libpeer.so.meta" << EOF
fileFormatVersion: 2
guid: GENERATE_NEW_GUID_HERE
PluginImporter:
  externalObjects: {}
  serializedVersion: 2
  iconMap: {}
  executionOrder: {}
  defineConstraints: []
  isPreloaded: 0
  isOverridable: 0
  isExplicitlyReferenced: 0
  validateReferences: 1
  platformData:
  - first:
      : Any
    second:
      enabled: 0
      settings:
        Exclude Android: 0
        Exclude Editor: 1
        Exclude Linux64: 1
        Exclude OSXUniversal: 1
        Exclude Win: 1
        Exclude Win64: 1
  - first:
      Android: Android
    second:
      enabled: 1
      settings:
        CPU: ${abi}
  - first:
      Any:
    second:
      enabled: 0
      settings: {}
  - first:
      Editor: Editor
    second:
      enabled: 0
      settings:
        DefaultValueInitialized: true
  userData:
  assetBundleName:
  assetBundleVariant:
EOF
done

echo ""
echo "================================================"
echo "Build Complete!"
echo "================================================"
echo ""
echo "Output: ${OUTPUT_DIR}"
echo ""
echo "Libraries:"
for abi in "${ABIS[@]}"; do
    SIZE=$(du -h "${OUTPUT_DIR}/libs/${abi}/libpeer.so" 2>/dev/null | cut -f1)
    echo "  ${abi}: ${SIZE}"
done
echo ""
echo "Note: Update GUIDs in .meta files before committing"
