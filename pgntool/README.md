# PGN USB Tool

A second, independent way to retrieve games recorded by BluetoothMax's [automatic PGN recording feature](../README.md) -- built because the BLE-based retrieval via Chess PGN Master turned out to be blocked on an undocumented step in Chessnut's own closed-source SDK. This tool and the firmware side define both ends of a simple protocol themselves, so there's nothing to reverse-engineer here.

## Usage

1. Plug the gateway into your PC via USB (it's powered from the same cable).
2. Run `BluetoothMaxPgnClient.exe` (Windows, standalone, no install needed).
3. Wait for the line `ready on COMx -- please place the second white queen on c4 now`.
4. On the board, set up the standard starting position with one extra white queen on c4 ("queen gesture") -- the corner squares (a1/a8/h1/h8) light up to confirm it was recognized.
5. Every saved game is written to this same folder as `game1.pgn`, `game2.pgn`, ... (auto-numbered from whatever's already there). The tool then exits -- run it again for the next retrieval.

A `usb_pgn_client.log` file is written alongside the exe with details of what happened, for troubleshooting.

## Building it yourself

Requires Python 3 and [PyInstaller](https://pyinstaller.org/):

```
pip install pyserial pyinstaller
pyinstaller --onefile --console --name BluetoothMaxPgnClient usb_pgn_client.py
```
