#include "board_driver_v3.h"

namespace {

uint8_t ledGrid[81] = {};

}  // namespace

void resetKingLedFrameBaseline() {
  // No-op: King's no-highlight/idle LED state is always all-zero.
}

size_t decodeKingLedFrame(const uint8_t frame167[167], SquareHighlight* out, size_t maxOut) {
  auto hexNibble = [](uint8_t c) -> int {
    c &= 0x7f;
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  for (int i = 0; i < 81; ++i) {
    const int hi = hexNibble(frame167[3 + i * 2]);
    const int lo = hexNibble(frame167[4 + i * 2]);
    if (hi < 0 || lo < 0) return 0;
    ledGrid[i] = static_cast<uint8_t>((hi << 4) | lo);
  }

  size_t count = 0;
  for (int file = 0; file < 8; ++file) {
    for (int rankTop = 0; rankTop < 8; ++rankTop) {
      const uint8_t c00 = ledGrid[file * 9 + rankTop];
      const uint8_t c10 = ledGrid[(file + 1) * 9 + rankTop];
      const uint8_t c01 = ledGrid[file * 9 + rankTop + 1];
      const uint8_t c11 = ledGrid[(file + 1) * 9 + rankTop + 1];
      const bool allLit = c00 != 0 && c10 != 0 && c01 != 0 && c11 != 0;
      if (!allLit) continue;
      const bool allHaveSourceBit =
          (c00 & 0x0F) && (c10 & 0x0F) && (c01 & 0x0F) && (c11 & 0x0F);
      const bool allHaveDestBit =
          (c00 & 0xF0) && (c10 & 0xF0) && (c01 & 0xF0) && (c11 & 0xF0);
      if (!allHaveSourceBit && !allHaveDestBit) continue;

      if (count < maxOut) {
        const int changedFile = 7 - file;  // 0-based a..h
        const int changedRank = rankTop + 1;
        out[count].squareIndex = static_cast<uint8_t>(
            boardSquareIndex(static_cast<char>('a' + changedFile), changedRank));
        if (allHaveSourceBit && !allHaveDestBit) {
          out[count].role = SquareHighlightRole::Source;
        } else if (allHaveDestBit && !allHaveSourceBit) {
          out[count].role = SquareHighlightRole::Destination;
        } else {
          out[count].role = SquareHighlightRole::Generic;
        }
      }
      ++count;
    }
  }
  return count < maxOut ? count : maxOut;
}
