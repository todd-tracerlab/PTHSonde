// =============================================================================
// TelemetryPacket.h  -- SHARED between SondeTransmitter and GroundReceiver
// -----------------------------------------------------------------------------
// v2: compact, packed, little-endian telemetry optimized for long range. NO
// floats on the air. Redundant/derivable fields (power, uptime, date, MS temp,
// raw ADC) are dropped to shrink airtime; the receiver re-derives power on the
// ground. Keep this file byte-for-byte identical in both projects.
//
// Wire format (little-endian):  [sync][version] ... fields ... [crc16]
// CRC-16/CCITT-FALSE over every byte from sync up to (not including) crc16.
// =============================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// ---- Framing / protocol -----------------------------------------------------
#define TELEM_SYNC0              0xAAu     // 2-byte sync for robust framing
#define TELEM_SYNC1              0x55u
#define TELEM_PROTOCOL_VERSION   2u

// ---- status_flags bits ------------------------------------------------------
#define TF_STAT_GPS_FIX   (1u << 0)   // GPS reports a valid position fix
#define TF_STAT_GPS_PPS   (1u << 1)   // 1PPS pulse seen recently
#define TF_STAT_LOCKED    (1u << 2)   // sonde frequency-locked (launched)

// ---- valid_flags bits (1 = sensor read OK) ----------------------------------
#define TF_VALID_SHT      (1u << 0)   // SHT41 temperature/RH
#define TF_VALID_MS       (1u << 1)   // MS5611 pressure
#define TF_VALID_MCP      (1u << 2)   // MCP3221 ADC
#define TF_VALID_INA      (1u << 3)   // INA219 battery monitor
#define TF_VALID_THERM    (1u << 4)   // thermistor temperature (derived)
#define TF_VALID_WIND     (1u << 5)   // wind direction
#define TF_VALID_GPS      (1u << 6)   // GPS data valid

#if defined(__GNUC__)
  #define TELEM_PACKED __attribute__((packed))
#else
  #define TELEM_PACKED
#endif

#pragma pack(push, 1)
typedef struct TELEM_PACKED {
  uint8_t  sync0;            // 0xAA
  uint8_t  sync1;            // 0x55
  uint8_t  version;          // protocol version (2)
  uint8_t  sonde_id;         // sonde identifier (0..255)
  uint16_t seq;              // packet sequence (wraps; ~54 h at 3 s)

  // UTC time of day (date is not transmitted to save airtime)
  uint8_t  hour;
  uint8_t  minute;
  uint8_t  second;

  // GPS
  int32_t  lat_e7;           // latitude,  degrees * 1e7
  int32_t  lon_e7;           // longitude, degrees * 1e7
  uint16_t alt_m;            // GPS altitude, meters (0..65535)
  uint8_t  sats;             // satellites in use
  uint16_t speed_cms;        // ground/wind speed, cm/s
  uint16_t course_cd;        // GPS course over ground, centi-degrees 0..35999
  uint16_t wind_dir_cd;      // wind direction, centi-degrees 0..35999

  // weather
  int16_t  sht_temp_c100;    // SHT41 temperature, deg C * 100
  uint8_t  sht_rh_x2;        // SHT41 RH, % * 2 (0..200 = 0..100 %)
  uint16_t press_half_pa;    // pressure, Pascals / 2 (decode: *2)
  int16_t  therm_temp_c100;  // thermistor temperature, deg C * 100

  // power
  uint16_t batt_mv;          // battery voltage, millivolts
  int16_t  current_ma;       // current, milliamps (signed)

  // flags + config echo
  uint8_t  status_flags;     // TF_STAT_*
  uint8_t  valid_flags;      // TF_VALID_*
  uint8_t  cfg_channel;      // sonde's staged/active LoRa channel (0..80)

  uint16_t crc16;            // CRC-16/CCITT-FALSE over all preceding bytes
} TelemetryPacket;
#pragma pack(pop)

static_assert(sizeof(TelemetryPacket) == 42, "TelemetryPacket must be 42 bytes");

// ---- CRC-16/CCITT-FALSE -----------------------------------------------------
static inline uint16_t telem_crc16(const uint8_t* data, size_t len) {
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

static inline uint16_t telem_compute_crc(const TelemetryPacket* p) {
  return telem_crc16((const uint8_t*)p, sizeof(TelemetryPacket) - sizeof(uint16_t));
}

static inline void telem_finalize(TelemetryPacket* p) {
  p->sync0   = TELEM_SYNC0;
  p->sync1   = TELEM_SYNC1;
  p->version = TELEM_PROTOCOL_VERSION;
  p->crc16   = telem_compute_crc(p);
}

static inline bool telem_validate(const TelemetryPacket* p) {
  if (p->sync0 != TELEM_SYNC0 || p->sync1 != TELEM_SYNC1) return false;
  return p->crc16 == telem_compute_crc(p);
}
