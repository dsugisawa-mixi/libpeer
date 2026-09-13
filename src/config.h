#ifndef CONFIG_H_
#define CONFIG_H_

// uncomment this if you want to handshake with a aiortc
// #define CONFIG_DTLS_USE_ECDSA 1

#define SCTP_MTU (1200)
#define CONFIG_MTU (1300)

#ifndef CONFIG_USE_ZEPHYR
#ifdef __ZEPHYR__
#define CONFIG_USE_ZEPHYR 1
#else
#define CONFIG_USE_ZEPHYR 0
#endif
#endif

#ifndef CONFIG_USE_LWIP
#define CONFIG_USE_LWIP 0
#endif

#ifndef CONFIG_MBEDTLS_DEBUG
#define CONFIG_MBEDTLS_DEBUG 0
#endif

#ifndef CONFIG_MBEDTLS_2_X
#define CONFIG_MBEDTLS_2_X 0
#endif

#if CONFIG_MBEDTLS_2_X
#define RSA_KEY_LENGTH 512
#else
#define RSA_KEY_LENGTH 1024
#endif

#ifndef CONFIG_DTLS_USE_ECDSA
#define CONFIG_DTLS_USE_ECDSA 0
#endif

#ifndef CONFIG_USE_USRSCTP
#define CONFIG_USE_USRSCTP 1
#endif

#ifndef CONFIG_SDP_BUFFER_SIZE
#define CONFIG_SDP_BUFFER_SIZE 8096
#endif

#ifndef CONFIG_MQTT_BUFFER_SIZE
#define CONFIG_MQTT_BUFFER_SIZE 4096
#endif

#ifndef CONFIG_HTTP_BUFFER_SIZE
#define CONFIG_HTTP_BUFFER_SIZE 4096
#endif

#ifndef CONFIG_TLS_READ_TIMEOUT
#define CONFIG_TLS_READ_TIMEOUT 3000
#endif

#ifndef CONFIG_STUN_KEEPALIVE_INTERVAL
#define CONFIG_STUN_KEEPALIVE_INTERVAL 0
#endif

#ifndef CONFIG_STUN_KEEPALIVE_TIMEOUT
#define CONFIG_STUN_KEEPALIVE_TIMEOUT 15000
#endif

#if CONFIG_STUN_KEEPALIVE_INTERVAL > 0 && \
    CONFIG_STUN_KEEPALIVE_TIMEOUT <= CONFIG_STUN_KEEPALIVE_INTERVAL
#error "CONFIG_STUN_KEEPALIVE_TIMEOUT must be greater than CONFIG_STUN_KEEPALIVE_INTERVAL"
#endif

#ifndef CONFIG_AUDIO_DURATION
#define CONFIG_AUDIO_DURATION 20
#endif

#ifndef CONFIG_MAX_NALU_SIZE
#define CONFIG_MAX_NALU_SIZE (100 * 1024)  // 100KB
#endif

// One reassembled AV1 temporal unit. A keyframe is far bigger than a NALU,
// so this gets its own budget; shrink it on targets where the BSS matters.
#ifndef CONFIG_MAX_AV1_TU_SIZE
#define CONFIG_MAX_AV1_TU_SIZE (256 * 1024)  // 256KB
#endif

// CONFIG_MTU bounds what we send. What we receive is bounded by the peer, and
// an SFU happily sends larger datagrams. recvfrom truncates silently, so a
// receive buffer sized to CONFIG_MTU loses the tail of every full-size packet
// -- including the SRTP auth tag, which turns into srtp_err_status_auth_fail.
#ifndef CONFIG_RECV_BUFFER_SIZE
#define CONFIG_RECV_BUFFER_SIZE 2048
#endif

// How many packets the AV1 depacketizer holds back to absorb reordering. An
// SFU may deliver two adjacent packets swapped, and a depacketizer that can
// only read them in order takes that for a loss and discards everything until
// the next keyframe. Nothing is held while packets arrive in order, so this
// costs no latency in the normal case.
// Uses CONFIG_RECV_BUFFER_SIZE x this many bytes of BSS.
#ifndef CONFIG_AV1_REORDER_DEPTH
#define CONFIG_AV1_REORDER_DEPTH 4
#endif

#define CONFIG_IPV6 0
// empty will use first active interface
#define CONFIG_IFACE_PREFIX ""

// #define LOG_LEVEL LEVEL_DEBUG
#ifndef LOG_REDIRECT
#define LOG_REDIRECT 0
#endif

// Disable MQTT and HTTP signaling
// #define DISABLE_PEER_SIGNALING 1

#endif  // CONFIG_H_
