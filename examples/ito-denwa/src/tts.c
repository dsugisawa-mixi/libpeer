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
#include "opus_stream.h"

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
    opus_stream_reset();
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

int tts_kick_request(const char *message, const char *gender,
                     const ip_addr_t *ip, const char *host,
                     const char *device_id, bool opus_enabled) {
    g_https_mode = HM_TTS;

    char body[TTS_MSG_LEN * 2 + 96];
    char escaped[TTS_MSG_LEN * 2];
    if (json_escape(escaped, sizeof(escaped), message) < 0) {
        printf("[tts] escape overflow\n");
        return -1;
    }
    const char *g = (gender && *gender) ? gender : "male";
    int blen = snprintf(body, sizeof(body),
                        "{\"gender\":\"%s\",\"style\":\"neutral\","
                        "\"out_lang\":\"ja\",\"opus\":%s,\"text\":\"%s\"}",
                        g, opus_enabled ? "true" : "false", escaped);
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
        // Must wipe the FULL DMA ring; otherwise ENDLESS DMA keeps reading
        // the stale PCM tail and we hear a periodic "chi-chi-chi" loop at
        // ring-lap frequency.
        g_tts_pad_remaining = (size_t)BUF_FRAMES + 256u;
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
// Core1: opus stream decoder + PCM forwarder
//
// Body framing (server emits when request JSON has "opus": true):
//   for each 20ms frame: [u16 BE length L][L bytes opus packet]
// Each frame decodes to OPUS_STREAM_FRAME_SAMPLES (480) int16 mono samples
// = 960 PCM bytes, shipped as one IC_MSG_TTS_PCM_CHUNK to core0.
//
// Bound work per call to keep lwIP poll cadence responsive — opus_decode on
// RP2350 M33 + FPU at 153.6 MHz is ~1 ms per frame, so a handful of frames
// per iteration is comfortably under the lwIP poll budget.
//=============================================================================
#define TTS_FWD_MAX_FRAMES_PER_PASS  4

bool tts_forward_pump(void) {
    if (!g_tts_play_active)       return false;
    if (g_core1_tts_end_sent)     return false;
    if (!g_https_headers_done)    return true;

    if (g_https_chunked) http_chunked_decode_in_place();

    size_t body_have = g_https_chunked
                         ? g_chunked_write_pos
                         : (g_https_resp_len - g_https_body_start);

    if (g_https_body_opus) {
        const uint8_t *body = (const uint8_t *)g_https_resp + g_https_body_start;
        int frames_decoded = 0;
        size_t pcm_bytes_shipped = 0;
        while (frames_decoded < TTS_FWD_MAX_FRAMES_PER_PASS
               && g_tts_play_pos + 2 <= body_have) {
            uint16_t L = ((uint16_t)body[g_tts_play_pos]     << 8)
                       |  (uint16_t)body[g_tts_play_pos + 1];

            if (L == 0 || L > OPUS_STREAM_MAX_PACKET) {
                // Either zero-length (server-side framing bug) or absurd —
                // treat as a fatal framing desync. Drop the rest of the body
                // and end playback so we don't loop forever decoding garbage.
                printf("[tts/opus] bad packet length=%u at pos=%u — abort\n",
                       (unsigned)L, (unsigned)g_tts_play_pos);
                g_tts_play_pos = body_have;
                break;
            }
            if (g_tts_play_pos + 2 + L > body_have) break;  // incomplete

            int16_t pcm[OPUS_STREAM_FRAME_SAMPLES];
            int nsamples = opus_stream_decode_frame(
                body + g_tts_play_pos + 2, L,
                pcm, OPUS_STREAM_FRAME_SAMPLES);
            g_tts_play_pos += 2u + L;

            if (nsamples > 0) {
                size_t nbytes = (size_t)nsamples * sizeof(int16_t);
                ic_send(IC_MSG_TTS_PCM_CHUNK, (const uint8_t *)pcm, (uint16_t)nbytes);
                pcm_bytes_shipped += nbytes;
            }
            frames_decoded++;
        }

        if (frames_decoded > 0) tts_resp_compact();

        static uint32_t last_log_opus = 0;
        uint32_t now = board_millis();
        if (frames_decoded > 0 && (now - last_log_opus) >= 250) {
            last_log_opus = now;
            printf("[tts/opus] fwd pos=%u/%u (+%d frames, +%u PCM bytes)\n",
                   (unsigned)g_tts_play_pos, (unsigned)body_have,
                   frames_decoded, (unsigned)pcm_bytes_shipped);
        }
    } else {
        // Raw-PCM forwarder (server sends int16 mono PCM verbatim — opus=false
        // in the request JSON). Forward up to one IC payload per pass; the IC
        // ring's depth handles burst-vs-drain skew.
        size_t bytes_left = body_have - g_tts_play_pos;
        if (bytes_left >= 2) {
            size_t n = bytes_left;
            if (n > TTS_FWD_CHUNK_SIZE) n = TTS_FWD_CHUNK_SIZE;
            n &= ~1u;

            const uint8_t *src = (const uint8_t *)g_https_resp
                                 + g_https_body_start + g_tts_play_pos;
            ic_send(IC_MSG_TTS_PCM_CHUNK, src, (uint16_t)n);
            g_tts_play_pos += n;

            tts_resp_compact();

            static uint32_t last_log_pcm = 0;
            uint32_t now = board_millis();
            if ((now - last_log_pcm) >= 250) {
                last_log_pcm = now;
                printf("[tts/pcm] fwd pos=%u/%u (+%u bytes)\n",
                       (unsigned)g_tts_play_pos, (unsigned)body_have, (unsigned)n);
            }
        }
    }

    bool stream_done    = g_https_chunked
                            ? (g_chunked_state == CHUNK_DONE)
                            : (g_https_state == HC_IDLE);
    bool source_drained = (body_have - g_tts_play_pos) < 2;

    if (stream_done && source_drained) {
        printf("[tts/%s] fwd done: %u body bytes consumed\n",
               g_https_body_opus ? "opus" : "pcm",
               (unsigned)g_tts_play_pos);
        ic_send(IC_MSG_TTS_END, NULL, 0);
        g_core1_tts_end_sent = true;
        return false;
    }
    return true;
}
