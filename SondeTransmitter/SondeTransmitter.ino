// =============================================================================
// SondeTransmitter.ino  -- ESP32-C3 radiosonde transmitter
//
// Board: "ESP32C3 Dev Module"  (Tools -> USB CDC On Boot: Enabled)
//
// Samples sensors at 1 Hz, block-averages them, and transmits a compact 41-byte
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
static bool s_mcpOk = false;   // MCP3221 read OK at least once this TX window

static void resetAverages() {
  aShtT.reset(); aShtRH.reset();
  aTherm.reset(); aBattV.reset(); aCurr.reset();
  s_mcpOk = false;
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
  if (s.ms_ok)    { pressAdd(s.ms_press_pa); }   // spike-filtered (median), not averaged
  if (s.mcp_ok)   { s_mcpOk = true; }
  if (s.therm_ok) { aTherm.add(s.therm_temp_c); }

  BatteryReading b = batteryRead();
  if (b.ok) { aBattV.add(b.voltage_v); aCurr.add(b.current_ma); }

  GpsData g = gpsGet();
  if (g.fix) windAvgAddSample((float)g.course_deg, (float)g.speed_mps, millis());
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

  // ---- GPS (instantaneous position) ----------------------------------------
  GpsData g = gpsGet();
  p->sats      = g.sats;
  p->hour      = g.hour;
  p->minute    = g.minute;
  p->second    = g.second;
  p->lat_e7    = (int32_t)lround(g.lat_deg * 1e7);
  p->lon_e7    = (int32_t)lround(g.lon_deg * 1e7);
  p->alt_m     = toU16cap(lround(g.alt_m));
  p->speed_cms = toU16cap(lround(g.speed_mps * 100.0));
  p->course_cd = degToCenti(g.course_deg);
  if (g.fix)       status |= TF_STAT_GPS_FIX;
  if (g.ppsActive) status |= TF_STAT_GPS_PPS;
  if (g.sats > 0 || g.fix) valid |= TF_VALID_GPS;

  // ---- Wind: vector average of the GPS track (fed by sampleSensors) --------
  {
    float avgCourse = 0.0f, avgSpeed = 0.0f;
    if (windAvgGet(&avgCourse, &avgSpeed)) {
      p->speed_cms = toU16cap(lround(avgSpeed * 100.0));   // mean wind speed
      bool wok = false;
      float wdeg = windDirectionDeg(avgCourse, avgSpeed, &wok);
      if (wok) { p->wind_dir_cd = degToCenti(wdeg); valid |= TF_VALID_WIND; }
    }
  }

  // ---- sensors (block-averaged over this TX window) ------------------------
  if (aShtT.ok())  { p->sht_temp_c100 = toC100(aShtT.mean());
                     p->sht_rh_x2     = toU8cap(lround(aShtRH.mean() * 2.0));
                     valid |= TF_VALID_SHT; }
  float medP;
  if (pressMedian(&medP)) { p->press_half_pa = toU16cap(lround(medP / 2.0));
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
  Serial.printf("     lat=%.7f lon=%.7f alt=%um spd=%.2fm/s crs=%.2fdeg\n",
                p->lat_e7 / 1e7, p->lon_e7 / 1e7, p->alt_m,
                p->speed_cms / 100.0, p->course_cd / 100.0);
  Serial.printf("SHT  %s T=%.2fC RH=%.1f%%   P %s %.1fPa  Therm %s %.2fC  Wind %s %.2fdeg\n",
                (p->valid_flags & TF_VALID_SHT) ? "ok" : "--",
                p->sht_temp_c100 / 100.0, p->sht_rh_x2 / 2.0,
                (p->valid_flags & TF_VALID_MS) ? "ok" : "--", (double)p->press_half_pa * 2.0,
                (p->valid_flags & TF_VALID_THERM) ? "ok" : "--", p->therm_temp_c100 / 100.0,
                (p->valid_flags & TF_VALID_WIND) ? "ok" : "--", p->wind_dir_cd / 100.0);
  Serial.printf("PWR  %s V=%.3fV I=%.1fmA   status=0x%02X valid=0x%02X  %u bytes crc=0x%04X\n",
                (p->valid_flags & TF_VALID_INA) ? "ok" : "--",
                p->batt_mv / 1000.0, (double)p->current_ma,
                p->status_flags, p->valid_flags,
                (unsigned)sizeof(TelemetryPacket), p->crc16);
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

  // Listen for ground commands. Always drain the UART; act only while unlocked.
  CommandPacket cmd;
  if (loraPollCommand(&cmd)) {
    if (!s_locked) handleCommand(&cmd);
  }

  unsigned long now = millis();

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
