# Firmware

Two Arduino sketches, both for the **ESP32-C3-WROOM-02-N4**:

- [`SondeTransmitter/`](../SondeTransmitter/) — the flight payload.
- [`GroundReceiver/`](../GroundReceiver/) — the ground station.

## Building & flashing

1. Install the **Arduino IDE** and the **ESP32 board package** (Boards Manager →
   "esp32" by Espressif).
2. Select board **ESP32C3 Dev Module** and set **USB CDC On Boot: Enabled** (the
   sonde/receiver use native USB-CDC serial; without this the port wedges).
3. Install libraries via Library Manager: **TinyGPSPlus** (Mikal Hart). The sensor,
   LoRa, and packet code is included in each sketch folder.
4. Open the `.ino`, pick the board's COM port, and **Upload**.

Both sketches share the same header style (`BoardPins.h`, `TelemetryPacket.h`,
`CommandPacket.h`, `LoRaManager`, `StatusLed`). The pin map is in
[`../pins.txt`](../pins.txt) / [hardware.md](hardware.md).

## Sonde firmware (`SondeTransmitter/`)

Reads all sensors + GPS each cycle, packs a `TelemetryPacket`, and transmits it over
LoRa. Notable modules:

- **`GpsManager.cpp`** — drives the L86 and **guarantees Balloon mode**. It sends
  `PMTK886,3` every 3 s until the module ACKs `flag=3`, then re-asserts every 15 s
  as a keep-alive so an in-flight reset can't strand the GPS at the 10 km Normal-mode
  ceiling. `gpsFrModeAck()` exposes the ACK; the serial diagnostic prints
  `FR-mode ACK=<n>` (3 = balloon confirmed). **Confirm ACK=3 on the bench.**
- **`Sensors` / `Thermistor` / `BatteryMonitor`** — I²C sensor reads; the thermistor
  is the primary air temperature.
- **`WindAverage` / `WindDirection`** — vector-averaged wind from the GPS track.
- **`LoRaManager`** — E22 config + transparent-mode TX (see RF plan in hardware.md).

## Ground-receiver firmware (`GroundReceiver/`)

Receives LoRa packets, validates CRC, and prints one **CSV line per packet** over
USB (`CsvOutput`). This is what the desktop app reads. It also appends the per-packet
RSSI/noise/SNR. `Serial.setTxTimeoutMs(0)` is set so a slow USB host drops bytes
instead of wedging the link.

### Status-LED codes (ground receiver)

| Pattern | Meaning |
|---------|---------|
| Short pulse | valid packet decoded |
| Double-blink | CRC failure (heard something, garbled) |
| Slow 2 s blink | no valid data |
| Fast 5 Hz | config failure / scanning |

(LED is active-low on GPIO2.)

## Telemetry protocol

`TelemetryPacket.h` (identical on both ends) defines the binary packet: sonde ID,
sequence, GPS fix/sats/position, pressure/temp/RH/thermistor, wind, battery, and
`status_flags` / `valid_flags` bitfields. `CommandPacket.h` defines the uplink
commands (channel staging, lock/launch). The receiver flattens each packet into the
CSV columns the dashboard parses (see [dashboard.md](dashboard.md)).
