// =============================================================================
// GpsManager.h  -- Quectel L86-M33 over UART1 (9600 baud) + 1PPS on GPIO3
//
// Non-blocking: call gpsUpdate() often to feed the NMEA parser. 1PPS is tracked
// by an ISR; ppsActive is true if a pulse was seen recently.
// =============================================================================
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
  bool     fix;
  uint8_t  sats;
  uint16_t year;
  uint8_t  month, day, hour, minute, second;
  double   lat_deg;
  double   lon_deg;
  double   alt_m;
  double   speed_mps;
  double   course_deg;
  bool     ppsActive;
} GpsData;

void    gpsBegin();
void    gpsUpdate();          // drain UART into the parser (non-blocking)
GpsData gpsGet();

// Balloon/high-altitude (PMTK886,3) mode ACK flag from the L86:
//   -1 no reply yet   0 invalid   1 unsupported   2 failed   3 OK (balloon set).
// Anything other than 3 means the GPS will stop fixing at its default ~12 km ceiling.
int8_t  gpsFrModeAck();
