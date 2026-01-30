#!/bin/bash
#
# iOS Build Script for libpeer Unity Plugin
#
# Builds:
#   - Device (arm64)
#   - Simulator (arm64, x86_64)
#
# Output:
#   - libpeer.xcframework (recommended for Unity 2021+)
#   - Or separate .a files for manual integration
#
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_ROOT="${PROJECT_ROOT}/_build/ios"
OUTPUT_DIR="${PROJECT_ROOT}/upm/jp.co.mixi.libpeer/Plugins/iOS"

# iOS deployment target
IOS_DEPLOYMENT_TARGET="12.0"

# Clean previous builds
rm -rf "${BUILD_ROOT}"
mkdir -p "${BUILD_ROOT}"

echo "================================================"
echo "Building libpeer for iOS"
echo "================================================"

# Function to build for a specific platform
build_platform() {
    local PLATFORM=$1
    local ARCH=$2
    local SDK=$3
    local BUILD_DIR="${BUILD_ROOT}/${PLATFORM}-${ARCH}"

    echo ""
    echo "Building for ${PLATFORM} (${ARCH})..."
    echo "----------------------------------------"

    mkdir -p "${BUILD_DIR}"

    # Determine sysroot
    local SYSROOT=$(xcrun --sdk ${SDK} --show-sdk-path)

    # Configure
    cmake -S "${PROJECT_ROOT}/cmake/unity" -B "${BUILD_DIR}" \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_ARCHITECTURES="${ARCH}" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET}" \
        -DCMAKE_OSX_SYSROOT="${SYSROOT}" \
        -DCMAKE_C_FLAGS="-fembed-bitcode -fvisibility=hidden" \
        -DCMAKE_BUILD_TYPE=Release \
        -DUNITY_PLATFORM=iOS

    # Build
    cmake --build "${BUILD_DIR}" --config Release --parallel

    # Build fat library (combines all .a into one)
    local FAT_LIB="${BUILD_DIR}/libpeer_fat.a"
    bash "${PROJECT_ROOT}/scripts/create_fat_library_ios.sh" "${BUILD_DIR}" "${FAT_LIB}"

    echo "Built: ${FAT_LIB}"
}

# Build for Device (arm64)
build_platform "iphoneos" "arm64" "iphoneos"

# Build for Simulator (arm64 for Apple Silicon)
# Note: x86_64 simulator is skipped as Intel Macs are rare now
build_platform "iphonesimulator" "arm64" "iphonesimulator"

echo ""
echo "================================================"
echo "Creating Libraries"
echo "================================================"

# Create directories
DEVICE_DIR="${BUILD_ROOT}/device"
SIMULATOR_DIR="${BUILD_ROOT}/simulator"
mkdir -p "${DEVICE_DIR}" "${SIMULATOR_DIR}"

# Copy device library
cp "${BUILD_ROOT}/iphoneos-arm64/libpeer_fat.a" "${DEVICE_DIR}/libpeer.a"

# Copy simulator library (arm64 only)
cp "${BUILD_ROOT}/iphonesimulator-arm64/libpeer_fat.a" "${SIMULATOR_DIR}/libpeer.a"

echo ""
echo "================================================"
echo "Creating XCFramework"
echo "================================================"

XCFRAMEWORK_PATH="${BUILD_ROOT}/libpeer.xcframework"
rm -rf "${XCFRAMEWORK_PATH}"

xcodebuild -create-xcframework \
    -library "${DEVICE_DIR}/libpeer.a" \
    -library "${SIMULATOR_DIR}/libpeer.a" \
    -output "${XCFRAMEWORK_PATH}"

echo "Created: ${XCFRAMEWORK_PATH}"

echo ""
echo "================================================"
echo "Installing to UPM Package"
echo "================================================"

rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}"

# Copy xcframework
cp -R "${XCFRAMEWORK_PATH}" "${OUTPUT_DIR}/"

# Copy headers
mkdir -p "${OUTPUT_DIR}/include"
cp "${PROJECT_ROOT}/include/peer.h" "${OUTPUT_DIR}/include/"
cp "${PROJECT_ROOT}/include/peer_connection.h" "${OUTPUT_DIR}/include/"
cp "${PROJECT_ROOT}/include/peer_signaling.h" "${OUTPUT_DIR}/include/"

# Create Unity meta file for xcframework
cat > "${OUTPUT_DIR}/libpeer.xcframework.meta" << 'EOF'
fileFormatVersion: 2
guid: GENERATE_NEW_GUID_HERE
folderAsset: yes
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
        Exclude Editor: 1
        Exclude Linux64: 1
        Exclude OSXUniversal: 1
        Exclude Win: 1
        Exclude Win64: 1
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
  - first:
      iPhone: iOS
    second:
      enabled: 1
      settings:
        AddToEmbeddedBinaries: false
        CPU: AnyCPU
        CompileFlags:
        FrameworkDependencies:
  userData:
  assetBundleName:
  assetBundleVariant:
EOF

# Also provide standalone .a for legacy Unity versions
echo ""
echo "Creating standalone .a for legacy support..."
LEGACY_DIR="${OUTPUT_DIR}/Legacy"
mkdir -p "${LEGACY_DIR}"
cp "${DEVICE_DIR}/libpeer.a" "${LEGACY_DIR}/libpeer.a"

# Create meta for legacy .a
cat > "${LEGACY_DIR}/libpeer.a.meta" << 'EOF'
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
        Exclude Editor: 1
        Exclude Linux64: 1
        Exclude OSXUniversal: 1
        Exclude Win: 1
        Exclude Win64: 1
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
  - first:
      iPhone: iOS
    second:
      enabled: 1
      settings:
        AddToEmbeddedBinaries: false
        CPU: ARM64
        CompileFlags:
        FrameworkDependencies:
  userData:
  assetBundleName:
  assetBundleVariant:
EOF

echo ""
echo "================================================"
echo "Build Complete!"
echo "================================================"
echo ""
echo "Output:"
echo "  XCFramework: ${OUTPUT_DIR}/libpeer.xcframework"
echo "  Legacy .a:   ${OUTPUT_DIR}/Legacy/libpeer.a"
echo ""
echo "Note: Update GUID in .meta files before committing"
