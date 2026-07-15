// =============================================================================
// SondeTransmitter.ino  -- ESP32-C3 radiosonde transmitter
//
// Board: "ESP32C3 Dev Module"  (Tools -> USB CDC On Boot: Enabled)
//
// Samples sensors at 1 Hz, block-averages them, and transmits a compact 39-byte
// TelemetryPacket every 3 s over the E22-900T22S (0.3 kbps long-range config).
//
// Pre-flight: the sonde boots on the rendezvous channel and listens for ground
// commands. SET_CHANNEL stages a target channel (still transmitting on
// rendezvous, echoing the staged channel so the ground can confirm). LOCK
// switches to the staged channel and stops listening for the rest of the flight
// (lock clears only on power-cycle).
// =============================================================================
#include <Arduino.h>

#include "BoardPins.h"
#include "TelemetryPacket.h"
#include "CommandPacket.h"
#include "Sensors.h"
#include "GpsManager.h"
#include "LoRaManager.h"
#include "BatteryMonitor.h"
#include "Thermistor.h"
#include "WindDirection.h"
#include "WindAverage.h"
#include "StatusLed.h"

#include <math.h>
#include <strings.h>   // strcasecmp for the USB STATUS command

// ---- User configuration -----------------------------------------------------
// Sonde ID: 0 = auto-assign a stable unique ID derived from this chip's factory
// MAC (no editing needed per board). Set non-zero to force a specific ID.
static const uint8_t  SONDE_ID_OVERRIDE = 0x00;
static uint8_t        SONDE_ID          = 0x01;    // resolved in setup()
static const uint32_t TX_INTERVAL_MS    = 3000;    // telemetry period (every 3 s)
static const uint32_t SENSOR_SAMPLE_MS  = 1000;    // sensor sampling period (1 Hz)
static const bool     PRINT_DIAG        = true;     // USB diagnostic summary
static const uint32_t WIND_AVG_MS       = 5000;    // wind rolling-average window

// ---- State ------------------------------------------------------------------
static uint16_t s_seq = 0;
static unsigned long s_lastTx = 0;
static unsigned long s_lastSample = 0;
static bool s_loraOk = false;

static uint8_t s_stagedChannel = LORA_RENDEZVOUS_CH;  // channel echoed / used at lock
static bool    s_locked        = false;               // frequency-locked (launched)

// ---- Rolling-average accumulators (one TX window) ---------------------------
struct Avg {
  double   sum = 0.0;
  uint32_t n   = 0;
  void add(double v) { sum += v; n++; }
  void reset()       { sum = 0.0; n = 0; }
  bool ok() const    { return n > 0; }
  double mean() const { return n ? sum / (double)n : 0.0; }
};
static Avg aShtT, aShtRH, aTherm, aBattV, aCurr;
static bool s_mcpOk  = false;   // MCP3221 read OK at least once this TX window
static bool s_pFresh = false;   // MS5611 produced a fresh reading this TX window

static void resetAverages() {
  aShtT.reset(); aShtRH.reset();
  aTherm.reset(); aBattV.reset(); aCurr.reset();
  s_mcpOk = false; s_pFresh = false;
}

// ---- Pressure spike filter --------------------------------------------------
// MS5611 occasionally throws a single wildly-wrong reading. A running median
// rejects those outliers far better than averaging (one spike can't move the
// median). This persists across TX windows (rolling, not reset each window).
#define PRESS_MED_N 5
static float s_pBuf[PRESS_MED_N];
static int   s_pCount = 0, s_pIdx = 0;

static void pressAdd(float pa) {
  s_pBuf[s_pIdx] = pa;
  s_pIdx = (s_pIdx + 1) % PRESS_MED_N;
  if (s_pCount < PRESS_MED_N) s_pCount++;
}
static bool pressMedian(float* out) {
  if (s_pCount == 0) return false;
  float t[PRESS_MED_N];
  for (int i = 0; i < s_pCount; i++) t[i] = s_pBuf[i];
  for (int i = 1; i < s_pCount; i++) {       // insertion sort
    float k = t[i]; int j = i - 1;
    while (j >= 0 && t[j] > k) { t[j + 1] = t[j]; j--; }
    t[j + 1] = k;
  }
  *out = t[s_pCount / 2];
  return true;
}

// Read every sensor once and fold valid values into the accumulators. Also
// feeds the wind vector-averager with the latest GPS track.
static void sampleSensors() {
  SensorData s;
  sensorsUpdate(&s);
  if (s.sht_ok)   { aShtT.add(s.sht_temp_c); aShtRH.add(s.sht_rh); }
  if (s.ms_ok)    { pressAdd(s.ms_press_pa); s_pFresh = true; }   // spike-filtered (median), not averaged
  if (s.mcp_ok)   { s_mcpOk = true; }
  if (s.therm_ok) { aTherm.add(s.therm_temp_c); }

  BatteryReading b = batteryRead();
  if (b.ok) { aBattV.add(b.voltage_v); aCurr.add(b.current_ma); }
}

// Derive a stable, unique sonde ID from the chip's 48-bit factory MAC.
// FNV-1a hash folded into 1..254 (avoids 0x00 = "unset" and 0xFF = broadcast).
static uint8_t deriveSondeId() {
  uint64_t mac = ESP.getEfuseMac();
  uint32_t h = 2166136261u;
  for (int i = 0; i < 6; i++) { h ^= (uint8_t)(mac >> (8 * i)); h *= 16777619u; }
  return (uint8_t)(1 + (h % 254));
}

// ---- helpers ----------------------------------------------------------------
static int16_t  toC100(float c)   { return (int16_t)lroundf(c * 100.0f); }
static uint16_t toU16cap(long v)  { if (v < 0) v = 0; if (v > 65535) v = 65535; return (uint16_t)v; }
static int16_t  toI16cap(long v)  { if (v < -32768) v = -32768; if (v > 32767) v = 32767; return (int16_t)v; }
static uint8_t  toU8cap(long v)   { if (v < 0) v = 0; if (v > 255) v = 255; return (uint8_t)v; }

static uint16_t degToCenti(double deg) {
  if (isnan(deg)) return 0;
  double d = fmod(deg, 360.0);
  if (d < 0) d += 360.0;
  long cd = lround(d * 100.0);
  if (cd >= 36000) cd -= 36000;
  if (cd < 0) cd = 0;
  return (uint16_t)cd;
}

static void buildPacket(TelemetryPacket* p) {
  memset(p, 0, sizeof(*p));
  p->sonde_id    = SONDE_ID;
  p->seq         = s_seq;
  p->cfg_channel = s_stagedChannel;

  uint8_t status = 0, valid = 0;
  if (s_locked) status |= TF_STAT_LOCKED;
  if (gpsBalloonConfirmed()) status |= TF_STAT_BALLOON;   // 80 km ceiling actually active?

  // ---- GPS (instantaneous position) ----------------------------------------
  GpsData g = gpsGet();
  p->sats      = g.sats;
  p->hour      = g.hour;
  p->minute    = g.minute;
  p->second    = g.second;
  p->lat_e7    = (int32_t)lround(g.lat_deg * 1e7);
  p->lon_e7    = (int32_t)lround(g.lon_deg * 1e7);
  p->alt_m     = toU16cap(lround(g.alt_m));
  p->course_cd = degToCenti(g.course_deg);
  if (g.fix)       status |= TF_STAT_GPS_FIX;
  if (g.ppsActive) status |= TF_STAT_GPS_PPS;
  if (g.sats > 0 || g.fix) valid |= TF_VALID_GPS;

  // Wind (speed + direction) is derived on the ground from successive GPS fixes,
  // so it is no longer computed or transmitted here.

  // ---- sensors (block-averaged over this TX window) ------------------------
  if (aShtT.ok())  { p->sht_temp_c100 = toC100(aShtT.mean());
                     p->sht_rh_x10    = toU16cap(lround(aShtRH.mean() * 10.0));
                     valid |= TF_VALID_SHT; }
  float medP;
  if (s_pFresh && pressMedian(&medP)) { p->press_half_pa = toU16cap(lround(medP / 2.0));
                                        valid |= TF_VALID_MS; }
  if (s_mcpOk)     { valid |= TF_VALID_MCP; }
  if (aTherm.ok()) { p->therm_temp_c100 = toC100(aTherm.mean()); valid |= TF_VALID_THERM; }

  // ---- battery (block-averaged) --------------------------------------------
  if (aBattV.ok()) {
    p->batt_mv    = toU16cap(lround(aBattV.mean() * 1000.0));
    p->current_ma = toI16cap(lround(aCurr.mean()));
    valid |= TF_VALID_INA;
  }

  p->status_flags = status;
  p->valid_flags  = valid;

  telem_finalize(p);   // stamp sync/version + CRC
}

// ---- Command handling (pre-lock only) --------------------------------------
static void handleCommand(const CommandPacket* c) {
  if (c->sonde_id != SONDE_ID && c->sonde_id != 0xFF) return;  // not for us
  if (s_locked) return;                                         // ignore once launched

  switch (c->cmd) {
    case CMD_PING:
      Serial.println(F("[CMD] PING"));
      break;
    case CMD_SET_CHANNEL:
      if (c->arg >= LORA_CH_MIN && c->arg <= LORA_CH_MAX) {
        s_stagedChannel = c->arg;          // stage only; keep transmitting on rendezvous
        Serial.printf("[CMD] STAGE channel %u (%.3f MHz)\n",
                      c->arg, loraFreqMHz(c->arg));
      }
      break;
    case CMD_LOCK:
      if (c->arg >= LORA_CH_MIN && c->arg <= LORA_CH_MAX) {
        s_stagedChannel = c->arg;
        if (loraSetChannel(c->arg)) {
          s_locked = true;
          Serial.printf("[CMD] LOCK + LAUNCH on channel %u (%.3f MHz)\n",
                        c->arg, loraFreqMHz(c->arg));
        } else {
          Serial.println(F("[CMD] LOCK failed (E22 retune no ACK)"));
        }
      }
      break;
    default: break;
  }
}

static void printDiag(const TelemetryPacket* p) {
  Serial.println(F("---- SONDE TELEMETRY -------------------------------------"));
  Serial.printf("proto=%u id=0x%02X seq=%u  ch=%u(%.3fMHz) %s  LoRa=%s\n",
                p->version, p->sonde_id, p->seq, p->cfg_channel,
                loraFreqMHz(p->cfg_channel),
                (p->status_flags & TF_STAT_LOCKED) ? "LOCKED" : "staged",
                s_loraOk ? "OK" : "FAIL");
  Serial.printf("GPS  fix=%u pps=%u sats=%u  %02u:%02u:%02uZ\n",
                (p->status_flags & TF_STAT_GPS_FIX) ? 1 : 0,
                (p->status_flags & TF_STAT_GPS_PPS) ? 1 : 0,
                p->sats, p->hour, p->minute, p->second);
  { int8_t ack = gpsFrModeAck();
    const char* fr = (ack == 3) ? "OK (balloon, no 12km cap)"
                   : (ack == -1) ? "NO REPLY YET"
                   : (ack == 1) ? "UNSUPPORTED -- will cap at ~12km!"
                   : (ack == 2) ? "FAILED -- will cap at ~12km!"
                                : "INVALID -- will cap at ~12km!";
    Serial.printf("     FR-mode ACK=%d  %s\n", ack, fr); }
  Serial.printf("     lat=%.7f lon=%.7f alt=%um crs=%.2fdeg\n",
                p->lat_e7 / 1e7, p->lon_e7 / 1e7, p->alt_m,
                p->course_cd / 100.0);
  Serial.printf("SHT  %s T=%.2fC RH=%.1f%%   P %s %.1fPa  Therm %s %.2fC\n",
                (p->valid_flags & TF_VALID_SHT) ? "ok" : "--",
                p->sht_temp_c100 / 100.0, p->sht_rh_x10 / 10.0,
                (p->valid_flags & TF_VALID_MS) ? "ok" : "--", (double)p->press_half_pa * 2.0,
                (p->valid_flags & TF_VALID_THERM) ? "ok" : "--", p->therm_temp_c100 / 100.0);
  Serial.printf("PWR  %s V=%.3fV I=%.1fmA   status=0x%02X valid=0x%02X  %u bytes crc=0x%04X\n",
                (p->valid_flags & TF_VALID_INA) ? "ok" : "--",
                p->batt_mv / 1000.0, (double)p->current_ma,
                p->status_flags, p->valid_flags,
                (unsigned)sizeof(TelemetryPacket), p->crc16);
}

// ---- USB STATUS command -----------------------------------------------------
// Answer a bench tool's "STATUS" query on demand with a one-line, live sensor
// health report. '#' prefix so it's ignored by anything parsing telemetry.
static char    s_usbLine[24];
static uint8_t s_usbLen = 0;

static void printSondeStatus() {
  SensorData s;      sensorsUpdate(&s);       // live read, right now
  BatteryReading b = batteryRead();
  GpsData g =        gpsGet();
  int8_t fr =        gpsFrModeAck();
  Serial.printf("# STATUS id=0x%02X SHT=%d MS=%d THERM=%d INA=%d LORA=%d GPS=%d "
                "sats=%u fr=%d T=%.1f RH=%.1f V=%.2f\n",
                SONDE_ID, s.sht_ok ? 1 : 0, s.ms_ok ? 1 : 0, s.therm_ok ? 1 : 0,
                b.ok ? 1 : 0, s_loraOk ? 1 : 0, (fr == 3) ? 1 : 0, g.sats, fr,
                s.sht_ok ? s.sht_temp_c : 0.0, s.sht_ok ? s.sht_rh : 0.0,
                b.ok ? b.voltage_v : 0.0);
}

static void pollUsbStatus() {
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (s_usbLen > 0) { s_usbLine[s_usbLen] = 0;
        if (!strcasecmp(s_usbLine, "STATUS")) printSondeStatus();
        s_usbLen = 0; }
    } else if (s_usbLen < sizeof(s_usbLine) - 1) {
      s_usbLine[s_usbLen++] = ch;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\nSondeTransmitter starting..."));

  SONDE_ID = SONDE_ID_OVERRIDE ? SONDE_ID_OVERRIDE : deriveSondeId();
  Serial.printf("Sonde ID = 0x%02X (%s)\n", SONDE_ID,
                SONDE_ID_OVERRIDE ? "manual override" : "auto from chip MAC");

  ledBegin();
  sensorsBegin();
  batteryBegin();
  gpsBegin();
  windAvgBegin(WIND_AVG_MS);

  s_loraOk = loraBegin(LORA_RENDEZVOUS_CH);   // boot on the control channel
  Serial.printf("E22 config: %s  rendezvous ch %u (%.3f MHz)\n",
                s_loraOk ? "acknowledged" : "no ACK (check wiring/AUX)",
                LORA_RENDEZVOUS_CH, loraFreqMHz(LORA_RENDEZVOUS_CH));

  ledSetPattern(s_loraOk ? LED_SLOW_BLINK : LED_FAST_BLINK);

  unsigned long now = millis();
  s_lastTx     = now;
  s_lastSample = now;
  sampleSensors();               // seed the first averaging window immediately
}

void loop() {
  gpsUpdate();
  ledUpdate();
  pollUsbStatus();     // answer a USB "STATUS" query from the bench tool

  // Listen for ground commands. Always drain the UART; act only while unlocked.
  CommandPacket cmd;
  if (loraPollCommand(&cmd)) {
    if (!s_locked) handleCommand(&cmd);
  }

  unsigned long now = millis();

  // ---- LoRa self-heal ------------------------------------------------------
  // If the E22 never ACKed its config at boot, keep re-attempting (throttled)
  // so a cold-boot provisioning miss recovers on its own instead of the sonde
  // transmitting on an unconfigured radio until a power cycle.
  static unsigned long s_loraRetry = 0;
  if (!s_loraOk && now - s_loraRetry >= 4000) {
    s_loraRetry = now;
    s_loraOk = loraBegin(s_stagedChannel);
  }

  // ---- Sensor sampling tick ------------------------------------------------
  if (now - s_lastSample >= SENSOR_SAMPLE_MS) {
    s_lastSample += SENSOR_SAMPLE_MS;
    if (now - s_lastSample > SENSOR_SAMPLE_MS) s_lastSample = now;
    sampleSensors();
  }

  // ---- Transmit tick -------------------------------------------------------
  if (now - s_lastTx >= TX_INTERVAL_MS) {
    s_lastTx += TX_INTERVAL_MS;
    if (now - s_lastTx > TX_INTERVAL_MS) s_lastTx = now;

    TelemetryPacket pkt;
    buildPacket(&pkt);
    loraSendPacket(&pkt);
    gpsAssertBalloonNow();          // re-assert balloon mode right after the TX current spike
    if (PRINT_DIAG) printDiag(&pkt);

    resetAverages();
    s_seq++;

    // ---- LED status -------------------------------------------------------
    //   radio not configured -> fast blink (fault)
    //   GPS fix + transmitting -> crisp flash on each transmit
    //   no fix yet            -> slow "searching" blink
    if (!s_loraOk) {
      ledSetPattern(LED_FAST_BLINK);
    } else if (pkt.status_flags & TF_STAT_GPS_FIX) {
      ledSetPattern(LED_OFF);
      ledPulse(50);
    } else {
      ledSetPattern(LED_SLOW_BLINK);
    }
  }

  delay(1);   // yield so the task watchdog stays serviced
}
