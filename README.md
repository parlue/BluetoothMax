# BluetoothMax

BluetoothMax is an independent open-source gateway for the MILLENNIUM ChessLink
protocol. It replaces the cable between a Bluetooth-enabled MILLENNIUM e-board
and a compatible chess computer module with a Bluetooth LE connection.

The current prototype connects:

```text
MILLENNIUM Supreme T2 BT e-board
              ⇅ Bluetooth LE
        ESP32-C3 SuperMini
              ⇅ UART / 3.3 V
       HW-027 with MAX3232
              ⇅ RS-232
        4-pin Mini-DIN
              ⇅
Generic chess computer module with DIN connector
```

## Web installer

Flash the gateway firmware directly from a supported browser (Chrome or
Edge), no toolchain install required:

**[https://parlue.github.io/BluetoothMax/](https://parlue.github.io/BluetoothMax/)**

Connect an ESP32-C3 SuperMini (or compatible board) via USB and follow the
on-page instructions. See [`docs/`](docs) for how the installer is built and
served.

## Project status

**Working.** The gateway holds a full, continuous game exchange between the
chess computer module and the e-board: the module reads and writes its
EEPROM registers over the cable exactly as it would with the original wired
peripheral, LED move suggestions are decoded and forwarded correctly, and
moves (including castling) are tracked and confirmed correctly in both
directions for the length of a real game.

Getting here took a long protocol- and hardware-level investigation. The
short version: the Mode B/ChessLink protocol implementation (framing, odd
parity, checksums, status/LED encoding, register semantics) was correct
early on and cross-validated against multiple independent open-source
ChessLink implementations. The actual, final blocker turned out to be the
**physical pin assignment of the hand-built 4-pin Mini-DIN cable** between
the gateway and the chess computer module -- see
[Electrical interfaces](#electrical-interfaces) below for the
confirmed-correct pinout. No amount of protocol-level correctness could work
around a cable that wasn't wired the way the module's receiver expected.

The published firmware for users remains unchanged until a new version is
explicitly approved for release.

## Components

- ESP32-C3 SuperMini
- HW-027 RS-232-to-TTL module with MAX3232
- 9 V to 3.2 V DC/DC step-down converter (TBD -- exact part/type to be filled in)
- 4-pin Mini-DIN plug or suitable connection cable
- Wires and suitable plug or solder connections
- Optional enclosure, strain relief and insulation material

## Electrical interfaces

### ChessLink cable side

Confirmed-correct pin assignment for the 4-pin Mini-DIN connector, viewed
face-on into the socket, keyway/notch at the bottom, guide pin at the top:

| Clock position | Wire color | Function |
|---:|---|---|
| 1 o'clock | Yellow | +9 V supply (from the chess computer module) |
| 5 o'clock | Black | RS-232 IN (into the level-shifter's input) |
| 7 o'clock | Green | GND |
| 11 o'clock | Red | RS-232 OUT (from the level-shifter's output) |

This was established by direct comparison against a known-working peripheral
bridge's cable and confirmed by measuring pin-by-pin against the module's own
socket. Earlier prototype cables used a plausible-looking but incorrect
sequential pin numbering, and that mismatch was the actual root cause behind
weeks of otherwise-correct-looking protocol traffic going nowhere -- if
you're building your own cable, trust this table over any datasheet-style
"pin 1/2/3/4" numbering, and verify against a multimeter/oscilloscope before
trusting a new build.

### Serial settings

- 38400 baud
- 7 data bits
- Odd parity
- 1 stop bit (`7O1`)

The firmware sends this as plain 8N1 with the odd-parity bit computed in
software and packed into the 8th data bit -- this produces the identical
10-bit wire waveform as native 7O1 framing while sidestepping inconsistent
7O1 support in common USB-UART hardware. This matches how other independent
ChessLink implementations handle the same spec text.

## Wiring diagram

### Power

```text
Module +9 V     ──> DC/DC IN+
Module GND      ──> DC/DC IN-

DC/DC OUT 3.2 V ──> ESP32-C3 3V3
DC/DC GND       ──> ESP32-C3 GND

ESP32-C3 3V3    ──> HW-027 VCC (+), TTL side
ESP32-C3 GND    ──> HW-027 GND (-), TTL side
```

All components must share a common ground. The 9 V cable supply must never be
connected directly to an ESP32 GPIO or to the ESP32 3.3 V pin.

### Data lines

```text
Module TX (RS-232 OUT, see pinout table)
    ──> HW-027 RS-232 input
    ──> HW-027 TTL output
    ──> ESP32-C3 GPIO20 (RX)

ESP32-C3 GPIO21 (TX)
    ──> HW-027 TTL input
    ──> HW-027 RS-232 output
    ──> Module RX (RS-232 IN, see pinout table)
```

GPIO20 is used exclusively as the receive path from the chess computer
module. GPIO21 is the transmit path to the module.

## Important safety notes

- Connect the ESP32 only to the **TTL side** of the HW-027.
- Verify supply voltages and ground with a multimeter before connecting the
  hardware.
- Power the TTL side of the HW-027 with 3.3 V.
- Never connect RS-232 levels directly to an ESP32 GPIO.
- Check the viewing direction of Mini-DIN connectors: the solder side and plug
  side are mirrored, and the pinout table above is specified face-on into the
  socket -- double-check orientation before trusting a measurement.
- Disconnect power before changing any wiring.

## Firmware

PlatformIO environment in this repository:

```ini
[env:esp32-c3-supermini]
```

The firmware operates as a bidirectional gateway between the chess
computer's serial Mode B interface and the e-board's transparent BLE UART
service: it scans for and connects to the e-board over BLE, and speaks Mode B
over the cable to the chess computer module, translating and forwarding
board status, LED move suggestions, and register reads/writes between the
two sides.

For a ready-to-flash build, see [Web installer](#web-installer) above.

## Trademark, copyright and protocol notice

BluetoothMax is an independent, unofficial interoperability project. It is not
affiliated with, endorsed by or sponsored by MILLENNIUM 2000 GmbH.

MILLENNIUM, ChessLink and related product names, trademarks, documentation and
protocol specifications remain the property of their respective rights
holders. This project does not claim ownership of the ChessLink protocol and
does not distribute original MILLENNIUM firmware, software or other copyrighted
material.
