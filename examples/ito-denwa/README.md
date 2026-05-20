# Ito-Denwa — String Phone Device

A WiFi-enabled communication device shaped like a traditional Japanese string telephone (糸電話). All electronics are housed inside a paper cup form factor, providing children with a safe, screen-free way to talk with friends and family.

## Background

Growing concern over children's smartphone dependency is driving demand for screen-free alternatives worldwide. In the US, a Seattle startup called **Tin Can Antechnologies** launched a $100 WiFi-enabled "landline" phone for kids in April 2025. With no large-scale marketing — only word-of-mouth — the device sold hundreds of thousands of units in its first year, fueled by parents' desire to delay smartphone adoption and a wave of nostalgia among Gen X and Millennial parents. Schools have emerged as one of the fastest-growing sales channels, with thousands of administrators exploring bulk deployments to curb early social media dependency.

Ito-Denwa takes this concept further by adopting the form factor of a Japanese string telephone (糸電話) — a paper cup that children speak into and hold to their ear, just like the real thing. Where Tin Can emulates a retro landline, Ito-Denwa leans into the tactile, analog warmth of a toy that every child recognizes.

## Concept

Key design principles:

- **Screen-free**: No touchscreen, no social media, no internet browsing
- **Simple operation**: Hold to mouth to talk, hold to ear to listen
- **Parental control**: Contact management and usage monitoring via a companion smartphone app
- **Safety first**: Only communicates with registered contacts; no location tracking

## Architecture

The firmware runs on an **RP2350 (Raspberry Pi Pico 2 W)** using a dual-core cooperative architecture:

| Core | Responsibility |
|------|---------------|
| Core 0 | Audio playback (I2S/PIO/DMA), LCD rendering (ST7789), button input, PCM ring drain |
| Core 1 | WiFi networking, HTTPS/TLS communication, timeline polling, TTS streaming, Opus decoding |

Cores communicate via a lock-free SPSC ring buffer (`ic_ring`) with hardware multicore FIFO notifications. Each direction has its own 64 KB ring; the FIFO carries a 32-bit descriptor per message (type + offset).

### Boot Sequence

1. Core 0 initializes UART, LCD, and buttons.
2. **BLE provisioning check** — if Button-Y is held at boot or no credentials exist in flash, the device enters BLE provisioning mode (single-core, no WiFi). A companion app writes SSID, password, device ID, and codec preference via GATT characteristics. On commit the credentials are persisted to the last flash sector (CRC-protected) and the device reboots.
3. Normal boot: Core 1 is launched for WiFi/HTTPS. Core 0 waits for `IC_MSG_NET_READY`, then runs audio init + boot beep + streaming self-test before signaling `IC_MSG_AUDIO_READY` back.
4. Both cores enter their main run loops — Core 1 drives HTTPS requests, TTS queue processing, and timeline polling; Core 0 drains PCM, renders the LCD, and handles button input.

### System Flow

![Architecture](docs/architecture.svg)

## Hardware

| Component | Detail |
|-----------|--------|
| CPU | RP2350 (Raspberry Pi Pico 2 W) |
| Audio | PicoAudio — I2S via PIO, DMA-driven ring buffer |
| Display | PicoLCD 1.3" (ST7789, 240×240, SPI) for status indication |
| Connectivity | WiFi 2.4 GHz (CYW43 on Pico 2 W) |
| Battery | Li-Po 1000 mAh (est. 6–8 hrs) |
| Charging | USB-C |
| Size | Paper cup form factor (~φ75 mm × 90 mm, ~120 g) |

Additional components inside the cup: microphone, speaker, proximity sensor (mouth/ear detection).

## Building

### Prerequisites

- [Pico SDK](https://github.com/raspberrypi/pico-sdk) (≥ 1.3.0)
- ARM GCC toolchain (`arm-none-eabi-gcc`)
- CMake ≥ 3.12

### Environment

Set the following environment variables (or edit `.envrc`):

```bash
export PICO_SDK_PATH="$HOME/git/pico-sdk"
export PICO_TOOLCHAIN_PATH="$HOME/git/gcc-arm-none-eabi-10.3-2021.10/bin"
export PICO_BOARD=pico2_w

# Optional — defaults to the CloudFront distribution URL baked into main.c
export API_URL="https://your-api-server.example.com"
```

WiFi SSID, password, device ID, and codec preference are **not** build-time settings — they are provisioned at runtime via BLE and stored in on-board flash. See [Device Provisioning](#device-provisioning) below.

### Build

```bash
cd cmake.rp2350
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

The output `.uf2` file can be flashed to the Pico 2 W via USB mass-storage mode.

## Device Provisioning

Credentials are provisioned over **BLE** using a companion smartphone app and persisted to the last sector of on-board flash (magic + version + CRC32 integrity check).

### Entering Provisioning Mode

- **First boot** (blank flash) — provisioning starts automatically.
- **Manual** — hold **Button-Y** (GPIO 21) while powering on.

### GATT Characteristics

The BLE peripheral exposes a custom GATT service. The companion app writes each field, then writes `0x01` to the Commit characteristic to finalize:

| Characteristic | Max Length | Description |
|----------------|-----------|-------------|
| SSID | 32 bytes | WiFi access point name (UTF-8) |
| Password | 64 bytes | WiFi passphrase (UTF-8) |
| Device ID | 64 bytes | Server-issued device UUID |
| Opus Enable | 1 byte | `0x01` to request Opus codec for TTS |
| Commit | 1 byte | Write `0x01` to save and reboot |

On successful commit the device saves credentials to flash and reboots into normal WiFi/HTTPS operation.

## Source Structure

```
examples/ito-denwa/
├── main.c                  # Application entry, dual-core state machines
├── cmake.rp2350/
│   ├── CMakeLists.txt      # Build configuration for RP2350
│   └── pico_sdk_import.cmake
├── inc/
│   ├── audio.h             # I2S audio playback API (PIO/DMA ring)
│   ├── ble_provision.h     # BLE WiFi-provisioning GATT service
│   ├── btstack_config.h    # BTstack BLE stack configuration
│   ├── common.h            # Shared utility macros and inline functions
│   ├── credentials.h       # Flash credential persistence (CRC-protected)
│   ├── gui.h               # LCD GUI, status logging, button handling
│   ├── http.h              # HTTPS client, connection state machine
│   ├── ic_ring.h           # Inter-core SPSC ring buffer (64 KB per direction)
│   ├── led.h               # Non-blocking LED blink state machine
│   ├── lwipopts.h          # lwIP stack configuration
│   ├── mbedtls_config.h    # mbedTLS configuration
│   ├── opus_stream.h       # Opus packet decoder (24 kHz mono, 20 ms frames)
│   ├── queue.h             # TTS message FIFO ring (SPSC, with gender)
│   ├── st7789.h            # LCD driver API
│   ├── tts.h               # TTS streaming pipeline (raw PCM + Opus)
│   └── wifi.h              # WiFi initialization and DNS resolution
└── src/
    ├── audio.c             # PIO I2S + DMA audio engine (4096-frame ring)
    ├── ble_provision.c     # BLE GATT server for credential provisioning
    ├── credentials.c       # Flash read/write/erase for wifi_creds_t
    ├── gui.c               # LCD display UI and view management
    ├── http.c              # HTTPS communication, chunked TE decoder
    ├── i2s.pio             # PIO program for I2S output
    ├── ic_ring.c           # Inter-core message bus implementation
    ├── led.c               # LED blink state machine
    ├── opus_stream.c       # Opus decoder wrapper (libopus)
    ├── queue.c             # Thread-safe circular TTS message queue
    ├── st7789.c            # ST7789 SPI LCD driver
    ├── tts.c               # TTS request building, PCM forwarding & playback
    └── wifi.c              # WiFi connectivity setup
```

## Dependencies

- [Pico SDK](https://github.com/raspberrypi/pico-sdk) — WiFi, lwIP, mbedTLS, PIO, DMA
- [BTstack](https://github.com/bluekitchen/btstack) — BLE GATT server (bundled with Pico SDK)
- [libopus](https://opus-codec.org/) — Opus audio decoding for compressed TTS streaming
- [cJSON](https://github.com/DaveGamble/cJSON) — JSON parsing (from `third_party/cJSON/`)
