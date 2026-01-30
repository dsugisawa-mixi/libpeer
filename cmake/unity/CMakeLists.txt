# Unity Plugin Build Configuration for libpeer
# Usage:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<toolchain> -DUNITY_PLATFORM=<platform>
#   cmake --build build --target unity_plugin

cmake_minimum_required(VERSION 3.16)
project(libpeer_unity C)

# Unity platform detection
set(UNITY_PLATFORM "" CACHE STRING "Target Unity platform: iOS, Android, macOS, Windows, Linux")

# Validate platform
if(NOT UNITY_PLATFORM)
    if(IOS)
        set(UNITY_PLATFORM "iOS")
    elseif(ANDROID)
        set(UNITY_PLATFORM "Android")
    elseif(APPLE)
        set(UNITY_PLATFORM "macOS")
    elseif(WIN32)
        set(UNITY_PLATFORM "Windows")
    elseif(UNIX)
        set(UNITY_PLATFORM "Linux")
    else()
        message(FATAL_ERROR "UNITY_PLATFORM not specified and cannot be auto-detected")
    endif()
endif()

message(STATUS "Building libpeer Unity plugin for: ${UNITY_PLATFORM}")

# Common settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Optimization for release
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -DNDEBUG")
set(CMAKE_C_FLAGS_MINSIZEREL "${CMAKE_C_FLAGS_MINSIZEREL} -Os -DNDEBUG")

# Source directory (relative to repository root)
set(LIBPEER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(LIBPEER_SRC "${LIBPEER_ROOT}/src")
set(LIBPEER_INCLUDE "${LIBPEER_ROOT}/include")
set(LIBPEER_THIRD_PARTY "${LIBPEER_ROOT}/third_party")

# Include third-party build helpers
include(${LIBPEER_THIRD_PARTY}/coreHTTP/httpFilePaths.cmake)
include(${LIBPEER_THIRD_PARTY}/coreMQTT/mqttFilePaths.cmake)

# Collect source files
file(GLOB LIBPEER_SOURCES "${LIBPEER_SRC}/*.c")

# Platform-specific configuration
if(UNITY_PLATFORM STREQUAL "iOS")
    # iOS: Static library only
    set(UNITY_LIB_TYPE STATIC)
    set(UNITY_OUTPUT_NAME "peer")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0")
    add_definitions(-DUNITY_IOS=1)

elseif(UNITY_PLATFORM STREQUAL "Android")
    # Android: Shared library
    set(UNITY_LIB_TYPE SHARED)
    set(UNITY_OUTPUT_NAME "peer")
    add_definitions(-DUNITY_ANDROID=1)
    add_definitions(-DANDROID=1)
    # Enable crypto extension for mbedtls AESCE on arm64
    if(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=armv8-a+crypto")
    endif()

elseif(UNITY_PLATFORM STREQUAL "macOS")
    # macOS: Bundle (MODULE library)
    set(UNITY_LIB_TYPE MODULE)
    set(UNITY_OUTPUT_NAME "peer")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15")
    add_definitions(-DUNITY_STANDALONE_OSX=1)

elseif(UNITY_PLATFORM STREQUAL "Windows")
    # Windows: DLL
    set(UNITY_LIB_TYPE SHARED)
    set(UNITY_OUTPUT_NAME "peer")
    add_definitions(-DUNITY_STANDALONE_WIN=1)
    add_definitions(-DWIN32_LEAN_AND_MEAN)

elseif(UNITY_PLATFORM STREQUAL "Linux")
    # Linux: Shared library
    set(UNITY_LIB_TYPE SHARED)
    set(UNITY_OUTPUT_NAME "peer")
    add_definitions(-DUNITY_STANDALONE_LINUX=1)

else()
    message(FATAL_ERROR "Unsupported UNITY_PLATFORM: ${UNITY_PLATFORM}")
endif()

# Common definitions
add_definitions(
    -DHTTP_DO_NOT_USE_CUSTOM_CONFIG
    -DMQTT_DO_NOT_USE_CUSTOM_CONFIG
)

# Dependencies output directory
set(DEPS_INSTALL_DIR "${CMAKE_BINARY_DIR}/deps")
include_directories(${DEPS_INSTALL_DIR}/include ${DEPS_INSTALL_DIR}/include/cjson)
link_directories(${DEPS_INSTALL_DIR}/lib)

# Build third-party dependencies
include(ExternalProject)

# Common ExternalProject arguments for cross-compilation
set(EXTERNAL_PROJECT_C_FLAGS "-fPIC")
if(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    set(EXTERNAL_PROJECT_C_FLAGS "${EXTERNAL_PROJECT_C_FLAGS} -march=armv8-a+crypto")
endif()

set(EXTERNAL_PROJECT_CMAKE_ARGS
    -DCMAKE_C_FLAGS=${EXTERNAL_PROJECT_C_FLAGS}
    -DCMAKE_INSTALL_PREFIX=${DEPS_INSTALL_DIR}
    -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
    -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
    -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
    -DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}
    -DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}
    -DCMAKE_ANDROID_ARCH_ABI=${CMAKE_ANDROID_ARCH_ABI}
    -DANDROID_ABI=${ANDROID_ABI}
    -DANDROID_PLATFORM=${ANDROID_PLATFORM}
    -DANDROID_STL=${ANDROID_STL}
)

ExternalProject_Add(cjson_ext
    SOURCE_DIR ${LIBPEER_THIRD_PARTY}/cJSON
    CMAKE_ARGS
        ${EXTERNAL_PROJECT_CMAKE_ARGS}
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        -DBUILD_SHARED_LIBS=OFF
        -DENABLE_CJSON_TEST=OFF
)

ExternalProject_Add(mbedtls_ext
    SOURCE_DIR ${LIBPEER_THIRD_PARTY}/mbedtls
    CMAKE_ARGS
        ${EXTERNAL_PROJECT_CMAKE_ARGS}
        -DMBEDTLS_FATAL_WARNINGS=OFF
        -DENABLE_TESTING=OFF
        -DENABLE_PROGRAMS=OFF
)

# Enable DTLS-SRTP in mbedtls
file(READ ${LIBPEER_THIRD_PARTY}/mbedtls/include/mbedtls/mbedtls_config.h MBEDTLS_CONFIG)
string(REPLACE "//#define MBEDTLS_SSL_DTLS_SRTP" "#define MBEDTLS_SSL_DTLS_SRTP" MBEDTLS_CONFIG "${MBEDTLS_CONFIG}")
file(WRITE ${LIBPEER_THIRD_PARTY}/mbedtls/include/mbedtls/mbedtls_config.h "${MBEDTLS_CONFIG}")

ExternalProject_Add(srtp2_ext
    SOURCE_DIR ${LIBPEER_THIRD_PARTY}/libsrtp
    CMAKE_ARGS
        ${EXTERNAL_PROJECT_CMAKE_ARGS}
        -DTEST_APPS=OFF
)

ExternalProject_Add(usrsctp_ext
    SOURCE_DIR ${LIBPEER_THIRD_PARTY}/usrsctp
    CMAKE_ARGS
        ${EXTERNAL_PROJECT_CMAKE_ARGS}
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        -Dsctp_build_programs=OFF
)

# Create the Unity plugin library
add_library(unity_plugin ${UNITY_LIB_TYPE}
    ${LIBPEER_SOURCES}
    ${HTTP_SOURCES}
    ${MQTT_SOURCES}
    ${MQTT_SERIALIZER_SOURCES}
)

set_target_properties(unity_plugin PROPERTIES
    OUTPUT_NAME ${UNITY_OUTPUT_NAME}
    PREFIX "lib"
)

target_include_directories(unity_plugin PRIVATE
    ${LIBPEER_INCLUDE}
    ${LIBPEER_SRC}
    ${HTTP_INCLUDE_PUBLIC_DIRS}
    ${MQTT_INCLUDE_PUBLIC_DIRS}
)

add_dependencies(unity_plugin cjson_ext mbedtls_ext srtp2_ext usrsctp_ext)

# Link dependencies
set(DEP_LIBS srtp2 usrsctp mbedtls mbedcrypto mbedx509 cjson)

if(UNITY_PLATFORM STREQUAL "iOS")
    # iOS: Static library, dependencies linked later by Unity
    # Create fat static library target
    add_custom_target(unity_fat_library
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/output
        COMMAND bash ${LIBPEER_ROOT}/scripts/create_fat_library.sh
            ${CMAKE_BINARY_DIR}
            ${CMAKE_BINARY_DIR}/output/libpeer.a
        DEPENDS unity_plugin
        COMMENT "Creating iOS fat static library"
    )

elseif(UNITY_PLATFORM STREQUAL "Android")
    target_link_libraries(unity_plugin PRIVATE
        ${DEPS_INSTALL_DIR}/lib/libsrtp2.a
        ${DEPS_INSTALL_DIR}/lib/libusrsctp.a
        ${DEPS_INSTALL_DIR}/lib/libmbedtls.a
        ${DEPS_INSTALL_DIR}/lib/libmbedcrypto.a
        ${DEPS_INSTALL_DIR}/lib/libmbedx509.a
        ${DEPS_INSTALL_DIR}/lib/libcjson.a
        log
    )

elseif(UNITY_PLATFORM STREQUAL "macOS")
    # macOS bundle settings
    set_target_properties(unity_plugin PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "bundle"
        MACOSX_BUNDLE_GUI_IDENTIFIER "jp.co.mixi.libpeer"
    )
    target_link_libraries(unity_plugin PRIVATE
        ${DEPS_INSTALL_DIR}/lib/libsrtp2.a
        ${DEPS_INSTALL_DIR}/lib/libusrsctp.a
        ${DEPS_INSTALL_DIR}/lib/libmbedtls.a
        ${DEPS_INSTALL_DIR}/lib/libmbedcrypto.a
        ${DEPS_INSTALL_DIR}/lib/libmbedx509.a
        ${DEPS_INSTALL_DIR}/lib/libcjson.a
        "-framework Foundation"
        "-framework Security"
    )

else()
    # Linux/Windows
    target_link_libraries(unity_plugin PRIVATE
        ${DEPS_INSTALL_DIR}/lib/libsrtp2.a
        ${DEPS_INSTALL_DIR}/lib/libusrsctp.a
        ${DEPS_INSTALL_DIR}/lib/libmbedtls.a
        ${DEPS_INSTALL_DIR}/lib/libmbedcrypto.a
        ${DEPS_INSTALL_DIR}/lib/libmbedx509.a
        ${DEPS_INSTALL_DIR}/lib/libcjson.a
    )
    if(UNIX)
        target_link_libraries(unity_plugin PRIVATE pthread)
    endif()
endif()

# Installation target for UPM package
set(UPM_OUTPUT_DIR "${LIBPEER_ROOT}/upm/jp.co.mixi.libpeer/Plugins")

install(TARGETS unity_plugin
    LIBRARY DESTINATION ${UPM_OUTPUT_DIR}/${UNITY_PLATFORM}
    ARCHIVE DESTINATION ${UPM_OUTPUT_DIR}/${UNITY_PLATFORM}
    RUNTIME DESTINATION ${UPM_OUTPUT_DIR}/${UNITY_PLATFORM}
    BUNDLE DESTINATION ${UPM_OUTPUT_DIR}/${UNITY_PLATFORM}
)
