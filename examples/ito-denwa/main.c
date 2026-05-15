#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <stdarg.h>

#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "pico/mutex.h"
#include "hardware/watchdog.h"
#include "hardware/clocks.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"

#include "cJSON.h"
#include "st7789.h"
#include "audio.h"
#include "ic_ring.h"
#include "http.h"
#include "queue.h"

//-----------------------------------------------------------------------------
// Button IDs (used as indices into g_buttons[])
//-----------------------------------------------------------------------------
typedef enum {
    GUI_BTN_A = 0,
    GUI_BTN_B,
    GUI_BTN_X,
    GUI_BTN_Y,
    GUI_KEY_UP,
    GUI_KEY_DOWN,
    GUI_KEY_LEFT,
    GUI_KEY_RIGHT,
    GUI_KEY_CTRL,
    GUI_BTN_COUNT,
} gui_button_id_t;

//=============================================================================
// Configuration - set via environment variables or defaults below
//=============================================================================
#ifndef WIFI_SSID
#define WIFI_SSID       "your-wifi-ssid"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD   "your-wifi-password"
#endif
#ifndef API_URL
#define API_URL         "https://example.com/"
#endif
#ifndef DEVICE_ID
#define DEVICE_ID       "1b505f7a-4e7b-11f1-a1dd-d83addf43636"
#endif

#define API_PATH        "/api/tunnel/info"
// start = MAX(g_last_publish_id, 0). Server expects start=0 on the first
// poll; once we've seen at least one item we pass the last seen publish_id
// as the start, and the server returns items from there.
#define TIMELINE_PATH_FMT "/api/qa/timeline?publish=1&operator_id=%s&start=%lld&end=-1&limit=1"
#define TTS_PATH        "/api/tts/generate_stream"
#define TIMELINE_POLL_INTERVAL_MS 10000
#define MAX_LABS        16
#define MAX_LAB_ID_LEN  64

static inline uint32_t board_millis(void) {
    return to_ms_since_boot(get_absolute_time());
}

static inline void mem_barrier(void) {
    __asm__ volatile("dmb" ::: "memory");
}

// Forward decls — defined later but called from earlier functions.
static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tts_apply_response_headers(void);
static void tts_start_playback(void);
static void resp_compact_locked(void);

//=============================================================================
// Timing measurement
//=============================================================================
static uint32_t g_time_boot = 0;
static uint32_t g_time_wifi_start = 0;
static uint32_t g_time_wifi_scan_start = 0;
static uint32_t g_time_dhcp_start = 0;
static uint32_t g_time_dns_start = 0;
static uint32_t g_time_dns_done = 0;

//=============================================================================
// LED blink state machine (non-blocking)
//=============================================================================
typedef struct {
    uint32_t interval_ms;
    int      remaining;
    uint32_t last_toggle;
    bool     led_on;
} LedBlinkState;

static LedBlinkState g_led_blink = {0, 0, 0, false};

static void led_blink_start(uint32_t interval_ms, int count) {
    g_led_blink.interval_ms = interval_ms;
    g_led_blink.remaining   = count * 2;
    g_led_blink.last_toggle = board_millis();
    g_led_blink.led_on      = true;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
}

static void led_blink_loop(void) {
    if (g_led_blink.remaining <= 0) return;

    uint32_t now = board_millis();
    if ((now - g_led_blink.last_toggle) >= g_led_blink.interval_ms) {
        g_led_blink.last_toggle = now;
        g_led_blink.remaining--;

        if (g_led_blink.remaining > 0) {
            g_led_blink.led_on = !g_led_blink.led_on;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, g_led_blink.led_on ? 1 : 0);
        } else {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
            g_led_blink.led_on = false;
        }
    }
}

static uint32_t g_heartbeat_last = 0;
static bool     g_heartbeat_on   = false;

static void led_heartbeat_loop(void) {
    if (g_led_blink.remaining > 0) return;

    uint32_t now = board_millis();
    if ((now - g_heartbeat_last) >= 1000) {
        g_heartbeat_last = now;
        g_heartbeat_on   = !g_heartbeat_on;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, g_heartbeat_on ? 1 : 0);
    }
}

//=============================================================================
// WiFi initialization (detailed timing)
//=============================================================================
static int wifi_init(void) {
    int last_status = CYW43_LINK_DOWN;
    int status;
    uint32_t now;
    uint32_t timeout_start;

    cyw43_arch_enable_sta_mode();

    g_time_wifi_start      = board_millis();
    g_time_wifi_scan_start = g_time_wifi_start;
    printf("[TIMING] WiFi scan start: %lu ms\n", (unsigned long)g_time_wifi_scan_start);
    printf("Connecting to WiFi '%s'...\n", WIFI_SSID);
    set_status("WiFi: '%s' scan...", WIFI_SSID);

    if (cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK) != 0) {
        printf("WiFi connect_async failed\n");
        set_status("WiFi: connect_async fail");
        return -1;
    }

    timeout_start = board_millis();
    while (1) {
        cyw43_arch_poll();
        sleep_ms(10);

        now = board_millis();
        if ((now - timeout_start) > 30000) {
            printf("WiFi connection timeout\n");
            set_status("WiFi: timeout");
            return -1;
        }

        status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (status != last_status) {
            switch (status) {
                case CYW43_LINK_JOIN:
                    printf("[TIMING] WiFi scan done: %lu ms (took %lu ms)\n",
                           (unsigned long)now, (unsigned long)(now - g_time_wifi_scan_start));
                    printf("[TIMING] WiFi auth/assoc done: %lu ms\n", (unsigned long)now);
                    set_status("WiFi: assoc ok (%lums)", (unsigned long)(now - g_time_wifi_scan_start));
                    break;
                case CYW43_LINK_NOIP:
                    g_time_dhcp_start = now;
                    printf("[TIMING] DHCP start: %lu ms\n", (unsigned long)now);
                    set_status("DHCP: requesting...");
                    break;
                case CYW43_LINK_UP: {
                    printf("[TIMING] DHCP bound: %lu ms (took %lu ms)\n",
                           (unsigned long)now, (unsigned long)(now - g_time_dhcp_start));
                    printf("[TIMING] WiFi fully connected: %lu ms (total %lu ms)\n",
                           (unsigned long)now, (unsigned long)(now - g_time_wifi_start));
                    const ip_addr_t *my_ip = netif_ip_addr4(netif_default);
                    set_status("DHCP: %s", my_ip ? ipaddr_ntoa(my_ip) : "?");
                    set_status("WiFi: up (%lums)", (unsigned long)(now - g_time_wifi_start));
                    goto wifi_connected;
                }
                case CYW43_LINK_FAIL:
                    printf("WiFi connection failed\n");
                    set_status("WiFi: fail");
                    return -1;
                case CYW43_LINK_NONET:
                    printf("WiFi: No matching SSID found\n");
                    set_status("WiFi: no SSID");
                    return -1;
                case CYW43_LINK_BADAUTH:
                    printf("WiFi: Authentication failure\n");
                    set_status("WiFi: bad auth");
                    return -1;
                default:
                    break;
            }
            last_status = status;
        }
    }

wifi_connected:
    for (int i = 0; i < 5; i++) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        sleep_ms(100);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        sleep_ms(100);
    }
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    return 0;
}

//=============================================================================
// DNS resolution (blocking)
//=============================================================================
static volatile bool g_dns_done = false;
static ip_addr_t     g_dns_result;
static ip_addr_t     g_api_ip;

static void dns_callback(const char *name, const ip_addr_t *ipaddr, void *arg) {
    (void)name; (void)arg;
    g_time_dns_done = board_millis();
    if (ipaddr) {
        g_dns_result = *ipaddr;
        printf("[TIMING] DNS resolved: %lu ms (took %lu ms) -> %s\n",
               (unsigned long)g_time_dns_done,
               (unsigned long)(g_time_dns_done - g_time_dns_start),
               ipaddr_ntoa(ipaddr));
    } else {
        printf("[TIMING] DNS resolution failed: %lu ms\n", (unsigned long)g_time_dns_done);
    }
    g_dns_done = true;
}

static void extract_hostname(const char *url, char *hostname, size_t hostname_size) {
    const char *start = url;
    const char *end;
    if (strncmp(url, "http://", 7) == 0)  start = url + 7;
    else if (strncmp(url, "https://", 8) == 0) start = url + 8;
    end = start;
    while (*end && *end != ':' && *end != '/') end++;
    size_t len = end - start;
    if (len >= hostname_size) len = hostname_size - 1;
    memcpy(hostname, start, len);
    hostname[len] = '\0';
}

static int dns_resolve_blocking(const char *hostname, ip_addr_t *out) {
    g_time_dns_start = board_millis();
    printf("[TIMING] DNS lookup start: %lu ms (host: %s)\n",
           (unsigned long)g_time_dns_start, hostname);
    set_status("DNS: %s", hostname);

    g_dns_done = false;
    err_t err = dns_gethostbyname(hostname, &g_dns_result, dns_callback, NULL);
    if (err == ERR_OK) {
        g_time_dns_done = board_millis();
        printf("[TIMING] DNS resolved (cached): %lu ms -> %s\n",
               (unsigned long)g_time_dns_done, ipaddr_ntoa(&g_dns_result));
        *out = g_dns_result;
        set_status("DNS: %s (cached)", ipaddr_ntoa(&g_dns_result));
        return 0;
    } else if (err == ERR_INPROGRESS) {
        uint32_t dns_timeout = board_millis();
        while (!g_dns_done && (board_millis() - dns_timeout) < 10000) {
            cyw43_arch_poll();
            sleep_ms(10);
        }
        if (!g_dns_done) {
            printf("[TIMING] DNS timeout\n");
            set_status("DNS: timeout");
            return -1;
        }
        *out = g_dns_result;
        set_status("DNS: %s", ipaddr_ntoa(&g_dns_result));
        return 0;
    } else {
        printf("[TIMING] DNS lookup failed: err=%d\n", err);
        set_status("DNS: err=%d", err);
        return -1;
    }
}

//=============================================================================
// Lab list state (shared between cores)
//=============================================================================
typedef enum {
    LAB_IDLE,
    LAB_LOADING,
    LAB_OK,
    LAB_ERR,
} lab_state_t;

static volatile lab_state_t g_lab_state    = LAB_IDLE;
// Core1-local "fetch pending" flag (was a cross-core volatile; now set by
// handle_core1_notify on IC_MSG_BTN_X and consumed by on_core1_running).
static bool                 g_core1_fetch_pending = false;
// (was g_labs_ready — now signalled via IC_MSG_LABS_READY)
// Parallel arrays: g_operator_ids[i] is the operator UUID (op.id, used in
// /api/qa/timeline?operator_id=…) and g_lab_ids[i] is op.lab.lab_id (per-Lab
// UUID used for the per-Lab uid lookup in auth headers).
static char     g_operator_ids[MAX_LABS][MAX_LAB_ID_LEN];
static char     g_lab_ids[MAX_LABS][MAX_LAB_ID_LEN];
static int      g_lab_count    = 0;
static int      g_lab_selected = 0;

//=============================================================================
// Timeline polling + TTS queue
//=============================================================================
// core0-only UI intent. Toggled by the Enter button handler; read by all
// rendering paths. core1 no longer touches this — its own polling decision
// uses g_core1_tl_active (set from IC_MSG_TIMELINE_START/STOP).
static bool          g_timeline_active    = false;
static uint32_t      g_timeline_last_poll = 0;      // core1 only
static int64_t       g_last_publish_id    = -1;     // core1 only — newest seen, -1 = none yet
static char          g_timeline_lab_id[MAX_LAB_ID_LEN] = "";  // core0 only (UI title)
static volatile int  g_timeline_status_ver = 0;     // bumped on poll events for UI

// core1-private timeline polling gate. Set by IC_MSG_TIMELINE_START (with an
// operator_id payload), cleared by IC_MSG_TIMELINE_STOP. Replaces the cross-
// core read of g_timeline_active.
static bool          g_core1_tl_active    = false;

// TTS playback streaming state.
// core1 owns the network/decoder side; it advances g_tts_play_pos as bytes
// are forwarded to core0 via IC_MSG_TTS_PCM_CHUNK. core0 owns the audio
// ring side and drains g_core0_pcm_pending into the I2S DMA buffer.
// g_tts_play_active is a shared status flag — both cores set it false at
// end-of-life and agree on that as the resting state.
static volatile bool   g_tts_play_active = false;
static volatile size_t g_tts_play_pos    = 0;       // bytes forwarded via FIFO (core1)
// Once the HTTP body drains, we still need to overwrite the audio ring with
// silence — otherwise the DMA wraps and re-plays the trailing 2048 samples
// of TTS audio in a loop. Counts silence samples still owed to the ring.
// core0-only; armed when IC_MSG_TTS_END is received.
static size_t          g_tts_pad_remaining = 0;

// Max bytes core1 forwards per IC_MSG_TTS_PCM_CHUNK send. Sized to match
// IC_RECV_BUF_BYTES (1024) so a single FIFO entry carries a full chunk.
// 1024 B = 512 int16 samples = ~21 ms of audio @ 24 kHz.
#define TTS_FWD_CHUNK_SIZE  1024

// core0 staging buffer for PCM bytes received from core1. The FIFO drain in
// core0_loop only pops IC_MSG_TTS_PCM_CHUNK when this has room for a full
// chunk (see ic_peek_type gate). Extra slack means we can absorb a small
// burst from the ic_ring without overrunning.
#define TTS_PCM_PENDING_CAP  4096
static uint8_t           g_core0_pcm_pending[TTS_PCM_PENDING_CAP];
static volatile size_t   g_core0_pcm_pending_len = 0;
// Set by handle_core0_notify on IC_MSG_TTS_END; cleared when playback wraps
// up. While true, tts_play_pump arms the silence pad and tears playback down
// once both pending and pad are drained.
static volatile bool     g_core0_tts_stream_done = false;

//=============================================================================
// HTTPS client (altcp_tls) — see http.h / src/http.c
//=============================================================================

// Parse JSON, populate g_lab_ids / g_lab_count
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
        if (!cJSON_IsObject(lab)) continue;  // tunnel just connected, lab still empty
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

// Parse /api/qa/timeline JSON body. For each item with publish_id > the last
// one we've seen, push the "message" string onto the TTS queue and bump
// g_last_publish_id. Returns 0 on success, -1 on parse error.
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

// Pull X-Sample-Rate (default 24000) and Transfer-Encoding out of the
// response headers. Called once headers complete in HM_TTS mode.
static void tts_apply_response_headers(void) {
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


// Arm streaming playback. Called the moment response headers parse — body
// bytes will arrive later via recv_cb appending to g_https_resp;
// tts_forward_pump (core1 main loop) ships them to core0 via the ic_ring.
// So we DON'T require body bytes here — only that we know the body offset.
// core1-private guard: set true after we ship IC_MSG_TTS_END so the
// forward pump doesn't re-send. Reset on each new playback start.
static bool g_core1_tts_end_sent = false;

static void tts_start_playback(void) {
    if (!g_https_headers_done) {
        printf("[tts] start_playback called before headers_done — ignoring\n");
        return;
    }
    audio_set_sample_rate(g_tts_sample_rate);
    audio_stream_reset();
    g_tts_play_pos      = 0;
    g_tts_pad_remaining = 0;
    // Reset core0-side PCM pipeline state. We're the producer; clearing
    // these from here is safe because no PCM_CHUNK has been sent yet on
    // this stream, so core0 isn't concurrently mutating them. The
    // mem_barrier below ensures these writes land before the
    // g_tts_play_active=true publish.
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

// Hook called from src/http.c when response headers are parsed in HM_TTS
// mode — gives us a chance to read X-Sample-Rate / Transfer-Encoding and
// arm the audio engine before the body finishes streaming.
void http_on_tts_headers_done(void) {
    tts_apply_response_headers();
    tts_start_playback();
}

// Hook called from src/http.c (inside the lwIP recv callback) when the
// next pbuf would overflow g_https_resp — we reclaim already-played bytes
// inline. The recv path already holds the async_context lock, so the
// no-lock variant is what we need here.
void http_on_recv_overflow(void) {
    resp_compact_locked();
}

static void https_handle_done(void) {
    bool ok = (g_https_state == HC_DONE_OK);
    printf("[https] done %s (mode=%d, %u bytes)\n",
           ok ? "OK" : "ERR", (int)g_https_mode, (unsigned)g_https_resp_len);

    // Ensure header offset is computed even if the connection closed at the
    // exact moment recv_cb hadn't finished parsing.
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
        // Tell core0 to refresh the lab-list view. g_lab_state etc are
        // already published via volatile writes above.
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
                // Mid-stream failure. core0 may still be holding pending
                // PCM in g_core0_pcm_pending, and the audio ring is full
                // of trailing samples — if we just clear play_active, the
                // DMA will wrap and loop those forever. Hand the wind-down
                // to core0 by shipping TTS_END; its pump will silence-pad
                // and send TTS_PLAYED, which pops the queue here.
                if (!g_core1_tts_end_sent) {
                    ic_send(IC_MSG_TTS_END, NULL, 0);
                    g_core1_tts_end_sent = true;
                }
            } else {
                // No playback ever started — safe to pop directly.
                tts_queue_pop();
            }
        } else if (!g_tts_play_active) {
            // Headers showed up only at FIN (very small reply). Start now.
            tts_apply_response_headers();
            tts_start_playback();
        }
        // Otherwise: streaming playback already in flight. tts_forward_pump
        // (core1) will ship remaining PCM + a TTS_END marker; core0's pump
        // plays it out and sends TTS_PLAYED, which pops the queue.
        break;
    }

    http_cleanup();
    http_set_state(HC_IDLE);
    mem_barrier();
}

//=============================================================================
// Request kickoff helpers (core1)
//=============================================================================
static int kick_info_request(void) {
    g_https_mode = HM_INFO;
    if (http_build_request("GET", API_PATH,
                            "X-Device-Id: " DEVICE_ID "\r\n",
                            NULL, NULL, 0) != 0) return -1;
    return http_request_start(&g_api_ip, g_https_host);
}

// We remember which operator's timeline we're polling. Set by core1 when
// timeline_start_req fires (uses g_lab_selected snapshot from core0).
static char g_timeline_operator_id[MAX_LAB_ID_LEN] = "";

static int kick_timeline_request(void) {
    g_https_mode = HM_TIMELINE;
    char path[256];
    int64_t start_id = g_last_publish_id > 0 ? g_last_publish_id : 0;
    int n = snprintf(path, sizeof(path), TIMELINE_PATH_FMT,
                     g_timeline_operator_id,
                     (long long)start_id);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        printf("[timeline] path overflow\n");
        return -1;
    }
    if (http_build_request("GET", path,
                            "X-Device-Id: " DEVICE_ID "\r\n",
                            NULL, NULL, 0) != 0) return -1;
    return http_request_start(&g_api_ip, g_https_host);
}

// Escape ASCII control / quote / backslash for JSON string body. UTF-8 multi-
// byte sequences pass through (Japanese is fine). Returns bytes written, or -1
// if the destination is too small.
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

static int kick_tts_request(const char *message) {
    g_https_mode = HM_TTS;

    // Build JSON body: {"gender":"male","style":"neutral","out_lang":"ja","text":"<escaped>"}
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

    if (http_build_request("POST", TTS_PATH,
                            "X-Device-Id: " DEVICE_ID "\r\n"
                            "X-Sample-Rate: 24000\r\n",
                            "application/json",
                            body, (size_t)blen) != 0) return -1;
    return http_request_start(&g_api_ip, g_https_host);
}

// Copy as much pending PCM as fits into the audio ring, advance position.
// Runs concurrently with https_recv_cb so audio starts the moment headers
// are parsed — no need to wait for the full body. Returns true while playback
// is still in flight; false once the body has fully drained AND the HTTPS
// Discard already-played bytes from g_https_resp so the body region can grow
// past RESP_BUF_SIZE. Without this, TTS audio that decodes to more than
// (RESP_BUF_SIZE - headers) bytes overflows the response buffer and recv_cb
// drops the tail — which manifests as playback cutting off mid-utterance.
//
// All chunked decoder + player offsets are RELATIVE TO body_start, so a
// memmove inside the body region followed by a uniform subtract on each
// offset preserves correctness. We hold the lwIP lock during the shift so
// recv_cb can't append at the (now invalid) old g_https_resp_len position.
//
// Amortize: only compact once play_pos reaches a threshold so the per-shift
// memmove is worthwhile (and the lwIP lock isn't held more than ~once per
// few hundred ms).
// Smaller threshold so the buffer never gets close to full between
// compactions during chunked TTS bursts (CloudFront delivers 30+ KB chunks
// faster than the pump can drain at 48 KB/s = audio rate).
#define TTS_COMPACT_THRESHOLD  2048

// Core compaction: shifts unplayed bytes to body_start, rebases all offsets.
// Caller must hold the lwIP/async_context lock so recv_cb can't append at the
// (about to be invalid) old g_https_resp_len position.
static void resp_compact_locked(void) {
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
    // For chunked: chunked_read_pos >= chunked_write_pos >= play_pos always
    // (decoder converts chunked bytes → PCM in-place, player only consumes
    // decoded PCM), so the subtractions can't underflow. For non-chunked,
    // those offsets stay at 0 throughout and we leave them alone.
    if (g_https_chunked) {
        g_chunked_write_pos -= shift;
        g_chunked_read_pos  -= shift;
    }
    g_https_resp_len -= shift;
    g_tts_play_pos    = 0;
}

// Pump-side compaction (runs in main loop, must acquire lock).
static void tts_resp_compact(void) {
    if (g_tts_play_pos < TTS_COMPACT_THRESHOLD) return;
    cyw43_arch_lwip_begin();
    resp_compact_locked();
    cyw43_arch_lwip_end();
}

// core0-side audio pump. Consumes PCM bytes that core1 staged into
// g_core0_pcm_pending (via IC_MSG_TTS_PCM_CHUNK) and pushes them into the
// audio ring. No g_https_resp access — that buffer is now core1-only.
// Returns true while playback is still in flight.
static bool tts_play_pump(void) {
    // Refresh the audio layer's wrap counter on every pump iteration. If
    // a chunk gap exceeded one ring lap (~85ms @ 24kHz) without us touching
    // the audio layer, dma_consumed_abs() would miss a wrap and the
    // producer would mistakenly think the ring was full → silent loop of
    // the last 85ms.
    (void)audio_stream_buffered();

    // While streaming, watch for "DMA passed the writer" underruns and
    // silence the ring on the spot. Gated on g_tts_play_active so a button-
    // triggered audio_play_sine (which fills the ring without touching
    // g_write_abs) doesn't get wiped by an apparent-underrun detection.
    if (g_tts_play_active) audio_stream_underrun_recover();

    // Drain pending PCM into the audio ring (full samples only; bytes_left
    // can be odd transiently because a chunk boundary doesn't have to land
    // on a stereo-frame boundary on core1's side).
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

    // Arm the silence pad once the stream has ended AND we've pushed every
    // PCM byte we received. (g_tts_pad_remaining == 0 acts as the guard so
    // we don't re-arm after the pad has been started.)
    if (stream_done && source_drained && g_tts_pad_remaining == 0
        && g_tts_play_active) {
        // 2304 samples > ring size (2048) — guarantees full overwrite of
        // the trailing PCM with zeros, regardless of where DMA's read
        // pointer lands.
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
        // Tell core1: pop the TTS queue (the playback half of the
        // request/response pair is complete).
        ic_send(IC_MSG_TTS_PLAYED, NULL, 0);
        return false;
    }
    return g_tts_play_active;
}

// core1-side forwarder. Reads PCM bytes from g_https_resp[body_start +
// g_tts_play_pos ...] and ships them to core0 via IC_MSG_TTS_PCM_CHUNK in
// TTS_FWD_CHUNK_SIZE-byte slices. When the HTTP stream ends and every byte
// has been forwarded, sends one IC_MSG_TTS_END. Returns true while still
// active. Bounded per call so a CloudFront burst doesn't tie core1 up
// inside one big ic_send and starve cyw43_arch_poll.
static bool tts_forward_pump(void) {
    if (!g_tts_play_active)       return false;
    if (g_core1_tts_end_sent)     return false;
    if (!g_https_headers_done)    return true;

    // Convert chunked-TE framing → raw PCM bytes in-place. After this
    // call, body[0..g_chunked_write_pos) holds decoded PCM (non-chunked
    // bodies leave both offsets at 0 and we use resp_len directly).
    if (g_https_chunked) http_chunked_decode_in_place();

    size_t body_have = g_https_chunked
                         ? g_chunked_write_pos
                         : (g_https_resp_len - g_https_body_start);
    size_t bytes_left = body_have - g_tts_play_pos;

    if (bytes_left >= 2) {
        size_t n = bytes_left;
        if (n > TTS_FWD_CHUNK_SIZE) n = TTS_FWD_CHUNK_SIZE;
        n &= ~1u;   // keep stereo-frame boundaries

        const uint8_t *src = (const uint8_t *)g_https_resp + g_https_body_start
                             + g_tts_play_pos;
        ic_send(IC_MSG_TTS_PCM_CHUNK, src, (uint16_t)n);
        g_tts_play_pos += n;

        // Free already-forwarded bytes from g_https_resp so long TTS
        // bodies don't overflow RESP_BUF_SIZE.
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
        // Note: do NOT clear g_tts_play_active here. core0 needs it as a
        // gate for its silence-pad logic and will clear it once playback
        // wraps up (which it then signals back via IC_MSG_TTS_PLAYED).
        return false;
    }
    return true;
}

//=============================================================================
// LCD layout & views
//=============================================================================
typedef enum {
    VIEW_STATUS,
    VIEW_BUTTONS,
    VIEW_LABS,
} view_state_t;

static volatile view_state_t g_view = VIEW_STATUS;

#define FONT_SCALE  2
#define ROW_H       22
#define ROW_Y0      28
#define TEXT_X      8
#define IND_X       160

//=============================================================================
// Status log (cross-core mini log on LCD)
//=============================================================================
// Match the LAB list font (FONT_SCALE=2, ROW_H=22). With ROW_Y0=28 the title
// "Boot" sits above; (240 - 28) / 22 ≈ 9 status rows fit on the 240px LCD.
#define STATUS_LINES    9
#define STATUS_LINE_LEN 24   // ~20 columns visible at scale 2, +slack
#define STATUS_FONT     2
#define STATUS_ROW_H    22
#define STATUS_ROW_Y0   28

static char g_status_lines[STATUS_LINES][STATUS_LINE_LEN];
static int  g_status_head  = 0;   // next slot to write (oldest is here when full)
static int  g_status_count = 0;
static volatile uint32_t g_status_version = 0;
auto_init_mutex(g_status_mutex);

static void set_status(const char *fmt, ...) {
    mutex_enter_blocking(&g_status_mutex);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status_lines[g_status_head], STATUS_LINE_LEN, fmt, ap);
    va_end(ap);
    int written = g_status_head;
    g_status_head = (g_status_head + 1) % STATUS_LINES;
    if (g_status_count < STATUS_LINES) g_status_count++;
    g_status_version++;
    mutex_exit(&g_status_mutex);

    // Slot is only re-used after STATUS_LINES more writes, so printf can safely
    // read it without holding the mutex.
    printf("[status] %s\n", g_status_lines[written]);
}

static void render_status_view(void) {
    lcd_fill(COLOR_BLACK);
    lcd_draw_text(8, 4, "Boot", FONT_SCALE, COLOR_CYAN, COLOR_BLACK);

    mutex_enter_blocking(&g_status_mutex);
    int n = g_status_count;
    int start = (g_status_head - n + STATUS_LINES) % STATUS_LINES;
    for (int i = 0; i < n; i++) {
        int idx = (start + i) % STATUS_LINES;
        int y = STATUS_ROW_Y0 + i * STATUS_ROW_H;
        lcd_draw_text(TEXT_X, y, g_status_lines[idx], STATUS_FONT, COLOR_WHITE, COLOR_BLACK);
    }
    mutex_exit(&g_status_mutex);
}

//-----------------------------------------------------------------------------
// Buttons (Waveshare Pico-LCD-1.3)
//   A=GP15, B=GP17, X=GP19, Y=GP21
//   UP=GP2, DOWN=GP18, LEFT=GP16, RIGHT=GP20, CTRL=GP3
//-----------------------------------------------------------------------------
typedef struct {
    const char *name;
    uint8_t     pin;
    bool        pressed;
} button_t;

static button_t g_buttons[] = {
    {"Button-A",  15, false},
    {"Button-B",  17, false},
    {"Button-X",  19, false},
    {"Button-Y",  21, false},
    {"Key-Up",     2, false},
    {"Key-Down",  18, false},
    {"Key-Left",  16, false},
    {"Key-Right", 20, false},
    {"Key-Ctrl",   3, false},
};
#define NUM_BUTTONS (sizeof(g_buttons)/sizeof(g_buttons[0]))

static void buttons_init(void) {
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        gpio_init(g_buttons[i].pin);
        gpio_set_dir(g_buttons[i].pin, GPIO_IN);
        gpio_pull_up(g_buttons[i].pin);
    }
}

static void draw_button_row(int row, const char *name, bool pressed) {
    int y = ROW_Y0 + row * ROW_H;
    uint16_t bg = pressed ? COLOR_GREEN : COLOR_BLACK;
    uint16_t fg = pressed ? COLOR_BLACK : COLOR_WHITE;
    lcd_fill_rect(0, y, LCD_W, ROW_H - 2, bg);
    lcd_draw_text(TEXT_X, y + 3, name, FONT_SCALE, fg, bg);
    lcd_draw_text(IND_X,  y + 3, pressed ? "ON " : "OFF", FONT_SCALE, fg, bg);
}

static void render_buttons_view(void) {
    lcd_fill(COLOR_BLACK);
    lcd_draw_text(8, 4, "PicoLCD-1.3 Buttons", FONT_SCALE, COLOR_CYAN, COLOR_BLACK);
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        draw_button_row((int)i, g_buttons[i].name, g_buttons[i].pressed);
    }
}

static void render_lab_loading(void) {
    lcd_fill(COLOR_BLACK);
    lcd_draw_text(8, 4, "Labs", FONT_SCALE, COLOR_CYAN, COLOR_BLACK);
    lcd_draw_text(8, ROW_Y0, "Fetching...", FONT_SCALE, COLOR_YELLOW, COLOR_BLACK);
}

static void render_lab_error(void) {
    lcd_fill(COLOR_BLACK);
    lcd_draw_text(8, 4, "Labs", FONT_SCALE, COLOR_CYAN, COLOR_BLACK);
    lcd_draw_text(8, ROW_Y0, "Fetch error", FONT_SCALE, COLOR_RED, COLOR_BLACK);
}

static void render_lab_list(void) {
    lcd_fill(COLOR_BLACK);
    const char *title = g_timeline_active
        ? "Labs (Enter=stop)"
        : "Labs (Enter=start)";
    lcd_draw_text(8, 4, title, FONT_SCALE, COLOR_CYAN, COLOR_BLACK);
    if (g_lab_count == 0) {
        lcd_draw_text(8, ROW_Y0, "(no labs)", FONT_SCALE, COLOR_WHITE, COLOR_BLACK);
        return;
    }
    for (int i = 0; i < g_lab_count; i++) {
        int y = ROW_Y0 + i * ROW_H;
        bool sel = (i == g_lab_selected);
        // Selection color signals lock state: green = selectable (UP/DOWN
        // moves it), orange = locked because timeline polling is in flight
        // (UP/DOWN ignored until Enter stops polling).
        uint16_t bg = sel
            ? (g_timeline_active ? COLOR_YELLOW : COLOR_GREEN)
            : COLOR_BLACK;
        uint16_t fg = sel ? COLOR_BLACK : COLOR_WHITE;
        lcd_fill_rect(0, y, LCD_W, ROW_H - 2, bg);
        lcd_draw_text(TEXT_X, y + 3, g_lab_ids[i], FONT_SCALE, fg, bg);
    }
}

// Top-right status indicators (LABS view):
//   [speaking icon (fast blink)] [polling spinner (slow rotate)]
// The speaking icon flashes at ~100ms while audio is actually coming out
// of the I2S DAC (g_tts_play_active). The polling spinner rotates at
// ~200ms while timeline polling is in flight (g_timeline_active).
static void render_lab_spinner(uint32_t slow_phase, bool fast_blink_on) {
    if (g_view != VIEW_LABS) return;
    const char *spinner_frames[] = {"|", "/", "-", "\\"};
    int sx = LCD_W - 14;   // polling spinner slot
    int ix = LCD_W - 28;   // speaking icon slot (left of spinner)

    // Polling spinner (slow rotation, only while timeline_active)
    lcd_fill_rect(sx, 4, 12, 16, COLOR_BLACK);
    if (g_timeline_active) {
        const char *frame = spinner_frames[slow_phase & 3];
        lcd_draw_text(sx, 4, frame, FONT_SCALE, COLOR_YELLOW, COLOR_BLACK);
    }

    // Speaking icon (fast blink, only while audio is actively playing)
    lcd_fill_rect(ix, 4, 12, 16, COLOR_BLACK);
    if (g_tts_play_active && fast_blink_on) {
        lcd_draw_text(ix, 4, "*", FONT_SCALE, COLOR_GREEN, COLOR_BLACK);
    }
}

// Trigger an HTTPS fetch from core0: render loading screen + signal core1.
// Ignored if a fetch is already in progress.
static void trigger_fetch(void) {
    if (g_lab_state == LAB_LOADING) return;
    g_lab_state = LAB_LOADING;
    g_view      = VIEW_LABS;
    render_lab_loading();
    // Tell core1: "user wants the lab list (button X / auto-fetch)".
    ic_send(IC_MSG_BTN_X, NULL, 0);
}

static void on_button_event(int idx, bool now_pressed) {
    if (!now_pressed) return;

    switch (idx) {
        case GUI_BTN_A:
            audio_play_sine();
            break;
        case GUI_BTN_B:
            audio_stop();
            break;
        case GUI_BTN_X:
            trigger_fetch();
            break;
        case GUI_BTN_Y:
            // Return to button view
            g_view = VIEW_BUTTONS;
            render_buttons_view();
            break;
        case GUI_KEY_UP:
            if (g_view == VIEW_LABS && g_lab_state == LAB_OK && g_lab_count > 0
                && !g_timeline_active) {   // locked while polling/playback active
                g_lab_selected = (g_lab_selected - 1 + g_lab_count) % g_lab_count;
                render_lab_list();
            }
            break;
        case GUI_KEY_DOWN:
            if (g_view == VIEW_LABS && g_lab_state == LAB_OK && g_lab_count > 0
                && !g_timeline_active) {   // locked while polling/playback active
                g_lab_selected = (g_lab_selected + 1) % g_lab_count;
                render_lab_list();
            }
            break;
        case GUI_KEY_CTRL:
            // "Enter": toggle timeline polling for the currently selected lab.
            if (g_view == VIEW_LABS && g_lab_state == LAB_OK && g_lab_count > 0) {
                if (!g_timeline_active) {
                    // Payload: the OPERATOR id (what core1 needs to build the
                    // /api/qa/timeline URL). lab_id is purely for the UI title.
                    strncpy(g_timeline_lab_id, g_lab_ids[g_lab_selected], MAX_LAB_ID_LEN - 1);
                    g_timeline_lab_id[MAX_LAB_ID_LEN - 1] = '\0';
                    const char *op = g_operator_ids[g_lab_selected];
                    ic_send(IC_MSG_TIMELINE_START, op, (uint16_t)strlen(op));
                    g_timeline_active = true;   // UI intent
                    printf("[core0] Enter: start timeline for lab=%s\n", g_timeline_lab_id);
                } else {
                    ic_send(IC_MSG_TIMELINE_STOP, NULL, 0);
                    g_timeline_active = false;  // UI intent
                    printf("[core0] Enter: stop timeline\n");
                }
                render_lab_list();
            }
            break;
        default:
            break;
    }
}

static void buttons_poll(void) {
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        bool now_pressed = !gpio_get(g_buttons[i].pin);  // active-low
        if (now_pressed != g_buttons[i].pressed) {
            g_buttons[i].pressed = now_pressed;
            printf("[core0] %-9s %s @ %lu ms\n",
                   g_buttons[i].name,
                   now_pressed ? "PRESS  " : "RELEASE",
                   (unsigned long)board_millis());

            if (g_view == VIEW_BUTTONS) {
                draw_button_row((int)i, g_buttons[i].name, now_pressed);
            }
            on_button_event((int)i, now_pressed);
        }
    }
}

//=============================================================================
// Core 1: CYW43 + WiFi + HTTPS
//=============================================================================
static int network_init(void) {
    if (wifi_init() != 0) return -1;
    srand(board_millis());

    char hostname[128];
    extract_hostname(API_URL, hostname, sizeof(hostname));
    if (dns_resolve_blocking(hostname, &g_api_ip) != 0) {
        printf("[net] DNS failed for %s\n", hostname);
        return -1;
    }
    strncpy(g_https_host, hostname, sizeof(g_https_host) - 1);
    g_https_host[sizeof(g_https_host) - 1] = '\0';
    return 0;
}

//=============================================================================
// Inter-core state machine
//=============================================================================
typedef enum {
    CORE0_S_INIT = 0,         // peripheral init already done in main(); just
                              // hand off to WAIT_NET on first dispatch
    CORE0_S_WAIT_NET,         // idle waiting for IC_MSG_NET_READY from core1
    CORE0_S_AUDIO_BEEP_PLAY,  // arm boot beep (audio_init + play_sine), record ts
    CORE0_S_AUDIO_BEEP_WAIT,  // wait ~300ms elapsed then audio_stop
    CORE0_S_AUDIO_TEST_START, // arm tick-driven sine streaming test
    CORE0_S_AUDIO_TEST_TICK,  // pump test sine until AUDIO_TEST_DONE
    CORE0_S_AUDIO_READY,      // notify core1 AUDIO_READY, hand off to RUNNING
    CORE0_S_RUNNING,          // steady-state main loop
} core0_state_t;

typedef enum {
    CORE1_S_INIT = 0,       // cyw43_arch_init
    CORE1_S_NETWORK,        // WiFi/DHCP/DNS bring-up
    CORE1_S_WAIT_AUDIO,     // idle waiting for IC_MSG_AUDIO_READY from core0
    CORE1_S_RUNNING,        // steady-state main loop
} core1_state_t;

static volatile core0_state_t g_core0_state = CORE0_S_INIT;
static volatile core1_state_t g_core1_state = CORE1_S_INIT;

// Inline payload buffer for FIFO message drain (sufficient for phase-2/3
// control messages; audio chunks in phase-4 will need a larger or peek-API).
#define IC_RECV_BUF_BYTES 1024
static uint8_t g_core0_recv_buf[IC_RECV_BUF_BYTES];
static uint8_t g_core1_recv_buf[IC_RECV_BUF_BYTES];

static void core0_loop(void);
static void on_core0_init(void);
static void on_core0_wait_net(void);
static void on_core0_audio_beep_play(void);
static void on_core0_audio_beep_wait(void);
static void on_core0_audio_test_start(void);
static void on_core0_audio_test_tick(void);
static void on_core0_audio_ready(void);
static void on_core0_running(void);
static void handle_core0_notify(ic_msg_t type, const uint8_t *payload, uint16_t length);

// Timestamp captured when the boot beep started; used by AUDIO_BEEP_WAIT to
// hold off audio_stop() until the beep duration has elapsed without
// blocking the event loop.
#define CORE0_BEEP_DURATION_MS 300
static uint32_t g_core0_beep_started_ms = 0;

static void on_core1_init(void);
static void on_core1_network(void);
static void on_core1_wait_audio(void);
static void on_core1_running(void);
static void handle_core1_notify(ic_msg_t type, const uint8_t *payload, uint16_t length);

static void core1_loop(void) {
    while (1) {
        ic_msg_t mtype; uint16_t mlen;
        while (ic_try_recv(&mtype, g_core1_recv_buf,
                           sizeof g_core1_recv_buf, &mlen) == 0) {
            handle_core1_notify(mtype, g_core1_recv_buf, mlen);
        }
        switch (g_core1_state) {
            case CORE1_S_INIT:       on_core1_init();       break;
            case CORE1_S_NETWORK:    on_core1_network();    break;
            case CORE1_S_WAIT_AUDIO: on_core1_wait_audio(); break;
            case CORE1_S_RUNNING:    on_core1_running();    break;
        }
    }
}

static void core1_entry(void) {
    core1_loop();
}

//=============================================================================
// Core 1 state handlers
//=============================================================================
static void on_core1_init(void) {
    if (cyw43_arch_init()) {
        printf("core1: cyw43_arch_init failed\n");
        while (1) sleep_ms(1000);   // hard stop
    }
    g_core1_state = CORE1_S_NETWORK;
}

static void on_core1_network(void) {
    if (network_init() != 0) {
        printf("core1: network init failed\n");
        set_status("Net: init failed");
        while (1) sleep_ms(1000);   // hard stop
    }
    set_status("Net: ready");
    printf("[core1] S0 done → notify core0 NET_READY\n");
    ic_send(IC_MSG_NET_READY, NULL, 0);
    g_core1_state = CORE1_S_WAIT_AUDIO;
}

static void on_core1_wait_audio(void) {
    // Idle — FIFO drain at top of core1_loop will deliver AUDIO_READY which
    // triggers the state transition. Yield a touch so we don't busy-spin.
    tight_loop_contents();
}

static void handle_core1_notify(ic_msg_t type, const uint8_t *payload, uint16_t length) {
    (void)payload; (void)length;
    switch (type) {
    case IC_MSG_AUDIO_READY:
        if (g_core1_state == CORE1_S_WAIT_AUDIO) {
            printf("[core1] S1 → entering main loop\n");
            g_core1_state = CORE1_S_RUNNING;
        }
        break;
    case IC_MSG_BTN_X:
        // Core0 asked for a lab-list refresh. Mark it pending; the running
        // state's main loop body will kick the HTTPS request when the
        // network layer is idle.
        g_core1_fetch_pending = true;
        break;
    case IC_MSG_TIMELINE_START: {
        // Payload = operator_id string (no NUL). Cache it for kick_timeline_
        // request(), reset paging state so the first poll uses start=0, and
        // arm the poll cadence.
        uint16_t n = length;
        if (n >= MAX_LAB_ID_LEN) n = MAX_LAB_ID_LEN - 1;
        memcpy(g_timeline_operator_id, payload, n);
        g_timeline_operator_id[n] = '\0';
        g_last_publish_id    = -1;
        g_timeline_last_poll = 0;
        g_core1_tl_active    = true;
        printf("[core1] TIMELINE_START operator=%s\n", g_timeline_operator_id);
        break;
    }
    case IC_MSG_TIMELINE_STOP:
        g_core1_tl_active = false;
        printf("[core1] TIMELINE_STOP\n");
        break;
    case IC_MSG_TTS_PLAYED:
        // core0 finished playing back the current TTS. Now safe to pop the
        // head of the queue so the next entry (or the next timeline poll)
        // can proceed. (IC_MSG_TTS_END will be Phase-4's core1→core0
        // "no more PCM bytes" signal — distinct event, opposite direction.)
        if (g_tts_play_active) g_tts_play_active = false;
        tts_queue_pop();
        printf("[core1] TTS_PLAYED (queue=%d)\n", tts_queue_count());
        break;
    default:
        printf("[core1] unhandled msg type=0x%02x in state=%d\n",
               type, (int)g_core1_state);
        break;
    }
}

static void on_core1_running(void) {
    static uint32_t last_dbg = 0;
    if (last_dbg == 0) last_dbg = board_millis();   // first iter init
    {
        cyw43_arch_poll();

        // Detect HTTPS completion first — frees g_https_resp for the next job.
        if (g_https_state == HC_DONE_OK || g_https_state == HC_DONE_ERR) {
            https_handle_done();
        }

        // Forward decoded PCM from g_https_resp → core0 via the ic_ring.
        // core1 owns the network/decoder side entirely now; core0 only
        // sees PCM bytes that arrive via IC_MSG_TTS_PCM_CHUNK and the final
        // IC_MSG_TTS_END marker. The pump is bounded (one chunk per call)
        // so ic_send backpressure can't starve cyw43_arch_poll.
        tts_forward_pump();

        // Timeline polling on/off is now driven by IC_MSG_TIMELINE_START
        // (with operator_id payload) and IC_MSG_TIMELINE_STOP, processed
        // in handle_core1_notify(). The actual GET cadence is enforced
        // further below against the core1-private g_core1_tl_active.

        // Only one HTTPS request at a time; never start a new one while audio
        // is still draining out of g_https_resp.
        bool https_busy = (g_https_state != HC_IDLE) || g_tts_play_active;

        // Lab-list fetch (Button-X / auto-fetch). The pending flag is set
        // from handle_core1_notify() on IC_MSG_BTN_X; we consume it here
        // when the HTTPS layer is free.
        if (g_core1_fetch_pending && !https_busy) {
            g_core1_fetch_pending = false;
            printf("[core1] HTTPS GET info\n");
            if (kick_info_request() != 0) http_set_state(HC_DONE_ERR);
        }

        // TTS queue processing — always, regardless of polling mode. This
        // lets a debug trigger (or any caller) push a message and have it
        // played even when timeline polling hasn't been started yet.
        if (!https_busy && tts_queue_count() > 0) {
            const char *msg = tts_queue_peek();
            if (msg) {
                printf("[core1] POST tts (queue=%d)\n", tts_queue_count());
                if (kick_tts_request(msg) != 0) {
                    tts_queue_pop();
                    http_set_state(HC_DONE_ERR);
                }
                https_busy = true;  // hold off polling kickoff this iter
            }
        }

        // Timeline polling: only when active + idle + interval elapsed.
        // (Gate is the core1-private flag set by IC_MSG_TIMELINE_START.)
        if (g_core1_tl_active && !https_busy && tts_queue_count() == 0) {
            uint32_t now = board_millis();
            if ((now - g_timeline_last_poll) >= TIMELINE_POLL_INTERVAL_MS) {
                g_timeline_last_poll = now;
                printf("[core1] GET timeline\n");
                if (kick_timeline_request() != 0) http_set_state(HC_DONE_ERR);
            }
        }

        // Debug: after the initial /api/tunnel/info fetch succeeds, push one
        // "こんにちは" through TTS so audio + POST path can be verified without
        // pressing the joystick CTRL key.
        static bool s_debug_tts_sent = false;
        if (!s_debug_tts_sent && g_lab_state == LAB_OK && !https_busy
            && !g_tts_play_active) {
            s_debug_tts_sent = true;
            printf("[core1] DEBUG: auto-queueing 'こんにちは'\n");
            tts_queue_push("こんにちは");
        }

        // Periodic state dump while a request is in flight
        if (g_https_state != HC_IDLE) {
            uint32_t now = board_millis();
            if ((now - last_dbg) >= 1000) {
                last_dbg = now;
                printf("[core1] https_state=%d mode=%d elapsed=%lums resp_len=%u tts_q=%d play=%d\n",
                       (int)g_https_state, (int)g_https_mode,
                       (unsigned long)(now - g_https_state_at),
                       (unsigned)g_https_resp_len,
                       tts_queue_count(), (int)g_tts_play_active);
            }
        } else {
            last_dbg = board_millis();
        }

        led_blink_loop();
        led_heartbeat_loop();
        sleep_us(500);
    }
}

//=============================================================================
// Main (Core 0: LCD + buttons)
//=============================================================================
int main() {
    // 153.6 MHz = 100 * 1.536 MHz → PIO clkdiv for 24kHz I2S is exactly 100,
    // eliminating the fractional-divider jitter that the default 150 MHz
    // clk_sys produces (div=97.65625 → BCK micro-jitter that the PCM5101A
    // PLL has to fight). Must be set BEFORE stdio_init_all() so UART baud
    // recalculates against the new sys clock.
    set_sys_clock_khz(153600, true);

    uart_init(uart0, 115200);
    gpio_set_function(0, GPIO_FUNC_UART);
    gpio_set_function(1, GPIO_FUNC_UART);
    stdio_init_all();

    sleep_ms(2000);
    g_time_boot = board_millis();
    printf("\n\n=== RP2350 HTTPS + PicoLCD-1.3 ===\n");
    printf("[TIMING] Boot complete: %lu ms\n", (unsigned long)g_time_boot);

    lcd_init();
    buttons_init();
    set_status("Boot %lums", (unsigned long)g_time_boot);
    render_status_view();

    // Bring up the inter-core message bus BEFORE launching core1 so its
    // very first ic_send() finds a clean ring/FIFO. Subsequent calls from
    // either core are no-ops thanks to the idempotent guard.
    ic_init();

    printf("Launching core1 for CYW43/WiFi/HTTPS...\n");
    multicore_launch_core1(core1_entry);

    // Hand off to the state-machine loop. core0_loop drains the FIFO
    // (notifications from core1) and dispatches per g_core0_state. The
    // state starts at CORE0_S_INIT which immediately transitions to
    // WAIT_NET; on receiving IC_MSG_NET_READY we run audio init, send
    // back IC_MSG_AUDIO_READY, and enter RUNNING.
    core0_loop();
    return 0;   // unreachable
}

//=============================================================================
// Core 0 dispatcher + state handlers
//=============================================================================
static void core0_loop(void) {
    while (1) {
        // Peek-gated FIFO drain. PCM_CHUNK messages are only popped when
        // g_core0_pcm_pending has room for a full chunk — otherwise we'd
        // either drop data or have to spin until tts_play_pump made space.
        // All other message types pop immediately so control latency
        // doesn't pile up behind buffered PCM.
        ic_msg_t mtype; uint16_t mlen;
        while (ic_peek_type(&mtype) == 0) {
            if (mtype == IC_MSG_TTS_PCM_CHUNK
                && g_core0_pcm_pending_len + TTS_FWD_CHUNK_SIZE
                       > TTS_PCM_PENDING_CAP) {
                break;   // leave it in the peek cache for next iter
            }
            ic_try_recv(&mtype, g_core0_recv_buf,
                        sizeof g_core0_recv_buf, &mlen);
            handle_core0_notify(mtype, g_core0_recv_buf, mlen);
        }
        switch (g_core0_state) {
            case CORE0_S_INIT:             on_core0_init();             break;
            case CORE0_S_WAIT_NET:         on_core0_wait_net();         break;
            case CORE0_S_AUDIO_BEEP_PLAY:  on_core0_audio_beep_play();  break;
            case CORE0_S_AUDIO_BEEP_WAIT:  on_core0_audio_beep_wait();  break;
            case CORE0_S_AUDIO_TEST_START: on_core0_audio_test_start(); break;
            case CORE0_S_AUDIO_TEST_TICK:  on_core0_audio_test_tick();  break;
            case CORE0_S_AUDIO_READY:      on_core0_audio_ready();      break;
            case CORE0_S_RUNNING:          on_core0_running();          break;
        }
    }
}

static void on_core0_init(void) {
    // Peripheral init (LCD/buttons/ic_init/launch core1) happens in main()
    // before core0_loop() is called, so the INIT state is just a transition
    // marker: hop straight to WAIT_NET.
    printf("[core0] S0: waiting for NET_READY from core1...\n");
    g_core0_state = CORE0_S_WAIT_NET;
}

static void on_core0_wait_net(void) {
    tight_loop_contents();   // FIFO drain at top of core0_loop wakes us
}

// Boot beep: arm PIO/DMA and start the sine tone. Records the start
// timestamp; the wait-state checks elapsed time without blocking.
// Audio init is safe here because cyw43_arch_init has already claimed
// its own PIO2 SM on core1.
static void on_core0_audio_beep_play(void) {
    printf("[core0] NET_READY received → audio_init + boot beep\n");
    audio_init();
    audio_play_sine();
    g_core0_beep_started_ms = board_millis();
    g_core0_state = CORE0_S_AUDIO_BEEP_WAIT;
}

static void on_core0_audio_beep_wait(void) {
    if (board_millis() - g_core0_beep_started_ms >= CORE0_BEEP_DURATION_MS) {
        audio_stop();
        printf("[core0] startup beep done → arming streaming test\n");
        g_core0_state = CORE0_S_AUDIO_TEST_START;
    }
}

static void on_core0_audio_test_start(void) {
    audio_test_stream_sine_begin();
    g_core0_state = CORE0_S_AUDIO_TEST_TICK;
}

// Pump the tick-driven streaming test. _tick returns RUNNING when the ring
// is full (we just yield) and DONE once all sine + silence blocks are in.
static void on_core0_audio_test_tick(void) {
    if (audio_test_stream_sine_tick() == AUDIO_TEST_DONE) {
        g_core0_state = CORE0_S_AUDIO_READY;
    }
}

static void on_core0_audio_ready(void) {
    printf("[core0] S0 done → notify core1 AUDIO_READY\n");
    ic_send(IC_MSG_AUDIO_READY, NULL, 0);
    g_core0_state = CORE0_S_RUNNING;
}

static void handle_core0_notify(ic_msg_t type, const uint8_t *payload, uint16_t length) {
    switch (type) {
    case IC_MSG_NET_READY:
        if (g_core0_state == CORE0_S_WAIT_NET) {
            g_core0_state = CORE0_S_AUDIO_BEEP_PLAY;
        }
        break;
    case IC_MSG_LABS_READY:
        // Core1 parsed the lab list. g_lab_state / g_lab_ids are already
        // populated; re-render the LABS view if it's currently showing.
        if (g_view == VIEW_LABS) {
            if (g_lab_state == LAB_OK)        render_lab_list();
            else if (g_lab_state == LAB_ERR)  render_lab_error();
        }
        break;
    case IC_MSG_TTS_PCM_CHUNK:
        // The drain gate in core0_loop holds back IC_MSG_TTS_PCM_CHUNK until
        // there's room for a full TTS_FWD_CHUNK_SIZE payload, so we never
        // overflow here. (The assertion catches bugs where someone routes
        // PCM_CHUNK around the gate.)
        if (g_core0_pcm_pending_len + length > TTS_PCM_PENDING_CAP) {
            printf("[core0] PCM_CHUNK overflow (have=%u +%u cap=%u) — dropping\n",
                   (unsigned)g_core0_pcm_pending_len, length,
                   (unsigned)TTS_PCM_PENDING_CAP);
            break;
        }
        memcpy(g_core0_pcm_pending + g_core0_pcm_pending_len, payload, length);
        g_core0_pcm_pending_len += length;
        break;
    case IC_MSG_TTS_END:
        g_core0_tts_stream_done = true;
        printf("[core0] TTS_END (pending=%u)\n",
               (unsigned)g_core0_pcm_pending_len);
        break;
    default:
        printf("[core0] unhandled msg type=0x%02x in state=%d\n",
               type, (int)g_core0_state);
        break;
    }
}

static void on_core0_running(void) {
    static uint32_t last_log = 0;
    static uint32_t last_status_ver = 0;
    static bool auto_fetched = false;
    if (last_log == 0) last_log = board_millis();
    {
        buttons_poll();

        // Audio consumer: drain g_core0_pcm_pending (filled by FIFO drain
        // from IC_MSG_TTS_PCM_CHUNK above) into the audio ring buffer.
        // Runs every core0 iteration so the DMA wrap tracker stays fresh
        // (need < 85ms cadence; core0 sleeps 10ms, well within).
        tts_play_pump();

        // Redraw status view when a new line arrives
        if (g_view == VIEW_STATUS) {
            uint32_t ver = g_status_version;
            if (ver != last_status_ver) {
                last_status_ver = ver;
                render_status_view();
            }
        }

        // Auto-fetch once network comes up. Entering CORE0_S_RUNNING already
        // implies AUDIO_READY → NET_READY happened, so just gate on the
        // one-shot.
        if (!auto_fetched) {
            auto_fetched = true;
            mem_barrier();
            printf("[core0] auto-fetch triggered\n");
            trigger_fetch();
        }

        // Lab-list refresh is now driven by IC_MSG_LABS_READY in
        // handle_core0_notify() (replaces the old g_labs_ready poll).

        uint32_t now = board_millis();

        // Re-paint the LAB list when polling state flips so the selected
        // row color updates (green = selectable / yellow = locked).
        static bool last_tl_active_ui = false;
        if (g_timeline_active != last_tl_active_ui) {
            last_tl_active_ui = g_timeline_active;
            if (g_view == VIEW_LABS && g_lab_state == LAB_OK) render_lab_list();
        }

        // Two indicators on the LABS view, two cadences:
        //   spinner = 200ms (polling activity)
        //   speaking icon = 100ms (audio actually coming out of I2S)
        static uint32_t spinner_last  = 0;
        static uint32_t spinner_phase = 0;
        static uint32_t fast_last     = 0;
        static bool     fast_blink    = false;
        static int last_tl_status_ver = 0;
        if ((now - spinner_last) >= 200) {
            spinner_last = now;
            if (g_timeline_active) spinner_phase++;
            if (g_timeline_status_ver != last_tl_status_ver && g_view == VIEW_LABS) {
                last_tl_status_ver = g_timeline_status_ver;
                set_status("TL: pid=%lld q=%d", (long long)g_last_publish_id,
                           tts_queue_count());
            }
        }
        if ((now - fast_last) >= 100) {
            fast_last = now;
            fast_blink = !fast_blink;
        }
        // Re-paint each frame; render_lab_spinner clears its slots itself so
        // both indicators flicker independently at their own cadences.
        if (g_view == VIEW_LABS && (g_timeline_active || g_tts_play_active)) {
            render_lab_spinner(spinner_phase, fast_blink);
        }

        if ((now - last_log) >= 1000) {
            last_log = now;
            printf("[core0] t=%lu ms view=%d lab_state=%d count=%d sel=%d tl_active=%d\n",
                   (unsigned long)now, (int)g_view, (int)g_lab_state,
                   g_lab_count, g_lab_selected, (int)g_timeline_active);
        }
        sleep_ms(10);
    }
}
