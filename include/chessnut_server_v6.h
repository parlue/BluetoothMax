#pragma once

// BLE peripheral masquerading as a real Chessnut board ("Chessnut GO"), for
// the BT-BT operating mode's second mode (queen on b4 -- see main.cpp's
// state machine). Sibling to chesslink_server.h -- same role, opposite
// protocol: Chessnut has no Mode-B framing at all, just its own small binary
// command set (real-time-mode/battery/version/date/LED) confirmed against
// gkalab/cer2nut (an open-source Certabo<->Chessnut bridge, read directly as
// ground truth for the exact GATT layout and command bytes) and cross-
// checked against this project's own already-proven client-side driver
// (chessnut_board.cpp), which decodes the identical board-status wire format
// from a real Chessnut GO.

#include "board_driver_v6.h"

// Creates the GATT server/service/characteristics (but does NOT advertise
// anything yet). Call once, early in setup() -- same timing requirement as
// chesslinkServerInit() (see its own comment for why: registering a new GATT
// server after the BLE client role is already active crashes the stack).
void chessnutServerInit();

// Starts advertising as a real Chessnut board. Call once BT-BT mode has
// confirmed Chessnut mode (second white queen on b4). Requires
// chessnutServerInit() to have already run.
void chessnutServerStart();

bool chessnutServerConnected();

// Drains queued incoming bytes and services any pending timed work. Call
// every loop() tick once started.
void chessnutServerPoll();

// Call whenever a fresh confirmed board status is available (the same
// kModeBStatusFrameLength-byte data already cached for the cable/ChessLink
// paths) so it can be re-encoded into Chessnut's own wire format and
// forwarded to a connected Chessnut client. Safe to call even before
// chessnutServerStart().
void chessnutServerPublishStatus(const uint8_t frame[kModeBStatusFrameLength]);
