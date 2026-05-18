#include "http.h"
#include "common.h"
#include "wifi.h"    // g_api_ip
#include "gui.h"     // g_lab_state, g_lab_ids, g_timeline_status_ver, ...
#include "tts.h"     // tts_start_playback, tts_send_end_if_needed
#include "queue.h"   // tts_queue_push, tts_queue_pop
#include "ic_ring.h" // ic_send, IC_MSG_*
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "pico/stdlib.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/pbuf.h"
#include "mbedtls/ssl.h"
#include "mbedtls/debug.h"

static inline uint32_t http_now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

//=============================================================================
// Globals (extern in http.h — readable by application pumps)
//=============================================================================
volatile https_state_t g_https_state        = HC_IDLE;
volatile https_mode_t  g_https_mode         = HM_INFO;
uint32_t               g_https_state_at     = 0;
char                   g_https_host[128]    = {0};
char                   g_https_resp[RESP_BUF_SIZE];
volatile size_t        g_https_resp_len     = 0;
volatile bool          g_https_headers_done = false;
volatile size_t        g_https_body_start   = 0;
volatile int           g_https_content_len  = -1;
volatile uint32_t      g_tts_sample_rate    = 24000;

bool          g_https_chunked     = false;
size_t        g_chunked_read_pos  = 0;
size_t        g_chunked_write_pos = 0;
chunk_state_t g_chunked_state     = CHUNK_NEED_SIZE;

//=============================================================================
// Internal state
//=============================================================================
static char                     g_https_req[REQ_BUF_SIZE];
static size_t                   g_https_req_len = 0;
static struct altcp_pcb        *g_https_pcb     = NULL;
static struct altcp_tls_config *g_tls_cfg       = NULL;

// Internal-only chunked decoder accumulators.
static uint32_t g_chunked_size_acc  = 0;
static uint32_t g_chunked_remaining = 0;
static bool     g_chunked_in_ext    = false;  // in ";extension" tail of size line

//=============================================================================
// mbedtls debug printer (wired up via mbedtls_ssl_conf_dbg if needed)
//=============================================================================
static void mbedtls_debug_print(void *ctx, int level, const char *file, int line, const char *str) {
    (void)ctx;
    // strip trailing newline that mbedtls includes
    size_t n = strlen(str);
    while (n > 0 && (str[n-1] == '\n' || str[n-1] == '\r')) n--;
    // strip the long path prefix from file
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    printf("[mbedtls L%d %s:%d] %.*s\n", level, base, line, (int)n, str);
}

//=============================================================================
// State helpers
//=============================================================================
void http_cleanup(void) {
    if (g_https_pcb) {
        altcp_arg(g_https_pcb, NULL);
        altcp_recv(g_https_pcb, NULL);
        altcp_err(g_https_pcb, NULL);
        altcp_poll(g_https_pcb, NULL, 0);
        altcp_close(g_https_pcb);
        g_https_pcb = NULL;
    }
}

void http_set_state(https_state_t s) {
    g_https_state    = s;
    g_https_state_at = http_now_ms();
}

//=============================================================================
// Header / body lookup
//=============================================================================
const char *http_find_body(const char *resp, size_t len) {
    const char *sep = "\r\n\r\n";
    for (size_t i = 0; i + 3 < len; i++) {
        if (memcmp(resp + i, sep, 4) == 0) return resp + i + 4;
    }
    return NULL;
}

const char *http_find_header(const char *resp, size_t len, const char *name) {
    size_t nlen = strlen(name);
    for (size_t i = 0; i + nlen < len; i++) {
        if (strncasecmp(resp + i, name, nlen) == 0 && resp[i + nlen] == ':') {
            const char *p = resp + i + nlen + 1;
            while (*p == ' ' || *p == '\t') p++;
            return p;
        }
    }
    return NULL;
}

int http_kick_info(const char *device_id) {
    g_https_mode = HM_INFO;
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "X-Device-Id: %s\r\n", device_id);
    // ?t=simple → server omits recent_additions, response stays a few KB so
    // RESP_BUF_SIZE can be 16 KB instead of 160 KB.
    if (http_build_request("GET", "/api/tunnel/info?t=simple", hdr,
                            NULL, NULL, 0) != 0) return -1;
    return http_request_start(&g_api_ip, g_https_host);
}

int http_kick_timeline(const char *device_id, const char *operator_id,
                       int64_t last_publish_id) {
    g_https_mode = HM_TIMELINE;
    char path[256];
    int64_t start_id = last_publish_id > 0 ? last_publish_id : 0;
    int n = snprintf(path, sizeof(path),
                     "/api/qa/timeline?publish=1&operator_id=%s"
                     "&start=%lld&end=-1&limit=1",
                     operator_id, (long long)start_id);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        printf("[timeline] path overflow\n");
        return -1;
    }
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "X-Device-Id: %s\r\n", device_id);
    if (http_build_request("GET", path, hdr, NULL, NULL, 0) != 0) return -1;
    return http_request_start(&g_api_ip, g_https_host);
}

//=============================================================================
// Response JSON parsing (static — called from https_handle_done)
//=============================================================================
int64_t g_last_publish_id = -1;

static int parse_lab_response(const char *body) {
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        printf("[json] parse failed (body head: %.60s)\n", body);
        return -1;
    }
    cJSON *operators = cJSON_GetObjectItem(root, "operators");
    if (!cJSON_IsArray(operators)) {
        printf("[json] no 'operators' array\n");
        cJSON_Delete(root);
        return -1;
    }
    int count = 0;
    cJSON *op;
    cJSON_ArrayForEach(op, operators) {
        if (count >= MAX_LABS) break;
        cJSON *op_id = cJSON_GetObjectItem(op, "id");
        if (!cJSON_IsString(op_id) || !op_id->valuestring) continue;
        cJSON *lab = cJSON_GetObjectItem(op, "lab");
        if (!cJSON_IsObject(lab)) continue;
        cJSON *lab_id = cJSON_GetObjectItem(lab, "lab_id");
        if (!cJSON_IsString(lab_id) || !lab_id->valuestring) continue;
        strncpy(g_operator_ids[count], op_id->valuestring,  MAX_LAB_ID_LEN - 1);
        g_operator_ids[count][MAX_LAB_ID_LEN - 1] = '\0';
        strncpy(g_lab_ids[count],      lab_id->valuestring, MAX_LAB_ID_LEN - 1);
        g_lab_ids[count][MAX_LAB_ID_LEN - 1] = '\0';
        count++;
    }
    cJSON_Delete(root);
    g_lab_count    = count;
    g_lab_selected = 0;
    printf("[json] extracted %d operator/lab pair(s)\n", count);
    for (int i = 0; i < count; i++) {
        printf("        [%d] op=%s lab=%s\n", i, g_operator_ids[i], g_lab_ids[i]);
    }
    return 0;
}

static int parse_timeline_response(const char *body) {
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        printf("[timeline] parse failed (body head: %.60s)\n", body);
        return -1;
    }
    cJSON *items = cJSON_GetObjectItem(root, "items");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        printf("[timeline] no 'items' array\n");
        return -1;
    }

    int new_count = 0;
    int64_t max_seen = g_last_publish_id;
    cJSON *it;
    cJSON_ArrayForEach(it, items) {
        cJSON *pid = cJSON_GetObjectItem(it, "publish_id");
        cJSON *msg = cJSON_GetObjectItem(it, "message");
        if (!cJSON_IsNumber(pid) || !cJSON_IsString(msg) || !msg->valuestring) continue;
        int64_t this_pid = (int64_t)pid->valuedouble;
        if (this_pid <= g_last_publish_id) continue;
        if (tts_queue_push(msg->valuestring)) new_count++;
        if (this_pid > max_seen) max_seen = this_pid;
    }
    if (max_seen > g_last_publish_id) g_last_publish_id = max_seen;
    cJSON_Delete(root);
    printf("[timeline] poll done: %d new item(s), last_publish_id=%lld\n",
           new_count, (long long)g_last_publish_id);
    g_timeline_status_ver++;
    return 0;
}

//=============================================================================
// HTTPS response completion handler
//=============================================================================
void https_handle_done(void) {
    bool ok = (g_https_state == HC_DONE_OK);
    printf("[https] done %s (mode=%d, %u bytes)\n",
           ok ? "OK" : "ERR", (int)g_https_mode, (unsigned)g_https_resp_len);

    if (ok && !g_https_headers_done) {
        const char *body = http_find_body(g_https_resp, g_https_resp_len);
        if (body) {
            g_https_body_start   = (size_t)(body - g_https_resp);
            g_https_headers_done = true;
        }
    }

    switch (g_https_mode) {
    case HM_INFO:
        if (ok && g_https_headers_done) {
            const char *body = g_https_resp + g_https_body_start;
            g_lab_state = (parse_lab_response(body) == 0) ? LAB_OK : LAB_ERR;
        } else {
            g_lab_state = LAB_ERR;
        }
        mem_barrier();
        ic_send(IC_MSG_LABS_READY, NULL, 0);
        break;

    case HM_TIMELINE:
        if (ok && g_https_headers_done) {
            const char *body = g_https_resp + g_https_body_start;
            parse_timeline_response(body);
        } else {
            printf("[timeline] poll failed\n");
        }
        break;

    case HM_TTS:
        if (!ok || !g_https_headers_done) {
            printf("[tts] request failed — drop one queue entry\n");
            if (g_tts_play_active) {
                tts_send_end_if_needed();
            } else {
                tts_queue_pop();
            }
        } else if (!g_tts_play_active) {
            http_apply_tts_headers();
            tts_start_playback();
        }
        break;
    }

    http_cleanup();
    http_set_state(HC_IDLE);
    mem_barrier();
}

void http_apply_tts_headers(void) {
    const char *sr = http_find_header(g_https_resp, g_https_body_start, "X-Sample-Rate");
    uint32_t hz = 24000;
    if (sr) {
        int v = atoi(sr);
        if (v >= 8000 && v <= 96000) hz = (uint32_t)v;
    }
    g_tts_sample_rate = hz;
    const char *te = http_find_header(g_https_resp, g_https_body_start, "Transfer-Encoding");
    g_https_chunked = (te && strncasecmp(te, "chunked", 7) == 0);
    printf("[tts] sample_rate=%u chunked=%d\n", (unsigned)hz, (int)g_https_chunked);
}

void http_resp_compact_locked(void) {
    if (g_tts_play_pos == 0) return;
    size_t shift    = g_tts_play_pos;
    size_t body_len = g_https_resp_len - g_https_body_start;
    if (shift > body_len) shift = body_len;
    size_t keep     = body_len - shift;
    if (keep > 0) {
        memmove(g_https_resp + g_https_body_start,
                g_https_resp + g_https_body_start + shift,
                keep);
    }
    if (g_https_chunked) {
        g_chunked_write_pos -= shift;
        g_chunked_read_pos  -= shift;
    }
    g_https_resp_len -= shift;
    g_tts_play_pos    = 0;
}

//=============================================================================
// recv-side completion check
//=============================================================================
// After new bytes arrived: detect end-of-headers, parse Content-Length, and
// (if Content-Length known and reached) flip state to DONE_OK so the main loop
// closes the connection without waiting for a TCP FIN.
static void http_check_complete(void) {
    if (!g_https_headers_done) {
        const char *body = http_find_body(g_https_resp, g_https_resp_len);
        if (!body) return;
        g_https_body_start   = (size_t)(body - g_https_resp);
        g_https_headers_done = true;

        // Dump first ~256 bytes for visibility
        size_t dump = g_https_resp_len < 256 ? g_https_resp_len : 256;
        char saved = g_https_resp[dump];
        g_https_resp[dump] = '\0';
        printf("[https] head:\n%s\n", g_https_resp);
        g_https_resp[dump] = saved;

        const char *cl = http_find_header(g_https_resp, g_https_body_start, "Content-Length");
        if (cl) {
            g_https_content_len = atoi(cl);
            printf("[https] Content-Length=%d body@%u\n",
                   g_https_content_len, (unsigned)g_https_body_start);
        } else {
            printf("[https] no Content-Length (chunked?), will rely on FIN/timeout\n");
        }
        // True streaming for TTS: parse X-Sample-Rate / Transfer-Encoding,
        // then hand off to the application so it can arm the audio engine.
        // core1's tts_forward_pump will then ship decoded PCM chunk-by-chunk.
        if (g_https_mode == HM_TTS) {
            http_apply_tts_headers();
            tts_start_playback();
        }
    }
    if (g_https_headers_done && g_https_content_len >= 0) {
        size_t body_have = g_https_resp_len - g_https_body_start;
        if ((int)body_have >= g_https_content_len) {
            printf("[https] body complete (%u/%d)\n",
                   (unsigned)body_have, g_https_content_len);
            http_set_state(HC_DONE_OK);
        }
    }
}

//=============================================================================
// altcp callbacks
//=============================================================================
static err_t https_poll_cb(void *arg, struct altcp_pcb *pcb) {
    (void)arg; (void)pcb;
    uint32_t elapsed = http_now_ms() - g_https_state_at;
    printf("[https] poll: state=%d elapsed=%lums resp_len=%u\n",
           (int)g_https_state, (unsigned long)elapsed, (unsigned)g_https_resp_len);
    if (elapsed > HTTPS_TIMEOUT_MS) {
        printf("[https] timeout in state %d\n", (int)g_https_state);
        http_set_state(HC_DONE_ERR);
        return ERR_ABRT;  // abort the connection
    }
    return ERR_OK;
}

static void https_err_cb(void *arg, err_t err) {
    (void)arg;
    printf("[https] err_cb err=%d state=%d\n", err, (int)g_https_state);
    g_https_pcb = NULL;  // pcb already freed by lwIP
    http_set_state(HC_DONE_ERR);
}

static err_t https_recv_cb(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (p == NULL || err != ERR_OK) {
        // Remote closed → we're done
        if (p) pbuf_free(p);
        printf("[https] recv close (resp_len=%u err=%d)\n",
               (unsigned)g_https_resp_len, err);
        http_set_state(HC_DONE_OK);
        return ERR_OK;
    }
    u16_t need = p->tot_len;
    // If accepting this pbuf would overflow, give the application a chance to
    // reclaim already-consumed body bytes (we're already holding the
    // async_context lock, so the hook must not re-acquire it).
    if (g_https_resp_len + need >= sizeof(g_https_resp)) {
        http_resp_compact_locked();
    }
    // Still no room → return ERR_MEM so lwIP keeps the pbuf and re-delivers
    // it later. This is real TCP backpressure: the server's send window stays
    // closed until we drain enough of g_https_resp via the pump.
    if (g_https_resp_len + need >= sizeof(g_https_resp)) {
        return ERR_MEM;
    }
    pbuf_copy_partial(p, g_https_resp + g_https_resp_len, need, 0);
    g_https_resp_len += need;
    g_https_resp[g_https_resp_len] = '\0';
    altcp_recved(pcb, need);
    pbuf_free(p);
    http_check_complete();
    return ERR_OK;
}

static err_t https_connected_cb(void *arg, struct altcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) {
        printf("[https] connect failed err=%d\n", err);
        http_set_state(HC_DONE_ERR);
        return ERR_OK;
    }
    printf("[https] TLS connected (after %lu ms), sending request (%u bytes, mode=%d)\n",
           (unsigned long)(http_now_ms() - g_https_state_at),
           (unsigned)g_https_req_len, (int)g_https_mode);
    // For POSTs, the body may follow the headers — send everything we've prebuilt.
    err_t werr = altcp_write(pcb, g_https_req, (u16_t)g_https_req_len, TCP_WRITE_FLAG_COPY);
    if (werr != ERR_OK) {
        printf("[https] write err=%d\n", werr);
        http_set_state(HC_DONE_ERR);
        return werr;
    }
    err_t oerr = altcp_output(pcb);
    printf("[https] write ok (%u B), output err=%d\n", (unsigned)g_https_req_len, oerr);
    http_set_state(HC_REQUESTING);
    return ERR_OK;
}

// Fires when the server ACKs the bytes we sent. If this never fires, our
// GET never reached the server's TCP stack.
static err_t https_sent_cb(void *arg, struct altcp_pcb *pcb, u16_t len) {
    (void)arg; (void)pcb;
    printf("[https] sent ACK: %u bytes (elapsed=%lums in state=%d)\n",
           len,
           (unsigned long)(http_now_ms() - g_https_state_at),
           (int)g_https_state);
    return ERR_OK;
}

//=============================================================================
// Request building / kickoff
//=============================================================================
int http_build_request(const char *method,
                       const char *path,
                       const char *extra_headers,
                       const char *content_type,
                       const char *body,
                       size_t      body_len) {
    int n;
    if (body) {
        n = snprintf(g_https_req, sizeof(g_https_req),
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: pico-lcd/1.0\r\n"
                     "Accept: */*\r\n"
                     "%s"
                     "Content-Type: %s\r\n"
                     "Content-Length: %u\r\n"
                     "\r\n",
                     method, path, g_https_host,
                     extra_headers ? extra_headers : "",
                     content_type ? content_type : "application/octet-stream",
                     (unsigned)body_len);
        if (n < 0 || (size_t)n + body_len >= sizeof(g_https_req)) {
            printf("[https] request too long (hdr=%d body=%u)\n", n, (unsigned)body_len);
            return -1;
        }
        memcpy(g_https_req + n, body, body_len);
        g_https_req_len = (size_t)n + body_len;
    } else {
        n = snprintf(g_https_req, sizeof(g_https_req),
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: pico-lcd/1.0\r\n"
                     "Accept: */*\r\n"
                     "%s"
                     "\r\n",
                     method, path, g_https_host,
                     extra_headers ? extra_headers : "");
        if (n < 0 || (size_t)n >= sizeof(g_https_req)) {
            printf("[https] request too long (%d)\n", n);
            return -1;
        }
        g_https_req_len = (size_t)n;
    }
    return 0;
}

int http_request_start(const ip_addr_t *ip, const char *host) {
    g_https_resp_len     = 0;
    g_https_resp[0]      = '\0';
    g_https_headers_done = false;
    g_https_body_start   = 0;
    g_https_content_len  = -1;
    g_tts_sample_rate    = 24000;
    g_https_chunked      = false;
    g_chunked_read_pos   = 0;
    g_chunked_write_pos  = 0;
    g_chunked_state      = CHUNK_NEED_SIZE;
    g_chunked_size_acc   = 0;
    g_chunked_remaining  = 0;
    g_chunked_in_ext     = false;

    if (!g_tls_cfg) {
        g_tls_cfg = altcp_tls_create_config_client(NULL, 0);
        if (!g_tls_cfg) {
            printf("[https] altcp_tls_create_config_client failed\n");
            return -1;
        }
        // mbedtls internal debug: disabled (threshold=0). Flip to 1..4 to debug.
        mbedtls_debug_set_threshold(0);
        // ALPN disabled for now.
    }
    // g_https_host is set by the application's network bring-up — caller
    // passes the same pointer in for the SNI hostname below.
    (void)mbedtls_debug_print;  // keep referenced even when ssl_conf_dbg unused

    g_https_pcb = altcp_tls_new(g_tls_cfg, IPADDR_TYPE_V4);
    if (!g_https_pcb) {
        printf("[https] altcp_tls_new failed\n");
        return -1;
    }
    // SNI hostname
    mbedtls_ssl_set_hostname((mbedtls_ssl_context *)altcp_tls_context(g_https_pcb), host);

    altcp_arg(g_https_pcb, NULL);
    altcp_recv(g_https_pcb, https_recv_cb);
    altcp_err(g_https_pcb,  https_err_cb);
    altcp_sent(g_https_pcb, https_sent_cb);
    altcp_poll(g_https_pcb, https_poll_cb, HTTPS_POLL_INTERVAL);

    http_set_state(HC_CONNECTING);
    printf("[https] connect %s:%d (%s) mode=%d\n",
           host, HTTPS_PORT, ipaddr_ntoa(ip), (int)g_https_mode);
    err_t err = altcp_connect(g_https_pcb, ip, HTTPS_PORT, https_connected_cb);
    if (err != ERR_OK) {
        printf("[https] altcp_connect err=%d\n", err);
        http_cleanup();
        http_set_state(HC_DONE_ERR);
        return -1;
    }
    return 0;
}

//=============================================================================
// Chunked transfer-encoding decoder
//=============================================================================
// Decode chunked transfer-encoding in-place inside g_https_resp[body_start...].
// Reads from offset g_chunked_read_pos, writes decoded PCM bytes to offset
// g_chunked_write_pos. Both offsets are relative to body_start. Write offset
// is always <= read offset (framing only adds bytes), so memmove is safe.
// Resumes where the previous call left off.
void http_chunked_decode_in_place(void) {
    uint8_t *base = (uint8_t *)g_https_resp + g_https_body_start;
    size_t resp_have = g_https_resp_len - g_https_body_start;

    while (g_chunked_read_pos < resp_have && g_chunked_state != CHUNK_DONE) {
        uint8_t c = base[g_chunked_read_pos];
        switch (g_chunked_state) {
        case CHUNK_NEED_SIZE:
            if (c == '\r') {
                g_chunked_state = CHUNK_SIZE_SAW_CR;
            } else if (c == ';') {
                g_chunked_in_ext = true;
            } else if (!g_chunked_in_ext) {
                if      (c >= '0' && c <= '9') g_chunked_size_acc = g_chunked_size_acc * 16 + (c - '0');
                else if (c >= 'a' && c <= 'f') g_chunked_size_acc = g_chunked_size_acc * 16 + (c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') g_chunked_size_acc = g_chunked_size_acc * 16 + (c - 'A' + 10);
                // else: tolerate stray whitespace / unexpected chars silently
            }
            g_chunked_read_pos++;
            break;
        case CHUNK_SIZE_SAW_CR:
            if (c == '\n') {
                g_chunked_remaining = g_chunked_size_acc;
                g_chunked_size_acc  = 0;
                g_chunked_in_ext    = false;
                printf("[chunked] new chunk size=%u at read_pos=%u (resp_have=%u write_pos=%u)\n",
                       (unsigned)g_chunked_remaining, (unsigned)g_chunked_read_pos,
                       (unsigned)resp_have, (unsigned)g_chunked_write_pos);
                if (g_chunked_remaining == 0) {
                    g_chunked_state = CHUNK_DONE;
                    g_chunked_read_pos++;
                    // Notify HTTPS layer that the body is complete (server uses
                    // keep-alive so it won't FIN — we close from our side).
                    if (g_https_state == HC_REQUESTING || g_https_state == HC_CONNECTING) {
                        printf("[tts] chunked stream complete, closing\n");
                        http_set_state(HC_DONE_OK);
                    }
                    return;
                }
                g_chunked_state = CHUNK_DATA;
            }
            g_chunked_read_pos++;
            break;
        case CHUNK_DATA: {
            size_t avail = resp_have - g_chunked_read_pos;
            size_t take  = (avail < g_chunked_remaining) ? avail : g_chunked_remaining;
            if (take == 0) return;  // need more input
            size_t prev_wp = g_chunked_write_pos;
            if (g_chunked_write_pos != g_chunked_read_pos) {
                memmove(base + g_chunked_write_pos,
                        base + g_chunked_read_pos, take);
            }
            g_chunked_read_pos  += take;
            g_chunked_write_pos += take;
            g_chunked_remaining -= take;
            if (g_chunked_remaining == 0) g_chunked_state = CHUNK_DATA_SAW_CR;
            // One-shot dump: first time write_pos crosses 16 bytes, show layout
            // + first 16 PCM bytes so we can cross-check against the pump side.
            static bool dumped = false;
            if (!dumped && g_chunked_write_pos >= 16) {
                dumped = true;
                uint8_t *pcm = base;  // == g_https_resp + body_start
                printf("[decoder] resp=%p body_start=%u write_pos=%u "
                       "base=%p decoded_end=%p\n",
                       (void*)g_https_resp, (unsigned)g_https_body_start,
                       (unsigned)g_chunked_write_pos,
                       (void*)base, (void*)(base + g_chunked_write_pos));
                printf("[decoder] first16:");
                for (int i = 0; i < 16; i++) printf(" %02x", pcm[i]);
                printf(" (prev_wp=%u take=%u)\n",
                       (unsigned)prev_wp, (unsigned)take);
            }
            break;
        }
        case CHUNK_DATA_SAW_CR:
            // expect \r — server is well-formed, just skip
            if (c == '\r') g_chunked_state = CHUNK_NEED_SIZE;  // next we want \n then size
            g_chunked_read_pos++;
            // Need to also consume the LF after CR before next size hex.
            if (g_chunked_read_pos < resp_have && base[g_chunked_read_pos] == '\n') {
                g_chunked_read_pos++;
            }
            break;
        case CHUNK_DONE:
            return;
        }
    }
}
