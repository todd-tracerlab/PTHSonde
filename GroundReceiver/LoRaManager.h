// =============================================================================
// LoRaManager.h  (receiver)  -- Ebyte E22-900T22S driver
//
// Long-range config (0.3 kbps), configured identically to the transmitter, with
// the per-packet RSSI byte (REG3 bit7) and ambient-noise RSSI register (REG1
// bit5) enabled. Also sends ground->sonde commands and can retune at runtime.
// =============================================================================
#pragma once
#include "TelemetryPacket.h"
#include "CommandPacket.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  LORA_RX_NONE = 0,    // nothing complete yet
  LORA_RX_OK   = 1,    // a valid packet was decoded
  LORA_RX_CRC  = 2     // a sync-matched frame failed CRC (corrupt)
} LoRaRxResult;

bool    loraBegin(uint8_t channel);            // configure the E22 on this channel
LoRaRxResult loraPoll(TelemetryPacket* out, int* rssi_dbm, bool* rssi_valid);

bool    loraSetChannel(uint8_t channel);       // runtime retune (writes REG2)
uint8_t loraGetChannel();
float   loraFreqMHz(uint8_t channel);          // 850.125 + channel
void    loraDumpRegisters();                    // read + print the E22's actual registers
void    loraHoldConfig(bool config);            // force M0/M1 high (config) or low (run) for probing
void    loraTxTest();                            // test the ESP->module TX line via AUX (no meter)
void    loraModeTest();                           // test M0 and M1 lines independently via AUX

// Send a ground->sonde command (transmitted on the current channel).
void    loraSendCommand(uint8_t cmd, uint8_t arg, uint8_t sonde_id);

// ---- Ambient-noise / pseudo-SNR (E22 register read) ------------------------
void    loraRequestNoise();
bool    loraGetNoise(int* noise_dbm, int* lastrx_dbm, int* snr_db);
