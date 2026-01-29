#ifndef TCP_TRANSPORT_H_
#define TCP_TRANSPORT_H_

#ifndef DISABLE_PEER_SIGNALING

#include <stdint.h>

#include "socket.h"
#include "transport_interface.h"

struct PlainNetworkContext {
  TcpSocket tcp_socket;
};

typedef struct PlainNetworkContext PlainNetworkContext_t;

int tcp_transport_connect(PlainNetworkContext_t* net_ctx,
                          const char* host,
                          uint16_t port);

void tcp_transport_disconnect(PlainNetworkContext_t* net_ctx);

int32_t tcp_transport_recv(PlainNetworkContext_t* net_ctx, void* buf, size_t len);

int32_t tcp_transport_send(PlainNetworkContext_t* net_ctx, const void* buf, size_t len);

#endif  // DISABLE_PEER_SIGNALING
#endif  // TCP_TRANSPORT_H_
