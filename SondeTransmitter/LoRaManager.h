// =============================================================================
// LoRaManager.h  (transmitter)  -- Ebyte E22-900T22S transceiver driver
//
// Long-range config (0.3 kbps air rate, 22 dBm). Driven directly over the E22
// UART protocol. The sonde TRANSMITS telemetry and, until locked, also LISTENS
// for ground commands (transparent mode is bidirectional). Channel can be
// retuned at runtime.
// =============================================================================
#pragma once
#include "TelemetryPacket.h"
#include "CommandPacket.h"
#include <stdbool.h>
#include <stdint.h>

bool    loraBegin(uint8_t channel);            // configure E22 on this channel
void    loraSendPacket(const TelemetryPacket* p);
bool    loraPollCommand(CommandPacket* out);   // non-blocking; true on valid cmd
bool    loraSetChannel(uint8_t channel);       // runtime retune (writes REG2)
uint8_t loraGetChannel();
float   loraFreqMHz(uint8_t channel);          // 850.125 + channel
