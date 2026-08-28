#pragma once

// Shared, board-agnostic types and Mode-B wire-format utilities used across
// main.cpp (King's cable side, always Mode-B/ChessLink) and every per-board
// BLE driver (millennium_board.*, chessnut_board.*, ...).
//
// Design note: rather than inventing a new canonical board-state struct,
// this project reuses the existing Mode-B 's' status frame (67 bytes: 's' +
// 64 piece codes + 2 hex checksum digits) as the canonical, board-agnostic
// representation -- every driver's job is to produce valid frames in this
// exact format from its own board's native data, and to hand them to
// onBoardStatusFrame() (implemented in main.cpp), which owns the single
// existing King-facing pipeline (caching, immediate + periodic resend,
// LED-clear timing) unchanged and unaware of which board produced the data.

#include <Arduino.h>
#include <NimBLEDevice.h>

constexpr size_t kFrameBufferSize = 256;
constexpr size_t kModeBStatusFrameLength = 67;

enum class BoardType : uint8_t { Unknown, Millennium, Chessnut };

// ---------------------------------------------------------------------------
// Mode-B wire-format utilities (odd parity, block checksum, frame framing).
// King always speaks this dialect on the cable. The real Millennium board
// (and anything masquerading as one, e.g. CynusLink) speaks it over BLE too,
// so millennium_board.cpp reuses these same functions. Chessnut does NOT --
// it has its own binary protocol with no odd parity or Mode-B framing at
// all, so chessnut_board.cpp never touches these.
// ---------------------------------------------------------------------------

inline uint8_t encodeOddParity(uint8_t ascii) {
  ascii &= 0x7f;
  uint8_t ones = 0;
  for (uint8_t value = ascii; value != 0; value >>= 1) ones += value & 1;
  return (ones % 2 == 0) ? static_cast<uint8_t>(ascii | 0x80) : ascii;
}

// Length of a full command frame (host -> board), keyed by its first byte.
inline size_t modeBCommandLength(uint8_t first) {
  switch (first & 0x7f) {
    case 'S': case 'X': case 'T': case 'V': return 3;
    case 'R': return 5;
    case 'W': return 7;
    case 'L': return 167;
    default: return 0;
  }
}

// Length of a full reply frame (board -> host), keyed by its first byte.
inline size_t modeBReplyLength(uint8_t first) {
  switch (first & 0x7f) {
    case 's': return kModeBStatusFrameLength;
    case 'x': case 'l': return 3;
    // 'r' is 'r' + 2 hex addr + 2 hex value + 2 hex checksum = 7 bytes, not
    // 5 -- confirmed against CynusLink's own real, King-proven source
    // (`sendCL("r" + hx(a) + hx(ee[a]))`, checksum appended automatically)
    // and cross-checked against our own sniffer test project's identical
    // reply format.
    case 'r': return 7;
    case 'v': case 'w': return 7;
    default: return 0;
  }
}

inline bool modeBValidBlock(const uint8_t* frame, size_t length) {
  if (length < 3) return false;
  auto hexNibble = [](uint8_t c) -> int {
    c &= 0x7f;
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  const int high = hexNibble(frame[length - 2]);
  const int low = hexNibble(frame[length - 1]);
  if (high < 0 || low < 0) return false;
  uint8_t check = 0;
  for (size_t i = 0; i < length - 2; ++i) check ^= frame[i] & 0x7f;
  return check == static_cast<uint8_t>((high << 4) | low);
}

// ---------------------------------------------------------------------------
// Canonical square highlighting, decoded from King's own 'L' frame so a
// non-Mode-B board (Chessnut) can re-encode it into its own native LED
// command. squareIndex is 0..63, a1=0 .. h8=63 (see boardSquareIndex()).
// role distinguishes source vs destination when King's frame marks them
// differently; drivers that can't represent a role (Chessnut only has
// on/off per square) are free to ignore it.
// ---------------------------------------------------------------------------

enum class SquareHighlightRole : uint8_t { Generic, Source, Destination };

struct SquareHighlight {
  uint8_t squareIndex;
  SquareHighlightRole role;
};

constexpr int boardSquareIndex(char file, int rank) {
  return (rank - 1) * 8 + (file - 'a');
}

// Byte position of a square within a Mode-B `'s'` status frame's 64-char
// payload. This is NOT the same as boardSquareIndex(): King's own status
// wire format uses reversed (h..a) file order per rank, confirmed against a
// real board's raw output. file0 is 0-based, a=0..h=7.
constexpr int modeBStatusWireIndex(int file0, int rank) {
  return (rank - 1) * 8 + (7 - file0);
}

// Decodes King's 167-byte 'L' frame (already parity-stripped, checksum
// already validated by the caller) into up to maxOut highlighted squares,
// writing them to out[] and returning how many were found (capped at
// maxOut). Covers both a 1-2 square move suggestion (source/destination
// markers) and a many-square New Game/reset frame ("these pieces need to go
// back"), which highlights every square that differs from the starting
// position at once.
//
// This is the exact per-square decode algorithm proven three times against
// real King captures (g1-f3, e2-e4, d2-d4) during the earlier sniffer/
// King-simulator investigation: source squares are marked with corner value
// 0x0F, destination squares with 0xF0, relative to the very first (generic,
// no-highlight) baseline frame this connection has seen.
size_t decodeKingLedFrame(const uint8_t frame167[167], SquareHighlight* out, size_t maxOut);

// Resets the decoder's per-connection baseline (call on every fresh BLE
// board connection, since King's generic "New Game" frame content is what
// establishes the no-highlight reference point).
void resetKingLedFrameBaseline();

// ---------------------------------------------------------------------------
// Shared callback: every board driver calls this whenever it has a fresh,
// valid Mode-B 's' status frame (kModeBStatusFrameLength bytes, checksum
// already correct) to hand off to the single existing King-facing pipeline.
// Implemented in main.cpp.
// ---------------------------------------------------------------------------
void onBoardStatusFrame(const uint8_t frame[kModeBStatusFrameLength]);

// Writes a logical (unencoded, 7-bit ASCII) frame to King's cable, applying
// odd-parity encoding. Implemented in main.cpp; used by driver .cpp files
// that need to reply to King directly (currently only millennium_board.cpp,
// for its instant local 'l' ack and cached-status-on-probe behavior).
size_t writeFrameToKing(const uint8_t* logicalFrame, size_t length);

// True once any board driver has ever handed a status frame to
// onBoardStatusFrame() during the current connection. Implemented in
// main.cpp; drivers use this to know whether they still need to actively
// request an initial status.
bool haveAnyBoardStatus();

// The current auto-report interval (ms) King's status gets resent at if
// unchanged; defaults to a generic fallback and may be refined by a driver
// that can read its board's own real scan-rate (Millennium's register 1).
// Defined in main.cpp.
extern uint32_t autonomousStatusIntervalMs;
