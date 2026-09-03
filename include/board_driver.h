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

enum class BoardType : uint8_t { Unknown, Millennium, Chessnut, Cynus, IChessOne };

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

// usedEncodedChecksum, if non-null, reports which checksum convention
// matched (false = plain 7-bit, true = odd-parity-encoded) -- callers on
// King's cable side use this to learn which convention the connected host
// itself expects, so replies can be built the same way. Left untouched (not
// written) if validation fails.
inline bool modeBValidBlock(const uint8_t* frame, size_t length,
                             bool* usedEncodedChecksum = nullptr) {
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
  const uint8_t received = static_cast<uint8_t>((high << 4) | low);

  uint8_t checkPlain = 0;
  for (size_t i = 0; i < length - 2; ++i) checkPlain ^= frame[i] & 0x7f;
  if (received == checkPlain) {
    if (usedEncodedChecksum != nullptr) *usedEncodedChecksum = false;
    return true;
  }

  // Some Mode-B hosts (confirmed: Mephisto Phoenix) compute the checksum
  // over the odd-parity-encoded wire bytes instead of the plain 7-bit
  // values -- accept that convention too rather than rejecting the frame
  // outright. Purely additive: anything that already validated above still
  // does, unchanged.
  uint8_t checkEncoded = 0;
  for (size_t i = 0; i < length - 2; ++i) checkEncoded ^= encodeOddParity(frame[i]);
  if (received == checkEncoded) {
    if (usedEncodedChecksum != nullptr) *usedEncodedChecksum = true;
    return true;
  }
  return false;
}

// Computes a Mode-B block checksum over data[0..length) and writes it as 2
// uppercase hex digits to out[0..1]. useEncodedConvention selects which of
// the two conventions modeBValidBlock() recognizes (see above) to match --
// pass cableHostUsesEncodedChecksum so replies we generate ourselves match
// whichever convention the connected cable host was observed to use.
inline void computeModeBChecksumHex(uint8_t out[2], const uint8_t* data, size_t length,
                                     bool useEncodedConvention) {
  uint8_t check = 0;
  for (size_t i = 0; i < length; ++i) {
    check ^= useEncodedConvention ? encodeOddParity(data[i]) : (data[i] & 0x7f);
  }
  static constexpr char hex[] = "0123456789ABCDEF";
  out[0] = static_cast<uint8_t>(hex[check >> 4]);
  out[1] = static_cast<uint8_t>(hex[check & 0x0f]);
}

// True once an incoming cable frame has been observed to validate only under
// the odd-parity-encoded checksum convention (see modeBValidBlock()) --
// i.e. the connected host is not King/Millennium-compatible in this one
// respect. Sticky for the session once set. Defined in main.cpp; drivers
// that build their own reply checksums (currently chessnut_board.cpp's
// status frame) read this so their output matches what the connected host
// expects, without touching the proven default (plain) convention.
extern bool cableHostUsesEncodedChecksum;

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

// Inverse of decodeKingLedFrame(): builds a valid Mode-B 167-byte 'L' frame
// (slot '01' + 81 LED corner bytes + 2-hex checksum, ready to send as-is)
// that marks each of the given squares generically (all 4 corners 0xFF --
// the same convention Mephisto Phoenix uses, and one decodeKingLedFrame()
// already understands as SquareHighlightRole::Generic). Used to synthesize
// our own LED signals (e.g. the BT-BT mode "ready"/"confirmed" blink
// patterns) rather than relaying a signal that came from a real host.
// squareIndices use boardSquareIndex()'s convention (a1=0, rank-ascending).
// useEncodedChecksum should match cableHostUsesEncodedChecksum for a cable
// recipient, or be false for a BLE recipient (no cable quirk applies there).
void encodeLedFrame(const uint8_t* squareIndices, size_t count, uint8_t frame167[167],
                     bool useEncodedChecksum);

// ---------------------------------------------------------------------------
// Shared callback: every board driver calls this whenever it has a fresh,
// valid Mode-B 's' status frame (kModeBStatusFrameLength bytes, checksum
// already correct) to hand off to the single existing King-facing pipeline.
// Implemented in main.cpp.
// ---------------------------------------------------------------------------
void onBoardStatusFrame(const uint8_t frame[kModeBStatusFrameLength]);

// (Re)starts a short, one-shot raw-byte capture window on the cable's
// incoming side -- every byte Phoenix sends while armed gets buffered and
// dumped as one hex block (a single Serial.write(), not a per-byte
// printf loop) when the window closes. Call this whenever a board change
// is detected that we want full cable visibility around. Implemented in
// main.cpp.
void armVerboseCableLog();

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

// The most recently cached kModeBStatusFrameLength-byte status frame, or
// nullptr if haveAnyBoardStatus() is still false. Implemented in main.cpp;
// used by dispatchLedFrameToBoard()'s Chessnut ghost-square filter, which
// needs to know the board's current occupancy.
const uint8_t* cachedBoardStatusBytes();

// Decodes a raw, already parity-stripped and checksum-validated 167-byte
// 'L' frame and relays/dispatches it to whichever board type is currently
// connected, using each driver's own proven LED-suggestion handling
// (Millennium: relay the raw frame as-is; Chessnut: decode + ghost-square
// filter + highlight; Cynus: its own engineSide-aware decoder). Shared
// between the cable-facing pipeline (main.cpp, fed by King/Phoenix) and the
// BT-BT ChessLink masquerade server (chesslink_server.cpp, fed by external
// ChessLink software) so both hosts get identical behavior. Implemented in
// board_driver.cpp.
void dispatchLedFrameToBoard(BoardType type, const uint8_t frame167[167]);

// Physically clears whichever board type is connected (Millennium: sends
// 'X58' to the real board; Chessnut: clears its highlight; Cynus: no-op, no
// LEDs of its own). Shared between main.cpp's own use (King's 'X'/initial-
// status handling) and the BT-BT ChessLink masquerade server's 'X' handler.
void clearBoardLeds(BoardType type);

// The board type the gateway is currently connected to over BLE
// (Unknown if not connected to anything yet). Implemented in main.cpp;
// used by chesslink_server.cpp to know which driver to dispatch to.
BoardType currentBoardType();

// The current auto-report interval (ms) King's status gets resent at if
// unchanged; defaults to a generic fallback and may be refined by a driver
// that can read its board's own real scan-rate (Millennium's register 1).
// Defined in main.cpp.
extern uint32_t autonomousStatusIntervalMs;
