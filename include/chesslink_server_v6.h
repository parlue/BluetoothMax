#pragma once

// BLE peripheral masquerading as a real ChessLink "MILLENNIUM CHESS" board,
// for the BT-BT operating mode (no cable-side host present -- see the
// startup detection in main.cpp). Ported from the user's own proven
// CynusLink project, which plays this exact role toward real ChessLink-
// compatible chess software (PicoChess, BearChess, Diablillo, ...) and is
// confirmed to handle each of their own quirks correctly.
//
// Unlike this project's cable-facing pipeline -- which, for a real
// Millennium board, just relays bytes through to/from the genuine peer --
// there is no real Mode-B peer on this side of the bridge: this module must
// answer every host command (S/V/X/T/L/R/W) itself, using its own small
// EEPROM-style register set, exactly as CynusLink's own handleCL() does.

#include "board_driver_v6.h"

// Creates the GATT server/service/characteristics (but does NOT advertise
// anything yet). Call once, early in setup() -- right after
// NimBLEDevice::init(), before the BLE client role ever starts scanning or
// connecting. Registering a new GATT server later, once the client role is
// already active, crashed with "assert failed: ble_svc_gap_init" on real
// hardware -- CynusLink itself avoids this the same way (creates its own
// ChessLink server immediately in setup(), gates only the advertising).
void chesslinkServerInit();

// Starts advertising as "MILLENNIUM CHESS". Call once BT-BT mode has been
// confirmed (see main.cpp's state machine) -- i.e. once a real e-board is
// connected, its starting position is confirmed, and the human has
// selected ChessLink mode via the second-white-queen signal. Requires
// chesslinkServerInit() to have already run.
void chesslinkServerStart();

bool chesslinkServerConnected();

// Drains queued incoming bytes and services any pending timed work
// (periodic status resend). Call every loop() tick once started.
void chesslinkServerPoll();

// Call whenever a fresh confirmed board status is available (the same data
// already cached for the cable path) so it can be forwarded to a connected
// ChessLink client. Safe to call even before chesslinkServerStart().
void chesslinkServerPublishStatus(const uint8_t frame[kModeBStatusFrameLength]);

// Sends a raw, already-built logical (plain 7-bit ASCII, unencoded -- no
// cable parity applies over BLE) Mode-B frame to the connected ChessLink
// client, checksummed and MTU-aware chunked. main.cpp's writeFrameToKing()
// routes here instead of the UART when the BT-BT ChessLink masquerade is
// the active host transport -- e.g. forwarding a genuine reply from the
// real underlying Millennium board (register read, its own 'l' ack, ...).
size_t chesslinkServerWriteFrame(const uint8_t* logicalFrame, size_t length);
