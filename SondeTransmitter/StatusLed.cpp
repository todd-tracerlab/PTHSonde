#include "StatusLed.h"
#include "BoardPins.h"
#include <Arduino.h>

static LedPattern s_bg = LED_OFF;

// One-shot event sequence: alternating on/off durations (ms), starting with on.
#define LED_EVT_MAX 6
static uint16_t s_evt[LED_EVT_MAX];
static uint8_t  s_evtLen   = 0;
static uint32_t s_evtStart = 0;

static inline void writeLed(bool on) {
#if STATUS_LED_ACTIVE_LOW
  digitalWrite(PIN_STATUS_LED, on ? LOW : HIGH);
#else
  digitalWrite(PIN_STATUS_LED, on ? HIGH : LOW);
#endif
}

void ledBegin() {
  pinMode(PIN_STATUS_LED, OUTPUT);
  s_bg = LED_OFF;
  s_evtLen = 0;
  writeLed(false);
}

void ledSetPattern(LedPattern p) { s_bg = p; }

static void startSeq(const uint16_t* seq, uint8_t len) {
  if (len > LED_EVT_MAX) len = LED_EVT_MAX;
  for (uint8_t i = 0; i < len; i++) s_evt[i] = seq[i];
  s_evtLen   = len;
  s_evtStart = millis();
}

void ledPulse(uint16_t on_ms) {
  uint16_t seq[1] = { on_ms };
  startSeq(seq, 1);
}

void ledDoubleBlink() {
  uint16_t seq[3] = { 60, 90, 60 };   // on, off, on
  startSeq(seq, 3);
}

// Returns true while a one-shot event is running; *on = current level.
static bool eventLevel(uint32_t now, bool* on) {
  if (s_evtLen == 0) return false;
  uint32_t elapsed = now - s_evtStart;
  uint32_t acc = 0;
  for (uint8_t i = 0; i < s_evtLen; i++) {
    if (elapsed < acc + s_evt[i]) { *on = ((i & 1) == 0); return true; }
    acc += s_evt[i];
  }
  s_evtLen = 0;   // sequence finished
  return false;
}

static bool backgroundLevel(uint32_t now) {
  switch (s_bg) {
    case LED_ON:         return true;
    case LED_SLOW_BLINK: return (now % 2000UL) < 150UL;   // flash every 2 s
    case LED_FAST_BLINK: return (now % 200UL)  < 100UL;   // ~5 Hz
    case LED_OFF:
    default:             return false;
  }
}

void ledUpdate() {
  uint32_t now = millis();
  bool on;
  if (eventLevel(now, &on)) writeLed(on);
  else                      writeLed(backgroundLevel(now));
}
