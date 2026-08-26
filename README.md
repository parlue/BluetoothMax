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
MILLENNIUM The King chess computer module
```

## Project status

The project is currently in the prototype and protocol-testing stage. A
bidirectional connection to the original e-board and the original The King
module has been established. Full Mode B / ChessLink compatibility for normal
games and analysis modes is currently being tested.

The published firmware for users remains unchanged until a new version is
explicitly approved for release.

## Components

- ESP32-C3 SuperMini
- HW-027 RS-232-to-TTL module with MAX3232
- 9 V to 5 V DC/DC step-down converter
- 4-pin Mini-DIN plug or suitable connection cable
- Wires and suitable plug or solder connections
- Optional enclosure, strain relief and insulation material

## Electrical interfaces

### ChessLink cable side

| Mini-DIN pin | Function |
|---:|---|
| 1 | +9 V supply |
| 2 | GND |
| 3 | TxD |
| 4 | RxD |

TxD and RxD labels must always be checked from the perspective of the device
that transmits the signal. The signal directions in the wiring diagram below
are authoritative.

### Serial settings

- 38400 baud
- 7 data bits
- Odd parity
- 1 stop bit (`7O1`)

## Wiring diagram

### Power

```text
The King +9 V  ──> DC/DC IN+
The King GND   ──> DC/DC IN-

DC/DC OUT 5 V ──> ESP32-C3 5V/VBUS
DC/DC GND      ──> ESP32-C3 GND

ESP32-C3 3V3   ──> HW-027 VCC (+), TTL side
ESP32-C3 GND   ──> HW-027 GND (-), TTL side
```

All components must share a common ground. The 9 V cable supply must never be
connected directly to an ESP32 GPIO or to the ESP32 3.3 V pin.

### Data lines

```text
The King TX
    ──> HW-027 RS-232 input
    ──> HW-027 TTL output
    ──> ESP32-C3 GPIO20 (RX)

ESP32-C3 GPIO21 (TX)
    ──> HW-027 TTL input
    ──> HW-027 RS-232 output
    ──> The King RX
```

GPIO20 is used exclusively as the receive path from The King. GPIO21 is the
transmit path to The King.

## Important safety notes

- Connect the ESP32 only to the **TTL side** of the HW-027.
- Verify supply voltages and ground with a multimeter before connecting the
  hardware.
- Power the TTL side of the HW-027 with 3.3 V.
- Never connect RS-232 levels directly to an ESP32 GPIO.
- Check the viewing direction of Mini-DIN connectors: the solder side and plug
  side are mirrored.
- Disconnect power before changing any wiring.

## Firmware

PlatformIO environment for the current prototype:

```ini
[env:esp32-c3-supermini]
```

The firmware operates as a bidirectional gateway between the chess computer's
serial Mode B interface and the e-board's transparent BLE UART service.

## Trademark, copyright and protocol notice

BluetoothMax is an independent, unofficial interoperability project. It is not
affiliated with, endorsed by or sponsored by MILLENNIUM 2000 GmbH.

MILLENNIUM, ChessLink and related product names, trademarks, documentation and
protocol specifications remain the property of their respective rights
holders. This project does not claim ownership of the ChessLink protocol and
does not distribute original MILLENNIUM firmware, software or other copyrighted
material.
