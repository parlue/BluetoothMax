#include "board_driver.h"

#include <cstring>

#include "chessnut_board.h"
#include "cynus_board.h"
#include "ichessone_board.h"
#include "millennium_board.h"

namespace {

// King's own file/rank convention on the wire is reversed (h..a, not a..h)
// and rank-ascending -- confirmed against three independent real King
// captures (g1-f3, e2-e4, d2-d4). fileCorner/rankTop below are LED-grid
// corner indices (a 9x9 grid over the 8x8 board), not board squares
// directly; see the loop in decodeKingLedFrame().
uint8_t ledGrid[81] = {};

// Standard starting position in Mode-B wire order (h..a per rank), used only
// to filter Chessnut LED highlights in dispatchLedFrameToBoard() below.
constexpr char kStartPositionWire[] =
    "RNBKQBNRPPPPPPPP................................pppppppprnbkqbnr";

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

void encodeLedFrame(const uint8_t* squareIndices, size_t count, uint8_t frame167[167],
                     bool useEncodedChecksum) {
  uint8_t grid[81] = {};
  for (size_t i = 0; i < count; ++i) {
    const int squareIndex = squareIndices[i];
    const int file0 = squareIndex % 8;
    const int rank = squareIndex / 8 + 1;
    // Inverse of the changedFile/changedRank mapping in decodeKingLedFrame()
    // above.
    const int file = 7 - file0;
    const int rankTop = rank - 1;
    grid[file * 9 + rankTop] = 0xFF;
    grid[(file + 1) * 9 + rankTop] = 0xFF;
    grid[file * 9 + rankTop + 1] = 0xFF;
    grid[(file + 1) * 9 + rankTop + 1] = 0xFF;
  }

  static constexpr char hex[] = "0123456789ABCDEF";
  frame167[0] = 'L';
  frame167[1] = '0';
  frame167[2] = '1';
  for (int i = 0; i < 81; ++i) {
    frame167[3 + i * 2] = static_cast<uint8_t>(hex[grid[i] >> 4]);
    frame167[4 + i * 2] = static_cast<uint8_t>(hex[grid[i] & 0x0F]);
  }
  computeModeBChecksumHex(frame167 + 165, frame167, 165, useEncodedChecksum);
}

void dispatchLedFrameToBoard(BoardType type, const uint8_t frame167[167]) {
  if (type == BoardType::Millennium) {
    millenniumSuppressNextLedAck();
    // millenniumRequestBoardStatus() used to fire here on every single 'L'
    // frame -- removed 2026-08-31. It writes directly (bypassing the queue
    // transmitQueuedFrame() uses) and stamps the same lastBleFrameSentMs
    // throttle timestamp that gates the LED-frame queue -- so with Phoenix
    // sending 'L' frequently (it blinks its suggestion), this kept
    // resetting the throttle before the queued LED frame ever got a turn,
    // permanently starving it: T2's own physical LEDs never lit at all,
    // confirmed on real hardware (no "L frame relayed" log line ever
    // appeared despite dozens of L frames arriving). Not needed anyway --
    // a real board already streams status continuously on its own
    // (register 2 auto-report mode, confirmed non-zero on this unit).
    millenniumRelayLedFrame(frame167, 167);
  } else if (type == BoardType::Chessnut) {
    // Sized for a full New Game/reset frame (every square that differs from
    // the starting position at once), not just a single move's
    // source/destination pair.
    SquareHighlight squares[32];
    size_t count = decodeKingLedFrame(frame167, squares, 32);

    // Two generic (reset/error) squares sharing the same corner value can
    // make a square sandwiched between them appear lit too, purely because
    // it shares corners with both -- the raw corner data alone can't tell a
    // real generic highlight apart from this geometric side effect. Only a
    // New Game/reset frame (many squares highlighted at once) can actually
    // produce this; a single generic-marked square is a normal, real move
    // suggestion (e.g. Mephisto Phoenix marks moves generically rather than
    // with King's distinct source/destination bits) and must never be
    // filtered -- comparing it against the standard starting position
    // wrongly ate every such suggestion for any piece that hadn't moved
    // yet.
    //
    // Threshold raised 3 -> 8 (2026-08-31): a real BearChess capture proved
    // the same bug recurs at count==3, not just count==1. BearChess marks a
    // 2-square-apart-on-one-file move suggestion (e.g. e7-e5) as two
    // uniform-cornered squares (e5, e7); those share corners through the
    // square between them (e6), which the decoder also reports as a third,
    // "sandwiched" Generic square purely from the geometry. At that point in
    // a game e5/e6/e7 all still match the starting position, so the old
    // count>=3 check eliminated all three -- the ghost AND both real
    // suggestion squares -- reproducing "decoded 0 squares" for a genuine
    // 2-square move hint. Phoenix's own actual reset/New-Game frame lights
    // both entire home ranks at once (16+ real squares, per this project's
    // own earlier real-hardware analysis) -- well clear of 8, so this still
    // catches that case exactly as before; it just no longer misfires on a
    // small, real 2-3-square cluster like BearChess's.
    // Second, independent (not count-gated) ghost check, added 2026-08-31
    // alongside the count>=8 threshold change above: a square directly
    // between two OTHER decoded squares on the same file or rank (distance 1
    // each way) is geometrically sandwiched exactly like e6 between e5 and
    // e7 above -- but unlike Phoenix's own reset splash (one single value
    // painted across the whole grid, so a real square and a sandwich ghost
    // both end up with uniform corners and are indistinguishable that way),
    // a real independently-marked square always has all 4 of its own
    // corners equal to each other, while a pure corner-sharing ghost's
    // corners are a literal mix of its two neighbors' distinct values (here:
    // two corners 0x33 from e5, two corners 0xCC from e7) -- confirmed
    // directly against the real captured e5/e6/e7 frame. Requiring BOTH
    // "sandwiched" AND "corners not uniform" avoids misreading a genuinely
    // separate, independently-marked square that merely happens to sit
    // between two others (e.g. f1 for a real O-O rook destination alongside
    // an e1/g1 king move) -- such a square is marked with its own uniform
    // value, not a mix, so this check leaves it alone.
    auto cornersUniform = [](int file0, int rank) {
      const int file = 7 - file0;
      const int rankTop = rank - 1;
      const uint8_t c00 = ledGrid[file * 9 + rankTop];
      const uint8_t c10 = ledGrid[(file + 1) * 9 + rankTop];
      const uint8_t c01 = ledGrid[file * 9 + rankTop + 1];
      const uint8_t c11 = ledGrid[(file + 1) * 9 + rankTop + 1];
      return c00 == c10 && c10 == c01 && c01 == c11;
    };
    auto hasSquare = [&](int file0, int rank) {
      for (size_t j = 0; j < count; ++j) {
        if (squares[j].squareIndex % 8 == file0 && squares[j].squareIndex / 8 + 1 == rank) return true;
      }
      return false;
    };

    const uint8_t* cached = cachedBoardStatusBytes();
    size_t kept = 0;
    for (size_t i = 0; i < count; ++i) {
      bool eliminate = false;
      if (count >= 8 && squares[i].role == SquareHighlightRole::Generic && cached != nullptr) {
        const int file0 = squares[i].squareIndex % 8;
        const int rank = squares[i].squareIndex / 8 + 1;
        const int wireIndex = modeBStatusWireIndex(file0, rank);
        eliminate = cached[1 + wireIndex] == kStartPositionWire[wireIndex];
      }
      if (!eliminate && squares[i].role == SquareHighlightRole::Generic) {
        const int file0 = squares[i].squareIndex % 8;
        const int rank = squares[i].squareIndex / 8 + 1;
        const bool sandwichedOnFile = hasSquare(file0, rank - 1) && hasSquare(file0, rank + 1);
        const bool sandwichedOnRank = hasSquare(file0 - 1, rank) && hasSquare(file0 + 1, rank);
        if ((sandwichedOnFile || sandwichedOnRank) && !cornersUniform(file0, rank)) eliminate = true;
      }
      if (!eliminate) squares[kept++] = squares[i];
    }
    count = kept;

    // Log the decoded square set only on an actual change -- this dispatch
    // fires on every incoming 'L' frame, which Phoenix sends continuously
    // while blinking a suggestion (~every 43ms over cable) -- unconditional
    // per-call logging here would flood the log during real cable play.
    // Added 2026-08-31 to diagnose a BearChess-over-BLE report ("no LEDs
    // after Black's/the engine's move") -- decodeKingLedFrame()'s corner-bit
    // convention (0x0F=source, 0xF0=destination, both=generic) was reverse-
    // engineered specifically from King/Mephisto Phoenix hardware; this makes
    // it visible whether BearChess's own 'L' frames decode to zero squares
    // under that same convention (which would explain the symptom directly)
    // or to the wrong/right squares.
    {
      // Dedup key is the RAW incoming frame, not the decoded result -- two
      // genuinely different frames can legitimately decode to the same
      // result (e.g. two different "no real suggestion" frames both give
      // count==0), which a result-keyed dedup would wrongly treat as "no new
      // frame" and stay silent for the second one. Phoenix's own repeated
      // blink frames are byte-identical resends, so a raw-content dedup
      // still suppresses those exactly as before.
      static uint8_t lastLoggedFrame[167];
      static bool everLogged = false;
      const bool changed = !everLogged || memcmp(lastLoggedFrame, frame167, 167) != 0;
      if (changed) {
        everLogged = true;
        memcpy(lastLoggedFrame, frame167, 167);
        Serial.printf("[CHESSNUT LED] decoded %u square(s) from L frame:", static_cast<unsigned>(count));
        for (size_t i = 0; i < count; ++i) {
          const int file0 = squares[i].squareIndex % 8;
          const int rank = squares[i].squareIndex / 8 + 1;
          const char* roleName = squares[i].role == SquareHighlightRole::Source      ? "src"
                                  : squares[i].role == SquareHighlightRole::Destination ? "dst"
                                                                                        : "gen";
          Serial.printf(" %c%d(%s)", static_cast<char>('a' + file0), rank, roleName);
        }
        Serial.println();
        if (count == 0) {
          // Raw grid dump for the zero-square case only (keeps normal-
          // suggestion logging above uncluttered) -- lets us see BearChess's
          // actual corner-byte content directly instead of just knowing our
          // King/Phoenix-specific decoder found nothing in it, to tell apart
          // "BearChess uses a different marking convention we don't decode
          // yet" from "this frame is genuinely a real no-suggestion/idle
          // frame". frame167[3..164] is the 81-corner grid, 2 hex chars each.
          Serial.print("[CHESSNUT LED] raw grid: ");
          Serial.write(frame167 + 3, 162);
          Serial.println();
        }
      }
    }

    // count==0 (no real suggestion, or an ambiguous/undecodable frame, e.g.
    // two adjacent suggested squares sharing corners) does NOT mean "clear
    // the LEDs" here -- Phoenix re-sends an empty 'L' frame continuously
    // (confirmed on real hardware), so unconditionally clearing on every
    // one of those used to erase chessnut_board.cpp's own local "square is
    // missing its piece" highlight within about a second of it appearing,
    // long before the player could act on it. Defer to that local display
    // instead.
    phoenixHasActiveLedSuggestion = count > 0;

    // A real physical discrepancy always wins over Phoenix's own suggestion,
    // even a currently-active one -- confirmed on real hardware 2026-08-31
    // that the reverse priority let a stale Phoenix suggestion (still
    // blinking from before a capture started) silently swallow the "piece
    // is missing" indicator during exactly the moment it mattered most: the
    // player lifts the opponent's piece to capture, and nothing showed that
    // the square was now empty. Phoenix's own suggestion is moot anyway
    // until the board is physically corrected, so it can wait.
    if (chessnutHasLocalDeviation()) {
      chessnutShowLocalBoardDeviations();
    } else if (count > 0) {
      // Mephisto Phoenix reveals one move by alternating between two
      // single-square frames over time (source, then destination, then
      // back) rather than marking both at once -- relaying each frame as
      // it arrives made Chessnut's LEDs visibly blink/alternate along with
      // Phoenix's own cycle. The user wants a steady display instead (both
      // squares lit together, no blinking), so accumulate recently-seen
      // suggested squares here and show the union. A 3rd, unrelated square
      // appearing (accumulator already has 2 *different* squares) means
      // Phoenix moved on to suggesting something else -- start over with
      // just the new one instead of accumulating unboundedly.
      static SquareHighlight accumulated[2];
      static size_t accumulatedCount = 0;
      for (size_t i = 0; i < count; ++i) {
        bool alreadyKnown = false;
        for (size_t j = 0; j < accumulatedCount; ++j) {
          if (accumulated[j].squareIndex == squares[i].squareIndex) {
            alreadyKnown = true;
            break;
          }
        }
        if (alreadyKnown) continue;
        if (accumulatedCount < 2) {
          accumulated[accumulatedCount++] = squares[i];
        } else {
          // Both slots already hold a different square than this one --
          // this is a new suggestion cycle, discard the stale pair entirely
          // rather than mixing one old square with the new one.
          accumulated[0] = squares[i];
          accumulatedCount = 1;
        }
      }
      chessnutSetHighlightedSquares(accumulated, accumulatedCount);
    } else {
      chessnutShowLocalBoardDeviations();
    }
  } else if (type == BoardType::Cynus) {
    // Cynus's own driver decodes the raw frame itself (engineSide-aware,
    // with a stability wait) rather than the shared decoder above -- see
    // cynus_board.cpp for why.
    cynusHandleLedFrame(frame167);
  } else if (type == BoardType::IChessOne) {
    // Simple relay via the shared decoder, same as Chessnut's own decode
    // step above -- but without Chessnut's ghost-square filter, which was
    // built specifically for Chessnut/Phoenix-observed geometry quirks with
    // no evidence (yet) that iChessOne needs the same treatment. Role
    // (source/destination) is dropped -- iChessOne only gets one steady
    // color from this project (see ichessoneSetHighlightedSquares()), so
    // there's nothing to distinguish.
    SquareHighlight squares[32];
    const size_t count = decodeKingLedFrame(frame167, squares, 32);
    ichessoneSetHighlightedSquares(count > 0 ? squares : nullptr, count);
  }
}

// Shows a 3-second confirmation signal after a king-gesture result
// (win/loss/draw, see pgn_recorder.cpp's detectKingGestureResult()) has been
// recognized -- user's own explicit request, 2026-09-03. LED boards
// (Millennium, Chessnut, iChessOne) light the same four center squares
// (d4/d5/e4/e5) main.cpp's own BT-BT "ready" signal already uses, with a
// single steady write -- no repeated/timed toggling, matching this
// project's own established "rapid repeated LED writes are fragile on real
// hardware" findings elsewhere. Cynus has no LEDs, so it shows the result
// as text instead. result must be one of detectKingGestureResult()'s own
// return values ("1-0", "0-1", "1/2-1/2"). Call
// clearKingGestureResultConfirmation() once the 3-second hold is over.
void showKingGestureResultConfirmation(BoardType type, const char* result) {
  const uint8_t centerSquares[4] = {
      static_cast<uint8_t>(boardSquareIndex('d', 4)), static_cast<uint8_t>(boardSquareIndex('d', 5)),
      static_cast<uint8_t>(boardSquareIndex('e', 4)), static_cast<uint8_t>(boardSquareIndex('e', 5)),
  };
  switch (type) {
    case BoardType::Millennium: {
      uint8_t frame167[167];
      encodeLedFrame(centerSquares, 4, frame167, /*useEncodedChecksum=*/false);
      millenniumRelayLedFrame(frame167, 167);
      break;
    }
    case BoardType::Chessnut: {
      SquareHighlight highlights[4];
      for (int i = 0; i < 4; ++i) highlights[i] = {centerSquares[i], SquareHighlightRole::Generic};
      chessnutSetHighlightedSquares(highlights, 4);
      break;
    }
    case BoardType::IChessOne: {
      SquareHighlight highlights[4];
      for (int i = 0; i < 4; ++i) highlights[i] = {centerSquares[i], SquareHighlightRole::Generic};
      ichessoneSetHighlightedSquares(highlights, 4);
      break;
    }
    case BoardType::Cynus: {
      // "1/2:1/2" (7 characters) confirmed by the user to fit the display,
      // same as "1:0"/"0:1".
      const char* text = (strcmp(result, "1-0") == 0)   ? "1:0"
                          : (strcmp(result, "0-1") == 0) ? "0:1"
                                                          : "1/2:1/2";
      cynusShowText(text);
      break;
    }
    default:
      break;
  }
}

void clearKingGestureResultConfirmation(BoardType type) {
  switch (type) {
    case BoardType::Millennium: millenniumClearLeds(); break;
    case BoardType::Chessnut: chessnutSetHighlightedSquares(nullptr, 0); break;
    case BoardType::IChessOne: ichessoneClearLeds(); break;
    case BoardType::Cynus: cynusShowText("play"); break;
    default: break;
  }
}

void clearBoardLeds(BoardType type) {
  switch (type) {
    case BoardType::Millennium: millenniumClearLeds(); break;
    case BoardType::Chessnut: chessnutSetHighlightedSquares(nullptr, 0); break;
    case BoardType::Cynus: cynusClearLeds(); break;
    case BoardType::IChessOne: ichessoneClearLeds(); break;
    default: break;
  }
}
