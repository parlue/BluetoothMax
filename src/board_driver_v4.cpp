#include "board_driver_v4.h"

namespace {

// King's own file/rank convention on the wire is reversed (h..a, not a..h)
// and rank-ascending -- confirmed against three independent real King
// captures (g1-f3, e2-e4, d2-d4). fileCorner/rankTop below are LED-grid
// corner indices (a 9x9 grid over the 8x8 board), not board squares
// directly; see the loop in decodeKingLedFrame().
uint8_t ledGrid[81] = {};

}  // namespace

void resetKingLedFrameBaseline() {
  // No-op: King's no-highlight/idle LED state is always all-zero (confirmed
  // across every fresh connection observed), so decodeKingLedFrame() always
  // diffs against zero rather than capturing a per-connection baseline from
  // whatever frame happens to arrive first. Capturing the first frame as
  // baseline silently discarded it whenever that first frame was itself
  // meaningful -- e.g. a New Game reset frame King re-sends immediately on
  // reconnect, which then never got displayed.
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

  // Covers both a 1-2 square move suggestion (source/destination markers)
  // and a many-square "these pieces need to go back" New Game/reset frame.
  //
  // Two highlighted squares that are grid-adjacent share one or more of
  // their 4 corners; a shared corner carries BOTH squares' marker bits at
  // once (0x0F | 0xF0 = 0xFF), so requiring all 4 corners to be byte-
  // identical (as an earlier version of this function did) fails for both
  // squares whenever they're adjacent -- confirmed against a real King
  // capture (active-values=6, corner pattern F0 FF 0F) that produced no
  // highlight at all under the strict check. Instead, a square counts as
  // marked with a given role if EVERY one of its 4 corners carries that
  // role's bit (0x0F for source, 0xF0 for destination) -- true even for a
  // shared corner, since 0xFF still contains both bits.
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
        // File order on the wire is reversed (h..a) -- confirmed for King
        // via three independent real captures, and now also confirmed for
        // Mephisto Phoenix via two real move captures (e2-e4 and c7-c5,
        // both mirrored without this correction). Same convention for both.
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
