#pragma once

// Driver for the Millennium/ChessLink BLE protocol -- used identically by
// the real Millennium Supreme board and by any ChessLink-masquerading
// peripheral (e.g. CynusLink), since they share the same BLE name, UUIDs
// and Mode-B wire format. This is today's proven, unchanged behavior,
// wrapped behind a small connect/poll/relay interface so main.cpp can
// dispatch to it alongside other board drivers.

#include "board_driver_v5.h"

extern const char kMillenniumBoardName[];

bool millenniumConnect(const NimBLEAddress& address);
bool millenniumIsConnected();
void millenniumPoll();

// Sends 'S53' to actively (re-)request the board's status. millenniumPoll()
// already does this automatically until the first status is ever cached;
// callers may also invoke it directly for an urgent refresh.
void millenniumRequestBoardStatus();

// Relays King's own 167-byte 'L' frame to the real board unchanged -- the
// real board understands Mode-B natively, so no translation is needed or
// wanted here (this is the proven, zero-risk path).
void millenniumRelayLedFrame(const uint8_t* frame167, size_t length);

// Relays any other valid Mode-B command from King (S/X/T/V/W/R) to the real
// board unchanged, exactly as today.
void millenniumRelayCommand(const uint8_t* frame, size_t length);

// Physically clears the real board's setup LEDs (Mode-B 'X58').
void millenniumClearLeds();

// Call after sending King an instant local 'l' ack for its 'L' command: the
// real board will eventually send its own genuine 'l' ack too, which must
// not be forwarded a second time.
void millenniumSuppressNextLedAck();
