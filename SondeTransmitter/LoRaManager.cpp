#include "LoRaManager.h"
#include "BoardPins.h"
#include <Arduino.h>
#include <string.h>

static HardwareSerial s_lora(LORA_UART_NUM);
static uint8_t        s_channel = LORA_RENDEZVOUS_CH;

float loraFreqMHz(uint8_t channel) { return 850.125f + (float)channel; }
uint8_t loraGetChannel() { return s_channel; }

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

// Wait for the module's command echo (0xC1 ...) after a register write.
// verbose=true prints the raw bytes returned (boot diagnostic).
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
    Serial.print(F("CFG resp: "));
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

  // C0 00 09  ADDH ADDL NETID REG0 REG1 REG2(ch) REG3 CRYPT_H CRYPT_L
  const uint8_t cfg[12] = {
    0xC0, 0x00, 0x09,
    0x00, 0x00, 0x00,
    LORA_REG0_VALUE, LORA_REG1_VALUE, channel, LORA_REG3_VALUE,
    0x00, 0x00
  };

  // Retry a few times with a fixed settle after entering config mode.
  bool acked = false;
  for (int attempt = 0; attempt < 4 && !acked; attempt++) {
    MODE_CONFIG();                 // config mode
    while (s_lora.available()) s_lora.read();
    s_lora.write(cfg, sizeof(cfg));
    s_lora.flush();
    acked = waitConfigAck(attempt == 0);
    if (!acked) delay(50);
  }

  MODE_NORMAL();                   // normal transparent mode
  return acked;
}

bool loraSetChannel(uint8_t channel) {
  MODE_CONFIG();
  while (s_lora.available()) s_lora.read();
  const uint8_t cmd[4] = { 0xC0, 0x05, 0x01, channel };  // write REG2 only
  s_lora.write(cmd, sizeof(cmd));
  s_lora.flush();
  bool acked = waitConfigAck(false);
  MODE_NORMAL();
  if (acked) s_channel = channel;
  return acked;
}

void loraSendPacket(const TelemetryPacket* p) {
  auxWaitHigh(200);
  s_lora.write((const uint8_t*)p, sizeof(TelemetryPacket));
  s_lora.flush();
  auxWaitHigh(200);
}

// ---- Command receive --------------------------------------------------------
// Frames on CMD_SYNC (0xC3), reads a fixed CommandPacket, validates CRC. Any
// RSSI byte the E22 appends after the frame is simply left for the next scan.
static uint8_t s_cbuf[64];
static size_t  s_clen = 0;

bool loraPollCommand(CommandPacket* out) {
  while (s_lora.available() && s_clen < sizeof(s_cbuf)) {
    s_cbuf[s_clen++] = (uint8_t)s_lora.read();
  }
  const size_t N = sizeof(CommandPacket);
  size_t i = 0;
  while (s_clen - i >= N) {
    if (s_cbuf[i] == CMD_SYNC) {
      CommandPacket tmp;
      memcpy(&tmp, s_cbuf + i, N);
      if (cmd_validate(&tmp)) {
        *out = tmp;
        size_t consumed = i + N;
        memmove(s_cbuf, s_cbuf + consumed, s_clen - consumed);
        s_clen -= consumed;
        return true;
      }
    }
    i++;
  }
  // discard scanned bytes, keep a possible partial frame tail
  if (i > 0) {
    if (i > s_clen) i = s_clen;
    memmove(s_cbuf, s_cbuf + i, s_clen - i);
    s_clen -= i;
  }
  if (s_clen >= sizeof(s_cbuf) - 1) s_clen = 0;
  return false;
}
