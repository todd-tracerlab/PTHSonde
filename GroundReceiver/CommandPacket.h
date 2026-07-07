// =============================================================================
// CommandPacket.h  -- SHARED ground -> sonde uplink command frame
// -----------------------------------------------------------------------------
// Tiny packed frame the ground station sends to the sonde during PRE-FLIGHT
// setup only. The sonde reflects the result in its telemetry (cfg_channel +
// LOCKED bit); there is no separate over-air ACK. Keep identical in both
// projects. A distinct sync byte (0xC3) keeps these from colliding with
// telemetry (0xAA) in either parser.
// =============================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>

#define CMD_SYNC  0xC3u

enum {
  CMD_NONE        = 0,
  CMD_PING        = 1,   // link check (no effect)
  CMD_SET_CHANNEL = 2,   // arg = channel: STAGE this channel (no switch yet)
  CMD_LOCK        = 3     // arg = channel: switch to it and LOCK (launch)
};

#if defined(__GNUC__)
  #define CMD_PACKED __attribute__((packed))
#else
  #define CMD_PACKED
#endif

#pragma pack(push, 1)
typedef struct CMD_PACKED {
  uint8_t  sync;       // 0xC3
  uint8_t  version;    // protocol version
  uint8_t  sonde_id;   // target sonde (0xFF = broadcast/any)
  uint8_t  cmd;        // CMD_*
  uint8_t  arg;        // channel for SET_CHANNEL / LOCK
  uint16_t crc16;      // CRC-16/CCITT-FALSE over preceding bytes
} CommandPacket;
#pragma pack(pop)

static_assert(sizeof(CommandPacket) == 7, "CommandPacket must be 7 bytes");

static inline uint16_t cmd_crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; b++) {
      if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x1021);
      else              crc = (uint16_t)(crc << 1);
    }
  }
  return crc;
}

static inline void cmd_finalize(CommandPacket* c, uint8_t version) {
  c->sync    = CMD_SYNC;
  c->version = version;
  c->crc16   = cmd_crc16((const uint8_t*)c, sizeof(CommandPacket) - sizeof(uint16_t));
}

static inline bool cmd_validate(const CommandPacket* c) {
  if (c->sync != CMD_SYNC) return false;
  return c->crc16 == cmd_crc16((const uint8_t*)c, sizeof(CommandPacket) - sizeof(uint16_t));
}
