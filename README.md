# BluetoothMax

BluetoothMax is an independent open-source Bluetooth LE gateway for chess
computer modules that speak the ChessLink (Mode B) protocol over a cable. It
replaces the cable between the chess computer module and a Bluetooth-enabled
e-board with a Bluetooth LE connection -- and, unlike a single-board bridge,
it scans for and connects to **whichever supported e-board is present** at
runtime, translating each board's own native protocol to and from Mode B on
the fly. One firmware image, multiple board brands.

```text
Generic chess computer module with DIN connector
              ⇅
        4-pin Mini-DIN
              ⇅ RS-232
       HW-027 with MAX3232
              ⇅ UART / 3.3 V
        ESP32-C3 SuperMini
              ⇅ Bluetooth LE
   MILLENNIUM Supreme T2 BT  --  or --  Chessnut Air / GO / Pro
```

## Supported e-boards

| Board | Status |
|---|---|
| MILLENNIUM Supreme T2 BT (and other genuine ChessLink boards) | Working -- native Mode B relayed as-is |
| Chessnut Air / GO / Pro | Working -- Chessnut's own BLE protocol translated to/from Mode B, including LED move suggestions and New Game/reset highlighting |

Board detection is automatic: on every (re)connect attempt, the gateway scans
for any known board's advertised BLE name and connects to whichever one it
finds, with no build-time board selection needed.

## Supported chess computer modules (cable side)

Tested working: MILLENNIUM King and Mephisto Phoenix. Both speak Mode B over
the cable, but not identically -- the gateway auto-detects a module's exact
checksum convention (plain 7-bit vs. odd-parity-encoded) from its first
frame and matches it in its own replies, so no build-time module selection
is needed here either.

## Web installer

Flash the gateway firmware directly from a supported browser (Chrome or
Edge), no toolchain install required:

**[https://parlue.github.io/BluetoothMax/](https://parlue.github.io/BluetoothMax/)**

Connect an ESP32-C3 SuperMini (or compatible board) via USB and follow the
on-page instructions. See [`docs/`](docs) for how the installer is built and
served.

## Project status

**Working**, against both a genuine ChessLink board and a Chessnut board.
The gateway holds a full, continuous game exchange between the chess
computer module and whichever e-board it's connected to: the module reads
and writes its EEPROM registers over the cable exactly as it would with the
original wired peripheral, LED move suggestions are decoded and forwarded
correctly (including New Game/reset highlighting on boards that need it
translated), and moves (including castling) are tracked and confirmed
correctly in both directions for the length of a real game.

Getting the cable side working took a long protocol- and hardware-level
investigation. The short version: the Mode B/ChessLink protocol
implementation (framing, odd parity, checksums, status/LED encoding, register
semantics) was correct early on and cross-validated against multiple
independent open-source ChessLink implementations. The actual, final blocker
turned out to be the **physical pin assignment of the hand-built 4-pin
Mini-DIN cable** between the gateway and the chess computer module -- see
[Electrical interfaces](#electrical-interfaces) below for the
confirmed-correct pinout. No amount of protocol-level correctness could work
around a cable that wasn't wired the way the module's receiver expected.

Adding Chessnut support meant translating between two genuinely different
LED conventions: ChessLink boards represent LED state as a 9x9 grid of
corner-shared points (so a highlighted square shares its corners with its
neighbors), while Chessnut boards have one independent LED per square. The
gateway decodes the corner grid into a plain list of highlighted squares,
disambiguating geometric side effects (two highlighted squares that are two
apart in the same file/rank can make the square between them look lit purely
because it shares corners with both) using the board's own known piece
positions.

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

Two PlatformIO environments in this repository:

```ini
[env:esp32-c3-supermini]     ; actively developed, multi-board (BLE via NimBLE-Arduino)
[env:esp32-c3-superminiv2]   ; frozen, ChessLink-only fallback (classic BLEDevice.h)
```

The firmware operates as a bidirectional gateway between the chess
computer's serial Mode B interface and whichever e-board's own BLE protocol
it connects to: on the cable side it always speaks Mode B (framing, odd
parity, checksums, LED encoding, register reads/writes) exactly as a genuine
peripheral would; on the BLE side, a real ChessLink board is relayed as-is,
while a Chessnut board's own binary protocol is translated to and from the
same Mode B representation the cable side already understands -- the rest of
the gateway (status caching, resend-on-change, LED-clear timing) is shared
and doesn't need to know which board produced the data.

`esp32-c3-superminiv2` is a frozen snapshot of the single-board
(ChessLink-only) gateway taken right before multi-board support was added,
kept as a known-good fallback build and never touched by ongoing multi-board
work.

For a ready-to-flash build, see [Web installer](#web-installer) above.

## Trademark, copyright and protocol notice

BluetoothMax is an independent, unofficial interoperability project. It is not
affiliated with, endorsed by or sponsored by MILLENNIUM 2000 GmbH.

MILLENNIUM, ChessLink, Chessnut and related product names, trademarks,
documentation and protocol specifications remain the property of their
respective rights holders. This project does not claim ownership of the
ChessLink or Chessnut protocols and does not distribute original firmware,
software or other copyrighted material from either vendor.
