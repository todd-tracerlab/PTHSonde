// =============================================================================
// StatusLed.h  -- active-low status LED on PIN_STATUS_LED (GPIO2)
//
// Non-blocking. A background "pattern" shows the steady state; short one-shot
// events (pulse / double-blink) momentarily override it. Call ledUpdate() every
// loop iteration to drive it.
// =============================================================================
#pragma once
#include <stdint.h>

typedef enum {
  LED_OFF = 0,
  LED_ON,
  LED_SLOW_BLINK,   // brief flash every 2 s  (e.g. searching / no link)
  LED_FAST_BLINK    // ~5 Hz                  (e.g. fault / not configured)
} LedPattern;

void ledBegin();
void ledSetPattern(LedPattern p);   // set the steady-state background pattern
void ledPulse(uint16_t on_ms);      // one-shot flash (overrides background)
void ledDoubleBlink();              // two quick flashes (e.g. error/corrupt)
void ledUpdate();                   // call frequently; non-blocking
