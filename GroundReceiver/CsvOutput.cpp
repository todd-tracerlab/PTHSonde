#include "CsvOutput.h"
#include <Arduino.h>

// Column names are kept stable for the dashboard. Fields the v2 packet no longer
// carries (uptime, date, MS temp, raw ADC) are emitted as "NA"; power is
// re-derived on the ground from voltage x current.
void csvPrintHeader() {
  Serial.println(
    "rx_ms,sonde_id,seq,seq_gap,pkt_count,proto,uptime_s,"
    "gps_fix,pps,sats,utc_date,utc_time,lat_deg,lon_deg,alt_m,speed_mps,course_deg,"
    "sht_temp_c,sht_rh_pct,ms_press_pa,ms_temp_c,mcp_raw,therm_temp_c,wind_deg,"
    "batt_v,current_ma,power_mw,status_flags,valid_flags,"
    "v_sht,v_ms,v_mcp,v_ina,v_therm,v_wind,v_gps,"
    "rssi_dbm,noise_dbm,snr_db,sonde_ch,locked,crc_ok"
  );
}

void csvPrintRow(const TelemetryPacket* p,
                 uint32_t rx_ms,
                 uint32_t seq_gap,
                 uint32_t pkt_count,
                 int rssi_dbm,
                 bool rssi_valid,
                 int noise_dbm,
                 int snr_db,
                 bool sig_valid) {
  char buf[480];

  char timeStr[12];
  snprintf(timeStr, sizeof(timeStr), "%02u:%02u:%02u", p->hour, p->minute, p->second);

  char rssi[12], noise[12], snr[12];
  if (rssi_valid) snprintf(rssi, sizeof(rssi), "%d", rssi_dbm);
  else            snprintf(rssi, sizeof(rssi), "NA");
  if (sig_valid)  { snprintf(noise, sizeof(noise), "%d", noise_dbm);
                    snprintf(snr,   sizeof(snr),   "%d", snr_db); }
  else            { snprintf(noise, sizeof(noise), "NA");
                    snprintf(snr,   sizeof(snr),   "NA"); }

  double power_mw = (p->batt_mv / 1000.0) * (double)p->current_ma;  // V * mA = mW
  int locked = (p->status_flags & TF_STAT_LOCKED) ? 1 : 0;

  int n = snprintf(buf, sizeof(buf),
    "%lu,0x%02X,%u,%lu,%lu,%u,NA,"                       // rx_ms..uptime_s(NA)
    "%u,%u,%u,,%s,%.7f,%.7f,%u,%.2f,%.2f,"               // gps..course (utc_date empty)
    "%.2f,%.1f,%ld,NA,NA,%.2f,%.2f,"                     // sht..wind (ms_temp/mcp NA)
    "%.3f,%d,%.1f,0x%02X,0x%02X,"                        // batt..flags
    "%u,%u,%u,%u,%u,%u,%u,"                              // v_*
    "%s,%s,%s,%u,%d,1",                                  // rssi,noise,snr,sonde_ch,locked,crc_ok
    (unsigned long)rx_ms,
    p->sonde_id,
    p->seq,
    (unsigned long)seq_gap,
    (unsigned long)pkt_count,
    p->version,
    (p->status_flags & TF_STAT_GPS_FIX) ? 1 : 0,
    (p->status_flags & TF_STAT_GPS_PPS) ? 1 : 0,
    p->sats,
    timeStr,
    p->lat_e7 / 1e7, p->lon_e7 / 1e7,
    p->alt_m,
    p->speed_cms / 100.0,
    p->course_cd / 100.0,
    p->sht_temp_c100 / 100.0,
    p->sht_rh_x2 / 2.0,
    (long)p->press_half_pa * 2,
    p->therm_temp_c100 / 100.0,
    p->wind_dir_cd / 100.0,
    p->batt_mv / 1000.0,
    (int)p->current_ma,
    power_mw,
    p->status_flags, p->valid_flags,
    (p->valid_flags & TF_VALID_SHT)   ? 1 : 0,
    (p->valid_flags & TF_VALID_MS)    ? 1 : 0,
    (p->valid_flags & TF_VALID_MCP)   ? 1 : 0,
    (p->valid_flags & TF_VALID_INA)   ? 1 : 0,
    (p->valid_flags & TF_VALID_THERM) ? 1 : 0,
    (p->valid_flags & TF_VALID_WIND)  ? 1 : 0,
    (p->valid_flags & TF_VALID_GPS)   ? 1 : 0,
    rssi, noise, snr,
    p->cfg_channel, locked
  );

  if (n > 0) Serial.println(buf);
}
