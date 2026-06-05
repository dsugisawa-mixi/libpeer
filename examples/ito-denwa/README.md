# Pico 2 W Realtime Opus Audio Streaming PoC

> Running I2S MIC + I2S DAC + LCD + CYW43 Wi-Fi simultaneously on RP2350 / Pico 2 W,
> streaming Opus-compressed audio to and from the cloud as a low-bandwidth realtime voice endpoint PoC.

Even on an educational, low-cost MCU, combining PIO I2S, DMA, dual-core separation, and Opus compression makes it possible to build a cloud-connected realtime audio endpoint — this firmware proves it.

---

## What this PoC proves

| Demonstrated | Detail |
|---|---|
| **I2S microphone input** | PIO-generated I2S protocol for realtime audio capture on-MCU |
| **I2S DAC/speaker output** | PIO + DMA ENDLESS ring for low-latency playback |
| **LCD status UI** | ST7789 1.3" SPI LCD for status display and controls |
| **CYW43 Wi-Fi always-on** | Maintaining TLS connection while running audio processing in parallel |
| **HTTPS cloud POST / polling** | Audio send and receive over HTTPS |
| **Opus compressed payloads** | 16–24 kHz mono Opus instead of raw PCM for drastic bandwidth reduction |
| **Dual-core full separation** | Audio/UI and Network separated without mutex, connected via lock-free SPSC ring |
| **BLE provisioning** | Zero-touch Wi-Fi setup from an iOS app |

**In short:** A single RP2350-class ($7) MCU runs Opus / TLS / Wi-Fi / I2S Audio concurrently — a cloud voice endpoint without a smartphone or PC.

---

## Why Pico 2 W makes this hard

- **520 KB RAM** — TLS buffers, Opus decoder, audio ring, and Wi-Fi stack all coexist
- **Concurrent Wi-Fi and Audio** — cyw43_arch_poll and I2S DMA must keep running without stalling
- **I2S via PIO** — RP2350 has no I2S peripheral; PIO programs generate the protocol
- **Opus decoding** — SILK fixed-point decoder's VLA consumes ~3 KB of stack, overflowing the default 4 KB
- **Back-pressure control** — Flow control spanning cloud → TCP → ring buffer → DMA, built from scratch


## System Topology

<div style="text-align:center; width:80%; box-sizing:border-box;">
    <img src="images/radio_mix_talk_architecture.svg" style="max-width:70%; height:auto;" />
</div>

Three zones, fully separated.

**Cloud (minimum)** — Device registration, WSS tunnel relay, and downstream Opus audio relay only. No GPU, no CDN cache.

**Publisher (maximum)** — Dynamically registers custom services such as radio / MIC / music. Mixing and encoding happen entirely on the Publisher side; the cloud just passes packets through.

**MCU + Speaker** — RP2350 + CYW43 maintains a persistent TLS connection to the cloud over Wi-Fi, decoding Opus packets on-chip via the tunnel and playing them through the I2S DAC.


## Dual-Core Architecture

<div style="text-align:center; width:80%; box-sizing:border-box;">
    <img src="images/core_architecture.svg" style="max-width:70%; height:auto;" />
</div>

The two RP2350 cores are fully separated, forming the audio pipeline without any mutex.

| | Core 0 (20 KB stack) | Core 1 (4 KB stack) |
|---|---|---|
| **Role** | Audio / UI / Opus Encode & Decode | Network / Transport / Forwarding |
| **Stack location** | Main RAM top + SCRATCH_X (annexed) | SCRATCH_Y |
| **Heavy work** | opus_encode (SILK VLA peak 18.9 KB) | TLS handshake / cyw43_arch_poll |
| **Loop cadence** | 1 ms | 500 μs |
| **IC direction** | Consumer (audio data) | Producer (opus pkt / PCM chunk) |

Inter-core communication uses a **64 KB SPSC ring buffer + HW FIFO notify (8 slots)**. Back-pressure cascades automatically: audio ring fill 75% → IC dequeue stop → IC ring fill → ic_send_avail==0 → forward_pump skip → g_https_resp fill → recv_cb ERR_MEM → TCP window close.

<div style="page-break-before: always;"></div>

## Audio Pipeline

Upstream and downstream have **independent DMA rings with asymmetric size, direction, and buffering strategy**.

<div style="text-align:center; width:80%; box-sizing:border-box;">
    <img src="images/dma_asymmetry.png" style="max-width:70%; height:auto;" />
</div>

Downstream uses a large DMA ring (32 KB) to absorb jitter for realtime continuous playback. Upstream keeps the ring minimal (4 KB) and buffers on the encoder + accumulator side for per-utterance batch transmission. Different purposes — no reason to make them symmetric.

---

## Memory Layout

<div style="text-align:center; width:80%; box-sizing:border-box;">
    <img src="images/stack_layout.svg" style="max-width:70%; height:auto;" />
</div>

RP2350 has two dedicated 4 KB SRAM banks — **SCRATCH_X** and **SCRATCH_Y** — separate from Main RAM (512 KB). The SDK defaults place Core 0's stack in SCRATCH_Y and Core 1's in SCRATCH_X, allowing each core to access its own bank without bus contention.

However, the Opus (SILK fixed-point) encoder heavily uses **VLA (Variable Length Arrays)**, which are allocated on the stack, not the heap. During a single `opus_encode` call the stack grows to a measured peak of **18,912 bytes** (decode side is 4,864 B) — far exceeding the default 4 KB.

To solve this, Core 0's stack is extended to **20 KB spanning Main RAM top (0x2007c000) through the end of SCRATCH_X (0x20081000)**. Because these regions are physically contiguous they can be annexed as a single block at zero heap cost. The heap ceiling is set by `__StackBottom`. Core 1 gives up SCRATCH_X and moves to **SCRATCH_Y (4 KB)** — sufficient for TLS / Wi-Fi processing alone.

### How the stack peak was measured

On an MCU with no MMU and no OS-level stack guard pages, a stack overflow silently corrupts the heap or return addresses — the symptom is a hang or a bizarre crash far from the actual cause. Measuring the true peak is essential before choosing a stack size, but RP2350 has no hardware watchpoint support for memory ranges. The firmware uses a **canary-paint high-water mark** technique built into `main.c`:

1. **Paint** — At boot, while the stack is still shallow, `stackdiag_paint()` fills the entire stack region (and a 16 KB probe window below `__StackBottom` into the heap gap) with a known canary word (`0xC5C5C5C5`). A 256 B guard below the live SP prevents overwriting the current frame.

2. **Scan** — Every 2 seconds during the main loop, `stackdiag_hiwater()` scans upward from the floor until it finds the first non-canary word. The distance from `__StackTop` to that point is the peak usage so far — covering both `opus_encode` and `opus_decode` peaks across the entire run.

3. **Overflow detection** — If the high-water mark exceeds the stack region size, the overflow depth ("spill") is reported along with a hex dump of the 8 deepest words. Flash return addresses (`0x10xxxxxx`) vs RAM pointers (`0x20xxxxxx`) vs sample-like values fingerprint whether the corruption came from a real call chain or a rogue DMA write.

4. **Heap probe** — `stackdiag_heap_report()` reads `sbrk(0)` (the current heap break) and reports how much room remains before the heap collides with `__StackBottom`. This catches the opposite failure mode: raising `PICO_STACK_SIZE` too far shrinks the heap 1:1, and mbedTLS's `altcp_tls_new` alone needs a ~16 KB record buffer — `0x6000` (24 KB) stack broke Wi-Fi because the heap ran out.

5. **Static size probes** — `opus_encoder_get_size(1)` and `opus_decoder_get_size(1)` are printed at init to show the heap-allocated state size (separate from the VLA stack cost).

The combination of (A) static size probes, (B) heap break monitoring, and (C) canary-paint high-water scanning made it possible to converge on 20 KB: the smallest size that fits `opus_encode`'s 18,912 B measured peak while leaving enough heap for TLS.

---

## Bandwidth Reduction

Comparing raw PCM vs Opus compression:

| | PCM 16 kHz 16-bit mono | Opus 16 kbps |
|---|---|---|
| **Bitrate** | 256 kbps | 16 kbps |
| **Reduction** | — | **93.75%** |
| **Per minute** | 1.92 MB | 120 KB |

Given CYW43's Wi-Fi throughput and RP2350's processing headroom, Opus compression is not a nice-to-have — it is **essential for stable always-on connectivity**.

---

## Technical Stack

| Layer | What | Why |
|---|---|---|
| MCU | RP2350 dual-core 200 MHz | $7 class, realtime Opus decoding capable |
| Wi-Fi | CYW43 (Pico 2 W) | Always-on TLS |
| TLS | mbedTLS | HTTPS / WSS |
| Codec (decode) | Opus (SILK fixed-point) 24 kHz mono | Cloud → MCU streaming playback |
| Codec (encode) | Opus (SILK fixed-point) 16 kHz mono 16 kbps | MCU → Cloud push-to-talk upload |
| Audio Out | PIO I2S → PCM5101A DAC | DMA ring 32 KB, ENDLESS mode |
| Audio In | PIO I2S RX ← INMP441 MEMS mic | DMA ring 8 KB, Opus encode on Core 0 |
| IC Bus | SPSC ring + HW FIFO | lock-free, zero-copy |
| Provisioning | BLE (BTstack) | Zero-touch setup via iOS app |
| Display | ST7789 1.3" LCD | Status display + control UI |

---

## Wiring

<div style="text-align:center; width:80%; box-sizing:border-box;">
    <img src="images/rp2350-wired_bb.png" style="max-width:70%; height:auto;" />
</div>

---

## Building

### Prerequisites

- [Pico SDK](https://github.com/raspberrypi/pico-sdk) (≥ 1.3.0)
- ARM GCC toolchain (`arm-none-eabi-gcc`)
- CMake ≥ 3.12

### Build

```bash
cd cmake.rp2350
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

---

## Dependencies

- [libopus](https://opus-codec.org/) — Opus audio decoding (SILK fixed-point)
- [cJSON](https://github.com/DaveGamble/cJSON) — JSON parsing
