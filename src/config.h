#ifndef CONFIG_H_
#define CONFIG_H_

// uncomment this if you want to handshake with a aiortc
// #define CONFIG_DTLS_USE_ECDSA 1

#define SCTP_MTU (1200)
#define CONFIG_MTU (1300)

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

#ifndef CONFIG_KEEPALIVE_TIMEOUT
#define CONFIG_KEEPALIVE_TIMEOUT 10000
#endif

#ifndef CONFIG_AUDIO_DURATION
#define CONFIG_AUDIO_DURATION 20
#endif

#ifndef CONFIG_MAX_NALU_SIZE
#define CONFIG_MAX_NALU_SIZE (10 * 1024)  // 10KB
#endif

// One reassembled AV1 temporal unit. A keyframe is far bigger than a NALU,
// so this gets its own budget; shrink it on targets where the BSS matters.
#ifndef CONFIG_MAX_AV1_TU_SIZE
#define CONFIG_MAX_AV1_TU_SIZE (256 * 1024)  // 256KB
#endif

// How often an established connection sends an RFC 7675 consent check. Has to
// stay well under CONFIG_KEEPALIVE_TIMEOUT so a lost check still leaves room
// for the next one to be answered. 0 disables it.
#ifndef CONFIG_CONSENT_INTERVAL
#define CONFIG_CONSENT_INTERVAL 3000
#endif

// CONFIG_MTU bounds what we send. What we receive is bounded by the peer, and
// an SFU happily sends larger datagrams. recvfrom truncates silently, so a
// receive buffer sized to CONFIG_MTU loses the tail of every full-size packet
// -- including the SRTP auth tag, which turns into srtp_err_status_auth_fail.
#ifndef CONFIG_RECV_BUFFER_SIZE
#define CONFIG_RECV_BUFFER_SIZE 2048
#endif

// AV1 の depacketizer が並べ替えを吸収するために持つ packet 数。SFU は隣接
// する 2 つを入れ替えて届けることがあり、順番どおりにしか読めない
// depacketizer はそれを欠落と見なして次のキーフレームまで捨ててしまう。
// 順番どおりに届いている限り 1 つも保持しないので、遅延は増えない。
// CONFIG_RECV_BUFFER_SIZE x この数だけ BSS を使う
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
