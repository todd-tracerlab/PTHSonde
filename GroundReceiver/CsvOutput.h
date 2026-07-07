// =============================================================================
// CsvOutput.h  -- one CSV line per valid packet over the native USB Serial
// =============================================================================
#pragma once
#include "TelemetryPacket.h"
#include <stdint.h>
#include <stdbool.h>

void csvPrintHeader();

// Emit one CSV row for a decoded packet.
//   rx_ms      : receiver millis() timestamp
//   seq_gap    : packets skipped since the previous valid packet (0 = none)
//   pkt_count  : running count of valid packets
//   rssi_dbm   : E22 per-packet RSSI in dBm (valid only if rssi_valid)
//   noise_dbm  : channel ambient-noise floor in dBm
//   snr_db     : derived pseudo-SNR in dB
//   sig_valid  : whether noise_dbm/snr_db hold a real reading
void csvPrintRow(const TelemetryPacket* p,
                 uint32_t rx_ms,
                 uint32_t seq_gap,
                 uint32_t pkt_count,
                 int rssi_dbm,
                 bool rssi_valid,
                 int noise_dbm,
                 int snr_db,
                 bool sig_valid);
