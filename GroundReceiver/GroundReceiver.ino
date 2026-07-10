// =============================================================================
// GroundReceiver.ino  -- ESP32-C3 ground station for the radiosonde
//
// Board: "ESP32C3 Dev Module"  (Tools -> USB CDC On Boot: Enabled)
//
// Receives + validates telemetry and prints one CSV line per valid packet over
// USB. Also accepts simple text commands over USB (from the dashboard) to set
// the receiver channel, stage/lock the sonde's frequency over the air, ping, or
// scan the band. Boots on the rendezvous (control) channel.
//
// USB command grammar (one per line):
//   RXCH <ch>    set THIS receiver's channel
//   STAGE <ch>   tell the sonde to stage <ch>  (it keeps TXing on rendezvous)
//   LOCK <ch>    tell the sonde to switch to <ch> and lock; receiver follows
//   PING         link check
//   SCAN         toggle band scan (CH_MIN..CH_MAX) until a valid packet is heard
// =============================================================================
#include <Arduino.h>
#include <string.h>
#include <strings.h>   // strcasecmp
#include "BoardPins.h"
#include "TelemetryPacket.h"
#include "CommandPacket.h"
#include "LoRaManager.h"
#include "CsvOutput.h"
#include "StatusLed.h"

// ---- Optional filter: 0 = accept any sonde, else require this ID ------------
static const uint8_t  ACCEPT_SONDE_ID = 0x00;     // 0 = accept all
static const uint8_t  TARGET_SONDE_ID = 0xFF;     // command target (0xFF = broadcast)

// ---- Ground-station ID: stable, unique per board (same scheme as the sonde) --
static const uint8_t  STATION_ID_OVERRIDE = 0x00; // 0 = auto from chip MAC
static uint8_t        STATION_ID          = 0x01; // resolved in setup()

// ---- State ------------------------------------------------------------------
static uint32_t s_pktCount    = 0;
static uint32_t s_lastSeq     = 0;
static bool     s_haveSeq     = false;
static uint32_t s_crcErrs     = 0;
static bool     s_cfgOk       = false;
static uint32_t s_lastValidMs = 0;

// Scan state
static bool     s_scan        = false;
static uint8_t  s_scanCh      = LORA_CH_MIN;
static uint32_t s_scanHopMs   = 0;
static const uint32_t SCAN_DWELL_MS = 1500;

// USB command line buffer
static char     s_line[64];
static uint8_t  s_lineLen = 0;

// ---- helpers ----------------------------------------------------------------
static void setReceiverChannel(int ch) {
  if (ch < LORA_CH_MIN || ch > LORA_CH_MAX) {
    Serial.printf("# ERR channel %d out of range [%d..%d]\n", ch, LORA_CH_MIN, LORA_CH_MAX);
    return;
  }
  bool ok = loraSetChannel((uint8_t)ch);
  Serial.printf("# RXCH %d %.3f %s\n", ch, loraFreqMHz((uint8_t)ch), ok ? "ok" : "NOACK");
}

// Send a command a few times so it lands in one of the sonde's listen windows.
static void sendCmdRetry(uint8_t cmd, uint8_t arg) {
  for (int k = 0; k < 4; k++) {
    loraSendCommand(cmd, arg, TARGET_SONDE_ID);
    delay(120);
  }
}

static void toggleScan() {
  s_scan = !s_scan;
  if (s_scan) {
    s_scanCh = LORA_CH_MIN;
    setReceiverChannel(s_scanCh);
    s_scanHopMs = millis();
    Serial.println(F("# SCAN start"));
  } else {
    Serial.println(F("# SCAN stop"));
  }
}

static void handleSerialLine(char* line) {
  char cmd[16];
  int arg = -1;
  int nf = sscanf(line, "%15s %d", cmd, &arg);
  if (nf < 1) return;

  if      (!strcasecmp(cmd, "RXCH")  && arg >= 0) { s_scan = false; setReceiverChannel(arg); }
  else if (!strcasecmp(cmd, "STAGE") && arg >= 0) { sendCmdRetry(CMD_SET_CHANNEL, (uint8_t)arg);
                                                    Serial.printf("# TX STAGE ch %d\n", arg); }
  else if (!strcasecmp(cmd, "LOCK")  && arg >= 0) { sendCmdRetry(CMD_LOCK, (uint8_t)arg);
                                                    setReceiverChannel(arg);
                                                    Serial.printf("# TX LOCK ch %d (RX following)\n", arg); }
  else if (!strcasecmp(cmd, "PING"))              { sendCmdRetry(CMD_PING, 0);
                                                    Serial.println(F("# TX PING")); }
  else if (!strcasecmp(cmd, "SCAN"))              { toggleScan(); }
  else if (!strcasecmp(cmd, "REG"))               { loraDumpRegisters(); }
  else if (!strcasecmp(cmd, "CFG"))               { bool ok = loraBegin(loraGetChannel());
                                                    Serial.printf("# CFG re-run: %s\n", ok ? "ACK" : "NOACK"); }
  else if (!strcasecmp(cmd, "HOLD"))              { loraHoldConfig(true);
                                                    Serial.println(F("# HOLD: M0/M1 forced HIGH (config). Type RUN to restore.")); }
  else if (!strcasecmp(cmd, "RUN"))               { loraHoldConfig(false);
                                                    Serial.println(F("# RUN: M0/M1 forced LOW (transparent).")); }
  else if (!strcasecmp(cmd, "STATUS"))            { Serial.printf("# STATUS id=0x%02X E22=%s rxch=%u pkts=%lu\n",
                                                      STATION_ID, s_cfgOk ? "ok" : "FAIL",
                                                      loraGetChannel(), (unsigned long)s_pktCount); }
  else if (!strcasecmp(cmd, "TXTEST"))            { loraTxTest(); }
  else if (!strcasecmp(cmd, "MODETEST"))          { loraModeTest(); }
  else if (!strcasecmp(cmd, "HELP"))              { Serial.println(F("# cmds: STATUS REG CFG TXTEST MODETEST HOLD RUN PING SCAN RXCH<n> STAGE<n> LOCK<n>")); }
  else { Serial.printf("# ERR unknown command: %s\n", line); }
}

static void pollSerialCommands() {
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (s_lineLen > 0) { s_line[s_lineLen] = 0; handleSerialLine(s_line); s_lineLen = 0; }
    } else if (s_lineLen < sizeof(s_line) - 1) {
      s_line[s_lineLen++] = ch;
    }
  }
}

// ---- Ground-station ID (FNV-1a over the 48-bit factory MAC, folded to 1..254) --
static uint8_t deriveStationId() {
  uint64_t mac = ESP.getEfuseMac();
  uint32_t h = 2166136261u;
  for (int i = 0; i < 6; i++) { h ^= (uint8_t)(mac >> (8 * i)); h *= 16777619u; }
  return (uint8_t)(1 + (h % 254));
}

void setup() {
  Serial.begin(115200);
  // Native ESP32-C3 USB-CDC: by default a write BLOCKS when the host (browser)
  // is slow to read, and the CDC can wedge -> port stays "open" but no bytes flow
  // ("connected, no data"). Non-blocking TX drops bytes instead of stalling the
  // loop, so RX keeps running and the link self-heals when the host catches up.
  Serial.setTxTimeoutMs(0);
  delay(200);
  Serial.println(F("# GroundReceiver starting..."));

  STATION_ID = STATION_ID_OVERRIDE ? STATION_ID_OVERRIDE : deriveStationId();
  Serial.printf("# Ground Station ID = 0x%02X (%s)\n", STATION_ID,
                STATION_ID_OVERRIDE ? "manual override" : "auto from chip MAC");

  ledBegin();
  s_cfgOk = loraBegin(LORA_RENDEZVOUS_CH);
  Serial.printf("# E22 config: %s\n", s_cfgOk ? "acknowledged" : "no ACK (check wiring/AUX)");
  Serial.printf("# packet size = %u bytes, proto = %u\n",
                (unsigned)sizeof(TelemetryPacket), TELEM_PROTOCOL_VERSION);
  Serial.printf("# RXCH %u %.3f %s\n", LORA_RENDEZVOUS_CH,
                loraFreqMHz(LORA_RENDEZVOUS_CH), s_cfgOk ? "ok" : "NOACK");
  ledSetPattern(s_cfgOk ? LED_SLOW_BLINK : LED_FAST_BLINK);

  csvPrintHeader();
}

void loop() {
  ledUpdate();
  pollSerialCommands();

  // Periodically poll the E22 ambient-noise floor (for pseudo-SNR). If the
  // module never answers (some E22 firmware lacks the C0C1C2C3 register read),
  // give up after a few tries so we stop transmitting the command over the air.
  static uint32_t lastNoiseReq = 0;
  static int      noiseAttempts = 0;
  static bool     noiseSupported = false;
  static bool     noiseGaveUp = false;
  // Poll the noise floor slowly: each request interleaves E22 register bytes into
  // the same UART stream as telemetry, so a faster rate just adds desync risk for
  // a value that barely changes. 4 s is plenty for a pseudo-SNR readout.
  if (!noiseGaveUp && millis() - lastNoiseReq >= 4000) {
    lastNoiseReq = millis();
    loraRequestNoise();
    if (loraGetNoise(NULL, NULL, NULL)) {
      if (!noiseSupported) { noiseSupported = true;
        Serial.println(F("# NOISE: ambient-RSSI read OK (SNR enabled)")); }
    } else if (!noiseSupported && ++noiseAttempts >= 6) {
      noiseGaveUp = true;
      Serial.println(F("# NOISE: no response - ambient-RSSI/SNR unsupported on this E22; disabled"));
    }
  }

  // Band scan: hop channels until a valid packet is heard.
  if (s_scan && (millis() - s_scanHopMs >= SCAN_DWELL_MS)) {
    s_scanCh = (s_scanCh >= LORA_CH_MAX) ? LORA_CH_MIN : (uint8_t)(s_scanCh + 1);
    setReceiverChannel(s_scanCh);
    s_scanHopMs = millis();
  }

  // Background LED state.
  if (!s_cfgOk) {
    ledSetPattern(LED_FAST_BLINK);
  } else if (s_scan) {
    ledSetPattern(LED_FAST_BLINK);              // scanning indicator
  } else {
    bool linkAlive = (s_lastValidMs != 0) && (millis() - s_lastValidMs < 7000);
    ledSetPattern(linkAlive ? LED_OFF : LED_SLOW_BLINK);
  }

  TelemetryPacket pkt;
  int  rssi = 0;
  bool rssiValid = false;
  LoRaRxResult r = loraPoll(&pkt, &rssi, &rssiValid);

  if (r == LORA_RX_CRC) {
    s_crcErrs++;
    Serial.printf("# CORRUPT packet (CRC fail) #%lu\n", (unsigned long)s_crcErrs);
    ledDoubleBlink();
    return;
  }
  if (r != LORA_RX_OK) return;

  // ---- Validation ----------------------------------------------------------
  if (pkt.version != TELEM_PROTOCOL_VERSION) {
    Serial.printf("# DROP: proto mismatch (got %u, want %u)\n",
                  pkt.version, TELEM_PROTOCOL_VERSION);
    return;
  }
  if (ACCEPT_SONDE_ID != 0 && pkt.sonde_id != ACCEPT_SONDE_ID) {
    Serial.printf("# DROP: sonde id 0x%02X (want 0x%02X)\n", pkt.sonde_id, ACCEPT_SONDE_ID);
    return;
  }

  // A valid packet ends a scan.
  if (s_scan) {
    s_scan = false;
    Serial.printf("# SCAN locked on ch %u (%.3f MHz)\n", loraGetChannel(),
                  loraFreqMHz(loraGetChannel()));
  }

  // ---- Sequence-gap accounting ---------------------------------------------
  uint32_t gap = 0;
  if (s_haveSeq && (uint16_t)pkt.seq > (uint16_t)(s_lastSeq + 1)) {
    gap = (uint16_t)pkt.seq - (uint16_t)s_lastSeq - 1;
  }
  s_lastSeq = pkt.seq;
  s_haveSeq = true;
  s_pktCount++;
  s_lastValidMs = millis();

  ledPulse(60);

  // One-time RSSI-presence verdict: tells us whether the E22 is appending the
  // per-packet RSSI byte (REG3 bit7). If not, the config likely didn't apply.
  static bool rssiVerdict = false;
  static int  rssiMiss = 0;
  if (!rssiVerdict) {
    if (rssiValid) { rssiVerdict = true; Serial.println(F("# RSSI: per-packet RSSI OK")); }
    else if (++rssiMiss >= 10) { rssiVerdict = true;
      Serial.println(F("# RSSI: no appended byte from E22 (REG3 not applied? check 'E22 config' ACK)")); }
  }

  int  noiseDbm = 0, snrDb = 0;
  bool sigValid = loraGetNoise(&noiseDbm, NULL, &snrDb);

  csvPrintRow(&pkt, millis(), gap, s_pktCount, rssi, rssiValid,
              noiseDbm, snrDb, sigValid);
}
