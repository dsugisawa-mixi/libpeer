// Opus packet decoder for streaming TTS (24 kHz mono, 20 ms frames).
//
// Wire format on the HTTPS body (server-side: request JSON has "opus": true):
//
//     for each packet:
//         [u16 big-endian length L][L bytes of opus packet]
//
// Decoded PCM is shipped to core0 over the IC ring as IC_MSG_TTS_PCM_CHUNK,
// identical to the raw-PCM path — so audio.c / tts_play_pump are unchanged.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 20 ms @ 24 kHz mono.
#define OPUS_STREAM_FRAME_SAMPLES  480
// Defensive upper bound on a single opus packet. RFC 6716 allows up to
// 1275 bytes per packet; at 24 kbps / 20 ms we expect ~60 B average, so
// anything beyond this is almost certainly a framing desync.
#define OPUS_STREAM_MAX_PACKET     1500

// One-shot decoder bring-up. Idempotent. Returns 0 on success, -1 on failure.
int  opus_stream_init(void);

// Forget all stateful decoder context (PLC history, etc.). Call at the
// start of each new TTS playback.
void opus_stream_reset(void);

// Decode a single opus packet. Returns the number of decoded samples
// (mono int16), or a negative opus error code on failure.
int  opus_stream_decode_frame(const uint8_t *packet, size_t packet_len,
                              int16_t *pcm_out, int pcm_max);
