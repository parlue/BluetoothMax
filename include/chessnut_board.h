#pragma once

// Driver for the Chessnut Air BLE protocol (reverse-engineered; there is no
// official BLE SDK, only a USB-HID one). Translates Chessnut's own binary
// board-status/LED wire format to and from the canonical Mode-B `'s'`
// status frame and SquareHighlight list used throughout this project.

#include "board_driver.h"

// Matched as a case-insensitive substring against the BLE advertised name --
// covers "Chessnut Air", "Chessnut GO", MAC-suffixed names, etc.
extern const char kChessnutBoardName[];
extern const char kChessnutBoardNameAlt[];  // some units advertise as "Smart Chess"

bool chessnutConnect(const NimBLEAddress& address);
bool chessnutIsConnected();
void chessnutPoll();

// Lights exactly the given squares and turns off all others. Pass count=0
// (squares may be nullptr) to turn all LEDs off.
void chessnutSetHighlightedSquares(const SquareHighlight* squares, size_t count);
