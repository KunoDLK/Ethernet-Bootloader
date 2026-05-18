#ifndef PROTO_COMMON_H
#define PROTO_COMMON_H

#include <stdint.h>

#define PROTO_MAGIC                     (0x424C4452UL) /* "BLDR" */
#define PROTO_VERSION                   (1U)
#define DISCOVERY_PORT                  (45000U)
#define CONTROL_PORT                    (45001U)

#define PROTO_FLAG_REPLY                (1U << 0)

typedef enum
{
  PROTO_MSG_DISCOVER_REQ = 0x01,
  PROTO_MSG_DISCOVER_REPLY = 0x02,
  PROTO_MSG_BCAST_UID_REQ = 0x12,
  PROTO_MSG_BCAST_UID_REPLY = 0x13,
  PROTO_MSG_DEVICETREE_REQ = 0x20,
  PROTO_MSG_DEVICETREE_REPLY = 0x21,
  PROTO_MSG_PING_REQ = 0x40,
  PROTO_MSG_PING_REPLY = 0x41,
  PROTO_MSG_ERROR_REPLY = 0x7F,
} ProtoMessageType;

typedef enum
{
  PROTO_RESULT_OK = 0,
  PROTO_RESULT_GENERIC = -1,
  PROTO_RESULT_PARSE = -2,
  PROTO_RESULT_UNSUPPORTED_VERSION = -3,
  PROTO_RESULT_UNAUTHORIZED = -4,
  PROTO_RESULT_LOCKED = -5,
  PROTO_RESULT_NOT_FOUND = -6,
  PROTO_RESULT_INVALID_VALUE = -7,
  PROTO_RESULT_BUSY = -8,
} ProtoResult;

typedef struct __attribute__((packed))
{
  uint32_t magic;
  uint8_t proto_version;
  uint8_t msg_type;
  uint16_t flags;
  uint32_t transaction_id;
} ProtoDiscoveryHeader;

typedef struct __attribute__((packed))
{
  uint32_t magic;
  uint8_t proto_version;
  uint8_t msg_type;
  uint16_t flags;
  uint32_t transaction_id;
  uint16_t payload_len;
  uint16_t reserved;
} ProtoCommandHeader;

typedef struct __attribute__((packed))
{
  uint32_t magic;
  uint8_t proto_version;
  uint8_t frame_type;
  uint16_t flags;
  uint32_t seq;
  uint16_t payload_len;
  uint16_t reserved;
} ProtoProgFrameHeader;

#endif /* PROTO_COMMON_H */
