# BluetoothMax

BluetoothMax is an independent open-source Bluetooth LE gateway for chess
computer modules that speak the ChessLink (Mode B) protocol over a cable. It
replaces the cable between the chess computer module and a Bluetooth-enabled
e-board with a Bluetooth LE connection -- and, unlike a single-board bridge,
it scans for and connects to **whichever supported e-board is present** at
runtime, translating each board's own native protocol to and from Mode B on
the fly. One firmware image, multiple board brands.

No cable chess computer at hand? The gateway can also run
[standalone](#standalone-mode-no-cable-computer-required), masquerading as a
ChessLink or Chessnut board over BLE so wireless chess software can connect
to it directly.

```text
Generic chess computer module with DIN connector
              ⇅
        4-pin Mini-DIN
              ⇅ RS-232
       HW-027 with MAX3232
              ⇅ UART / 3.3 V
        ESP32-C3 SuperMini
              ⇅ Bluetooth LE
MILLENNIUM Supreme T2 BT -- or -- Chessnut Air/GO/Pro -- or -- ManyaCynus robot
```

## Supported e-boards

| Board | Status |
|---|---|
| MILLENNIUM Supreme T2 BT (and other genuine ChessLink boards) | Working -- native Mode B relayed as-is |
| Chessnut Air / GO / Pro | Working -- Chessnut's own BLE protocol translated to/from Mode B, including LED move suggestions and New Game/reset highlighting |
| ManyaCynus (camera-vision chess robot) | Working -- ManyaCynus's own line-based BLE protocol translated to/from Mode B; decodes the chess computer's own LED move suggestions and commands ManyaCynus's arm to execute them, including castling, en passant and pawn promotion |

Board detection is automatic: on every (re)connect attempt, the gateway scans
for any known board's advertised BLE name and connects to whichever one it
finds, with no build-time board selection needed.

### ManyaCynus special cases

ManyaCynus is a camera-vision robot, not a per-square sensor board, so its
driver reproduces several behaviors of a real ChessLink board rather than
having them natively. This follows the same conventions as
[CynusLink](https://github.com/parlue/CynusLink), an independent project for
the same robot that this gateway's ManyaCynus support is ported from.

- **Startup**: the scanned position is checked against the normal and
  mirrored starting position before anything else; an incorrect setup is
  rescanned automatically every 5 seconds until it matches.
- **Options via the black King**: with the board in its starting position,
  moving only the black King onto one of the following squares toggles a
  setting instead of being treated as a move -- move it back to its home
  square (or the option's own "off" square) to leave the option again:

  | Black King square | Function |
  |---|---|
  | e5 / e6 | Sound off / on |
  | h5 / h6 | Board-orientation flip on / off |
  | d5 / d6 | Free Analysis mode on / off |
  | c5 / c6 | Set Position mode on / off |

- **Free Analysis mode**: while active, every scanned position is forwarded
  to the chess computer as-is, without checking it's a legal move -- the
  board is re-scanned automatically every 5 seconds. The connected chess
  software also needs its own support for this mode.
- **Castling, en passant and promotion**: reported to the chess computer as
  the individual piece lift/place steps a physical board would produce
  (e.g. king lifted, king placed, rook lifted, rook placed for castling),
  not as a single jump to the final position.
- **Display**: ManyaCynus's own screen shows the last move made (e.g.
  "E2-E4", "0-0" for castling), "POS OK" once the starting position is
  confirmed, and the specific offending square(s) (e.g. "+E4") when a scan
  doesn't settle into a legal position.

## Standalone mode (no cable computer required)

If no chess computer module is detected on the cable within 5 seconds of
power-up, the gateway switches permanently (until the next power cycle) into
a standalone mode: it keeps its normal BLE connection to the e-board, and
**additionally** starts advertising itself as a BLE peripheral -- masquerading
as either a genuine ChessLink board or a genuine Chessnut board -- so wireless
chess software can connect directly to the gateway with no chess computer
module in the loop at all.

**Setup sequence** (all signalled on the connected e-board itself -- LED
squares light up on Millennium/Chessnut boards, ManyaCynus shows text):

1. The gateway connects to the e-board as usual and confirms its starting
   position.
2. **Ready signal**: the four center squares (d4/d5/e4/e5) light up (ManyaCynus:
   "OK") for 2 seconds, then clear.
3. Place a **second white queen** on the board (every one of these e-boards'
   piece sets includes a spare queen for promotion anyway):
   - **a4** selects ChessLink masquerade -- the gateway advertises as
     "MILLENNIUM CHESS", the same name and protocol a real Millennium
     Supreme board uses.
   - **b4** selects Chessnut masquerade -- the gateway advertises as
     "Chessnut GO", speaking Chessnut's own native BLE protocol.
4. **Confirmed signal**: the four corner squares light up (ManyaCynus:
   "CSLMode" / "NutMode") for 2 seconds, then clear, and the selected
   masquerade starts advertising.
5. Connect to the gateway from your chess software like you would to a real
   board of that type.

| Masquerade | Confirmed working with | Known issue |
|---|---|---|
| ChessLink ("MILLENNIUM CHESS") | PicoChess, and at least one other independent ChessLink client | -- |
| Chessnut ("Chessnut GO") | [Chess PGN Master](https://pgnmaster.kalab.com/) | Does not currently work with [BearChess](https://www.solanosoft.com/index.php?page=bearchess): the BLE link itself connects and negotiates correctly (MTU and PHY both succeed), but BearChess's own client never proceeds to GATT discovery/use. Root cause not yet identified; being investigated with BearChess's developer. |

This mode is independent of, and doesn't change, normal cable operation --
if a chess computer module is present on the cable at power-up, standalone
mode never activates.

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

**Working**, against a genuine ChessLink board, a Chessnut board, a ManyaCynus
robot, and both a MILLENNIUM King and a Mephisto Phoenix chess computer
module. The gateway holds a full, continuous game exchange between the chess
computer module and whichever board it's connected to: the module reads and
writes its EEPROM registers over the cable exactly as it would with the
original wired peripheral, LED move suggestions are decoded and forwarded
correctly (including New Game/reset highlighting on boards that need it
translated), and moves -- including castling, en passant and promotion -- are
tracked and confirmed correctly in both directions for the length of a real
game. [Standalone mode](#standalone-mode-no-cable-computer-required)'s
ChessLink masquerade is confirmed the same way against real ChessLink
client software; its Chessnut masquerade against Chess PGN Master -- see
that section for the one known client incompatibility.

## Version history

| Version | Adds |
|---|---|
| v1.0 | MILLENNIUM ChessLink board support (single-board) |
| v2.0 | + Chessnut Air/GO/Pro support (multi-board) |
| v3.0 | + Mephisto Phoenix chess computer support (cable-side checksum auto-detect) |
| v4.0 | + ManyaCynus robot support (castling, en passant, promotion) |
| v5.0 (current) | + [Standalone mode](#standalone-mode-no-cable-computer-required): BLE-to-BLE ChessLink/Chessnut masquerade, no cable chess computer required |

v1.0 and v3.0 are also kept as frozen fallback PlatformIO environments in
this repository (`esp32-c3-superminiv2` and `esp32-c3-superminiv3`
respectively) -- see [Firmware](#firmware) below. v2.0 was superseded by
v3.0 before it was ever frozen as its own environment.

## Components

- ESP32-C3 SuperMini
- HW-027 RS-232-to-TTL module with MAX3232
- 9 V to 3.2 V DC/DC step-down converter (Pololu D36V6F3)
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

If you're building your own cable, trust this table over any datasheet-style
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

Actively-developed PlatformIO environment in this repository:

```ini
[env:esp32-c3-supermini]
```

The firmware operates as a bidirectional gateway between the chess
computer's serial Mode B interface and whichever board's own BLE protocol it
connects to: on the cable side it always speaks Mode B (framing, odd parity,
checksums, LED encoding, register reads/writes) exactly as a genuine
peripheral would; on the BLE side, a real ChessLink board is relayed as-is,
while a Chessnut board's own binary protocol -- or ManyaCynus's own line-based
robot-control protocol -- is translated to and from the same Mode B
representation the cable side already understands. The rest of the gateway
(status caching, resend-on-change, LED-clear timing) is shared and doesn't
need to know which board produced the data.

[Standalone mode](#standalone-mode-no-cable-computer-required)'s two BLE
masquerade roles are self-contained modules (`chesslink_server.*`,
`chessnut_server.*`) that answer every host command themselves using the
same status cache, rather than relaying a real Mode B/Chessnut peer.

Two additional environments are frozen snapshots of earlier,
independently-confirmed-working versions of this same gateway (see
[Version history](#version-history) above) -- kept as fallback builds, never
touched by ongoing development:

| Environment | Version frozen |
|---|---|
| `esp32-c3-superminiv2` | v1.0 (single-board, ChessLink-only) |
| `esp32-c3-superminiv3` | v3.0 (Millennium + Chessnut + Phoenix, before ManyaCynus) |

For a ready-to-flash build, see [Web installer](#web-installer) above.

## Contact

Board manufacturers who would like their board supported, and anyone who'd
like a ready-built device but can't solder one themselves, are welcome to
get in touch: dsommerfeld@mac.com

## Trademark, copyright and protocol notice

BluetoothMax is an independent, unofficial interoperability project. It is not
affiliated with, endorsed by or sponsored by MILLENNIUM 2000 GmbH or any other
vendor named in this document.

MILLENNIUM, ChessLink, Chessnut, ManyaCynus and related product names, trademarks,
documentation and protocol specifications remain the property of their
respective rights holders. This project does not claim ownership of any of
these protocols and does not distribute original firmware, software or other
copyrighted material from any vendor.
