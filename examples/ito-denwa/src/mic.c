#include "mic.h"
#include "common.h"
#include "ic_ring.h"

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "i2s_in.pio.h"
#include "opus.h"

#include <stdio.h>
#include <string.h>

//=============================================================================
// Wiring (Waveshare PicoLCD-1.3 user-button pins, hijacked while recording):
//   BCK  GP16   (also Key-Left  — non-functional during capture)
//   LRCK GP20   (also Key-Right — non-functional during capture)
//   SD   GP21   (also Button-Y / PROV_BUTTON_PIN — read at boot before init,
//                fine; non-functional once capture starts)
// mic_stop() restores them to GPIO input + pull-up so the buttons work again.
//=============================================================================
#define MIC_PIO          pio0       // pio2 is owned by CYW43+audio; use pio0
#define MIC_BCK_PIN      16
#define MIC_LRCK_PIN     20
#define MIC_SD_PIN       21

//=============================================================================
// Opus encoder config — lowest reasonable voice quality
//   - VOIP application, SILK-only at 16 kHz
//   - 8 kbps CBR → ~20 bytes per 20 ms frame
//   - complexity 0 → cheapest CPU
//=============================================================================
#define MIC_OPUS_BITRATE     8000
#define MIC_OPUS_COMPLEXITY  0
#define MIC_OPUS_MAX_PKT     128    // generous upper bound for 8 kbps × 20 ms

//=============================================================================
// PIO RX ring (DMA writes 32-bit words here, two per stereo frame).
// 1024 words = 4 KB; ~32 ms cushion @ 16 kHz stereo capture, easily covers
// the main loop's 1 ms tick.
//=============================================================================
#define MIC_RING_WORDS   1024
#define MIC_RING_BYTES   (MIC_RING_WORDS * 4)
#define MIC_RING_BITS    12         // log2(MIC_RING_BYTES)

static uint32_t __attribute__((aligned(MIC_RING_BYTES))) g_mic_ring[MIC_RING_WORDS];

static int          g_mic_sm        = -1;
static int          g_mic_dma_ch    = -1;
static uint         g_mic_pio_off   = 0;
static OpusEncoder *g_mic_enc       = NULL;
static volatile bool g_mic_active   = false;

// PCM accumulator: PIO produces stereo words; we collect 16-bit mono samples
// (left channel only) into pcm_acc[] until we have a full 20 ms Opus frame.
static int16_t  g_mic_pcm_acc[MIC_FRAME_SAMPLES];
static size_t   g_mic_pcm_acc_len   = 0;

// DMA write-position tracking. Same trick as audio.c: derive an absolute word
// counter from the modulo write_addr + a software wrap count, so we can tell
// "ring empty" from "DMA lapped the reader".
static uint32_t g_mic_read_abs      = 0;
static uint32_t g_mic_last_ring_pos = 0;
static uint32_t g_mic_produced_abs  = 0;

// Per-recording PCM-level diagnostics. CBR Opus hides silence-vs-speech, so
// inspect the raw samples directly to tell a live mic from a dead/stuck line:
//   peak ~0                  → no signal (mic unpowered / SD not wired)
//   and==or (single value)   → line stuck at a constant (e.g. floating high)
//   wide min..max + variance  → real audio
static int32_t  g_mic_dbg_min   = 0;
static int32_t  g_mic_dbg_max   = 0;
static uint32_t g_mic_dbg_or    = 0;       // OR of raw 16-bit samples
static uint32_t g_mic_dbg_and   = 0xFFFF;  // AND of raw 16-bit samples
static uint32_t g_mic_dbg_n     = 0;       // samples seen
static uint64_t g_mic_dbg_absum = 0;       // sum of |sample| → mean level

bool mic_is_active(void) { return g_mic_active; }

//=============================================================================
// Ring helpers
//=============================================================================
static inline uint32_t mic_dma_write_ring_pos(void) {
    uintptr_t base = (uintptr_t)g_mic_ring;
    uintptr_t wa   = (uintptr_t)dma_hw->ch[g_mic_dma_ch].write_addr;
    uint32_t off_words = (uint32_t)((wa - base) >> 2);
    return off_words & (MIC_RING_WORDS - 1);
}

static inline uint32_t mic_dma_produced_abs(void) {
    uint32_t now = mic_dma_write_ring_pos();
    uint32_t delta = (now - g_mic_last_ring_pos) & (MIC_RING_WORDS - 1);
    g_mic_produced_abs += delta;
    g_mic_last_ring_pos = now;
    return g_mic_produced_abs;
}

//=============================================================================
// Init
//=============================================================================
int mic_init(void) {
    if (g_mic_enc) return 0;
    printf("[mic] init begin\n");

    memset(g_mic_ring, 0, sizeof g_mic_ring);

    g_mic_sm      = pio_claim_unused_sm(MIC_PIO, true);
    g_mic_pio_off = pio_add_program(MIC_PIO, &i2s_in_program);
    g_mic_dma_ch  = dma_claim_unused_channel(true);

    int err = OPUS_OK;
    g_mic_enc = opus_encoder_create(MIC_SAMPLE_RATE_HZ, 1,
                                    OPUS_APPLICATION_VOIP, &err);
    if (!g_mic_enc || err != OPUS_OK) {
        printf("[mic] opus_encoder_create failed err=%d\n", err);
        g_mic_enc = NULL;
        return -1;
    }
    opus_encoder_ctl(g_mic_enc, OPUS_SET_BITRATE(MIC_OPUS_BITRATE));
    opus_encoder_ctl(g_mic_enc, OPUS_SET_VBR(0));                    // CBR
    opus_encoder_ctl(g_mic_enc, OPUS_SET_COMPLEXITY(MIC_OPUS_COMPLEXITY));
    opus_encoder_ctl(g_mic_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(g_mic_enc, OPUS_SET_INBAND_FEC(0));
    opus_encoder_ctl(g_mic_enc, OPUS_SET_DTX(0));
    opus_encoder_ctl(g_mic_enc, OPUS_SET_FORCE_CHANNELS(1));
    opus_encoder_ctl(g_mic_enc, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND));

    printf("[mic] ready: pio=%p sm=%d dma_ch=%d enc=%p (16k mono VOIP %d bps)\n",
           (void*)MIC_PIO, g_mic_sm, g_mic_dma_ch, (void*)g_mic_enc,
           MIC_OPUS_BITRATE);
    return 0;
}

//=============================================================================
// Start / stop
//=============================================================================
// PIO cycles per stereo frame: two channels × (4 setup + 64 loop) = 136.
// Must match the i2s_in.pio program layout exactly or the WS rate drifts off
// 16 kHz.
#define MIC_FRAME_CYCLES  136.0f

static void mic_apply_clkdiv(void) {
    // Frame = 136 PIO cycles → PIO clock = 16000 × 136 = 2.176 MHz @ 16 kHz.
    float sys_hz = (float)clock_get_hz(clk_sys);
    float div    = sys_hz / ((float)MIC_SAMPLE_RATE_HZ * MIC_FRAME_CYCLES);
    pio_sm_set_clkdiv(MIC_PIO, g_mic_sm, div);
    pio_sm_clkdiv_restart(MIC_PIO, g_mic_sm);
    printf("[mic] clk_sys=%.0f div=%.3f → PIO=%.0fHz BCK~%.0fHz frame=%.0fHz\n",
           (double)sys_hz, (double)div, (double)(sys_hz / div),
           (double)(sys_hz / div / 2.0f),
           (double)(sys_hz / div / MIC_FRAME_CYCLES));
}

void mic_start(void) {
    if (!g_mic_enc) {
        printf("[mic] start: not initialized\n");
        return;
    }
    if (g_mic_active) return;

    // Reset encoder + accumulator
    opus_encoder_ctl(g_mic_enc, OPUS_RESET_STATE);
    g_mic_pcm_acc_len   = 0;

    // Reset PCM diagnostics for this recording.
    g_mic_dbg_min   = 32767;
    g_mic_dbg_max   = -32768;
    g_mic_dbg_or    = 0;
    g_mic_dbg_and   = 0xFFFF;
    g_mic_dbg_n     = 0;
    g_mic_dbg_absum = 0;

    // Configure pins as PIO function. i2s_in_program_init also sets pindirs.
    i2s_in_program_init(MIC_PIO, g_mic_sm, g_mic_pio_off,
                        MIC_BCK_PIN, MIC_LRCK_PIN, MIC_SD_PIN);
    mic_apply_clkdiv();

    // Wipe ring and reset progress trackers BEFORE arming DMA so the first
    // mic_dma_write_ring_pos() reads a known baseline.
    memset(g_mic_ring, 0, sizeof g_mic_ring);
    g_mic_read_abs      = 0;
    g_mic_last_ring_pos = 0;
    g_mic_produced_abs  = 0;

    // DMA: read PIO RX FIFO into ring, ENDLESS write-side ring wrap.
    dma_channel_config cfg = dma_channel_get_default_config(g_mic_dma_ch);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, pio_get_dreq(MIC_PIO, g_mic_sm, false));
    channel_config_set_ring(&cfg, /*write=*/true, MIC_RING_BITS);
    dma_channel_configure(
        g_mic_dma_ch, &cfg,
        g_mic_ring,
        &MIC_PIO->rxf[g_mic_sm],
        dma_encode_endless_transfer_count(),
        /*start=*/true);

    pio_sm_clear_fifos(MIC_PIO, g_mic_sm);
    pio_sm_restart(MIC_PIO, g_mic_sm);
    pio_sm_set_enabled(MIC_PIO, g_mic_sm, true);

    mem_barrier();
    g_mic_active = true;
    printf("[mic] START\n");
}

void mic_stop(void) {
    if (!g_mic_active) return;
    g_mic_active = false;
    mem_barrier();

    pio_sm_set_enabled(MIC_PIO, g_mic_sm, false);
    dma_channel_abort(g_mic_dma_ch);

    // Restore GP16/20/21 to plain GPIO + pull-up so the Waveshare buttons
    // (Key-Left, Key-Right, Button-Y) work again.
    const uint8_t pins[] = { MIC_BCK_PIN, MIC_LRCK_PIN, MIC_SD_PIN };
    for (size_t i = 0; i < sizeof pins; i++) {
        gpio_set_function(pins[i], GPIO_FUNC_SIO);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_up(pins[i]);
    }
    g_mic_pcm_acc_len = 0;

    // Report the captured PCM level so we can tell a live mic from a dead line.
    uint32_t mean = g_mic_dbg_n ? (uint32_t)(g_mic_dbg_absum / g_mic_dbg_n) : 0;
    printf("[mic] STOP — pcm: n=%lu min=%ld max=%ld mean|s|=%lu or=%04lx and=%04lx%s\n",
           (unsigned long)g_mic_dbg_n,
           (long)g_mic_dbg_min, (long)g_mic_dbg_max,
           (unsigned long)mean,
           (unsigned long)(g_mic_dbg_or  & 0xFFFF),
           (unsigned long)(g_mic_dbg_and & 0xFFFF),
           (g_mic_dbg_max - g_mic_dbg_min < 64) ? "  <-- FLAT/NO SIGNAL" : "");
}

//=============================================================================
// Pump: drain DMA ring, encode 20 ms frames, ship via IC ring
//=============================================================================
static void mic_ship_frame(void) {
    uint8_t pkt[MIC_OPUS_MAX_PKT];
    opus_int32 n = opus_encode(g_mic_enc, g_mic_pcm_acc,
                                MIC_FRAME_SAMPLES, pkt, sizeof pkt);
    if (n <= 0) {
        printf("[mic] opus_encode err=%ld (frame skipped)\n", (long)n);
        return;
    }
    // ic_send_avail covers both ring space AND notification FIFO; if either
    // is full, drop this frame rather than stall core0 — losing 20 ms of mic
    // audio is better than wedging the IC bus.
    uint32_t need = 4u + ((uint32_t)((n + 3) & ~3));
    if (ic_send_avail() < need) {
        static uint32_t last_drop_log = 0;
        uint32_t now = board_millis();
        if ((now - last_drop_log) >= 500) {
            last_drop_log = now;
            printf("[mic] ship-block: need=%u avail=%u — frame dropped\n",
                   (unsigned)need, (unsigned)ic_send_avail());
        }
        return;
    }
    ic_send(IC_MSG_LINEPHONE_OPUS_PKT, pkt, (uint16_t)n);
}

void mic_pump(void) {
    if (!g_mic_active) return;

    uint32_t produced = mic_dma_produced_abs();
    while (g_mic_read_abs + 2u <= produced) {
        // Consume one stereo frame (2 words: left, right). PIO autopush is
        // MSB-first (shift_left), so each word holds a left-justified 16-bit
        // sample in bits [31:16] — extract that as the signed PCM.
        uint32_t left_word = g_mic_ring[g_mic_read_abs       & (MIC_RING_WORDS - 1)];
        // right_word is discarded (L/R tied to GND → right slot is zero).
        (void)            g_mic_ring[(g_mic_read_abs + 1)    & (MIC_RING_WORDS - 1)];
        g_mic_read_abs += 2;

        int16_t s = (int16_t)(left_word >> 16);

        // PCM-level diagnostics (see g_mic_dbg_* declarations).
        uint32_t raw = (uint32_t)(uint16_t)s;
        g_mic_dbg_or  |= raw;
        g_mic_dbg_and &= raw;
        if (s < g_mic_dbg_min) g_mic_dbg_min = s;
        if (s > g_mic_dbg_max) g_mic_dbg_max = s;
        g_mic_dbg_absum += (uint32_t)(s < 0 ? -s : s);
        g_mic_dbg_n++;

        g_mic_pcm_acc[g_mic_pcm_acc_len++] = s;
        if (g_mic_pcm_acc_len == MIC_FRAME_SAMPLES) {
            mic_ship_frame();
            g_mic_pcm_acc_len = 0;
        }
    }
}
