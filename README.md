# PTHSonde

**A complete, open radiosonde (weather-balloon) telemetry system** — flight
hardware firmware, a long-range LoRa ground link, and a packaged desktop ground
station that shows live atmospheric data and renders a full SHARPpy sounding.

PTHSonde flies a small ESP32-C3 payload that measures **P**ressure, **T**emperature,
and **H**umidity (plus wind, GPS, and battery), streams it over 915 MHz LoRa to a
ground receiver, and displays it in a one-click Windows app: live Skew-T, hodograph,
time-series, a GFS-fed landing-prediction map, and a full SHARPpy SPC analysis panel.

---

## System at a glance

```
   ┌──────────────────────── SONDE (in the air) ────────────────────────┐
   │  ESP32-C3-WROOM                                                     │
   │   • SHT41         temperature / relative humidity                   │
   │   • MS5611        barometric pressure  (drives altitude + ascent)   │
   │   • MCP3221 + NTC thermistor (primary air temperature)             │
   │   • INA219        battery voltage / current                         │
   │   • Quectel L86-M33 GPS   (Balloon mode — good to 80 km)            │
   │   • Ebyte E22-900T22S  LoRa @ 915 MHz, 22 dBm, 0.3 kbps (max range) │
   └───────────────────────────────┬────────────────────────────────────┘
                                    │  915 MHz LoRa
   ┌────────────────────────────────▼───────────────────────────────────┐
   │  GROUND RECEIVER — ESP32-C3 + E22, decodes packets, prints CSV      │
   └───────────────────────────────┬────────────────────────────────────┘
                                    │  USB serial
   ┌────────────────────────────────▼───────────────────────────────────┐
   │  PTHSonde DESKTOP APP  (Windows)                                    │
   │   • Python (Flask) owns the serial port + records the flight CSV    │
   │   • Native window (pywebview) renders the dashboard (single HTML)   │
   │   • SHARPpy renders the real SPC Skew-T/analysis panel on demand    │
   └────────────────────────────────────────────────────────────────────┘
```

## Repository layout

| Path | What it is |
|------|------------|
| [`SondeTransmitter/`](SondeTransmitter/) | **Flight firmware** (Arduino/ESP32-C3). Reads the sensors + GPS and transmits telemetry over LoRa. |
| [`GroundReceiver/`](GroundReceiver/) | **Ground-station firmware** (Arduino/ESP32-C3). Receives LoRa packets, prints them as CSV over USB. |
| [`Dashboard/`](Dashboard/) | The ground-station UI — a single self-contained `PTHSonde.html` (+ logo). |
| [`processor/`](processor/) | The Python app: `app.py` (pywebview launcher), `sonde_server.py` (Flask + serial + flight recording), `sharppy_render.py` (headless SHARPpy), and the PyInstaller spec. |
| [`docs/`](docs/) | Hardware map, firmware build/flash guide, and dashboard guide. |
| [`pins.txt`](pins.txt) | Verified ESP32-C3 pin map for the sonde board. |
| `Start PTHSonde.bat` | One-click launcher (runs the app from source). |

## Quick start

**Run the ground station (recommended):** download the prebuilt Windows app from
the [**Releases**](../../releases) page, unzip, and run `PTHSonde.exe`. Connect the
ground receiver over USB, select its COM port, and click **Connect**.

**Run from source** (needs Python 3.13):
```bash
pip install -r processor/requirements.txt      # flask, pyserial, pywebview, ...
# then, from the repo root:
Start PTHSonde.bat                              # or:  py -3.13 processor/app.py
```

**Build the firmware:** open `SondeTransmitter/SondeTransmitter.ino` and
`GroundReceiver/GroundReceiver.ino` in the Arduino IDE (ESP32-C3 board support,
`USB CDC On Boot: Enabled`) and flash each board. See
[`docs/firmware.md`](docs/firmware.md).

## Documentation

- [**docs/hardware.md**](docs/hardware.md) — boards, sensors, pin map, LoRa RF plan, GPS balloon mode.
- [**docs/firmware.md**](docs/firmware.md) — building & flashing both firmwares, the telemetry protocol, LED codes.
- [**docs/dashboard.md**](docs/dashboard.md) — using the app, recording a flight, the SHARPpy analysis, building the `.exe`.

## Key features

- **SHARPpy SPC analysis** — an SPC-style Skew-T and index panel rendered headless
  from each flight's own data.
- **Barometric flight data** — altitude, ascent rate, and the sounding are derived
  directly from the MS5611 pressure sensor, so the profile continues past the GPS
  altitude ceiling.
- **Thermal-soak correction** — pad-heated temperature and humidity sensors carry
  residual heat into the early climb, producing a non-physical superadiabatic layer
  near the surface. The processor fits the true lapse rate from the clean free
  atmosphere and removes this layer so lifted-parcel indices reflect the real profile.
- **Confirmed balloon-mode GPS** — the sonde asserts and verifies the L86 receiver's
  high-altitude (80 km) mode, extending position tracking beyond the 12 km
  standard-mode ceiling.
- **Long-range LoRa link** — the Ebyte E22 is configured at 22 dBm and a 0.3 kbps
  air rate for maximum link margin.

---

*PTHSonde — Tracer Lab.*
