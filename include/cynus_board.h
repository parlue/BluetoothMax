#pragma once

// Driver for the Cynus robot's own BLE protocol (service 0xFFF0 / char
// 0xFFF1, line-based text commands). This is a port of the user's own
// proven CynusLink project (C:\Users\DirkSommerfeld\Documents\CynusLink_Test),
// which bridges the exact same Cynus robot to a real chess computer over its
// OWN BLE "MILLENNIUM CHESS" server role and is confirmed to hold full games
// with King. Here the chess computer (King/Phoenix) is instead on our
// project's cable side, so only CynusLink's chess/game logic is ported --
// its own BLE-server transport (sendCL) is replaced by this project's
// existing cable pipeline (onBoardStatusFrame()/writeFrameToKing() in
// main.cpp), which already handles acks, checksums and framing generically
// for every board driver.

#include "board_driver.h"

extern const char kCynusBoardName[];  // matched as a "CYNUS-" prefix

bool cynusConnect(const NimBLEAddress& address);
bool cynusIsConnected();
void cynusPoll();

// Call on every King/Phoenix 'L' frame with the raw, already parity-stripped
// and checksum-validated 167-byte Mode-B frame ('L' + 2 hex slot digits + 81
// LED bytes as hex pairs + 2 hex checksum digits). Parses the LED corner
// grid itself and, once a candidate move has been stable for
// kLedMoveStableMs (ported from CynusLink's LED_MOVE_STABLE_MS), commits it
// by sending "move <uci>" to Cynus's robot arm.
void cynusHandleLedFrame(const uint8_t frame167[167]);

// Cynus has no LEDs of its own to clear; present only so main.cpp's
// clearActiveBoardLeds() dispatch stays uniform across board types.
void cynusClearLeds();
