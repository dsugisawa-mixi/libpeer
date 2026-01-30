#!/bin/bash
#
# Create FAT static library for iOS
# Combines libpeer with all dependencies into a single .a file
#
set -e

BUILD_DIR="$1"
OUTPUT_LIB="$2"

if [ -z "$BUILD_DIR" ] || [ -z "$OUTPUT_LIB" ]; then
    echo "Usage: $0 <build_dir> <output_lib>"
    exit 1
fi

WORK_DIR="${BUILD_DIR}/fat_lib_work"
rm -rf "${WORK_DIR}"
mkdir -p "${WORK_DIR}"

# Find the Unity plugin library
UNITY_LIB=$(find "${BUILD_DIR}" -name "libpeer.a" -path "*/CMakeFiles/*" -prune -o -name "libpeer.a" -print | head -1)
if [ -z "$UNITY_LIB" ]; then
    # Try alternative location
    UNITY_LIB="${BUILD_DIR}/libunity_plugin.a"
fi

# Library paths
declare -A LIBS=(
    ["peer"]="${UNITY_LIB}"
    ["usrsctp"]="${BUILD_DIR}/deps/lib/libusrsctp.a"
    ["srtp2"]="${BUILD_DIR}/deps/lib/libsrtp2.a"
    ["mbedtls"]="${BUILD_DIR}/deps/lib/libmbedtls.a"
    ["mbedx509"]="${BUILD_DIR}/deps/lib/libmbedx509.a"
    ["mbedcrypto"]="${BUILD_DIR}/deps/lib/libmbedcrypto.a"
    ["cjson"]="${BUILD_DIR}/deps/lib/libcjson.a"
)

echo "Creating FAT library from:"

# Extract each library and prefix object files
for prefix in "${!LIBS[@]}"; do
    lib_path="${LIBS[$prefix]}"

    if [ ! -f "$lib_path" ]; then
        echo "  [SKIP] ${prefix}: ${lib_path} (not found)"
        continue
    fi

    echo "  [OK] ${prefix}: ${lib_path}"

    lib_work_dir="${WORK_DIR}/${prefix}"
    mkdir -p "${lib_work_dir}"

    # Extract objects using libtool for iOS compatibility
    (cd "${lib_work_dir}" && ar -x "${lib_path}")

    # Rename objects with prefix to avoid conflicts
    for obj in "${lib_work_dir}"/*.o; do
        if [ -f "$obj" ]; then
            basename=$(basename "$obj")
            mv "$obj" "${lib_work_dir}/${prefix}_${basename}"
        fi
    done
done

# Create output directory
mkdir -p "$(dirname "${OUTPUT_LIB}")"

# Combine all objects into fat library using libtool (better for iOS)
if command -v libtool &> /dev/null; then
    libtool -static -o "${OUTPUT_LIB}" "${WORK_DIR}"/*/*.o
else
    ar rcs "${OUTPUT_LIB}" "${WORK_DIR}"/*/*.o
fi

# Show library info
echo ""
echo "Created: ${OUTPUT_LIB}"
echo "Size: $(du -h "${OUTPUT_LIB}" | cut -f1)"

# Cleanup
rm -rf "${WORK_DIR}"
