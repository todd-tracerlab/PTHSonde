# Hardware

The system is two custom **ESP32-C3-WROOM-02-N4** boards: a flight **sonde** and a
**ground receiver**, linked by a 915 MHz LoRa radio on each end. The canonical,
schematic-verified pin map lives in [`../pins.txt`](../pins.txt); this doc
summarizes it and the RF/GPS configuration that matters for flights.

## Sonde board

| Part | Ref | Bus / pins | Role |
|------|-----|-----------|------|
| ESP32-C3-WROOM-02-N4 | U8 | — | MCU |
| SHT41A (0x44) | — | I²C SDA=GPIO4, SCL=GPIO5 | temperature + relative humidity |
| MS5611 (0x77) | — | I²C | barometric pressure (→ altitude, ascent rate) |
| MCP3221 (0x4D) + NTC | — | I²C | thermistor ADC → **primary** air temperature |
| INA219 (0x40) | — | I²C | battery voltage / current |
| Quectel L86-M33 | U11 | UART1: RX=GPIO0, TX=GPIO1, 1PPS=GPIO3 @ 9600 | GPS |
| Ebyte E22-900T22S | U10 | UART0: RX=GPIO20, TX=GPIO21; AUX=GPIO6, M0=GPIO7, M1=GPIO10 | LoRa radio |
| Status LED (D3) | — | GPIO2 (**active-low**) | status |

Two temperature sensors are read: the **thermistor** (fast, small bead) is the
primary air temperature used for the sounding; the **SHT41** is secondary and, with
RH, gives dewpoint. The thermistor recovers from launch-pad sun-soak much faster
than the packaged SHT41 (see the soak-trim note in [dashboard.md](dashboard.md)).

## Ground receiver board

Same ESP32-C3 + **E22-900T22S** on UART0 (identical pin map). It only needs the
radio + USB; it decodes LoRa telemetry and prints CSV lines over USB serial for the
desktop app. LED codes are in [firmware.md](firmware.md).

## LoRa RF plan (E22-900T22S)

Frequency is `850.125 + channel` MHz. The rendezvous/control channel is **65 =
915.125 MHz**; usable channels span **52–77** (902.125–927.125 MHz, the US 902–928
ISM band). The E22 config registers are set for **maximum range**:

| Reg | Value | Meaning |
|-----|-------|---------|
| REG0 | `0x60` | 9600 baud UART **+ 0.3 kbps air rate** (slowest = most sensitive) |
| REG1 | `0x20` | 240-byte subpacket, ambient-noise RSSI on, **22 dBm (max power)** |
| REG3 | `0x80` | append per-packet RSSI byte, transparent mode |

The sonde and receiver **must match** REG0/REG1/REG3. Config mode is entered with
**M0=LOW, M1=HIGH**. The received RSSI byte is `raw − 256` dBm. For the 915 MHz wire
antenna use ≈ **3.2 in** (quarter-wave).

> **Link budget:** at these settings the link has ~+55 dB of margin at 10 km. Range
> is limited by the **antenna null straight up/down** as the balloon passes overhead,
> not by the radio settings.

## GPS: high-altitude ("Balloon") mode — REQUIRED

Per the Quectel *Lx0&Lx6&LC86L&LG77L GNSS Protocol Specification*, the L86's
navigation modes have hard altitude ceilings:

| Mode | Ceiling |
|------|---------|
| Normal / Fitness / Aviation / Stationary | **10,000 m** |
| **Balloon** (`PMTK886,3`) | **80,000 m** |

A radiosonde **must** be in Balloon mode or the fix dies just past 10 km. The mode
is **volatile** (lost on every power-up/brown-out), so the firmware asserts it
continuously and *verifies the module's ACK*:

```
Send:  $PMTK886,3*2B        Reply: $PMTK001,886,3*36   ← flag 3 = accepted
```

On the bench (USB serial) the sonde prints `FR-mode ACK=3  OK (balloon, no 12km cap)`
once the L86 confirms it. **Always confirm ACK=3 before a flight.** See
[firmware.md](firmware.md) for the assert-and-keep-alive logic.
