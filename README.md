# ChessL1nkWireless

**Wireless bridge for MILLENNIUM chess computers using the ChessLink
protocol.**

ChessL1nkWireless is an open-source DIY project that aims to connect
modern Bluetooth-enabled MILLENNIUM chessboards wirelessly to compatible
MILLENNIUM chess computer modules.

## 💡 The Idea

Traditionally, MILLENNIUM chessboards and chess computer modules
communicate through a 4-pin Mini-DIN cable using the ChessLink protocol.

ChessL1nkWireless replaces the chessboard side of this cable with a
small ESP32-based wireless adapter.

``` text
MILLENNIUM Bluetooth Board
        ⇅ Bluetooth LE
      ESP32-C3
        ⇅ ChessLink / UART
  4-pin Mini-DIN
        ⇅
MILLENNIUM Chess Computer
```

The first target is the **MILLENNIUM Supreme T2** and compatible
MILLENNIUM chess computer modules.

## 🔧 Hardware

The prototype is designed as an easy-to-build DIY project without a
custom PCB.

Planned components:

-   ESP32-C3 SuperMini
-   4-pin Mini-DIN socket
-   9 V → 5 V DC/DC converter
-   3D-printed enclosure
-   A few passive components and wires

The adapter is powered directly by the MILLENNIUM chess computer through
the Mini-DIN connection.

## 🔌 Mini-DIN Interface

  Pin   Function
  ----- ----------
  1     +9 V
  2     GND
  3     TxD
  4     RxD

The serial interface uses **3.3 V logic levels**.

ChessLink communication uses **38400 baud, 7 data bits, odd parity, 1
stop bit (7O1)**.

## 🚧 Project Status

**Early development / prototype stage.**

Current goals:

-   Build and test the ESP32 hardware
-   Establish Bluetooth LE communication with the chessboard
-   Implement the ChessLink serial bridge
-   Test communication with MILLENNIUM chess computer modules
-   Provide firmware, wiring documentation and printable STL files

## ⚠️ Disclaimer

This is an independent open-source hobby project and is not affiliated
with or endorsed by MILLENNIUM 2000 GmbH.
