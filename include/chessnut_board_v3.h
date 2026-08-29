#pragma once

// Frozen v3 snapshot -- see gateway_v3_multiboard.cpp.

#include "board_driver_v3.h"

extern const char kChessnutBoardName[];
extern const char kChessnutBoardNameAlt[];

bool chessnutConnect(const NimBLEAddress& address);
bool chessnutIsConnected();
void chessnutPoll();
void chessnutSetHighlightedSquares(const SquareHighlight* squares, size_t count);
