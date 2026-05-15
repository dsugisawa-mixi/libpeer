// TTS streaming pipeline: request building, PCM forwarding (core1),
// and audio ring draining (core0).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TTS_FWD_CHUNK_SIZE   1024
#define TTS_PCM_PENDING_CAP  4096

// Pending PCM staging buffer — core0 drains via tts_play_pump().
// Written by handle_core0_notify (IC_MSG_TTS_PCM_CHUNK).
extern uint8_t           g_core0_pcm_pending[TTS_PCM_PENDING_CAP];
extern volatile size_t   g_core0_pcm_pending_len;
extern volatile bool     g_core0_tts_stream_done;

// Arm audio streaming playback (called from http.c after headers parse).
void tts_start_playback(void);

// Send IC_MSG_TTS_END if not already sent (error recovery path).
void tts_send_end_if_needed(void);

// Build and send a TTS POST request via the HTTPS client.
// ip/host are the resolved API endpoint.
struct ip_addr;
int tts_kick_request(const char *message, const struct ip_addr *ip, const char *host);

// Core0: drain g_core0_pcm_pending into the audio ring.
// Returns true while playback is in flight.
bool tts_play_pump(void);

// Core1: forward PCM from g_https_resp to core0 via IC ring.
// Returns true while forwarding is in flight.
bool tts_forward_pump(void);
