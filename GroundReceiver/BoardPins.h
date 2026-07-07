// =============================================================================
// BoardPins.h  -- ESP32-C3-WROOM-02-N4 radiosonde board pin map (receiver)
// Identical board; the receiver only uses the LoRa UART and the status LED.
// =============================================================================
#pragma once

// ---- I2C (present on board, unused by the receiver) -------------------------
#define PIN_I2C_SDA      4
#define PIN_I2C_SCL      5

// ---- LoRa (Ebyte E22-900T22S, UART0) ----------------------------------------
// UART crosses: ESP RX <- module TXD (GPIO20), ESP TX -> module RXD (GPIO21).
#define PIN_LORA_RX      20      // ESP32 RX  <- module TXD
#define PIN_LORA_TX      21      // ESP32 TX  -> module RXD
#define PIN_LORA_AUX     6
#define PIN_LORA_M0      7
#define PIN_LORA_M1      10
#define LORA_UART_NUM    0
#define LORA_BAUD        9600    // UART link baud (matches E22 REG0 config)

// ---- LoRa RF plan (E22-900: freq = 850.125 + channel MHz) -------------------
#define LORA_RENDEZVOUS_CH  65   // default/control channel -> 915.125 MHz
#define LORA_CH_MIN         52   // 902.125 MHz (bottom of US 902-928 ISM)
#define LORA_CH_MAX         77   // 927.125 MHz (top of US 902-928 ISM)
// REG0: UART 9600 8N1 + air data rate. 0x60=9600 baud, +0x00 = 0.3 kbps air
// (slowest air rate = max range/sensitivity). MUST match the transmitter.
// REG1 0x20 enables ambient-noise RSSI; power bits 00 = 22 dBm.
#define LORA_REG0_VALUE     0x60
#define LORA_REG1_VALUE     0x20
#define LORA_REG3_VALUE     0x80   // append per-packet RSSI byte on receive

// ---- Misc -------------------------------------------------------------------
#define PIN_STATUS_LED   2       // active-LOW
#define STATUS_LED_ACTIVE_LOW 1
