// WiFi initialization and DNS resolution.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "lwip/ip_addr.h"

// set_status is declared in gui.h — include it for wifi.c to call.
#include "gui.h"

// Connect to the configured AP (blocking, with 30 s timeout).
int wifi_init(void);

// Extract hostname from a URL (e.g. "https://host:port/path" -> "host").
void extract_hostname(const char *url, char *hostname, size_t hostname_size);

// Resolve `hostname` via lwIP DNS (blocking, 10 s timeout).
int dns_resolve_blocking(const char *hostname, ip_addr_t *out);

// Resolved API server address (set by network_init).
extern ip_addr_t g_api_ip;

// wifi_init + DNS resolve + populate g_api_ip / g_https_host.
int network_init(void);
