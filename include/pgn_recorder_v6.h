#pragma once

// Automatic PGN game recorder + on-demand WiFi pickup access point.
//
// Purely a passive observer of onBoardStatusFrame() -- never touches
// anything on the King-cable or BLE-masquerade forwarding paths, so it
// cannot affect any of that already-proven behavior. Two independent parts:
//
// 1. Game recording: maintains its own internal chess ruleset (legal move
//    generation, check/mate detection) to turn the raw board-position
//    stream into SAN moves, purely by diffing consecutive *settled* board
//    snapshots against the position implied by moves played so far -- no
//    external engine or GUI is assumed to exist. A game is saved to
//    LittleFS when checkmate is detected, or when the standard starting
//    position is recognized again (treated as "a new game begins now").
//    Only the most recent 10 saved games are kept (oldest deleted first).
//
// 2. Pickup access point: WiFi AP ("BTMAX") + captive-portal download page,
//    active only while the board shows the standard starting position with
//    one spare white queen additionally placed on c4 -- both the extra
//    piece and the "rest of the board must be untouched" requirement are
//    checked continuously, so the AP cannot be active during any real game
//    position by construction (no separate on/off command to forget).
//
// All work here happens off the time-critical path: pgnRecorderOnBoardStatus()
// only copies the latest frame; the actual move inference, SAN/PGN building,
// flash writes and WiFi/webserver servicing all happen in pgnRecorderPoll(),
// called from loop().

#include "board_driver_v6.h"

void pgnRecorderInit();
void pgnRecorderOnBoardStatus(const uint8_t frame[kModeBStatusFrameLength]);
void pgnRecorderPoll();

// Number of games currently saved on LittleFS (0 if LittleFS never mounted).
// Used by chessnut_server.cpp to answer a real client's "how many saved
// games?" query truthfully instead of the old hardcoded 0.
int pgnRecorderSavedGameCount();

// Path (usable directly with LittleFS.open()) of the `index`-th saved
// game's (0-based, oldest first) position-snapshot file, or false if index
// is out of range. Each snapshot file is a sequence of consecutive
// kModeBStatusFrameLength-byte Mode-B status frames, one per position after
// each recorded move -- captured alongside the PGN text at record time.
// Used by chessnut_server.cpp to stream real board frames for Chess PGN
// Master's file-download protocol (commands 0x33/0x34, framed by 0x37
// 0xbe/0xed) -- a real captured session (2026-09-01) showed this protocol
// has no per-file index parameter, i.e. one download call appears to
// retrieve every saved game in a single transfer rather than one call per
// game, so the caller is expected to loop index 0..pgnRecorderSavedGameCount()-1
// and stream them all back to back within one 0xbe/0xed window.
bool pgnRecorderGameSnapshotPathByIndex(int index, char* outPath, size_t outPathSize);

// Path of the `index`-th saved game's own raw .pgn text file (0-based,
// oldest first), or false if index is out of range. A real Chessnut GO's
// own download (captured and compared against this gateway's output
// 2026-09-01) turned out to be plain human-readable PGN text concatenated
// across games -- NOT a stream of board-frame positions for the client to
// reconstruct via toFen() as originally assumed from the HID-transport
// code path in EasyLinkSDK. chessnut_server.cpp streams this file's raw
// bytes directly (MTU-chunked) instead of the position-snapshot file.
bool pgnRecorderGamePgnPathByIndex(int index, char* outPath, size_t outPathSize);

// Deletes the `index`-th saved game's (0-based, oldest first) .pgn and
// companion .snap files from LittleFS. Returns true if the .pgn file was
// actually removed (the .snap removal is best-effort and doesn't affect the
// result -- an already-missing/backfilled .snap is not an error). Used by
// usb_pgn_dump.cpp to free a game's slot only after the USB client has
// confirmed it saved the transfer successfully -- deletion is opt-in/ack-
// gated, never automatic just because a dump happened, so a dump that never
// reaches a client (bad cable, client crashed mid-save) leaves the games
// intact on the device for the next attempt.
bool pgnRecorderDeleteGameByIndex(int index);

// Writes the `index`-th saved game's (0-based, oldest first) position
// history as a sequence of bare board-only FEN strings separated by ';'
// (starting position first, then one per recorded move) into outBuffer,
// null-terminated, and returns the number of bytes written (0 if index is
// out of range or outBuffer is too small for even one FEN). This is the
// exact format documented for Chessnut's own C SDK's
// cl_get_file_and_delete() ("a sequence of FEN strings, separated by ;") --
// confirmed 2026-09-01 against that documentation after two earlier wrong
// guesses (raw 38-byte board frames, then full PGN text) both produced no
// visible result in Chess PGN Master.
size_t pgnRecorderGameFenSequenceByIndex(int index, char* outBuffer, size_t outBufferSize);
