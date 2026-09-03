# BluetoothMax

BluetoothMax is an independent source-available Bluetooth LE gateway for chess
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

<p>
  <img src="images/module-usb-c.jpg" alt="BluetoothMax module, USB-C side" width="300">
  <img src="images/module-minidin.jpg" alt="BluetoothMax module, Mini-DIN cable side" width="300">
</p>

**Full user manual (PDF):** [English](BluetoothMax-Manual-EN.pdf) &middot;
[Deutsch](BluetoothMax-Handbuch-DE.pdf)

## Supported e-boards

| Board | Status |
|---|---|
| MILLENNIUM Supreme T2 BT (and other genuine ChessLink boards) | Working -- native Mode B relayed as-is |
| Chessnut Air / GO / Pro | Working -- Chessnut's own BLE protocol translated to/from Mode B, including LED move suggestions and New Game/reset highlighting |
| ManyaCynus (camera-vision chess robot) | Working -- ManyaCynus's own line-based BLE protocol translated to/from Mode B; decodes the chess computer's own LED move suggestions and commands ManyaCynus's arm to execute them, including castling, en passant and pawn promotion |
| iChessOne | Untested on real hardware -- driver built from the vendor's own documented BLE protocol (Nordic UART Service), not yet confirmed against a real unit |

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
- **Illegal-move check disabled automatically (firmware 1.4.2+ only)**: on
  every connect, the gateway asks Cynus for its app version and, if it's
  1.4.2 or newer, sends `set illegal move check off` -- Cynus's own
  onboard legality checking otherwise fights the gateway whenever an
  external chess computer (not Cynus's own built-in engine) is actually
  driving the game. Units on older firmware are left untouched; update
  Cynus's own firmware to get this.
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
- **Recovering from an unexpected reconnect**: if ManyaCynus loses its BLE
  connection mid-game (e.g. it resets itself) and reconnects, the gateway
  would normally insist on seeing the exact starting position again before
  resuming, blocking the game in progress. Lifting both Kings off the board
  at any point -- right after reconnecting, or even mid-game -- arms a
  manual override instead: the display shows "newpos", automatic rescanning
  stops, and the gateway waits (no time limit) for the desired position to
  be rebuilt. Once both Kings are back on the board, press ManyaCynus's own
  Clock/Scan button to submit that position as-is, no legality check --
  exactly what a real chess computer's own "set up position" mode expects.

## Startup sequence

Follow this order every time, especially when using a Mephisto Phoenix or
Chess Element module -- both can take a few seconds to boot, and the gateway
only gets one look at the cable during that window:

1. **Make sure the USB port has no power on it.** Don't have the gateway
   powered from USB (e.g. still plugged into a computer or power bank) at
   the same time it's powered from the chess computer module's cable -- power
   the gateway from the module only, exactly as the [wiring diagram](#power)
   describes.
2. Power on the chess computer module and **wait until it has fully booted**
   before doing anything else.
3. **Only then** switch on the e-board (Chessnut, MILLENNIUM Supreme T2 BT,
   ManyaCynus, etc.).

If the module is unusually slow to boot and the gateway's own 5-second cable
check elapses first, it falls back to [standalone mode](#standalone-mode-no-cable-computer-required)
as described below -- but this is self-correcting: as soon as the gateway
sees any data arrive on the cable after that, even minutes later, it
automatically drops any standalone BLE-to-BLE connection and restarts itself
into normal cable mode. No manual power cycle is needed to recover from a
slow-booting module.

## Standalone mode (no cable computer required)

If no chess computer module is detected on the cable within 5 seconds of
power-up, the gateway switches into a standalone mode: it keeps its normal
BLE connection to the e-board, and **additionally** starts advertising itself
as a BLE peripheral -- masquerading as either a genuine ChessLink board or a
genuine Chessnut board -- so wireless chess software can connect directly to
the gateway with no chess computer module in the loop at all. As covered
under [Startup sequence](#startup-sequence) above, this is automatically
undone (with a self-restart back into normal cable mode) the moment cable
data is later detected, so it's safe even if a module just happened to boot
slowly rather than being genuinely absent.

**Setup sequence** (all signalled on the connected e-board itself -- LED
squares light up on Millennium/Chessnut boards, ManyaCynus shows text):

1. The gateway connects to the e-board as usual and confirms its starting
   position.
2. **Ready signal**: the four center squares (d4/d5/e4/e5) light up (ManyaCynus:
   "OK") for 3 seconds, then clear.
3. Place a **second white queen** on the board (every one of these e-boards'
   piece sets includes a spare queen for promotion anyway):
   - **a4** selects ChessLink masquerade -- the gateway advertises as
     "MILLENNIUM CHESS", the same name and protocol a real Millennium
     Supreme board uses.
   - **b4** selects Chessnut masquerade -- the gateway advertises as
     "Chessnut Air", speaking Chessnut's own native BLE protocol.
4. **Confirmed signal**: the four corner squares light up (ManyaCynus:
   "ChessL" / "Chnut") for 3 seconds, then clear, and the selected
   masquerade starts advertising.
5. Connect to the gateway from your chess software like you would to a real
   board of that type.

| Masquerade | Confirmed working with |
|---|---|
| ChessLink ("MILLENNIUM CHESS") | PicoChess, [BearChess](https://www.solanosoft.com/index.php?page=bearchess), and at least one other independent ChessLink client |
| Chessnut ("Chessnut Air") | [Chess PGN Master](https://pgnmaster.kalab.com/), [BearChess](https://www.solanosoft.com/index.php?page=bearchess) |

This mode is independent of, and doesn't change, normal cable operation --
if a chess computer module is present on the cable at power-up, standalone
mode never activates.

## Supported chess computer modules (cable side)

Tested working: MILLENNIUM King and Mephisto Phoenix. Both speak Mode B over
the cable, but not identically -- the gateway auto-detects a module's exact
checksum convention (plain 7-bit vs. odd-parity-encoded) from its first
frame and matches it in its own replies, so no build-time module selection
is needed here either.

## Automatic PGN recording

The gateway can automatically record every game played on the connected
e-board as a PGN file. Recording taps directly into the same board-status
stream every other part of the gateway already uses, so it works in every
supported configuration:

- Cable-connected chess computer (Phoenix/King) with any e-board
- [Standalone mode](#standalone-mode-no-cable-computer-required) (ChessLink/Chessnut masquerade)
- Plain human-vs-human over-the-board play, with nothing else connected at all

### How it works

The gateway runs its own self-contained chess rules engine (legal move
generation, check/checkmate detection, standard SAN notation) purely from
the sequence of board positions it observes -- no external engine, app or
GUI is required for recording itself.

A game is saved automatically when:

- **Checkmate** is detected -- result recorded as `1-0` or `0-1`.
- The standard **starting position** is recognized again, meaning a new
  game has begun. The just-finished game is saved with result `*`, unless
  it was fewer than 10 full moves long, in which case it's discarded
  instead (almost always a setup/test, not a real game).

A manual result signal is also available mid-game: place both kings on one
of three square pairs (a deliberate gesture, not a legal move):

| Kings on | Result |
|---|---|
| d5 and e4 | White wins (`1-0`) |
| d4 and e5 | Black wins (`0-1`) |
| d4 and d5, or e4 and e5 | Draw (`1/2-1/2`) |

It doesn't matter which specific king sits on which square within a pair --
only that the two kings together occupy exactly that pair. Manually-signaled
and checkmate results are always saved regardless of game length.

The board itself also confirms the result for 3 seconds once the gesture is
recognized: LED boards (Millennium, Chessnut) light the same d4/d5/e4/e5
squares steady (not blinking); ManyaCynus shows `1:0`, `0:1`, or `1/2:1/2`
on its display.

The 20 most recently saved games are kept in a round-robin, oldest slot
overwritten first once all 20 are in use -- except a game is removed sooner,
right after [retrieval](#retrieving-saved-games) below has confirmed it was
copied off the gateway successfully.

### Retrieving saved games

Two independent ways to get saved games off the gateway:

**Chess PGN Master, over BLE.** Put the gateway into
[standalone Chessnut masquerade mode](#standalone-mode-no-cable-computer-required)
and connect from [Chess PGN Master](https://pgnmaster.kalab.com/) like you
would to a real Chessnut board -- its own download feature retrieves saved
games directly. Getting this working required reading the real
[EasyLinkSDK](https://github.com/chessnutech/EasyLinkSDK) source: the app's
read thread only accepts raw board-status frames (the same format used for
live play), not pre-built PGN/FEN text, so the gateway replays each saved
game's recorded positions as if they were being played live. Once a
download completes, the gateway deletes that game from its own storage.

**USB, via a standalone Windows tool.** Trigger a **"queen gesture"**: set
up the standard starting position with one extra white queen placed on c4
(every one of these e-boards' piece sets includes a spare queen for
promotion anyway) -- the corner squares light up to confirm it was
recognized. Every saved game is then dumped over the gateway's native
USB-CDC connection to a PC, where a small standalone Windows tool saves each
one as its own `.pgn` file. Once the tool confirms every game saved
correctly, the gateway deletes exactly those games from its own storage --
a dump that never reaches the tool, or a partial transfer, leaves every game
in place for the next attempt. See [`pgntool/`](pgntool) for the tool itself
and full usage instructions.

This USB path was originally built as an independent fallback while the BLE
path above was still blocked on an undocumented SDK step -- both are now
confirmed working, so use whichever is more convenient.

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
ChessLink and Chessnut masquerades are both confirmed the same way against
real client software, including BearChess -- see that section for the full
client compatibility list.

## Version history

| Version | Adds |
|---|---|
| v1.0 | MILLENNIUM ChessLink board support (single-board) |
| v2.0 | + Chessnut Air/GO/Pro support (multi-board) |
| v3.0 | + Mephisto Phoenix chess computer support (cable-side checksum auto-detect) |
| v4.0 | + ManyaCynus robot support (castling, en passant, promotion) |
| v5.0 | + [Standalone mode](#standalone-mode-no-cable-computer-required): BLE-to-BLE ChessLink/Chessnut masquerade, no cable chess computer required |
| v5.1 | Fix: Chessnut+Mephisto Phoenix status checksum (single-square changes and captures could silently fail to register); standalone mode now recovers automatically if a cable module boots slowly instead of needing a power cycle |
| v5.2 | Fix: BearChess's ChessLink move-suggestion LEDs (a ghost-square filter built for Mephisto Phoenix's own reset splash was wrongly eating small real move suggestions from BearChess) |
| v6.0 | + [Automatic PGN game recording](#automatic-pgn-recording) with USB retrieval (queen-gesture trigger, standalone Windows tool, delete-on-confirmed-transfer) |
| v6.1 | Fix: the manual king-gesture result signal could be lost entirely if its target squares (d4/d5/e4/e5) were occupied by other pieces or the two kings didn't arrive together -- those in-progress states now get unlimited patience instead of counting against (and potentially triggering) the desync-recovery timeout |
| v6.2 | Chess PGN Master's BLE game download now actually works (sends raw board-status frames instead of pre-built text, matching what the real EasyLinkSDK's read thread expects) and properly deletes each game from the gateway after a successful pickup; + initial (real-hardware-untested) iChessOne board driver |
| v6.3 | Manual king-gesture game results are now also confirmed on the board itself (3s LED/display signal); + ManyaCynus manual position override to recover from an unexpected mid-game BLE reconnect without losing the game |
| v6.4 | ManyaCynus: on firmware 1.4.2+, the gateway now auto-detects the version and disables Cynus's own onboard illegal-move checking, which previously could interfere with an external chess computer driving the game |
| v6.5 (current) | Fix: the standalone-mode masquerade gesture (second white queen on a4/b4) never worked on ManyaCynus -- the gateway's own single-legal-move check rejected the extra queen before the masquerade selection logic ever saw it; confirmation signal text changed to "ChessL"/"Chnut", shown for 3s (was "CSLMode"/"NutMode", 2s) |

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

For a ready-to-flash build, see [Web installer](#web-installer) above.

## Contact

Board manufacturers who would like their board supported, and anyone who'd
like a ready-built device but can't solder one themselves, are welcome to
get in touch: dsommerfeld@mac.com

## License

Licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE). Any
noncommercial use -- building it for yourself, hobby projects, research -- is
permitted; commercial use and distribution are reserved to the copyright
holder.

## Trademark, copyright and protocol notice

BluetoothMax is an independent, unofficial interoperability project. It is not
affiliated with, endorsed by or sponsored by MILLENNIUM 2000 GmbH, Phoenix
Chess Systems, or any other vendor named in this document.

MILLENNIUM, ChessLink, Chessnut, ManyaCynus, Mephisto Phoenix and related
product names, trademarks, documentation and protocol specifications remain
the property of their respective rights holders -- Mephisto Phoenix is a
product of Phoenix Chess Systems (Netherlands, phoenixcs.nl), developed in
cooperation with MILLENNIUM 2000. This project does not claim ownership of
any of these protocols and does not distribute original firmware, software
or other copyrighted material from any vendor.
