#pragma once

// Driver for the Chessnut Air BLE protocol (reverse-engineered; there is no
// official BLE SDK, only a USB-HID one). Translates Chessnut's own binary
// board-status/LED wire format to and from the canonical Mode-B `'s'`
// status frame and SquareHighlight list used throughout this project.

#include "board_driver_v6.h"

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

// True whenever Phoenix's own last-decoded 'L' frame contained at least one
// real highlighted square (see dispatchLedFrameToBoard()) -- lets
// chessnut_board.cpp's own local "show whatever currently differs from the
// settled position" fallback defer to Phoenix's own suggestion whenever it
// has one, and only take over when Phoenix's frame is empty (observed to be
// the case for most squares/most of the time on real hardware, 2026-08-31).
extern bool phoenixHasActiveLedSuggestion;

// Re-applies the local "differs from settled position" highlight. Call this
// (instead of chessnutSetHighlightedSquares(nullptr, 0)) whenever Phoenix's
// own 'L' frame has nothing to show -- Phoenix re-sends an empty frame
// continuously (confirmed on real hardware), and blindly clearing the LEDs
// on every one of those undid the local highlight within about a second of
// it appearing, before the player could act on it.
void chessnutShowLocalBoardDeviations();

// True if any square currently differs from the last settled position --
// i.e. chessnutShowLocalBoardDeviations() would have something real to
// show right now. dispatchLedFrameToBoard() checks this BEFORE deferring to
// Phoenix's own suggestion: a real physical discrepancy (e.g. a captured
// piece just removed from the board) must always win over whatever stale
// suggestion Phoenix might still be blinking from an earlier, already-
// superseded move -- confirmed on real hardware 2026-08-31 that the
// opposite priority (Phoenix always wins) silently ate the "piece missing"
// indicator during exactly the moment it mattered most, a real capture.
bool chessnutHasLocalDeviation();

// Count of BLE board-data notifications lost because our internal queue was
// full when they arrived -- added 2026-08-31 to test whether "sometimes
// yes, sometimes no" status updates for a given square are being dropped
// here rather than a Chessnut sensor problem (ruled out by the user on real
// hardware: "das Board funktioniert mit allen 100%").
uint32_t chessnutDroppedBoardPackets();
