#ifndef DISABLE_PEER_SIGNALING
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#include "config.h"
#include "ports.h"
#include "tcp_transport.h"
#include "utils.h"

int tcp_transport_connect(PlainNetworkContext_t* net_ctx,
                          const char* host,
                          uint16_t port) {
  int ret;
  Address resolved_addr;

  memset(&resolved_addr, 0, sizeof(resolved_addr));
  tcp_socket_open(&net_ctx->tcp_socket, AF_INET);
  ports_resolve_addr(host, &resolved_addr);
  addr_set_port(&resolved_addr, port);
  if ((ret = tcp_socket_connect(&net_ctx->tcp_socket, &resolved_addr) < 0)) {
    return -1;
  }

  LOGI("TCP connected to %s:%d", host, port);
  return 0;
}

void tcp_transport_disconnect(PlainNetworkContext_t* net_ctx) {
  tcp_socket_close(&net_ctx->tcp_socket);
}

int32_t tcp_transport_recv(PlainNetworkContext_t* net_ctx, void* buf, size_t len) {
  int ret;
  fd_set read_fds;
  struct timeval tv;

  tv.tv_sec = CONFIG_TLS_READ_TIMEOUT / 1000;
  tv.tv_usec = (CONFIG_TLS_READ_TIMEOUT % 1000) * 1000;

  FD_ZERO(&read_fds);
  FD_SET(net_ctx->tcp_socket.fd, &read_fds);

  ret = select(net_ctx->tcp_socket.fd + 1, &read_fds, NULL, NULL, &tv);
  if (ret < 0) {
    return -1;
  } else if (ret == 0) {
    return 0;  // timeout
  }

  if (FD_ISSET(net_ctx->tcp_socket.fd, &read_fds)) {
    memset(buf, 0, len);
    ret = tcp_socket_recv(&net_ctx->tcp_socket, buf, len);
  }

  return ret;
}

int32_t tcp_transport_send(PlainNetworkContext_t* net_ctx, const void* buf, size_t len) {
  return tcp_socket_send(&net_ctx->tcp_socket, buf, len);
}
#endif  // DISABLE_PEER_SIGNALING
