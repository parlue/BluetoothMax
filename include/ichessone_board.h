#pragma once

// iChessOne BLE e-board driver -- added 2026-09-02 from the vendor's own
// "iChessOneAPI" PDF (v. 21.01.2025, Janusz Lisowski), following the exact
// same shape as chessnut_board.*/millennium_board.*: decode the board's own
// native protocol into the shared Mode-B 's' status frame and hand it to
// onBoardStatusFrame(); encode outgoing square highlights into the board's
// own native LED command. NOT YET TESTED AGAINST REAL HARDWARE -- built
// entirely from the documented protocol, no iChessOne unit available this
// session. Purely additive: no existing board driver, dispatch switch, or
// shared file's behavior for Millennium/Chessnut/Cynus is touched by this.
//
// Transport: BLE, Nordic UART Service (a very common "serial-over-BLE"
// pattern, unrelated to any Nordic chip in the board itself) -- device name
// "iChessOne", service 6E400001-B5A3-F393-E0A9-E50E24DCCA9E, write
// characteristic ...002, notify characteristic ...003.
//
// Protocol: plain ASCII text commands/replies, except the EL (LED) command
// and the position data, which are raw binary bytes (per the doc's own
// opening note). All replies start with '=' + an identifier letter. Commands
// used here:
//   CPIRQ  -- start position streaming, but only send an update when a
//             piece actually moves (recommended over CPMxxxx for battery
//             life; we have no reason to prefer polling since this project
//             is already fully async/event-driven).
//   EL<14 raw bytes> -- LED command (see ichessoneSetHighlightedSquares()).
// Reply consumed here:
//   =p<32 raw bytes> -- full board position, 2 squares per byte (nibble-
//                        packed), ordered a8,b8,...,h1. This is 34 bytes
//                        total including the "=p" prefix -- bigger than the
//                        doc's own default 20-byte notify payload, so the
//                        board splits it across two packets (20 + 14) that
//                        must be reassembled before decoding.

#include "board_driver.h"

// Matched as a case-insensitive substring against the BLE advertised name.
extern const char kIChessOneBoardName[];

bool ichessoneConnect(const NimBLEAddress& address);
bool ichessoneIsConnected();
void ichessonePoll();

// Highlights the given squares in the board's single supported color (a
// fixed, steady red -- per the user's own explicit choice, 2026-09-02: "wir
// brauchen nur eine Farbe... das Rot für immer wie bei den anderen board").
// squares/count follow the same convention as chessnutSetHighlightedSquares()
// -- pass nullptr/0 to clear. role is ignored (the board's own protocol can
// do per-square color/brightness/flash, but none of that is used here).
void ichessoneSetHighlightedSquares(const SquareHighlight* squares, size_t count);
void ichessoneClearLeds();
