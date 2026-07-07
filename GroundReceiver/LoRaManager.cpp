#include "LoRaManager.h"
#include "BoardPins.h"
#include <Arduino.h>
#include <string.h>

static HardwareSerial s_lora(LORA_UART_NUM);
static uint8_t        s_channel = LORA_RENDEZVOUS_CH;

float loraFreqMHz(uint8_t channel) { return 850.125f + (float)channel; }
uint8_t loraGetChannel() { return s_channel; }

// #############################################################################
// #  E22-900T22S CONFIG -- MUST MATCH THE TRANSMITTER                          #
// #    REG0 0x60  9600 8N1, air 0.3 kbps (max range)                          #
// #    REG1 0x20  240-byte sub-packet, ambient-noise RSSI enabled, 22 dBm      #
// #    REG2 = channel                                                          #
// #    REG3 0x80  per-packet RSSI byte appended on receive, transparent mode   #
// #############################################################################

static bool auxWaitHigh(uint32_t timeout_ms) {
  uint32_t start = millis();
  while (digitalRead(PIN_LORA_AUX) == LOW) {
    if (millis() - start > timeout_ms) return false;
    delay(1);
  }
  return true;
}

// E22 mode pins (M0, M1):
//   (LOW, LOW)  = normal/transparent
//   (LOW, HIGH) = CONFIGURATION  <-- needed to read/write registers
//   (HIGH,HIGH) = deep sleep
static void setMode(uint8_t m0, uint8_t m1) {
  auxWaitHigh(1000);         // docs: wait until module is IDLE before switching
  digitalWrite(PIN_LORA_M0, m0);
  digitalWrite(PIN_LORA_M1, m1);
  delay(10);
  auxWaitHigh(1000);         // wait until module is ready in the new mode
  delay(5);
}
#define MODE_CONFIG()  setMode(LOW,  HIGH)   // configuration mode
#define MODE_NORMAL()  setMode(LOW,  LOW)    // transparent mode

static bool waitConfigAck(bool verbose) {
  uint32_t start = millis();
  bool acked = false;
  uint8_t resp[20]; int rn = 0;
  uint32_t window = verbose ? 300 : 500;
  while (millis() - start < window) {
    if (s_lora.available()) {
      uint8_t b = (uint8_t)s_lora.read();
      if (rn < (int)sizeof(resp)) resp[rn++] = b;
      if (b == 0xC1) { acked = true; if (!verbose) break; }
    } else delay(1);
  }
  if (verbose) {
    Serial.print(F("# CFG resp: "));
    if (rn == 0) Serial.println(F("(none) - check ESP->E22 TX GPIO21 and M0/M1 wiring"));
    else { for (int i = 0; i < rn; i++) Serial.printf("%02X ", resp[i]); Serial.println(); }
  }
  delay(10);
  while (s_lora.available()) s_lora.read();
  return acked;
}

bool loraBegin(uint8_t channel) {
  pinMode(PIN_LORA_AUX, INPUT);
  pinMode(PIN_LORA_M0, OUTPUT);
  pinMode(PIN_LORA_M1, OUTPUT);

  s_channel = channel;
  s_lora.begin(LORA_BAUD, SERIAL_8N1, PIN_LORA_RX, PIN_LORA_TX);

  const uint8_t cfg[12] = {
    0xC0, 0x00, 0x09,
    0x00, 0x00, 0x00,
    LORA_REG0_VALUE, LORA_REG1_VALUE, channel, LORA_REG3_VALUE,
    0x00, 0x00
  };

  // Retry the config write a few times -- the module can need extra settle time
  // after entering config mode, and the first attempt is sometimes missed.
  bool acked = false;
  for (int attempt = 0; attempt < 4 && !acked; attempt++) {
    MODE_CONFIG();                 // config mode (now with a fixed settle)
    while (s_lora.available()) s_lora.read();
    s_lora.write(cfg, sizeof(cfg));
    s_lora.flush();
    acked = waitConfigAck(attempt == 0); // verbose on the first attempt only
    if (!acked) delay(50);
  }
  MODE_NORMAL();                     // back to transparent
  return acked;
}

bool loraSetChannel(uint8_t channel) {
  MODE_CONFIG();
  while (s_lora.available()) s_lora.read();
  const uint8_t cmd[4] = { 0xC0, 0x05, 0x01, channel };   // write REG2 only
  s_lora.write(cmd, sizeof(cmd));
  s_lora.flush();
  bool acked = waitConfigAck(false);
  MODE_NORMAL();
  if (acked) s_channel = channel;
  return acked;
}

// Read registers 0x00..0x08 (C1 00 09) and print them. Shows the module's
// ACTUAL current config so we can confirm whether our writes took effect.
void loraDumpRegisters() {
  MODE_CONFIG();
  while (s_lora.available()) s_lora.read();
  const uint8_t rd[3] = { 0xC1, 0x00, 0x09 };
  s_lora.write(rd, sizeof(rd));
  s_lora.flush();

  uint8_t resp[24]; int rn = 0;
  uint32_t start = millis();
  while (millis() - start < 300) {
    if (s_lora.available()) { uint8_t b = (uint8_t)s_lora.read(); if (rn < (int)sizeof(resp)) resp[rn++] = b; }
    else delay(1);
  }
  MODE_NORMAL();

  Serial.print(F("# REG resp: "));
  if (rn == 0) { Serial.println(F("(none) - config-mode comms dead (TX/M0/M1)")); return; }
  for (int i = 0; i < rn; i++) Serial.printf("%02X ", resp[i]);
  Serial.println();
  if (rn >= 12 && resp[0] == 0xC1) {
    uint8_t reg0 = resp[6], reg1 = resp[7], ch = resp[8], reg3 = resp[9];
    Serial.printf("#  REG0=0x%02X REG1=0x%02X CH=%u(%.3fMHz) REG3=0x%02X  RSSIbyte=%s noiseRSSI=%s\n",
                  reg0, reg1, ch, 850.125f + ch, reg3,
                  (reg3 & 0x80) ? "ON" : "off", (reg1 & 0x20) ? "ON" : "off");
  }
}

// Force the mode pins so M0/M1 can be measured with a meter (they normally
// toggle too fast). config=true -> both HIGH (config mode), false -> both LOW.
void loraHoldConfig(bool config) {
  digitalWrite(PIN_LORA_M0, LOW);                 // M0 low for both config and normal
  digitalWrite(PIN_LORA_M1, config ? HIGH : LOW); // M1 high = configuration mode
}

// Functional test of the ESP->module TX line (GPIO21->RXD) with no meter.
// In transparent mode, if the module RECEIVES bytes from us it will transmit
// them over the air, driving AUX busy (LOW). If TX is dead, AUX never moves.
void loraTxTest() {
  digitalWrite(PIN_LORA_M0, LOW);
  digitalWrite(PIN_LORA_M1, LOW);
  delay(20);
  while (s_lora.available()) s_lora.read();

  int auxBefore = digitalRead(PIN_LORA_AUX);
  uint8_t junk[24];
  for (int i = 0; i < 24; i++) junk[i] = 0x55;
  s_lora.write(junk, sizeof(junk));
  s_lora.flush();

  bool wentBusy = false;
  uint32_t start = millis();
  while (millis() - start < 400) {
    if (digitalRead(PIN_LORA_AUX) == LOW) { wentBusy = true; break; }
  }
  Serial.printf("# TXTEST: AUX idle-before=%d, went-busy=%s => TX path %s\n",
                auxBefore, wentBusy ? "YES" : "NO",
                wentBusy ? "WORKS" : "DEAD (GPIO21->module RXD not delivering)");
  Serial.println(F("#   (run a few times; ignore a 'YES' that lines up with an incoming packet)"));
}

// Does driving a mode pin make the module react? Changing a mode pin the module
// actually sees causes AUX to dip busy (LOW) during the mode transition. A pin
// that's open produces no reaction.
static bool modePinReacts(uint8_t pin) {
  bool dipped = false;
  for (int rep = 0; rep < 2; rep++) {
    uint8_t level = rep ? LOW : HIGH;     // HIGH then back LOW = two transitions
    digitalWrite(pin, level);
    uint32_t s = millis();
    while (millis() - s < 200) { if (digitalRead(PIN_LORA_AUX) == LOW) { dipped = true; break; } }
    delay(60);
  }
  return dipped;
}

void loraModeTest() {
  digitalWrite(PIN_LORA_M0, LOW); digitalWrite(PIN_LORA_M1, LOW); delay(120);
  bool m1 = modePinReacts(PIN_LORA_M1);
  digitalWrite(PIN_LORA_M0, LOW); digitalWrite(PIN_LORA_M1, LOW); delay(120);
  bool m0 = modePinReacts(PIN_LORA_M0);
  digitalWrite(PIN_LORA_M0, LOW); digitalWrite(PIN_LORA_M1, LOW); delay(60);
  Serial.printf("# MODETEST: M1(GPIO10) %s | M0(GPIO7) %s\n",
                m1 ? "reacts (wired)" : "NO REACTION (open?)",
                m0 ? "reacts (wired)" : "NO REACTION (open?)");
  Serial.println(F("#   run 2-3x; a pin that's consistently NO REACTION is the open one -> reflow that joint"));
}

void loraSendCommand(uint8_t cmd, uint8_t arg, uint8_t sonde_id) {
  CommandPacket c;
  memset(&c, 0, sizeof(c));
  c.sonde_id = sonde_id;
  c.cmd      = cmd;
  c.arg      = arg;
  cmd_finalize(&c, TELEM_PROTOCOL_VERSION);
  auxWaitHigh(200);
  s_lora.write((const uint8_t*)&c, sizeof(c));
  s_lora.flush();
  auxWaitHigh(200);
}

// ---- Packet RX buffer + RSSI-wait ------------------------------------------
static uint8_t  s_buf[256];
static size_t   s_len = 0;
#define RSSI_WAIT_MS 25
static uint32_t s_waitStart = 0;

// ---- Ambient-noise register read state -------------------------------------
static bool     s_expectNoise = false;
static uint32_t s_noiseReqMs  = 0;
static bool     s_noiseValid  = false;
static uint8_t  s_noiseRaw    = 0;
static uint8_t  s_lastRxRaw   = 0;
#define NOISE_RESP_TIMEOUT_MS 80

void loraRequestNoise() {
  if (s_expectNoise) return;
  auxWaitHigh(50);
  const uint8_t cmd[6] = { 0xC0, 0xC1, 0xC2, 0xC3, 0x00, 0x02 };
  s_lora.write(cmd, sizeof(cmd));
  s_lora.flush();
  s_expectNoise = true;
  s_noiseReqMs  = millis();
}

static void extractNoiseResponse(uint8_t* buf, size_t* len) {
  if (!s_expectNoise) return;
  if (millis() - s_noiseReqMs > NOISE_RESP_TIMEOUT_MS) { s_expectNoise = false; return; }
  for (size_t i = 0; i + 5 <= *len; i++) {
    if (buf[i] == 0xC1 && buf[i + 1] == 0x00 && buf[i + 2] == 0x02) {
      s_noiseRaw    = buf[i + 3];
      s_lastRxRaw   = buf[i + 4];
      s_noiseValid  = true;
      s_expectNoise = false;
      memmove(buf + i, buf + i + 5, *len - (i + 5));
      *len -= 5;
      return;
    }
  }
}

bool loraGetNoise(int* noise_dbm, int* lastrx_dbm, int* snr_db) {
  if (!s_noiseValid) return false;
  // This module reports RSSI registers in the same convention as the appended
  // per-packet byte: dBm = raw - 256 (NOT -raw/2). Verified against live data.
  int n  = (int)s_noiseRaw  - 256;
  int lr = (int)s_lastRxRaw - 256;
  if (noise_dbm)  *noise_dbm  = n;
  if (lastrx_dbm) *lastrx_dbm = lr;
  if (snr_db)     *snr_db     = lr - n;   // signal above noise floor
  return true;
}

LoRaRxResult loraPoll(TelemetryPacket* out, int* rssi_dbm, bool* rssi_valid) {
  if (rssi_valid) *rssi_valid = false;
  if (rssi_dbm)   *rssi_dbm = 0;

  while (s_lora.available() && s_len < sizeof(s_buf)) {
    s_buf[s_len++] = (uint8_t)s_lora.read();
  }
  extractNoiseResponse(s_buf, &s_len);

  const size_t PKT   = sizeof(TelemetryPacket);
  const size_t FRAME = PKT + 1;            // packet + appended RSSI byte

  size_t i = 0;
  LoRaRxResult result = LORA_RX_NONE;

  while (s_len - i >= PKT) {
    if (s_buf[i] == TELEM_SYNC0 && s_buf[i + 1] == TELEM_SYNC1) {
      TelemetryPacket tmp;
      memcpy(&tmp, s_buf + i, PKT);
      if (telem_validate(&tmp)) {
        bool haveRssi = (s_len - i) >= FRAME;
        if (!haveRssi) {
          if (s_waitStart == 0) s_waitStart = millis();
          if (millis() - s_waitStart < RSSI_WAIT_MS) {
            if (i > 0) { memmove(s_buf, s_buf + i, s_len - i); s_len -= i; }
            return LORA_RX_NONE;          // wait for the appended RSSI byte
          }
        }
        s_waitStart = 0;

        *out = tmp;
        if (haveRssi && rssi_dbm && rssi_valid) {
          uint8_t rv = s_buf[i + PKT];
          *rssi_dbm = (int)rv - 256;      // E22: dBm = rssiByte - 256
          *rssi_valid = true;
        }
        size_t consumed = i + (haveRssi ? FRAME : PKT);
        memmove(s_buf, s_buf + consumed, s_len - consumed);
        s_len -= consumed;
        return LORA_RX_OK;
      } else {
        result = LORA_RX_CRC;
        i += PKT;
        if (s_len - i < PKT) break;
        continue;
      }
    }
    i++;
  }

  if (i > 0) {
    if (i > s_len) i = s_len;
    memmove(s_buf, s_buf + i, s_len - i);
    s_len -= i;
  }
  if (s_len >= sizeof(s_buf) - 1) s_len = 0;
  return result;
}
