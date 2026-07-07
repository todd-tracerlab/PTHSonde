// =============================================================================
// BoardPins.h  -- ESP32-C3-WROOM-02-N4 radiosonde board pin map
// Mirrors the verified schematic / I2C-scan map in pins.txt.
// =============================================================================
#pragma once

// ---- I2C --------------------------------------------------------------------
#define PIN_I2C_SDA      4
#define PIN_I2C_SCL      5

#define I2C_ADDR_INA219  0x40   // battery voltage / current
#define I2C_ADDR_SHT41   0x44   // temperature / RH
#define I2C_ADDR_MCP3221 0x4D   // thermistor ADC
#define I2C_ADDR_MS5611  0x77   // pressure / temperature

// ---- GPS (Quectel L86-M33, UART1) -------------------------------------------
#define PIN_GPS_RX       0       // ESP32 RX  <- GPS TXD
#define PIN_GPS_TX       1       // ESP32 TX  -> GPS RXD
#define PIN_GPS_PPS      3       // GPS 1PPS
#define GPS_UART_NUM     1
#define GPS_BAUD         9600

// ---- LoRa (Ebyte E22-900T22S, UART0) ----------------------------------------
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
// (slowest air rate = max range/sensitivity). MUST match the receiver.
// REG1 0x20 enables ambient-noise RSSI; power bits 00 = 22 dBm.
#define LORA_REG0_VALUE     0x60
#define LORA_REG1_VALUE     0x20
#define LORA_REG3_VALUE     0x80   // append per-packet RSSI byte on receive

// ---- Misc -------------------------------------------------------------------
#define PIN_STATUS_LED   2       // active-LOW
#define STATUS_LED_ACTIVE_LOW 1
#define PIN_GPIO8_PULLUP 8       // external pullup to 3V3 (informational)
