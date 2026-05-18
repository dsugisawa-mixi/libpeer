#include "tts.h"
#include "common.h"

#include <stdio.h>
#include <string.h>
#include "pico/cyw43_arch.h"

#include "audio.h"
#include "http.h"
#include "ic_ring.h"
#include "queue.h"
#include "gui.h"      // g_tts_play_active

#define TTS_PATH  "/api/tts/generate_stream"

//=============================================================================
// Shared state (externed in tts.h)
//=============================================================================
uint8_t           g_core0_pcm_pending[TTS_PCM_PENDING_CAP];
volatile size_t   g_core0_pcm_pending_len = 0;
volatile bool     g_core0_tts_stream_done = false;
volatile size_t   g_tts_play_pos = 0;

//=============================================================================
// Private state
//=============================================================================
static size_t g_tts_pad_remaining = 0;
// core1-private guard: set true after we ship IC_MSG_TTS_END so the
// forward pump doesn't re-send. Reset on each new playback start.
static bool   g_core1_tts_end_sent = false;

void tts_send_end_if_needed(void) {
    if (!g_core1_tts_end_sent) {
        ic_send(IC_MSG_TTS_END, NULL, 0);
        g_core1_tts_end_sent = true;
    }
}

#define TTS_COMPACT_THRESHOLD  2048

//=============================================================================
// Playback arm (called from http.c after TTS headers parse)
//=============================================================================
void tts_start_playback(void) {
    if (!g_https_headers_done) {
        printf("[tts] start_playback called before headers_done — ignoring\n");
        return;
    }
    audio_set_sample_rate(g_tts_sample_rate);
    audio_stream_reset();
    g_tts_play_pos      = 0;
    g_tts_pad_remaining = 0;
    g_core0_pcm_pending_len  = 0;
    g_core0_tts_stream_done  = false;
    g_core1_tts_end_sent     = false;
    mem_barrier();
    g_tts_play_active = true;
    size_t body_have = g_https_resp_len - g_https_body_start;
    printf("[tts] playback armed: body@%u resp_len=%u initial_body=%u Hz=%u\n",
           (unsigned)g_https_body_start, (unsigned)g_https_resp_len,
           (unsigned)body_have, (unsigned)g_tts_sample_rate);
}

//=============================================================================
// JSON escape + TTS request builder
//=============================================================================
static int json_escape(char *dst, size_t dst_sz, const char *src) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        unsigned char c = *p;
        const char *esc = NULL;
        char tmp[8];
        switch (c) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\b': esc = "\\b";  break;
            case '\f': esc = "\\f";  break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default:
                if (c < 0x20) {
                    snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    esc = tmp;
                }
                break;
        }
        if (esc) {
            size_t n = strlen(esc);
            if (o + n >= dst_sz) return -1;
            memcpy(dst + o, esc, n);
            o += n;
        } else {
            if (o + 1 >= dst_sz) return -1;
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
    return (int)o;
}

int tts_kick_request(const char *message, const ip_addr_t *ip,
                     const char *host, const char *device_id) {
    g_https_mode = HM_TTS;

    char body[TTS_MSG_LEN * 2 + 96];
    char escaped[TTS_MSG_LEN * 2];
    if (json_escape(escaped, sizeof(escaped), message) < 0) {
        printf("[tts] escape overflow\n");
        return -1;
    }
    int blen = snprintf(body, sizeof(body),
                        "{\"gender\":\"male\",\"style\":\"neutral\","
                        "\"out_lang\":\"ja\",\"text\":\"%s\"}",
                        escaped);
    if (blen < 0 || (size_t)blen >= sizeof(body)) {
        printf("[tts] body overflow\n");
        return -1;
    }
    printf("[tts] POST body (%d B): %s\n", blen, body);

    // Header line is built at runtime now (used to be a string-literal concat
    // with a build-time DEVICE_ID macro). info/timeline already do the same.
    char hdr[160];
    snprintf(hdr, sizeof(hdr),
             "X-Device-Id: %s\r\n"
             "X-Sample-Rate: 24000\r\n",
             device_id ? device_id : "");
    if (http_build_request("POST", TTS_PATH, hdr,
                            "application/json",
                            body, (size_t)blen) != 0) return -1;
    return http_request_start(ip, host);
}

//=============================================================================
// Response buffer compaction (pump-side, acquires lwIP lock)
//=============================================================================
static void tts_resp_compact(void) {
    if (g_tts_play_pos < TTS_COMPACT_THRESHOLD) return;
    cyw43_arch_lwip_begin();
    http_resp_compact_locked();
    cyw43_arch_lwip_end();
}

//=============================================================================
// Core0: audio ring drain
//=============================================================================
bool tts_play_pump(void) {
    (void)audio_stream_buffered();

    if (g_tts_play_active) audio_stream_underrun_recover();

    if (g_core0_pcm_pending_len >= 2) {
        size_t samples = g_core0_pcm_pending_len / 2;
        const int16_t *src = (const int16_t *)g_core0_pcm_pending;
        size_t w = audio_stream_write_mono16(src, samples);
        if (w > 0) {
            size_t consumed = w * 2;
            size_t remain   = g_core0_pcm_pending_len - consumed;
            if (remain > 0) memmove(g_core0_pcm_pending,
                                    g_core0_pcm_pending + consumed, remain);
            g_core0_pcm_pending_len = remain;
        }
    }

    bool stream_done    = g_core0_tts_stream_done;
    bool source_drained = (g_core0_pcm_pending_len < 2);

    if (stream_done && source_drained && g_tts_pad_remaining == 0
        && g_tts_play_active) {
        g_tts_pad_remaining = 2304;
    }

    if (g_tts_pad_remaining > 0) {
        static const int16_t zero_block[128] = {0};
        size_t want = g_tts_pad_remaining < 128 ? g_tts_pad_remaining : 128;
        size_t w = audio_stream_write_mono16(zero_block, want);
        g_tts_pad_remaining -= w;
    }

    if (stream_done && source_drained && g_tts_pad_remaining == 0
        && g_tts_play_active) {
        printf("[tts] playback done (pending drained, pad written)\n");
        g_tts_play_active        = false;
        g_core0_tts_stream_done  = false;
        ic_send(IC_MSG_TTS_PLAYED, NULL, 0);
        return false;
    }
    return g_tts_play_active;
}

//=============================================================================
// Core1: PCM forwarder (g_https_resp → IC ring → core0)
//=============================================================================
bool tts_forward_pump(void) {
    if (!g_tts_play_active)       return false;
    if (g_core1_tts_end_sent)     return false;
    if (!g_https_headers_done)    return true;

    if (g_https_chunked) http_chunked_decode_in_place();

    size_t body_have = g_https_chunked
                         ? g_chunked_write_pos
                         : (g_https_resp_len - g_https_body_start);
    size_t bytes_left = body_have - g_tts_play_pos;

    if (bytes_left >= 2) {
        size_t n = bytes_left;
        if (n > TTS_FWD_CHUNK_SIZE) n = TTS_FWD_CHUNK_SIZE;
        n &= ~1u;

        const uint8_t *src = (const uint8_t *)g_https_resp + g_https_body_start
                             + g_tts_play_pos;
        ic_send(IC_MSG_TTS_PCM_CHUNK, src, (uint16_t)n);
        g_tts_play_pos += n;

        tts_resp_compact();

        static uint32_t last_log = 0;
        uint32_t now = board_millis();
        if ((now - last_log) >= 250) {
            last_log = now;
            printf("[tts] fwd pos=%u/%u (+%u bytes)\n",
                   (unsigned)g_tts_play_pos, (unsigned)body_have, (unsigned)n);
        }
    }

    bool stream_done    = g_https_chunked
                            ? (g_chunked_state == CHUNK_DONE)
                            : (g_https_state == HC_IDLE);
    bool source_drained = (body_have - g_tts_play_pos) < 2;

    if (stream_done && source_drained) {
        printf("[tts] fwd done: %u bytes forwarded\n", (unsigned)g_tts_play_pos);
        ic_send(IC_MSG_TTS_END, NULL, 0);
        g_core1_tts_end_sent = true;
        return false;
    }
    return true;
}
