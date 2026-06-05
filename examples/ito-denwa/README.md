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

## SRAM Budget Diagnosis — Running Opus + TLS + Wi-Fi on RP2350

<div style="text-align:center; width:80%; box-sizing:border-box;">
    <img src="images/stack_layout.svg" style="max-width:70%; height:auto;" />
</div>

RP2350 has two dedicated 4 KB SRAM banks — **SCRATCH_X** and **SCRATCH_Y** — separate from Main RAM (512 KB). The SDK defaults place Core 0's stack in SCRATCH_Y and Core 1's in SCRATCH_X, allowing each core to access its own bank without bus contention.

However, the Opus (SILK fixed-point) encoder heavily uses **VLA (Variable Length Arrays)**, which are allocated on the stack, not the heap. During a single `opus_encode` call the stack grows to a measured peak of **18,912 bytes** (decode side is 4,864 B) — far exceeding the default 4 KB.

To solve this, a **custom linker script `memmap_bigstack.ld`** replaces the SDK default (`memmap_default.ld`) with the following relocations:

- `.stack_dummy` moved from SCRATCH_Y → **RAM** section (places Core 0's stack in Main RAM top)
- `.stack1_dummy` moved from SCRATCH_X → **SCRATCH_Y** (evacuates Core 1's stack)
- `__StackTop` set to `ORIGIN(SCRATCH_X) + LENGTH(SCRATCH_X)` = 0x20081000 (annexes SCRATCH_X)
- `__HeapLimit = __StackBottom` explicitly defined (pins the heap ceiling to the stack bottom)

This extends Core 0's stack to **20 KB spanning Main RAM top (0x2007c000) through the end of SCRATCH_X (0x20081000)**. Because these regions are physically contiguous they can be annexed as a single block, and the 4 KB from SCRATCH_X adds to the stack without reducing the heap. Core 1 gives up SCRATCH_X and moves to **SCRATCH_Y (4 KB)** — Core 1 does not run Opus, and the cyw43 / lwIP / mbedTLS call chains are shallow (their heavy buffer allocations come from `malloc` on the shared Main RAM heap), so a 4 KB stack is sufficient.

### Three probes that determined the SRAM budget

On an MCU with no MMU and no OS-level stack guard pages, a stack overflow silently corrupts the heap or return addresses — the symptom is a hang or a bizarre crash far from the actual cause. RP2350 has no hardware watchpoint support for memory ranges. The following three probes were combined to converge on the SRAM budget.

#### A. `opus_encoder_get_size` / `opus_decoder_get_size` — static Opus state sizing

`opus_encoder_get_size(1)` and `opus_decoder_get_size(1)` are printed at init to reveal the contiguous memory size required for the Opus encoder/decoder state. When using `opus_encoder_create()` / `opus_decoder_create()`, this state is internally `malloc`'d — making it a **fixed heap-side cost** (alternatively, `opus_encoder_init()` / `opus_decoder_init()` allow placing the state in a static region or custom arena). This firmware uses `create`, so one encoder alone takes ~11 KB from the heap — before growing the stack, you must first confirm the heap can absorb this.

#### B. `sbrk(0)` heap break monitoring — TLS / Wi-Fi / malloc headroom

`stackdiag_heap_report()` reads `sbrk(0)` (the current heap break) and reports how much room remains before the heap collides with `__StackBottom` (= `__HeapLimit`). This probe detects the **reverse failure mode: growing the stack too far kills the heap**.

In this custom linker script (`memmap_bigstack.ld`), `__HeapLimit = __StackBottom` is intentionally defined so that the portion of Core 0's stack that extends into Main RAM eats into the heap 1:1 (however, the 4 KB annexed from SCRATCH_X adds to the stack without reducing the heap). Raising `PICO_STACK_SIZE` pushes `__StackBottom` down, shrinking the heap ceiling. mbedTLS's `altcp_tls_new` alone needs a ~16 KB record buffer via `malloc` — when that runs out, Wi-Fi silently dies. When `0x6000` (24 KB) was tried, this probe showed the break-to-limit gap had shrunk below 8 KB, pinpointing the OOM cause.

Note: `PICO_MALLOC_PANIC` (SDK default: ON) panics when `malloc` returns NULL, but `sbrk(0)` is read-only — **the probe itself cannot trigger an OOM crash**.

#### C. Canary-paint high-water scan — runtime VLA / call stack peak measurement

A canary-paint high-water mark technique built into `main.c`:

1. **Paint** — At boot, while the stack is still shallow, `stackdiag_paint()` fills the entire stack region (and a 16 KB probe window below `__StackBottom` into the heap gap) with a known canary word (`0xC5C5C5C5`). A 256 B guard below the live SP prevents overwriting the current frame.

2. **Scan** — Every 2 seconds during the main loop, `stackdiag_hiwater()` scans upward from the floor until it finds the first non-canary word. The distance from `__StackTop` to that point is the peak usage so far — covering both `opus_encode` and `opus_decode` peaks across the entire run.

3. **Overflow detection** — If the high-water mark exceeds the stack region size, the overflow depth ("spill") is reported along with a hex dump of the 8 deepest words. Flash return addresses (`0x10xxxxxx`) vs RAM pointers (`0x20xxxxxx`) vs sample-like values fingerprint whether the corruption came from a real call chain or a rogue DMA write.

This probe measured the `opus_encode` peak at **18,912 bytes**.

#### Conclusion: 20 KB derived from three constraints

| Probe | What it measures | Value obtained |
|---|---|---|
| A. `opus_*_get_size` | Heap fixed cost | encoder ~11 KB, decoder ~8 KB |
| B. `sbrk(0)` | Heap headroom | ~8 KB remaining at net-ready |
| C. canary-paint | Runtime stack peak | `opus_encode` 18,912 B / `opus_decode` 4,864 B |

- C shows the stack needs at least 18,912 B + headroom
- B shows `__StackBottom` cannot move any lower without starving TLS
- A confirms the heap-side fixed costs are already accounted for

→ Annexing SCRATCH_X to reach **20 KB without reducing the heap** is the only layout that satisfies all three constraints simultaneously.

### Stabilization — what went wrong along the way

On an MCU without MMU or stack guard pages, a stack overflow does not segfault — it silently corrupts adjacent memory. The symptom is never "stack overflow"; it is "audio stopped", "Wi-Fi died", or "the device hung for no apparent reason". Every step toward the final 20 KB layout was a blind experiment where the cause and the symptom were far apart.

**Phase 1 — SCRATCH_Y 4 KB (SDK default)**

The initial firmware ran `opus_decode` on Core 0 with the SDK-default 4 KB stack in SCRATCH_Y. SILK's VLA consumed ~3 KB, and together with `pcm[480]` (960 B on stack at the time) and the call chain, it silently overflowed. The `handle_core0_notify` dispatcher's return address was corrupted — it never returned, permanently wedging the IC FIFO. Core 1's `forward_pump` hit `ic_send_avail == 0` on every call and stopped shipping audio. The only visible symptom: **audio stayed silent**, with no error, no crash, no log.

Diagnosis required adding the canary-paint instrumentation, which revealed the stack was pegged at 4,096/4,096 B — clearly overflowing without any way to see how far.

**Phase 2 — Main RAM 16 KB (`PICO_STACK_SIZE=0x4000`)**

Custom linker script `memmap_bigstack.ld` relocated Core 0's stack to Main RAM top, giving it 16 KB. `pcm[]` was also moved to `static` to remove 960 B from the stack. `opus_decode` stabilized — canary showed ~4,864 B peak, well within 16 KB.

Then push-to-talk mic capture was added, bringing `opus_encode` onto Core 0. The canary immediately pegged at 16,384/16,384 — the encode peak exceeded the region. But since the canary only measures within the region, the true depth was unknown.

**Phase 3 — Main RAM 24 KB (`PICO_STACK_SIZE=0x6000`): Wi-Fi died**

Raising the stack to 24 KB should have solved the overflow. It did — but `__StackBottom` moved down by 8 KB, shrinking the heap by the same amount. mbedTLS's `altcp_tls_new` needs a ~16 KB record buffer via `malloc`; with the heap ceiling lowered, the TLS handshake OOM'd and Wi-Fi silently failed to connect. `PICO_MALLOC_PANIC` (SDK default: ON) would have panic'd — but only if `malloc` returned NULL; lwIP's internal allocator path doesn't always trigger it.

The `stackdiag_heap_report` probe showed the heap break was already within 8 KB of `__HeapLimit` at net-ready — confirming there was no room to give.

**Phase 4 — Main RAM 16 KB + SCRATCH_X annexed = 20 KB (`PICO_STACK_SIZE=0x5000`)**

The insight: Main RAM top (0x20080000) and SCRATCH_X (0x20080000–0x20081000) are physically contiguous. By moving Core 1's stack from SCRATCH_X to SCRATCH_Y and claiming SCRATCH_X as the top 4 KB of Core 0's stack, the linker produces a 20 KB region (0x207c000–0x20081000) without moving `__StackBottom` down at all — **zero heap cost**.

Canary scan with the extended probe window (16 KB below `__StackBottom`) finally revealed the true peak: **18,912 B** for `opus_encode`. 20 KB leaves ~1.5 KB headroom. Heap report confirmed the break stayed well below the limit. Wi-Fi and TLS remained stable.

**Other memory sizing traps encountered along the way:**

| What | Symptom | Root cause | Fix |
|---|---|---|---|
| IC ring 64 KB × 2 | cJSON OOM on 70 KB lab list | 128 KB BSS starved heap | Reduced to 32 KB × 2 |
| `g_https_resp` 128 KB | `altcp_tls_new` OOM | +188 KB BSS pushed heap past limit | Tuned to fit within BSS budget |
| `pcm[480]` on stack | Silent audio hang | 960 B + VLA exceeded SCRATCH_Y | Moved to `static` |
| `opus_encoder_create` | Heap pressure | ~11 KB heap alloc per encoder | Accepted; verified with `sbrk(0)` probe |

The general pattern: on a 520 KB MCU, **stack, heap, and BSS are a zero-sum game**. Fixing one overflow creates another unless all three are measured simultaneously. The canary-paint + heap-break + BSS-size triple probe was not optional — it was the only way to navigate the constraint space without trial-and-error in the dark.

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
