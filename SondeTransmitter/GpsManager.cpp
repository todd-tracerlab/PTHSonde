#include "GpsManager.h"
#include "BoardPins.h"
#include <Arduino.h>
#include <TinyGPSPlus.h>      // Library Manager: "TinyGPSPlus" (Mikal Hart)

static TinyGPSPlus    s_gps;
static HardwareSerial s_gpsSerial(GPS_UART_NUM);

// ---- 1PPS tracking ----------------------------------------------------------
static volatile uint32_t s_ppsCount    = 0;
static volatile uint32_t s_ppsLastMs    = 0;

static void IRAM_ATTR ppsIsr() {
  s_ppsCount++;
  s_ppsLastMs = millis();
}

// ---- L86 (MT3333) navigation-mode setup ------------------------------------
// The L86 powers up in NORMAL mode. Per the Quectel Lx0&Lx6&LC86L&LG77L GNSS
// Protocol Specification (PMTK886, PMTK_FR_MODE), every mode EXCEPT Balloon caps
// out at 10,000 m:
//     Normal / Fitness / Aviation / Stationary : 10000 m
//     Balloon                                   : 80000 m
// So a radiosonde MUST be in Balloon mode (PMTK886,3) or the fix dies just past
// 10 km -- exactly what happened on 2026-07-07 (lost fix at 12,007 m).
//
// PMTK886 (FR mode) is VOLATILE: it is not saved in flash and is lost on every
// power-up or brown-out. It also returns an ACK we can check:
//     Send:  $PMTK886,3*2B      Reply: $PMTK001,886,<flag>   (flag 3 = accepted)
// Strategy: hammer the command every few seconds until we see flag==3, then drop
// to a slow keep-alive that re-asserts it so an in-flight reset can't strand us
// back in the 10 km-capped Normal mode.
#define GPS_FR_MODE_BALLOON 3
static const uint32_t GPS_MODE_ASSERT_MS = 4000;   // steady re-assert cadence, WHOLE flight
static const uint32_t GPS_ACK_FRESH_MS   = 20000;  // an ACK is "current" only this long
static uint32_t s_lastModeMs = 0;

// Send an NMEA/PMTK sentence, computing the "$<body>*HH\r\n" checksum.
static void sendPMTK(const char* body) {
  uint8_t cks = 0;
  for (const char* p = body; *p; p++) cks ^= (uint8_t)*p;
  char line[48];
  snprintf(line, sizeof(line), "$%s*%02X\r\n", body, cks);
  s_gpsSerial.print(line);
  s_gpsSerial.flush();
}

static void gpsSetBalloonMode() {
  // $PMTK886,3*2B  -> Balloon (high-altitude) mode (verified checksum 0x2B)
  sendPMTK("PMTK886,3");
}

// ---- Balloon-mode ACK tracking ---------------------------------------------
// The L86 answers PMTK886 with "$PMTK001,886,<flag>" where flag is:
//   0 invalid   1 unsupported command   2 valid but action failed   3 OK/set.
// The 2026-07-07 flight died right at ~12 km (the default-mode altitude ceiling),
// which means balloon mode never took -- yet we never checked this reply, so we
// were flying blind. Watch the NMEA stream for the ACK and expose the flag so it
// can be confirmed on the bench (see the "FR-mode ACK" line in the serial debug).
static int8_t   s_frAck   = -1;       // -1 = no ACK seen yet; else last flag seen
static uint32_t s_frAckMs = 0;        // millis() when flag 3 was last confirmed
static bool     s_gpsReset = false;   // module restart just detected -> re-assert now
static char     s_nmea[96];
static uint8_t  s_nmeaLen = 0;

static void gpsScanForAck(char c) {
  if (c == '\n' || c == '\r') {
    if (s_nmeaLen >= 8) {
      s_nmea[s_nmeaLen] = '\0';
      char* p = strstr(s_nmea, "PMTK001,886,");
      if (p) {
        s_frAck = (int8_t)(p[12] - '0');          // char right after the 2nd comma
        if (s_frAck == GPS_FR_MODE_BALLOON) s_frAckMs = millis();
      }
      // L86 restart banner (PMTK010/PMTK011): the VOLATILE balloon mode is gone,
      // so mark unconfirmed and force an immediate re-assert on the next update.
      if (strstr(s_nmea, "PMTK010") || strstr(s_nmea, "PMTK011")) {
        s_frAck = -1;
        s_gpsReset = true;
      }
    }
    s_nmeaLen = 0;
  } else if (s_nmeaLen < sizeof(s_nmea) - 1) {
    s_nmea[s_nmeaLen++] = c;
  }
}

int8_t gpsFrModeAck() { return s_frAck; }

bool gpsBalloonConfirmed() {
  return s_frAck == GPS_FR_MODE_BALLOON && (millis() - s_frAckMs) < GPS_ACK_FRESH_MS;
}

void gpsAssertBalloonNow() {
  gpsSetBalloonMode();
  s_lastModeMs = millis();
}

void gpsBegin() {
  s_gpsSerial.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

  pinMode(PIN_GPS_PPS, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_GPS_PPS), ppsIsr, RISING);

  // Send once now; the module may still be booting, so gpsUpdate() keeps
  // retrying every few seconds until the module ACKs balloon mode.
  gpsSetBalloonMode();
  s_lastModeMs = millis();
}

void gpsUpdate() {
  // Re-assert balloon mode at a steady cadence for the WHOLE flight (never relax
  // to a slow keep-alive), and immediately if a module restart was just detected.
  // PMTK886 is volatile (lost on any reset/brown-out), so continuous re-assertion
  // is what actually keeps the 80 km ceiling instead of silently falling back to
  // the 10 km-capped Normal mode.
  uint32_t now = millis();
  if (s_gpsReset || (now - s_lastModeMs >= GPS_MODE_ASSERT_MS)) {
    s_gpsReset = false;
    s_lastModeMs = now;
    gpsSetBalloonMode();
  }

  // Bounded drain so we never stall the main loop.
  int budget = 256;
  while (s_gpsSerial.available() && budget-- > 0) {
    char c = (char)s_gpsSerial.read();
    s_gps.encode(c);
    gpsScanForAck(c);          // watch for the PMTK886 balloon-mode ACK
  }
}

GpsData gpsGet() {
  GpsData g;
  memset(&g, 0, sizeof(g));

  // TinyGPSPlus latches location.isValid() true forever after the first fix, so
  // it holds the LAST position through a dropout. Gate on position AGE instead,
  // so a lost/stale fix reports fix=false (the receiver then emits NaN) rather
  // than freezing the last lat/lon as a live position.
  const uint32_t POS_MAX_AGE_MS = 3000;
  bool locFresh = s_gps.location.isValid() && (s_gps.location.age() < POS_MAX_AGE_MS);
  g.fix  = locFresh && (s_gps.satellites.value() > 0);
  g.sats = (uint8_t)(s_gps.satellites.isValid() ? s_gps.satellites.value() : 0);

  if (s_gps.date.isValid()) {
    g.year  = s_gps.date.year();
    g.month = s_gps.date.month();
    g.day   = s_gps.date.day();
  }
  if (s_gps.time.isValid()) {
    g.hour   = s_gps.time.hour();
    g.minute = s_gps.time.minute();
    g.second = s_gps.time.second();
  }

  if (locFresh) {
    g.lat_deg = s_gps.location.lat();
    g.lon_deg = s_gps.location.lng();
  }
  if (s_gps.altitude.isValid() && s_gps.altitude.age() < POS_MAX_AGE_MS) g.alt_m      = s_gps.altitude.meters();
  if (s_gps.speed.isValid()    && s_gps.speed.age()    < POS_MAX_AGE_MS) g.speed_mps  = s_gps.speed.mps();
  if (s_gps.course.isValid()   && s_gps.course.age()   < POS_MAX_AGE_MS) g.course_deg = s_gps.course.deg();

  // PPS considered active if a pulse arrived within the last 1.5 s.
  uint32_t last = s_ppsLastMs;
  g.ppsActive = (last != 0) && ((millis() - last) < 1500);

  return g;
}
