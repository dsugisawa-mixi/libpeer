CMAKE_MINIMUM_REQUIRED(VERSION 3.12)

SET(PRJ rp2350bm-audio)
SET(PICO_BOARD pico2_w)
SET(HOME $ENV{HOME})
SET(PICO_SDK_FETCH_FROM_GIT on)
INCLUDE(pico_sdk_import.cmake)

PROJECT(${PRJ} C CXX ASM)
SET(CMAKE_C_STANDARD 11)
SET(CMAKE_CXX_STANDARD 17)

# pico_sdk_init must be called early, right after project()
pico_sdk_init()
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -g")
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -g")

# Include stdint.h/stddef.h for C/CXX only (not for assembler)
add_compile_options(
  "$<$<COMPILE_LANGUAGE:C,CXX>:-includestdint.h>"
  "$<$<COMPILE_LANGUAGE:C,CXX>:-includestddef.h>"
)

# Fix for pico-sdk runtime.c missing CLOCKS_PER_SEC
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DCLOCKS_PER_SEC=1000000")
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DCLOCKS_PER_SEC=1000000")

# Configuration from environment variables
message(STATUS "=== RP2350 Build Configuration ===")
if(DEFINED ENV{API_URL})
  message(STATUS "API_URL: $ENV{API_URL}")
  add_definitions(-DAPI_URL="$ENV{API_URL}")
else()
  message(STATUS "API_URL: (not set, using default)")
endif()
if(DEFINED ENV{WIFI_SSID})
  message(STATUS "WIFI_SSID: $ENV{WIFI_SSID}")
  add_definitions(-DWIFI_SSID="$ENV{WIFI_SSID}")
else()
  message(STATUS "WIFI_SSID: (not set, using default)")
endif()
if(DEFINED ENV{WIFI_PASSWORD})
  message(STATUS "WIFI_PASSWORD: (set)")
  add_definitions(-DWIFI_PASSWORD="$ENV{WIFI_PASSWORD}")
else()
  message(STATUS "WIFI_PASSWORD: (not set)")
endif()
if(DEFINED ENV{DEVICE_ID})
  message(STATUS "DEVICE_ID: $ENV{DEVICE_ID}")
  add_definitions(-DDEVICE_ID="$ENV{DEVICE_ID}")
else()
  message(STATUS "DEVICE_ID: (not set, using default)")
endif()
message(STATUS "==================================")

IF (PICO_SDK_VERSION_STRING VERSION_LESS "1.3.0")
    message(FATAL_ERROR "Raspberry Pi Pico SDK version 1.3.0 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
ENDIF()
SET(CMAKE_INCLUDE_CURRENT_DIR ON)

INCLUDE_DIRECTORIES(".")

INCLUDE_DIRECTORIES(
  ${CMAKE_CURRENT_LIST_DIR}/
  ${CMAKE_CURRENT_LIST_DIR}/inc/
  ${CMAKE_CURRENT_LIST_DIR}/../inc/
  ${CMAKE_BINARY_DIR}/
  ${CMAKE_CURRENT_LIST_DIR}/../../../third_party/cJSON/
)

ADD_EXECUTABLE(
    ${PRJ}
    ${CMAKE_CURRENT_LIST_DIR}/../../../third_party/cJSON/cJSON.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/st7789.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/audio.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/ic_ring.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/http.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/queue.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/led.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/wifi.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/gui.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/tts.c
    ${CMAKE_CURRENT_LIST_DIR}/../main.c
)

pico_generate_pio_header(${PRJ} ${CMAKE_CURRENT_LIST_DIR}/../src/i2s.pio)

PROJECT(${PRJ} VERSION 1.0.0)

TARGET_LINK_LIBRARIES(
    ${PRJ}
    pico_stdlib
    pico_multicore
    pico_time
    hardware_spi
    hardware_timer
    hardware_i2c
    hardware_pio
    hardware_dma
    hardware_clocks
    pico_cyw43_arch_lwip_threadsafe_background
    pico_mbedtls
    pico_lwip_mbedtls
)

pico_enable_stdio_usb(${PRJ} 0)
pico_enable_stdio_uart(${PRJ} 1)

# Explicit UART configuration for RP2350
target_compile_definitions(${PRJ} PRIVATE
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=0
    PICO_DEFAULT_UART_RX_PIN=1
    PICO_DEFAULT_UART_BAUD_RATE=115200
    MBEDTLS_PLATFORM_TIME_ALT
)

pico_add_extra_outputs(${PRJ})

target_compile_options(${PRJ} PRIVATE
    -Wall
    -Wno-format
    -Wno-unused-function
    -Wno-maybe-uninitialized
)
