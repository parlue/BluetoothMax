#pragma once

// Frozen v3 snapshot -- see gateway_v3_multiboard.cpp.

#include "board_driver_v3.h"

extern const char kMillenniumBoardName[];

bool millenniumConnect(const NimBLEAddress& address);
bool millenniumIsConnected();
void millenniumPoll();
void millenniumRequestBoardStatus();
void millenniumRelayLedFrame(const uint8_t* frame167, size_t length);
void millenniumRelayCommand(const uint8_t* frame, size_t length);
void millenniumClearLeds();
void millenniumSuppressNextLedAck();
