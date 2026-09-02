# PGN USB Tool

A second, independent way to retrieve games recorded by BluetoothMax's [automatic PGN recording feature](../README.md) -- built because the BLE-based retrieval via Chess PGN Master turned out to be blocked on an undocumented step in Chessnut's own closed-source SDK. This tool and the firmware side define both ends of a simple protocol themselves, so there's nothing to reverse-engineer here.

## Important: before you start

Make sure **no cable-connected chess computer (Mephisto Phoenix, MILLENNIUM King, etc.) is attached to the gateway**. Unplug that cable entirely, then power the gateway from your PC via USB only -- a clean startup with no other physical connection to the gateway at the same time. This tool and the normal cable-connected gateway role are not meant to run together.

## Usage

1. With no chess computer cable connected (see above), plug the gateway into your PC via USB (it's powered from the same cable).
2. Run `BluetoothMaxPgnClient.exe` (Windows, standalone, no install needed).
3. Wait for the line `ready on COMx -- please place the second white queen on c4 now`.
4. On the board, set up the standard starting position with one extra white queen on c4 ("queen gesture") -- the corner squares (a1/a8/h1/h8) light up to confirm it was recognized.
5. Every saved game is written to this same folder as `game1.pgn`, `game2.pgn`, ... (auto-numbered from whatever's already there). Once every game has been confirmed saved, the gateway deletes those games from its own storage. The tool then exits -- run it again for the next retrieval.

A `usb_pgn_client.log` file is written alongside the exe with details of what happened, for troubleshooting.

## Building it yourself

Requires Python 3 and [PyInstaller](https://pyinstaller.org/):

```
pip install pyserial pyinstaller
pyinstaller --onefile --console --name BluetoothMaxPgnClient usb_pgn_client.py
```
