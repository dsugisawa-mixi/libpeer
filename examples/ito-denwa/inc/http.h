// HTTPS client (altcp_tls).
//
// Layered as a thin transport: owns the connection state machine, request
// buffer, response buffer, header/body parsing, and chunked transfer-encoding
// decoder. Application-level concerns (URL paths, JSON parsing, TTS audio
// pumping) live in main.c and reach into the response via the extern globals
// declared below.
//
// Two callback hooks let the transport notify the application without
// pulling app code into http.c:
//   - http_on_tts_headers_done(): fired once per HM_TTS request the moment
//     response headers parse, so the audio engine can be armed before the
//     body finishes streaming.
//   - http_on_recv_overflow(): fired from inside the lwIP recv callback when
//     accepting the next pbuf would overflow g_https_resp; the implementer
//     should compact already-consumed body bytes and slide the rest down.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lwip/altcp.h"
#include "lwip/ip_addr.h"

#define HTTPS_PORT          443
// Response body holds either /api/tunnel/info (~70KB JSON) or one TTS reply
// (raw PCM, ~48 KB/s @ 24kHz mono16 → up to ~3.3s of audio at 160 KB).
#define RESP_BUF_SIZE       (160 * 1024)
// Big enough for HTTP POST headers + a few KB of UTF-8 message body.
#define REQ_BUF_SIZE        4096
#define HTTPS_TIMEOUT_MS    20000
// altcp_poll interval is in 500ms units; 4 = 2s
#define HTTPS_POLL_INTERVAL 4

typedef enum {
    HC_IDLE,
    HC_CONNECTING,
    HC_REQUESTING,
    HC_DONE_OK,
    HC_DONE_ERR,
} https_state_t;

// What kind of request is currently in flight (or just finished).
typedef enum {
    HM_INFO,      // GET /api/tunnel/info  → lab list
    HM_TIMELINE,  // GET /api/qa/timeline?...  → JSON
    HM_TTS,       // POST /api/tts/generate_stream  → raw PCM body
} https_mode_t;

// Chunked transfer-encoding decoder state (used when the server returns
// Transfer-Encoding: chunked, which uvicorn's StreamingResponse does).
// Decoded body bytes are compacted in-place inside g_https_resp[body_start...]
// so the downstream PCM pump can treat g_chunked_write_pos as the "decoded
// body length" without caring about framing.
typedef enum {
    CHUNK_NEED_SIZE,     // accumulating hex digits of chunk size
    CHUNK_SIZE_SAW_CR,   // got \r, expecting \n
    CHUNK_DATA,          // copying chunk data bytes
    CHUNK_DATA_SAW_CR,   // got \r after data, expecting \n
    CHUNK_DONE,          // 0-size chunk seen → stream finished
} chunk_state_t;

// === Transport globals ====================================================
// These are owned by http.c (it does all the writes during connect/recv) but
// readable by application pumps (TTS forwarder, JSON parsers, status logs).
extern volatile https_state_t g_https_state;
extern volatile https_mode_t  g_https_mode;
extern uint32_t               g_https_state_at;
extern char                   g_https_host[128];
extern char                   g_https_resp[RESP_BUF_SIZE];
extern volatile size_t        g_https_resp_len;
extern volatile bool          g_https_headers_done;
extern volatile size_t        g_https_body_start;
extern volatile int           g_https_content_len;   // -1 = unknown
extern volatile uint32_t      g_tts_sample_rate;     // parsed from X-Sample-Rate

extern bool          g_https_chunked;
extern size_t        g_chunked_read_pos;     // offset (from body_start) into raw chunked stream
extern size_t        g_chunked_write_pos;    // offset where decoded PCM bytes have been written
extern chunk_state_t g_chunked_state;

// === Public API ===========================================================
// Set the connection state and stamp g_https_state_at with the current ms.
void http_set_state(https_state_t s);

// Tear down the in-flight pcb (if any). Safe to call from any state.
void http_cleanup(void);

// Build "METHOD path HTTP/1.1\r\n..." into the internal request buffer.
// extra_headers (each line ending in \r\n) is appended verbatim. If body
// is non-NULL, Content-Type and Content-Length headers are added
// automatically and the body is appended after the header terminator.
// Returns 0 on success, -1 if the request would exceed REQ_BUF_SIZE.
int http_build_request(const char *method,
                       const char *path,
                       const char *extra_headers,
                       const char *content_type,
                       const char *body,
                       size_t      body_len);

// Reset response/decoder state, create a fresh TLS pcb, and kick off a
// connect to ip:HTTPS_PORT with SNI=host. The previously-built request body
// is flushed once the TCP/TLS handshake completes. Returns 0 on success,
// -1 if pcb allocation or altcp_connect failed.
int http_request_start(const ip_addr_t *ip, const char *host);

// Decode chunked transfer-encoding in-place inside g_https_resp[body_start...].
// Reads from g_chunked_read_pos, writes decoded PCM bytes to g_chunked_write_pos.
// Both offsets are relative to body_start. Resumes where the previous call
// left off; called from the TTS forward pump every iteration.
void http_chunked_decode_in_place(void);

// Locate the body separator ("\r\n\r\n") and return a pointer past it, or
// NULL if not yet present in the buffer.
const char *http_find_body(const char *resp, size_t len);

// Case-insensitive header lookup; returns a pointer to the value (past ':'
// and any leading whitespace) or NULL if not found.
const char *http_find_header(const char *resp, size_t len, const char *name);

// === Application hooks (implemented by main.c) ============================
// Called once when response headers are parsed in HM_TTS mode. The
// implementer reads X-Sample-Rate / Transfer-Encoding from the response and
// arms the audio engine so playback can start before the body finishes.
void http_on_tts_headers_done(void);

// Called from inside the lwIP recv callback when accepting the next pbuf
// would overflow g_https_resp. The implementer should compact already-
// consumed body bytes (e.g. forward the lwIP/async_context lock is already
// held — do NOT take it again).
void http_on_recv_overflow(void);
