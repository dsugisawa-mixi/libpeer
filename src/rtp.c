#include <stdio.h>
#include <string.h>

#include "address.h"
#include "config.h"
#include "peer_connection.h"
#include "rtp.h"
#include "utils.h"

typedef enum RtpH264Type {

  NALU = 23,
  FU_A = 28,

} RtpH264Type;

typedef struct NaluHeader {
  uint8_t type : 5;
  uint8_t nri : 2;
  uint8_t f : 1;
} NaluHeader;

typedef struct FuHeader {
  uint8_t type : 5;
  uint8_t r : 1;
  uint8_t e : 1;
  uint8_t s : 1;
} FuHeader;

#define RTP_PAYLOAD_SIZE (CONFIG_MTU - sizeof(RtpHeader))
#define FU_PAYLOAD_SIZE (CONFIG_MTU - sizeof(RtpHeader) - sizeof(FuHeader) - sizeof(NaluHeader))

int rtp_packet_validate(uint8_t* packet, size_t size) {
  if (size < 12)
    return 0;

  RtpHeader* rtp_header = (RtpHeader*)packet;
  return ((rtp_header->type < 64) || (rtp_header->type >= 96));
}

uint32_t rtp_get_ssrc(uint8_t* packet) {
  RtpHeader* rtp_header = (RtpHeader*)packet;
  return ntohl(rtp_header->ssrc);
}

static int rtp_encoder_encode_h264_single(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  RtpPacket* rtp_packet = (RtpPacket*)rtp_encoder->buf;

  rtp_packet->header.version = 2;
  rtp_packet->header.padding = 0;
  rtp_packet->header.extension = 0;
  rtp_packet->header.csrccount = 0;
  rtp_packet->header.markerbit = 0;
  rtp_packet->header.type = rtp_encoder->type;
  rtp_packet->header.seq_number = htons(rtp_encoder->seq_number++);
  rtp_packet->header.timestamp = htonl(rtp_encoder->timestamp);
  rtp_packet->header.ssrc = htonl(rtp_encoder->ssrc);

  // I frame and P frame
  if ((*buf & 0x1f) == 0x05 || (*buf & 0x1f) == 0x01) {
    rtp_packet->header.markerbit = 1;
    rtp_encoder->timestamp += rtp_encoder->timestamp_increment;
  }
#if 0
  LOGI("markbit: %d, timestamp: %d, nalu type: %d", rtp_packet->header.markerbit, rtp_encoder->timestamp, buf[0] & 0x1f);
#endif

  memcpy(rtp_packet->payload, buf, size);
  rtp_encoder->on_packet(rtp_encoder->buf, size + sizeof(RtpHeader), rtp_encoder->user_data);
  return 0;
}

static int rtp_encoder_encode_h264_fu_a(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  RtpPacket* rtp_packet = (RtpPacket*)rtp_encoder->buf;

  rtp_packet->header.version = 2;
  rtp_packet->header.padding = 0;
  rtp_packet->header.extension = 0;
  rtp_packet->header.csrccount = 0;
  rtp_packet->header.markerbit = 0;
  rtp_packet->header.type = rtp_encoder->type;
  rtp_packet->header.timestamp = htonl(rtp_encoder->timestamp);
  rtp_packet->header.ssrc = htonl(rtp_encoder->ssrc);
  uint8_t type = buf[0] & 0x1f;
  uint8_t nri = (buf[0] & 0x60) >> 5;
  buf = buf + 1;
  size = size - 1;

  // increase timestamp if I, P frame
  if (type == 0x05 || type == 0x01) {
    rtp_encoder->timestamp += rtp_encoder->timestamp_increment;
  }

  NaluHeader* fu_indicator = (NaluHeader*)rtp_packet->payload;
  FuHeader* fu_header = (FuHeader*)rtp_packet->payload + sizeof(NaluHeader);
  fu_header->s = 1;

  while (size > 0) {
    fu_indicator->type = FU_A;
    fu_indicator->nri = nri;
    fu_indicator->f = 0;
    fu_header->type = type;
    fu_header->r = 0;
    rtp_packet->header.seq_number = htons(rtp_encoder->seq_number++);

    if (size <= FU_PAYLOAD_SIZE) {
      fu_header->e = 1;
      rtp_packet->header.markerbit = 1;
      memcpy(rtp_packet->payload + sizeof(NaluHeader) + sizeof(FuHeader), buf, size);
      rtp_encoder->on_packet(rtp_encoder->buf, size + sizeof(RtpHeader) + sizeof(NaluHeader) + sizeof(FuHeader), rtp_encoder->user_data);
      break;
    }

    fu_header->e = 0;

    memcpy(rtp_packet->payload + sizeof(NaluHeader) + sizeof(FuHeader), buf, FU_PAYLOAD_SIZE);
    rtp_encoder->on_packet(rtp_encoder->buf, CONFIG_MTU, rtp_encoder->user_data);
    size -= FU_PAYLOAD_SIZE;
    buf += FU_PAYLOAD_SIZE;

    fu_header->s = 0;
  }
  return 0;
}

static uint8_t* h264_find_nalu(uint8_t* buf_start, uint8_t* buf_end) {
  uint8_t* p = buf_start + 2;

  while (p < buf_end) {
    if (*(p - 2) == 0x00 && *(p - 1) == 0x00 && *p == 0x01)
      return p + 1;
    p++;
  }

  return buf_end;
}

static int rtp_encoder_encode_h264(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  uint8_t* buf_end = buf + size;
  uint8_t *pstart, *pend;
  size_t nalu_size;

  for (pstart = h264_find_nalu(buf, buf_end); pstart < buf_end; pstart = pend) {
    pend = h264_find_nalu(pstart, buf_end);
    nalu_size = pend - pstart;

    if (pend != buf_end)
      nalu_size--;

    while (pstart[nalu_size - 1] == 0x00)
      nalu_size--;

    if (nalu_size <= RTP_PAYLOAD_SIZE) {
      rtp_encoder_encode_h264_single(rtp_encoder, pstart, nalu_size);

    } else {
      rtp_encoder_encode_h264_fu_a(rtp_encoder, pstart, nalu_size);
    }
  }

  return 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  AV1 (RTP Payload Format For AV1, v1.0.0)
 *
 *  The encoder hands us one temporal unit in the low-overhead bitstream
 *  format: a sequence of OBUs, each carrying its own obu_size field.  On the
 *  wire, every RTP packet looks like this:
 *
 *     0 1 2 3 4 5 6 7
 *    +-+-+-+-+-+-+-+-+
 *    |Z|Y| W |N|-|-|-|  aggregation header
 *    +-+-+-+-+-+-+-+-+
 *    | OBU element 1 |  leb128(length) + OBU, where the OBU header has
 *    |      ...      |  obu_has_size_field cleared and no obu_size field
 *
 *  Z  first element continues an OBU fragmented from the previous packet
 *  Y  last element continues into the next packet
 *  W  number of elements without a length prefix; 0 = every element is
 *     prefixed with its leb128 length (what this implementation emits)
 *  N  first packet of a new coded video sequence (keyframe w/ sequence header)
 * ────────────────────────────────────────────────────────────────────────── */

#define OBU_TYPE_SEQUENCE_HEADER 1
#define OBU_TYPE_TEMPORAL_DELIMITER 2
#define OBU_TYPE_TILE_LIST 8
#define OBU_TYPE_PADDING 15

#define OBU_HDR_TYPE(h) (((h) >> 3) & 0x0f)
#define OBU_HDR_HAS_EXTENSION(h) (((h) >> 2) & 0x01)
#define OBU_HDR_HAS_SIZE_FIELD(h) (((h) >> 1) & 0x01)

#define AV1_MAX_OBU_PER_TU 32
#define AV1_AGGR_HEADER_SIZE 1
#define AV1_PAYLOAD_SIZE (CONFIG_MTU - sizeof(RtpHeader) - AV1_AGGR_HEADER_SIZE)

typedef struct Av1ObuElement {
  uint8_t hdr[2];  // OBU header with obu_has_size_field cleared
  size_t hdr_size;
  const uint8_t* payload;
  size_t payload_size;
} Av1ObuElement;

static size_t av1_leb128_size(size_t value) {
  size_t bytes = 1;
  while (value >= 0x80) {
    value >>= 7;
    bytes++;
  }
  return bytes;
}

static size_t av1_leb128_write(uint8_t* buf, size_t value) {
  size_t bytes = 0;
  do {
    buf[bytes] = value & 0x7f;
    value >>= 7;
    if (value) {
      buf[bytes] |= 0x80;
    }
    bytes++;
  } while (value);
  return bytes;
}

static int av1_leb128_read(const uint8_t* buf, size_t size, size_t* value, size_t* bytes) {
  size_t v = 0;
  for (size_t i = 0; i < 8 && i < size; i++) {
    v |= ((size_t)(buf[i] & 0x7f)) << (i * 7);
    if (!(buf[i] & 0x80)) {
      *value = v;
      *bytes = i + 1;
      return 0;
    }
  }
  return -1;
}

// copy n bytes out of the virtual concatenation of (OBU header, OBU payload)
static void av1_element_copy(const Av1ObuElement* element, size_t offset, uint8_t* dst, size_t n) {
  size_t copied = 0;

  while (copied < n && offset + copied < element->hdr_size) {
    dst[copied] = element->hdr[offset + copied];
    copied++;
  }

  if (copied < n) {
    memcpy(dst + copied, element->payload + (offset + copied - element->hdr_size), n - copied);
  }
}

// split a temporal unit into the OBU elements that go on the wire
static int av1_parse_temporal_unit(const uint8_t* buf,
                                   size_t size,
                                   Av1ObuElement* elements,
                                   int max_elements,
                                   int* new_coded_video_sequence) {
  size_t pos = 0;
  int count = 0;

  *new_coded_video_sequence = 0;

  while (pos < size) {
    uint8_t hdr = buf[pos];
    size_t hdr_size = 1 + (OBU_HDR_HAS_EXTENSION(hdr) ? 1 : 0);
    size_t len_size = 0;
    size_t payload_size;
    size_t consumed;

    if (pos + hdr_size > size) {
      LOGE("AV1: truncated OBU header");
      return -1;
    }

    if (OBU_HDR_HAS_SIZE_FIELD(hdr)) {
      if (av1_leb128_read(buf + pos + hdr_size, size - pos - hdr_size, &payload_size, &len_size) != 0) {
        LOGE("AV1: malformed obu_size");
        return -1;
      }
      consumed = hdr_size + len_size + payload_size;
    } else {
      // without obu_size the OBU runs to the end of the temporal unit
      payload_size = size - pos - hdr_size;
      consumed = size - pos;
    }

    if (consumed > size - pos) {
      LOGE("AV1: OBU overruns the temporal unit");
      return -1;
    }

    switch (OBU_HDR_TYPE(hdr)) {
      // the temporal delimiter is rebuilt by the receiver, tile lists and
      // padding are never forwarded (RTP payload format, section 5)
      case OBU_TYPE_TEMPORAL_DELIMITER:
      case OBU_TYPE_TILE_LIST:
      case OBU_TYPE_PADDING:
        break;

      default:
        if (count >= max_elements) {
          LOGE("AV1: more than %d OBUs in one temporal unit", max_elements);
          return -1;
        }
        if (OBU_HDR_TYPE(hdr) == OBU_TYPE_SEQUENCE_HEADER) {
          *new_coded_video_sequence = 1;
        }
        elements[count].hdr[0] = hdr & ~0x02u;  // clear obu_has_size_field
        elements[count].hdr[1] = hdr_size > 1 ? buf[pos + 1] : 0;
        elements[count].hdr_size = hdr_size;
        elements[count].payload = buf + pos + hdr_size + len_size;
        elements[count].payload_size = payload_size;
        count++;
        break;
    }

    pos += consumed;
  }

  return count;
}

static int rtp_encoder_encode_av1(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  RtpPacket* rtp_packet = (RtpPacket*)rtp_encoder->buf;
  Av1ObuElement elements[AV1_MAX_OBU_PER_TU];
  int new_coded_video_sequence = 0;
  int n_elements;
  int index = 0;      // element being packetized
  size_t offset = 0;  // how much of that element is already sent
  int z = 0;
  int first_packet = 1;

  n_elements = av1_parse_temporal_unit(buf, size, elements, AV1_MAX_OBU_PER_TU, &new_coded_video_sequence);
  if (n_elements <= 0) {
    return n_elements;
  }

  while (index < n_elements) {
    uint8_t* payload = rtp_packet->payload + AV1_AGGR_HEADER_SIZE;
    size_t payload_len = 0;
    int y = 0;

    while (index < n_elements) {
      size_t element_len = elements[index].hdr_size + elements[index].payload_size;
      size_t remaining = element_len - offset;
      size_t room = AV1_PAYLOAD_SIZE - payload_len;
      size_t chunk = remaining;

      if (room < 3) {
        break;  // not enough room left for a length field plus payload
      }

      if (av1_leb128_size(chunk) + chunk > room) {
        chunk = room - 2;  // chunk < 16384, so its length field is 2 bytes at most
      }

      payload_len += av1_leb128_write(payload + payload_len, chunk);
      av1_element_copy(&elements[index], offset, payload + payload_len, chunk);
      payload_len += chunk;
      offset += chunk;

      if (offset < element_len) {
        y = 1;  // this element continues in the next packet
        break;
      }

      offset = 0;
      index++;
    }

    rtp_packet->payload[0] = (uint8_t)((z << 7) | (y << 6) |  // W = 0: every element carries its length
                                       ((first_packet && new_coded_video_sequence) ? 0x08 : 0x00));

    rtp_packet->header.version = 2;
    rtp_packet->header.padding = 0;
    rtp_packet->header.extension = 0;
    rtp_packet->header.csrccount = 0;
    rtp_packet->header.type = rtp_encoder->type;
    rtp_packet->header.seq_number = htons(rtp_encoder->seq_number++);
    rtp_packet->header.timestamp = htonl(rtp_encoder->timestamp);
    rtp_packet->header.ssrc = htonl(rtp_encoder->ssrc);
    rtp_packet->header.markerbit = (index == n_elements) ? 1 : 0;  // last packet of the temporal unit

    rtp_encoder->on_packet(rtp_encoder->buf, sizeof(RtpHeader) + AV1_AGGR_HEADER_SIZE + payload_len, rtp_encoder->user_data);

    z = (offset > 0);
    first_packet = 0;
  }

  rtp_encoder->timestamp += rtp_encoder->timestamp_increment;
  return 0;
}

static int rtp_encoder_encode_generic(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  RtpHeader* rtp_header = (RtpHeader*)rtp_encoder->buf;
  rtp_header->version = 2;
  rtp_header->padding = 0;
  rtp_header->extension = 0;
  rtp_header->csrccount = 0;
  rtp_header->markerbit = 0;
  rtp_header->type = rtp_encoder->type;
  rtp_header->seq_number = htons(rtp_encoder->seq_number++);
  rtp_header->timestamp = htonl(rtp_encoder->timestamp);
  rtp_encoder->timestamp += rtp_encoder->timestamp_increment;
  rtp_header->ssrc = htonl(rtp_encoder->ssrc);
  memcpy(rtp_encoder->buf + sizeof(RtpHeader), buf, size);

  rtp_encoder->on_packet(rtp_encoder->buf, size + sizeof(RtpHeader), rtp_encoder->user_data);

  return 0;
}

void rtp_encoder_init(RtpEncoder* rtp_encoder, MediaCodec codec, RtpOnPacket on_packet, void* user_data) {
  rtp_encoder->on_packet = on_packet;
  rtp_encoder->user_data = user_data;
  rtp_encoder->timestamp = 0;
  rtp_encoder->seq_number = 0;

  switch (codec) {
    case CODEC_H264:
      rtp_encoder->type = PT_H264;
      rtp_encoder->ssrc = SSRC_H264;
      rtp_encoder->timestamp_increment = 90000 / 30;  // 30 FPS.
      rtp_encoder->encode_func = rtp_encoder_encode_h264;
      break;
    case CODEC_AV1:
      rtp_encoder->type = PT_AV1;
      rtp_encoder->ssrc = SSRC_AV1;
      rtp_encoder->timestamp_increment = 90000 / 30;  // 30 FPS.
      rtp_encoder->encode_func = rtp_encoder_encode_av1;
      break;
    case CODEC_PCMA:
      rtp_encoder->type = PT_PCMA;
      rtp_encoder->ssrc = SSRC_PCMA;
      rtp_encoder->timestamp_increment = CONFIG_AUDIO_DURATION * 8000 / 1000;
      rtp_encoder->encode_func = rtp_encoder_encode_generic;
      break;
    case CODEC_PCMU:
      rtp_encoder->type = PT_PCMU;
      rtp_encoder->ssrc = SSRC_PCMU;
      rtp_encoder->timestamp_increment = CONFIG_AUDIO_DURATION * 8000 / 1000;
      rtp_encoder->encode_func = rtp_encoder_encode_generic;
      break;
    case CODEC_OPUS:
      rtp_encoder->type = PT_OPUS;
      rtp_encoder->ssrc = SSRC_OPUS;
      rtp_encoder->timestamp_increment = CONFIG_AUDIO_DURATION * 48000 / 1000;
      rtp_encoder->encode_func = rtp_encoder_encode_generic;
      break;
    default:
      break;
  }
}

int rtp_encoder_encode(RtpEncoder* rtp_encoder, const uint8_t* buf, size_t size) {
  if (rtp_encoder->encode_func == NULL) {
    LOGE("no rtp packetizer for this codec");
    return -1;
  }
  return rtp_encoder->encode_func(rtp_encoder, (uint8_t*)buf, size);
}

void rtp_encoder_set_timestamp(RtpEncoder* rtp_encoder, uint32_t timestamp) {
  rtp_encoder->timestamp = timestamp;
}

static int rtp_decode_h264(RtpDecoder* rtp_decoder, uint8_t* buf, size_t size) {
  static const uint32_t nalu_start_4bytecode = 0x01000000;
  static uint8_t nalu_buf[CONFIG_MAX_NALU_SIZE];
  static int offset = 0;
  RtpPacket* rtp_packet = (RtpPacket*)buf;
  uint8_t nalu_type = *rtp_packet->payload & 0x1f;
  int payload_size = size - sizeof(RtpHeader);
  if (nalu_type > 0 && nalu_type < 24) {
    // NALU type 1-23 are single NALUs
    memcpy(nalu_buf, &nalu_start_4bytecode, sizeof(nalu_start_4bytecode));
    offset = sizeof(nalu_start_4bytecode);
    memcpy(nalu_buf + offset, rtp_packet->payload, payload_size);
    offset += payload_size;
    if (rtp_decoder->on_packet != NULL) {
      rtp_decoder->on_packet(nalu_buf, offset, rtp_decoder->user_data);
    }
    return (int)size;
  } else {
    NaluHeader* fu_indicator = (NaluHeader*)rtp_packet->payload;
    FuHeader* fu_header = (FuHeader*)(rtp_packet->payload + sizeof(NaluHeader));
    uint8_t reconstructed_nalu_type = (fu_indicator->f << 7) |
                                      (fu_indicator->nri << 5) |
                                      fu_header->type;
    payload_size -= sizeof(NaluHeader) + sizeof(FuHeader);
    if (fu_header->s) {
      memcpy(nalu_buf, &nalu_start_4bytecode, sizeof(nalu_start_4bytecode));
      offset = sizeof(nalu_start_4bytecode);
      memcpy(nalu_buf + offset, &reconstructed_nalu_type, 1);
      offset += 1;
      memcpy(nalu_buf + offset, rtp_packet->payload + 2, payload_size);
      offset += payload_size;
    } else if (offset < CONFIG_MAX_NALU_SIZE) {
      memcpy(nalu_buf + offset, rtp_packet->payload + 2, payload_size);
      offset += payload_size;
      if (fu_header->e) {
        // end of fragmented NALU
        if (rtp_decoder->on_packet != NULL) {
          rtp_decoder->on_packet(nalu_buf, offset, rtp_decoder->user_data);
        }
        offset = 0;  // reset for next NALU
      }
    }
  }
  return 0;
}

static int rtp_decode_generic(RtpDecoder* rtp_decoder, uint8_t* buf, size_t size) {
  RtpPacket* rtp_packet = (RtpPacket*)buf;
  if (rtp_decoder->on_packet != NULL)
    rtp_decoder->on_packet(rtp_packet->payload, size - sizeof(RtpHeader), rtp_decoder->user_data);
  // even if there is no callback set, assume everything is ok for caller and do not return an error
  return (int)size;
}

void rtp_decoder_init(RtpDecoder* rtp_decoder, MediaCodec codec, RtpOnPacket on_packet, void* user_data) {
  rtp_decoder->on_packet = on_packet;
  rtp_decoder->user_data = user_data;

  switch (codec) {
    case CODEC_H264:
      rtp_decoder->decode_func = rtp_decode_h264;
      break;
    case CODEC_AV1:
      rtp_decoder->decode_func = NULL;  // send only for now
      break;
    case CODEC_PCMA:
    case CODEC_PCMU:
    case CODEC_OPUS:
      rtp_decoder->decode_func = rtp_decode_generic;
    default:
      break;
  }
}

int rtp_decoder_decode(RtpDecoder* rtp_decoder, const uint8_t* buf, size_t size) {
  if (rtp_decoder->decode_func == NULL)
    return -1;
  return rtp_decoder->decode_func(rtp_decoder, (uint8_t*)buf, size);
}
