#!/bin/bash
#
# macOS Build Script for libpeer Unity Plugin
#
# Builds Universal Binary (arm64 + x86_64) bundle with:
#   - Code signing
#   - Notarization (optional)
#
# Requirements:
#   - Xcode Command Line Tools
#   - Apple Developer certificate (for signing)
#   - App Store Connect API key (for notarization)
#
# Environment Variables:
#   CODESIGN_IDENTITY  - Certificate identity (e.g., "Developer ID Application: MIXI, Inc.")
#   NOTARIZE_PROFILE   - Notarytool profile name (created via: xcrun notarytool store-credentials)
#
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_ROOT="${PROJECT_ROOT}/_build/macos"
OUTPUT_DIR="${PROJECT_ROOT}/upm/jp.co.mixi.libpeer/Plugins/macOS"

# macOS deployment target
MACOS_DEPLOYMENT_TARGET="10.15"

# Bundle identifier
BUNDLE_ID="jp.co.mixi.libpeer"

# Clean previous builds
rm -rf "${BUILD_ROOT}"
mkdir -p "${BUILD_ROOT}"

echo "================================================"
echo "Building libpeer for macOS"
echo "================================================"

# Build for a specific architecture
build_arch() {
    local ARCH=$1
    local BUILD_DIR="${BUILD_ROOT}/${ARCH}"

    echo ""
    echo "Building for ${ARCH}..."
    echo "----------------------------------------"

    mkdir -p "${BUILD_DIR}"

    cmake -S "${PROJECT_ROOT}/cmake/unity" -B "${BUILD_DIR}" \
        -DCMAKE_OSX_ARCHITECTURES="${ARCH}" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DUNITY_PLATFORM=macOS

    cmake --build "${BUILD_DIR}" --config Release --parallel
}

# Build for both architectures
build_arch "arm64"
build_arch "x86_64"

echo ""
echo "================================================"
echo "Creating Universal Bundle"
echo "================================================"

BUNDLE_DIR="${BUILD_ROOT}/libpeer.bundle"
BUNDLE_CONTENTS="${BUNDLE_DIR}/Contents"
BUNDLE_MACOS="${BUNDLE_CONTENTS}/MacOS"

rm -rf "${BUNDLE_DIR}"
mkdir -p "${BUNDLE_MACOS}"

# Find the built bundles (output name is "peer" from CMake)
ARM64_BUNDLE="${BUILD_ROOT}/arm64/peer.bundle"
X64_BUNDLE="${BUILD_ROOT}/x86_64/peer.bundle"

# Get the actual binary inside the bundle
ARM64_BIN="${ARM64_BUNDLE}/Contents/MacOS/peer"
X64_BIN="${X64_BUNDLE}/Contents/MacOS/peer"

# Verify files exist
if [ ! -f "$ARM64_BIN" ]; then
    echo "Error: ARM64 binary not found at: $ARM64_BIN"
    exit 1
fi
if [ ! -f "$X64_BIN" ]; then
    echo "Error: x86_64 binary not found at: $X64_BIN"
    exit 1
fi

# Create universal binary
echo "Creating universal binary..."
lipo -create \
    "${ARM64_BIN}" \
    "${X64_BIN}" \
    -output "${BUNDLE_MACOS}/libpeer"

# Verify architectures
echo "Architectures:"
lipo -info "${BUNDLE_MACOS}/libpeer"

# Create Info.plist
cat > "${BUNDLE_CONTENTS}/Info.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleExecutable</key>
    <string>libpeer</string>
    <key>CFBundleIdentifier</key>
    <string>${BUNDLE_ID}</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>libpeer</string>
    <key>CFBundlePackageType</key>
    <string>BNDL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSMinimumSystemVersion</key>
    <string>${MACOS_DEPLOYMENT_TARGET}</string>
    <key>NSHumanReadableCopyright</key>
    <string>Copyright MIXI, Inc. All rights reserved.</string>
</dict>
</plist>
EOF

echo ""
echo "================================================"
echo "Code Signing"
echo "================================================"

if [ -n "$CODESIGN_IDENTITY" ]; then
    echo "Signing with: ${CODESIGN_IDENTITY}"

    # Create entitlements file
    ENTITLEMENTS="${BUILD_ROOT}/entitlements.plist"
    cat > "${ENTITLEMENTS}" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <!-- Allow network access -->
    <key>com.apple.security.network.client</key>
    <true/>
    <key>com.apple.security.network.server</key>
    <true/>

    <!-- Hardened runtime exceptions for Unity compatibility -->
    <key>com.apple.security.cs.allow-unsigned-executable-memory</key>
    <true/>
    <key>com.apple.security.cs.disable-library-validation</key>
    <true/>
</dict>
</plist>
EOF

    # Sign the bundle
    codesign --force --deep --sign "${CODESIGN_IDENTITY}" \
        --entitlements "${ENTITLEMENTS}" \
        --options runtime \
        --timestamp \
        "${BUNDLE_DIR}"

    # Verify signature
    echo ""
    echo "Verifying signature..."
    codesign --verify --verbose=2 "${BUNDLE_DIR}"

    echo "Signature valid"
else
    echo "CODESIGN_IDENTITY not set, skipping code signing"
    echo ""
    echo "To sign, set: export CODESIGN_IDENTITY=\"Developer ID Application: Your Name (TEAMID)\""
    echo "List available identities: security find-identity -v -p codesigning"

    # Ad-hoc sign for local testing (Unity requires signature on macOS)
    echo ""
    echo "Applying ad-hoc signature for local testing..."
    codesign --force --deep --sign - "${BUNDLE_DIR}"
fi

echo ""
echo "================================================"
echo "Notarization"
echo "================================================"

if [ -n "$CODESIGN_IDENTITY" ] && [ -n "$NOTARIZE_PROFILE" ]; then
    echo "Starting notarization..."

    # Create ZIP for notarization
    NOTARIZE_ZIP="${BUILD_ROOT}/libpeer-notarize.zip"
    ditto -c -k --keepParent "${BUNDLE_DIR}" "${NOTARIZE_ZIP}"

    # Submit for notarization
    echo "Submitting to Apple..."
    xcrun notarytool submit "${NOTARIZE_ZIP}" \
        --keychain-profile "${NOTARIZE_PROFILE}" \
        --wait

    # Staple the ticket
    echo "Stapling notarization ticket..."
    xcrun stapler staple "${BUNDLE_DIR}"

    echo "Notarization complete"
    rm -f "${NOTARIZE_ZIP}"

elif [ -n "$CODESIGN_IDENTITY" ]; then
    echo "NOTARIZE_PROFILE not set, skipping notarization"
    echo ""
    echo "To notarize, first create a profile:"
    echo "  xcrun notarytool store-credentials \"libpeer-notarize\" \\"
    echo "    --apple-id \"your@email.com\" \\"
    echo "    --team-id \"TEAMID\" \\"
    echo "    --password \"app-specific-password\""
    echo ""
    echo "Then set: export NOTARIZE_PROFILE=\"libpeer-notarize\""
else
    echo "Skipping notarization (no code signing identity)"
fi

echo ""
echo "================================================"
echo "Installing to UPM Package"
echo "================================================"

rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}"

cp -R "${BUNDLE_DIR}" "${OUTPUT_DIR}/"

# Create Unity meta file
cat > "${OUTPUT_DIR}/libpeer.bundle.meta" << 'EOF'
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
        Exclude Android: 1
        Exclude Editor: 0
        Exclude Linux64: 1
        Exclude OSXUniversal: 0
        Exclude Win: 1
        Exclude Win64: 1
        Exclude iOS: 1
  - first:
      Any:
    second:
      enabled: 0
      settings: {}
  - first:
      Editor: Editor
    second:
      enabled: 1
      settings:
        CPU: AnyCPU
        DefaultValueInitialized: true
        OS: OSX
  - first:
      Standalone: OSXUniversal
    second:
      enabled: 1
      settings:
        CPU: AnyCPU
  userData:
  assetBundleName:
  assetBundleVariant:
EOF

echo ""
echo "================================================"
echo "Build Complete!"
echo "================================================"
echo ""
echo "Output: ${OUTPUT_DIR}/libpeer.bundle"
echo ""
echo "Bundle info:"
echo "  Size: $(du -sh "${OUTPUT_DIR}/libpeer.bundle" | cut -f1)"
echo "  Archs: $(lipo -info "${OUTPUT_DIR}/libpeer.bundle/Contents/MacOS/libpeer" 2>/dev/null | sed 's/.*://')"
if [ -n "$CODESIGN_IDENTITY" ]; then
    echo "  Signed: Yes"
    if [ -n "$NOTARIZE_PROFILE" ]; then
        echo "  Notarized: Yes"
    else
        echo "  Notarized: No"
    fi
else
    echo "  Signed: Ad-hoc only"
fi
echo ""
echo "Note: Update GUID in .meta file before committing"
