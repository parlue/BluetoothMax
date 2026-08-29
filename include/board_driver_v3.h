#pragma once

// Frozen v3 snapshot of board_driver.h -- see gateway_v3_multiboard.cpp for
// why this exists. Self-contained: does not include or reference any of the
// live (non-_v3) project files, so ongoing work on those never affects this
// build.

#include <Arduino.h>
#include <NimBLEDevice.h>

constexpr size_t kFrameBufferSize = 256;
constexpr size_t kModeBStatusFrameLength = 67;

enum class BoardType : uint8_t { Unknown, Millennium, Chessnut };

inline uint8_t encodeOddParity(uint8_t ascii) {
  ascii &= 0x7f;
  uint8_t ones = 0;
  for (uint8_t value = ascii; value != 0; value >>= 1) ones += value & 1;
  return (ones % 2 == 0) ? static_cast<uint8_t>(ascii | 0x80) : ascii;
}

inline size_t modeBCommandLength(uint8_t first) {
  switch (first & 0x7f) {
    case 'S': case 'X': case 'T': case 'V': return 3;
    case 'R': return 5;
    case 'W': return 7;
    case 'L': return 167;
    default: return 0;
  }
}

inline size_t modeBReplyLength(uint8_t first) {
  switch (first & 0x7f) {
    case 's': return kModeBStatusFrameLength;
    case 'x': case 'l': return 3;
    case 'r': return 7;
    case 'v': case 'w': return 7;
    default: return 0;
  }
}

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

  uint8_t checkEncoded = 0;
  for (size_t i = 0; i < length - 2; ++i) checkEncoded ^= encodeOddParity(frame[i]);
  if (received == checkEncoded) {
    if (usedEncodedChecksum != nullptr) *usedEncodedChecksum = true;
    return true;
  }
  return false;
}

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

extern bool cableHostUsesEncodedChecksum;

enum class SquareHighlightRole : uint8_t { Generic, Source, Destination };

struct SquareHighlight {
  uint8_t squareIndex;
  SquareHighlightRole role;
};

constexpr int boardSquareIndex(char file, int rank) {
  return (rank - 1) * 8 + (file - 'a');
}

constexpr int modeBStatusWireIndex(int file0, int rank) {
  return (rank - 1) * 8 + (7 - file0);
}

size_t decodeKingLedFrame(const uint8_t frame167[167], SquareHighlight* out, size_t maxOut);

void resetKingLedFrameBaseline();

void onBoardStatusFrame(const uint8_t frame[kModeBStatusFrameLength]);

size_t writeFrameToKing(const uint8_t* logicalFrame, size_t length);

bool haveAnyBoardStatus();

extern uint32_t autonomousStatusIntervalMs;
