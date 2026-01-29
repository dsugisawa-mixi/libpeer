#!/bin/bash
set -e

BUILD_DIR="$1"
OUTPUT_LIB="$2"

WORK_DIR="${BUILD_DIR}/fat_lib_work"
rm -rf "${WORK_DIR}"
mkdir -p "${WORK_DIR}"

# Library list with their prefixes
declare -A LIBS=(
  ["peer"]="${BUILD_DIR}/src/libpeer.a"
  ["usrsctp"]="${BUILD_DIR}/dist/lib/libusrsctp.a"
  ["srtp2"]="${BUILD_DIR}/dist/lib/libsrtp2.a"
  ["mbedtls"]="${BUILD_DIR}/dist/lib/libmbedtls.a"
  ["mbedx509"]="${BUILD_DIR}/dist/lib/libmbedx509.a"
  ["mbedcrypto"]="${BUILD_DIR}/dist/lib/libmbedcrypto.a"
  ["cjson"]="${BUILD_DIR}/dist/lib/libcjson.a"
)

# Extract each library and prefix object files
for prefix in "${!LIBS[@]}"; do
  lib_path="${LIBS[$prefix]}"
  lib_work_dir="${WORK_DIR}/${prefix}"
  mkdir -p "${lib_work_dir}"

  # Extract objects
  (cd "${lib_work_dir}" && ar -x "${lib_path}")

  # Rename objects with prefix
  for obj in "${lib_work_dir}"/*.o; do
    if [ -f "$obj" ]; then
      basename=$(basename "$obj")
      mv "$obj" "${lib_work_dir}/${prefix}_${basename}"
    fi
  done
done

# Create output directory
mkdir -p "$(dirname "${OUTPUT_LIB}")"

# Combine all objects into fat library
ar rcs "${OUTPUT_LIB}" "${WORK_DIR}"/*/*.o

# Cleanup
rm -rf "${WORK_DIR}"

echo "Created: ${OUTPUT_LIB}"
