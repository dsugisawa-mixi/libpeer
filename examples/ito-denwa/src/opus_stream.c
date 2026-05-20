#include "opus_stream.h"

#include <stdio.h>
#include <string.h>

#include "opus.h"

#define OPUS_SR  24000

static OpusDecoder *g_decoder = NULL;

int opus_stream_init(void) {
    if (g_decoder) return 0;
    int err = OPUS_OK;
    g_decoder = opus_decoder_create(OPUS_SR, 1, &err);
    if (!g_decoder || err != OPUS_OK) {
        printf("[opus] decoder_create failed err=%d\n", err);
        g_decoder = NULL;
        return -1;
    }
    printf("[opus] decoder ready (24kHz mono, fixed-point)\n");
    return 0;
}

void opus_stream_reset(void) {
    if (g_decoder) {
        opus_decoder_ctl(g_decoder, OPUS_RESET_STATE);
    }
}

int opus_stream_decode_frame(const uint8_t *packet, size_t packet_len,
                              int16_t *pcm_out, int pcm_max) {
    if (!g_decoder && opus_stream_init() != 0) return -1;
    int n = opus_decode(g_decoder, packet, (int32_t)packet_len,
                        pcm_out, pcm_max, 0);
    if (n < 0) {
        printf("[opus] decode err=%d (pkt=%u)\n", n, (unsigned)packet_len);
    }
    return n;
}
