#include "cynus_board.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

const char kCynusBoardName[] = "CYNUS-";

namespace {

constexpr char kServiceUuid[] = "0000fff0-0000-1000-8000-00805f9b34fb";
constexpr char kCharUuid[] = "0000fff1-0000-1000-8000-00805f9b34fb";
constexpr uint32_t kConnectTimeoutMs = 10000;

// Ported from CynusLink's STARTUP_CORRECTION_RESCAN_MS: if the scanned board
// isn't yet a valid start position, wait this long before automatically
// asking Cynus to scan again (a manual Clock-scan on the robot itself can
// also satisfy this in CynusLink; not reproduced here).
constexpr uint32_t kStartupCorrectionRescanMs = 5000;

// CynusLink sends "set internal engine off" twice as a write-reliability
// retry, but a single send is sufficient (confirmed by the user). Matches
// CynusLink's own 300ms gap before its first "scan board".
constexpr uint32_t kInitialScanDelayMs = 300;

// Millennium/Phoenix may keep re-sending its L frame while still
// calculating -- only commit a move once the SAME decoded candidate has
// stayed stable for this long. Ported as-is from CynusLink's own proven
// LED_MOVE_STABLE_MS (any different L frame, including LEDs-off, cancels
// the candidate).
constexpr uint32_t kLedMoveStableMs = 2500;

constexpr uint32_t kFreeAnalysisScanMs = 5000;

// board64 index convention matches the ported logic exactly (from the
// user's own proven CynusLink project): index = rankTop * 8 + file0, where
// rankTop counts down from rank 8 (rankTop=0) to rank 1 (rankTop=7), file0
// is 0-based a..h. This is plain top-to-bottom FEN reading order.
char board64[65] = "................................................................";

constexpr char kStartFen[] = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR";
constexpr char kFlippedStartFen[] = "RNBKQBNR/PPPPPPPP/8/8/8/8/pppppppp/rnbkqbnr";

// Option squares: the black king lifted off e8 and placed on a specific
// square toggles a setting -- the same "menu via black king" scheme
// CynusLink already proved works against real King/Phoenix units.
constexpr char kSoundOffFen[] = "rnbq1bnr/pppppppp/8/4k3/8/8/PPPPPPPP/RNBQKBNR";
constexpr char kSoundOnFen[] = "rnbq1bnr/pppppppp/4k3/8/8/8/PPPPPPPP/RNBQKBNR";
constexpr char kFlipOnFen[] = "rnbq1bnr/pppppppp/8/7k/8/8/PPPPPPPP/RNBQKBNR";
constexpr char kFlipOffFen[] = "rnbq1bnr/pppppppp/7k/8/8/8/PPPPPPPP/RNBQKBNR";
constexpr char kAnalysisOnFen[] = "rnbq1bnr/pppppppp/8/3k4/8/8/PPPPPPPP/RNBQKBNR";
constexpr char kAnalysisOffFen[] = "rnbq1bnr/pppppppp/3k4/8/8/8/PPPPPPPP/RNBQKBNR";
constexpr char kSetPositionOnFen[] = "rnbq1bnr/pppppppp/8/2k5/8/8/PPPPPPPP/RNBQKBNR";
constexpr char kSetPositionOffFen[] = "rnbq1bnr/pppppppp/2k5/8/8/8/PPPPPPPP/RNBQKBNR";
// The black king simply lifted (not yet placed on an option square) while
// selecting an option -- not an error, just "still choosing".
constexpr char kKingLiftFen[] = "rnbq1bnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR";

enum class SyncState { WaitingForStartPosition, Ready };
SyncState syncState = SyncState::WaitingForStartPosition;

// Mirrors CynusLink's MoveCycleState: gates when an 'L' frame from
// King/Phoenix is even allowed to arm a move candidate (only while it's
// genuinely the chess computer's turn to suggest something) and when a
// human-confirmed move may be forwarded.
enum class MoveCycle { WaitFirstMove, WaitHumanMove, WaitEngineMove, WaitRobotPosition };
MoveCycle moveCycle = MoveCycle::WaitHumanMove;

const char* moveCycleName(MoveCycle cycle) {
  switch (cycle) {
    case MoveCycle::WaitFirstMove: return "WAIT_FIRST_MOVE";
    case MoveCycle::WaitHumanMove: return "WAIT_HUMAN_MOVE";
    case MoveCycle::WaitEngineMove: return "WAIT_ENGINE_MOVE";
    case MoveCycle::WaitRobotPosition: return "WAIT_ROBOT_POSITION";
  }
  return "?";
}

void setMoveCycle(MoveCycle cycle) {
  if (moveCycle == cycle) return;
  moveCycle = cycle;
  Serial.printf("[CYNUS MOVE] -> %s\r\n", moveCycleName(moveCycle));
}

// Which side King/Phoenix is playing -- learned either from who moves first
// (a human move forwarded while still WaitFirstMove locks it to Black; a
// decoded engine LED suggestion while WaitFirstMove locks it to White) or,
// failing that, from the LED frame's own dominant-pattern disambiguation
// (see extractMoveFromLCommand()). Ported from CynusLink's EngineSide.
enum class EngineSide { Unknown, White, Black };
EngineSide engineSide = EngineSide::Unknown;
bool firstMoveOrientationLocked = false;

enum class ExperimentalMode { None, FreeAnalysis, SetPosition };
ExperimentalMode experimentalMode = ExperimentalMode::None;
bool setPositionManualScanExpected = false;
uint32_t nextFreeAnalysisScanAt = 0;

constexpr size_t kMaxNotifyChunk = 64;

struct RawPacket {
  uint8_t length;
  uint8_t data[kMaxNotifyChunk];
};

NimBLEClient* bleClient = nullptr;
NimBLERemoteCharacteristic* chr = nullptr;
QueueHandle_t rxQueue = nullptr;
std::string rxLine;

// Last move UCI actually sent to Cynus, so a King/Phoenix L-frame it keeps
// retransmitting unchanged isn't re-sent every time.
std::string lastSentMoveUci;

// 0 = no automatic startup rescan pending. Set by handleStartupBoard() when
// the scanned position isn't a valid start position yet; consumed by
// cynusPoll().
uint32_t nextStartupRescanAt = 0;

// The last startup-error diff text actually shown on Cynus's display (e.g.
// "+E4"), so an unchanged error pattern across repeated rescans isn't
// re-displayed/re-sounded every 5 seconds -- ported from CynusLink's own
// lastStartupErrorDisplay dedup.
std::string lastStartupErrorDisplay;

// Manual override: lifting BOTH kings off the board while stuck at startup
// is a physical signal the player wants the position they set up next
// accepted as-is, bypassing the "must match a valid start position" check --
// user's own design, 2026-09-03 (real scenario: Cynus reset itself mid-game
// against Phoenix, forcing a BLE reconnect; startup logic then correctly
// refused the actual mid-game position, blocking play entirely). Replaces an
// earlier "3+ scans within 5s" attempt that turned out to be untriggerable:
// live testing showed the physical Scan/Clock button doesn't produce an
// independent BLE event outside Set Position mode, so no repeated-scan
// signal was ever actually observable. Both-kings-removed can't happen by
// accident during ordinary setup fumbling, so it's a safe, deliberate
// trigger -- armed once, then the next scan with both kings present again
// is accepted as the position to resume from.
bool startupManualOverrideArmed = false;

bool hasNoKings(const char board[65]) {
  for (int i = 0; i < 64; ++i) {
    if (board[i] == 'K' || board[i] == 'k') return false;
  }
  return true;
}

// 0 = not pending; set right after connect, consumed by cynusPoll() to send
// the first "scan board" once kInitialScanDelayMs has passed.
uint32_t initialScanAt = 0;

// LED corner-grid decode state, ported as-is from CynusLink's led[81] +
// pendingLedMove/pendingLedFrame/pendingLedMoveSince.
uint8_t ledGrid[81] = {};
std::string pendingLedMove;
std::string pendingLedFrame;
uint32_t pendingLedMoveSince = 0;

// Defined below (after ledGrid); forward-declared here so the single-square
// source-reveal helpers, which sit earlier in the file, can use it.
uint8_t ledValue(int fileCorner, int rankCornerTop);

bool sendCynus(const std::string& line) {
  if (chr == nullptr) return false;
  Serial.printf("[CYNUS TX] %s", line.c_str());
  if (chr->canWriteNoResponse()) return chr->writeValue((const uint8_t*)line.data(), line.size(), false);
  if (chr->canWrite()) return chr->writeValue((const uint8_t*)line.data(), line.size(), true);
  return false;
}

// Ported from CynusLink's cynusDisplay(): shows short feedback text on
// Cynus's own display (move made, config change, scan error, ...).
void cynusDisplay(const std::string& text) {
  sendCynus("display txt " + text.substr(0, 7) + "\n");
}

// Parses a FEN's placement field (ignores anything after the first space)
// into board64 (rank8-first, a..h reading order). Returns false if the
// placement doesn't describe exactly 64 squares.
bool fenPlacementToBoard(const std::string& fen, char out[65]) {
  int n = 0;
  for (char c : fen) {
    if (c == ' ') break;
    if (c == '/') continue;
    if (c >= '1' && c <= '8') {
      for (int k = 0; k < c - '0'; ++k) {
        if (n >= 64) return false;
        out[n++] = '.';
      }
    } else if (strchr("KQRBNPkqrbnp", c) != nullptr) {
      if (n >= 64) return false;
      out[n++] = c;
    } else {
      return false;
    }
  }
  if (n != 64) return false;
  out[64] = 0;
  return true;
}

std::string squareName(int file0, int rankTop) {
  std::string s;
  s += static_cast<char>('a' + file0);
  s += static_cast<char>('8' - rankTop);
  return s;
}

// Ported as-is from CynusLink's moveDisplayText(): short text for Cynus's
// own 7-character display, shown for both the human's move and the
// engine's own move once decoded.
std::string moveDisplayText(const std::string& uci) {
  if (uci == "e1g1" || uci == "e8g8") return "0-0";
  if (uci == "e1c1" || uci == "e8c8") return "0-0-0";
  if (uci.size() >= 5) {
    std::string t = "Chg ";
    t += static_cast<char>(toupper(static_cast<unsigned char>(uci[4])));
    return t;
  }
  if (uci.size() >= 4) {
    std::string t;
    t += static_cast<char>(toupper(static_cast<unsigned char>(uci[0])));
    t += uci[1];
    t += '-';
    t += static_cast<char>(toupper(static_cast<unsigned char>(uci[2])));
    t += uci[3];
    return t;
  }
  return "";
}

// Full legal-move check for one piece, used both to disambiguate which
// single move connects two board64 snapshots (requiredSide left at Unknown)
// and, with requiredSide set, to filter LED-decode candidates to only the
// side King/Phoenix is actually playing. Ported as-is from CynusLink's
// plausibleBoardMove(), extended with the requiredSide filter it also has.
bool plausibleBoardMove(const char board[65], int source, int destination,
                        EngineSide requiredSide = EngineSide::Unknown) {
  if (source < 0 || source >= 64 || destination < 0 || destination >= 64 || source == destination) return false;
  const char piece = board[source];
  const char target = board[destination];
  const bool pieceIsWhite = piece >= 'A' && piece <= 'Z';
  const bool pieceIsBlack = piece >= 'a' && piece <= 'z';
  if (!pieceIsWhite && !pieceIsBlack) return false;
  if (requiredSide == EngineSide::White && !pieceIsWhite) return false;
  if (requiredSide == EngineSide::Black && !pieceIsBlack) return false;
  if (target != '.') {
    const bool targetIsWhite = target >= 'A' && target <= 'Z';
    const bool targetIsBlack = target >= 'a' && target <= 'z';
    if (pieceIsWhite == targetIsWhite && pieceIsBlack == targetIsBlack) return false;
  }
  const int sf = source % 8, sr = source / 8, df = destination % 8, dr = destination / 8;
  const int dx = df - sf, dy = dr - sr, ax = abs(dx), ay = abs(dy);
  auto clearPath = [&](int stepX, int stepY) {
    int x = sf + stepX, y = sr + stepY;
    while (x != df || y != dr) {
      if (x < 0 || x > 7 || y < 0 || y > 7 || board[y * 8 + x] != '.') return false;
      x += stepX;
      y += stepY;
    }
    return true;
  };
  const char p = static_cast<char>(tolower(static_cast<unsigned char>(piece)));
  if (p == 'n') return (ax == 1 && ay == 2) || (ax == 2 && ay == 1);
  if (p == 'k') return (ax <= 1 && ay <= 1) || (ay == 0 && ax == 2);
  if (p == 'b') return ax == ay && ax > 0 && clearPath(dx > 0 ? 1 : -1, dy > 0 ? 1 : -1);
  if (p == 'r') {
    if (dx != 0 && dy != 0) return false;
    return clearPath(dx == 0 ? 0 : (dx > 0 ? 1 : -1), dy == 0 ? 0 : (dy > 0 ? 1 : -1));
  }
  if (p == 'q') {
    if (ax == ay && ax > 0) return clearPath(dx > 0 ? 1 : -1, dy > 0 ? 1 : -1);
    if (dx == 0 || dy == 0) return clearPath(dx == 0 ? 0 : (dx > 0 ? 1 : -1), dy == 0 ? 0 : (dy > 0 ? 1 : -1));
    return false;
  }
  if (p == 'p') {
    const int dir = pieceIsWhite ? -1 : 1;  // rankTop decreases going up the board (toward rank 8)
    if (dx == 0 && target == '.') {
      if (dy == dir) return true;
      const int startRow = pieceIsWhite ? 6 : 1;
      if (sr == startRow && dy == 2 * dir && board[(sr + dir) * 8 + sf] == '.') return true;
      return false;
    }
    if (ax == 1 && dy == dir) return true;  // capture (incl. en passant, checked by the caller)
    return false;
  }
  return false;
}

// Finds the single move that explains newBoard given oldBoard was correct,
// including castling and en passant. Returns "" if zero or more than one
// candidate move fits. Side-agnostic (requiredSide left at Unknown) since a
// human can move either color's pieces depending on the game's orientation.
// ALL-CAPS file letter (a..h -> A..H), used only for the scan-error display
// text -- matches CynusLink's own startupSquare() convention exactly, kept
// visually distinct from the lowercase squareName() used for real UCI move
// strings.
std::string startupSquare(int board64Index) {
  std::string s;
  s += static_cast<char>('A' + board64Index % 8);
  s += static_cast<char>('8' - board64Index / 8);
  return s;
}

int mismatchCount(const char actual[65], const char expected[65]) {
  int n = 0;
  for (int i = 0; i < 64; ++i) {
    if (actual[i] != expected[i]) ++n;
  }
  return n;
}

// Ported from CynusLink's startupErrorDisplay(): compares the scanned board
// against whichever of the normal/flipped starting positions is the closer
// match, and names up to 2 of the offending squares (Cynus's own display is
// only 7 characters wide) -- e.g. "+E4" (unexpected/wrong piece there) or
// "-F8" (expected piece missing). Returns "" if there's no single closest
// match or the board is already correct (handleStartupBoard() only calls
// this once it's confirmed neither orientation matches exactly).
std::string startupErrorDisplay(const char scanned[65]) {
  char normal[65], flipped[65];
  if (!fenPlacementToBoard(kStartFen, normal) || !fenPlacementToBoard(kFlippedStartFen, flipped)) return "";
  const char* expected = mismatchCount(scanned, flipped) < mismatchCount(scanned, normal) ? flipped : normal;
  std::string issues[2];
  int count = 0;
  for (int i = 0; i < 64 && count < 2; ++i) {
    if (scanned[i] == expected[i]) continue;
    issues[count++] = (scanned[i] == '.' ? "-" : "+") + startupSquare(i);
  }
  if (count == 0) return "";
  if (count == 1) return issues[0];
  return issues[0] + "/" + issues[1];
}

std::string inferMove(const char oldBoard[65], const char newBoard[65], std::string* scanError = nullptr) {
  int foundSource = -1, foundDestination = -1, matches = 0;
  int bestMismatch = 65, bestMismatchCount = 0;
  char bestExpected[65] = {0};
  for (int source = 0; source < 64; ++source) {
    const char mover = oldBoard[source];
    if (mover == '.') continue;
    const bool white = mover >= 'A' && mover <= 'Z';
    for (int destination = 0; destination < 64; ++destination) {
      if (destination == source || newBoard[destination] == '.') continue;
      const bool destWhite = newBoard[destination] >= 'A' && newBoard[destination] <= 'Z';
      const bool destBlack = newBoard[destination] >= 'a' && newBoard[destination] <= 'z';
      const bool destSameSide = white ? destWhite : destBlack;
      if (!destSameSide) continue;

      char simulated[65];
      memcpy(simulated, oldBoard, sizeof(simulated));
      simulated[source] = '.';
      simulated[destination] = newBoard[destination];

      const int sf = source % 8, sr = source / 8, df = destination % 8, dr = destination / 8;
      const char lowerMover = static_cast<char>(tolower(static_cast<unsigned char>(mover)));
      const bool enPassant = lowerMover == 'p' && sf != df && oldBoard[destination] == '.';
      if (enPassant) {
        const int dir = white ? -1 : 1;
        const char captured = oldBoard[sr * 8 + df];
        const bool capturedOpponentPawn = white ? captured == 'p' : captured == 'P';
        if (abs(df - sf) != 1 || dr - sr != dir || !capturedOpponentPawn) continue;
        simulated[sr * 8 + df] = '.';
      } else if (!plausibleBoardMove(oldBoard, source, destination)) {
        continue;
      }

      if (lowerMover == 'k' && sr == dr && abs(df - sf) == 2) {
        const int rookSource = sr * 8 + (df > sf ? 7 : 0);
        const int rookDestination = sr * 8 + (df > sf ? df - 1 : df + 1);
        simulated[rookDestination] = simulated[rookSource];
        simulated[rookSource] = '.';
      }

      int mismatch = 0;
      for (int i = 0; i < 64; ++i) {
        if (simulated[i] != newBoard[i]) ++mismatch;
      }
      if (mismatch < bestMismatch) {
        bestMismatch = mismatch;
        bestMismatchCount = 1;
        memcpy(bestExpected, simulated, sizeof(bestExpected));
      } else if (mismatch == bestMismatch) {
        ++bestMismatchCount;
      }
      if (mismatch != 0) continue;
      foundSource = source;
      foundDestination = destination;
      ++matches;
    }
  }
  if (matches != 1) {
    // Ported from CynusLink's inferMoveDisplayText(): if there's a single
    // best-fitting candidate move that's off by just 1-2 squares, name
    // those exact squares (e.g. "+E4"/"-D5") instead of a generic error --
    // much more useful feedback on Cynus's own display when a scan is
    // close but not quite settled.
    if (scanError != nullptr && bestMismatchCount == 1 && bestMismatch > 0) {
      std::string issues[2];
      int count = 0;
      for (int i = 0; i < 64 && count < 2; ++i) {
        if (newBoard[i] == bestExpected[i]) continue;
        issues[count++] = (newBoard[i] == '.' ? "-" : "+") + startupSquare(i);
      }
      if (count == 1) *scanError = issues[0];
      else if (count == 2) *scanError = issues[0] + "/" + issues[1];
    }
    return "";
  }
  std::string uci = squareName(foundSource % 8, foundSource / 8) + squareName(foundDestination % 8, foundDestination / 8);
  // Promotion: append the actually-scanned piece at the destination as a
  // 5th UCI character (e.g. "e7e8q") when a pawn reached the last rank --
  // moveDisplayText() turns this into CynusLink's own "Chg Q"/"Chg R"/etc.
  // display text. The camera already tells us which piece was placed
  // (newBoard[foundDestination]); no separate promotion-choice logic needed.
  const bool isPawn = tolower(static_cast<unsigned char>(oldBoard[foundSource])) == 'p';
  const int destRankTop = foundDestination / 8;
  if (isPawn && (destRankTop == 0 || destRankTop == 7)) {
    uci += static_cast<char>(tolower(static_cast<unsigned char>(newBoard[foundDestination])));
  }
  return uci;
}

bool hasBothKings(const char board[65]) {
  int whiteKings = 0, blackKings = 0;
  for (int i = 0; i < 64; ++i) {
    if (board[i] == 'K') ++whiteKings;
    else if (board[i] == 'k') ++blackKings;
  }
  return whiteKings == 1 && blackKings == 1;
}

// Converts a board64-convention 65-char position (rankTop=0..7 top-to-
// bottom, i.e. rank8..rank1) to the canonical Mode-B wire-format 's' status
// frame and hands it to the shared King-facing pipeline. Takes an explicit
// array (not just board64 itself) so callers can publish a snapshot other
// than the live confirmed position (Free Analysis forwards raw scans
// directly, for instance).
void publishBoard(const char position[65]) {
  uint8_t frame[kModeBStatusFrameLength] = {};
  frame[0] = 's';
  for (int i = 0; i < 64; ++i) {
    const int rankTop = i / 8;
    const int file0 = i % 8;
    const int rank = 8 - rankTop;
    frame[1 + modeBStatusWireIndex(file0, rank)] = static_cast<uint8_t>(position[i]);
  }
  // Plain checksum, not cableHostUsesEncodedChecksum -- see
  // sendCableStatusFrame()'s comment in chessnut_board.cpp (2026-08-31):
  // a real Mode-B board's own outgoing checksum is always computed over
  // plain content; that flag only describes how to interpret Phoenix's
  // own incoming frames, not what convention our replies should use.
  computeModeBChecksumHex(frame + 65, frame, 65, /*useEncodedConvention=*/false);
  onBoardStatusFrame(frame);
}

void publishStatus() { publishBoard(board64); }

bool boardsEqual(const char a[65], const char b[65]) {
  return memcmp(a, b, 64) == 0;
}

// Castling moves two pieces at once, but a physical ChessLink-style board
// (and Phoenix's own expectation of one) only ever reports one piece lifted
// and placed at a time. Sending the finished castled position as a single
// status jump skips the sequence Phoenix needs to actually track the move
// (king lifted, king placed, rook lifted, rook placed). This replays that
// exact 4-stage sequence as separate status frames, only updating the real
// board64 on the final stage (using the camera's own confirmed final
// position, not a hand-reconstructed one).
bool castlingTransitionActive = false;
int castlingStage = 0;  // 0=king lifted sent, 1=king placed sent, 2=rook lifted sent
uint32_t castlingStageAt = 0;
constexpr uint32_t kCastlingStageHoldMs = 400;
int ctKingSource = -1, ctKingDest = -1, ctRookSource = -1, ctRookDest = -1;
char ctFinalBoard[65];

void beginCastlingTransition(int kingSource, int kingDest, int rookSource, int rookDest,
                              const char finalBoard[65]) {
  ctKingSource = kingSource;
  ctKingDest = kingDest;
  ctRookSource = rookSource;
  ctRookDest = rookDest;
  memcpy(ctFinalBoard, finalBoard, 65);
  castlingStage = 0;
  castlingTransitionActive = true;

  char stage[65];
  memcpy(stage, board64, sizeof(stage));  // still the pre-castling position
  stage[kingSource] = '.';
  Serial.println("[CYNUS] castling: king lifted");
  publishBoard(stage);
  castlingStageAt = millis() + kCastlingStageHoldMs;
}

void processCastlingTransition() {
  if (!castlingTransitionActive) return;
  if (static_cast<int32_t>(millis() - castlingStageAt) < 0) return;

  char stage[65];
  memcpy(stage, board64, sizeof(stage));
  const char king = board64[ctKingSource];
  if (castlingStage == 0) {
    stage[ctKingSource] = '.';
    stage[ctKingDest] = king;
    Serial.println("[CYNUS] castling: king placed");
    publishBoard(stage);
    castlingStage = 1;
    castlingStageAt = millis() + kCastlingStageHoldMs;
  } else if (castlingStage == 1) {
    stage[ctKingSource] = '.';
    stage[ctKingDest] = king;
    stage[ctRookSource] = '.';
    Serial.println("[CYNUS] castling: rook lifted");
    publishBoard(stage);
    castlingStage = 2;
    castlingStageAt = millis() + kCastlingStageHoldMs;
  } else {
    memcpy(board64, ctFinalBoard, sizeof(board64));
    Serial.println("[CYNUS] castling: rook placed (final)");
    publishStatus();
    castlingTransitionActive = false;
  }
}

// En passant: the moving pawn's own lift+place looks like an ordinary
// (if geometrically odd -- diagonal onto an empty square) pawn move, but a
// third square (the captured pawn, on the same rank as the source and same
// file as the destination) also changes, with no lift/place of its own --
// it's just removed. Ported from the user's own description of exactly how
// a physical board reports it: pawn lifted, pawn placed (still on the
// "wrong-looking" empty destination), captured pawn lifted -- done, no
// placement stage for the captured square.
bool enPassantTransitionActive = false;
int enPassantStage = 0;  // 0 = pawn lifted sent, 1 = pawn placed sent
uint32_t enPassantStageAt = 0;
int epSource = -1, epDest = -1, epCapturedSquare = -1;
char epFinalBoard[65];

void beginEnPassantTransition(int source, int dest, int capturedSquare, const char finalBoard[65]) {
  epSource = source;
  epDest = dest;
  epCapturedSquare = capturedSquare;
  memcpy(epFinalBoard, finalBoard, 65);
  enPassantStage = 0;
  enPassantTransitionActive = true;

  char stage[65];
  memcpy(stage, board64, sizeof(stage));  // still the pre-move position
  stage[source] = '.';
  Serial.println("[CYNUS] en passant: pawn lifted");
  publishBoard(stage);
  enPassantStageAt = millis() + kCastlingStageHoldMs;
}

void processEnPassantTransition() {
  if (!enPassantTransitionActive) return;
  if (static_cast<int32_t>(millis() - enPassantStageAt) < 0) return;

  if (enPassantStage == 0) {
    char stage[65];
    memcpy(stage, board64, sizeof(stage));
    const char pawn = board64[epSource];
    stage[epSource] = '.';
    stage[epDest] = pawn;
    Serial.println("[CYNUS] en passant: pawn placed");
    publishBoard(stage);
    enPassantStage = 1;
    enPassantStageAt = millis() + kCastlingStageHoldMs;
  } else {
    memcpy(board64, epFinalBoard, sizeof(board64));
    Serial.println("[CYNUS] en passant: captured pawn removed (final)");
    publishStatus();
    enPassantTransitionActive = false;
  }
}

bool matchesOption(const char scanned[65], const char* optionFen) {
  char pattern[65];
  return fenPlacementToBoard(optionFen, pattern) && boardsEqual(scanned, pattern);
}

void enterFreeAnalysis();
void enterSetPosition();

// Black king moved to one of the known "menu" squares -- send the matching
// command to Cynus instead of treating this as a real move, or switch into
// one of the special modes. Returns true if the scanned board was consumed
// as an option selection (including a no-op "leave a mode you're not in").
bool handleOptionBoard(const char scanned[65]) {
  struct SoundFlipOption {
    const char* fen;
    const char* command;
    const char* displayText;
    const char* logName;
  };
  static const SoundFlipOption options[] = {
      {kSoundOnFen, "sound 70\n", "snd on", "sound on"},
      {kSoundOffFen, "sound 0\n", "snd off", "sound off"},
      {kFlipOnFen, "set flip board on\n", "flip on", "flip on"},
      {kFlipOffFen, "set flip board off\n", "flip off", "flip off"},
  };
  for (const SoundFlipOption& option : options) {
    if (!matchesOption(scanned, option.fen)) continue;
    Serial.printf("[CYNUS] option selected via black king: %s\r\n", option.logName);
    sendCynus(option.command);
    cynusDisplay(option.displayText);
    return true;
  }

  if (matchesOption(scanned, kAnalysisOnFen)) { enterFreeAnalysis(); return true; }
  if (matchesOption(scanned, kAnalysisOffFen)) return true;  // not in Free Analysis -- no-op
  if (matchesOption(scanned, kSetPositionOnFen)) { enterSetPosition(); return true; }
  if (matchesOption(scanned, kSetPositionOffFen)) return true;  // not in Set Position -- no-op
  return false;
}

// Startup: wait for the physical/camera-scanned position to match a normal
// or flipped starting position, then tell Cynus which orientation it's
// seeing so its own firmware corrects for it -- ported from CynusLink,
// which confirmed this against real hardware.
// Shared by handleStartupBoard() and the mid-game override branch in
// handleFenLine(): commits scanned as the new confirmed position and
// resumes normal play from it.
void acceptScannedPosition(const char scanned[65]) {
  nextStartupRescanAt = 0;
  lastStartupErrorDisplay.clear();
  memcpy(board64, scanned, sizeof(board64));
  syncState = SyncState::Ready;
  engineSide = EngineSide::Unknown;
  firstMoveOrientationLocked = false;
  setMoveCycle(MoveCycle::WaitFirstMove);
  cynusDisplay("POS OK");
  publishStatus();
}

// Manual override state machine, shared between handleStartupBoard() (right
// after a reconnect) and handleFenLine()'s mid-game path -- extended
// 2026-09-03 to work at any point during play, not just right after
// connecting (user's own follow-up request: "das wäre gut"). Returns true
// if scanned was consumed by the override (armed, still waiting, or
// accepted) -- caller should stop processing this fen either way.
bool handleManualOverrideScan(const char scanned[65]) {
  if (startupManualOverrideArmed) {
    if (hasBothKings(scanned)) {
      Serial.println("[CYNUS] manual override: kings restored -- accepting current position as-is "
                      "(e.g. after an unexpected mid-game reconnect)");
      startupManualOverrideArmed = false;
      acceptScannedPosition(scanned);
      return true;
    }
    Serial.println("[CYNUS] manual override: scanned position still missing a king -- waiting "
                    "for the next Clock/Scan button press");
    nextStartupRescanAt = 0;
    return true;
  }
  if (hasNoKings(scanned)) {
    startupManualOverrideArmed = true;
    // Stop the automatic 5s rescan entirely -- same trick Set Position mode
    // already relies on (see handleSetPositionFen()/the "promotions:"
    // comment above): since WE never ask for a scan again from here on, the
    // only way a new fen can possibly arrive is a spontaneous push from
    // Cynus itself, i.e. the player pressing its own physical Clock/Scan
    // button once the desired position is fully set up. This also fixes the
    // premature-accept bug the timer caused: with polling left running, an
    // auto-rescan could catch the board mid-setup (e.g. kings placed back
    // first) and accept an incomplete position.
    Serial.println("[CYNUS] manual override armed: both kings removed -- automatic rescanning "
                    "stopped; set up the desired position, then press Cynus's own Clock/Scan "
                    "button to submit it");
    cynusDisplay("newpos");
    nextStartupRescanAt = 0;
    return true;
  }
  return false;
}

void handleStartupBoard(const char scanned[65]) {
  char normal[65], flipped[65];
  if (!fenPlacementToBoard(kStartFen, normal) || !fenPlacementToBoard(kFlippedStartFen, flipped)) return;

  if (boardsEqual(scanned, normal)) {
    sendCynus("set flip board off\n");
    Serial.println("[CYNUS] startup position OK (normal orientation)");
    acceptScannedPosition(scanned);
    return;
  }
  if (boardsEqual(scanned, flipped)) {
    sendCynus("set flip board on\n");
    Serial.println("[CYNUS] startup position OK (flipped orientation)");
    acceptScannedPosition(scanned);
    return;
  }
  if (handleManualOverrideScan(scanned)) return;

  Serial.println("[CYNUS] startup position not yet a valid start position; "
                  "automatic rescan in 5 seconds");
  const std::string errorText = startupErrorDisplay(scanned);
  if (!errorText.empty() && errorText != lastStartupErrorDisplay) {
    lastStartupErrorDisplay = errorText;
    cynusDisplay(errorText);
    sendCynus("play audio error\n");
    Serial.printf("[CYNUS] startup position error display: %s (error audio)\r\n", errorText.c_str());
  }
  nextStartupRescanAt = millis() + kStartupCorrectionRescanMs;
}

// Locks which side King/Phoenix is playing, the first time either side
// actually moves: the human moving first means the chess computer plays
// Black (board stays as-is); the chess computer suggesting its own first
// move means it plays White (board must be flipped so its LED suggestions
// still land on the correct physical squares). Ported as-is from
// CynusLink's lockFirstMoveOrientation().
bool lockFirstMoveOrientation(bool computerMovesFirst) {
  if (firstMoveOrientationLocked) return true;
  const char* command = computerMovesFirst ? "set flip board on\n" : "set flip board off\n";
  if (!sendCynus(command)) {
    Serial.printf("[CYNUS FIRST MOVE] set flip board %s FAILED; first move remains gated\r\n",
                  computerMovesFirst ? "ON" : "OFF");
    return false;
  }
  firstMoveOrientationLocked = true;
  engineSide = computerMovesFirst ? EngineSide::White : EngineSide::Black;
  Serial.printf("[CYNUS FIRST MOVE] %s moved first -> flip board %s; orientation locked for game\r\n",
                computerMovesFirst ? "chess computer" : "Cynus board",
                computerMovesFirst ? "ON" : "OFF");
  return true;
}

void commitMoveToRobot(const std::string& uci) {
  if (uci == lastSentMoveUci) return;
  lastSentMoveUci = uci;
  const std::string displayText = moveDisplayText(uci);
  if (!displayText.empty()) cynusDisplay(displayText);
  Serial.printf("[CYNUS] move accepted: %s\r\n", uci.c_str());
  sendCynus("move " + uci + "\n");
  setMoveCycle(MoveCycle::WaitRobotPosition);
}

// --- Single-square-per-frame handling (Mephisto Phoenix only) -------------
//
// CynusLink's extractMoveFromLCommand() (ported above) assumes a King-style
// host that marks source AND destination together in the same L frame
// (>=6 lit corners at once). Confirmed on real hardware 2026-08-29 that this
// Mephisto Phoenix unit does NOT do that: every L frame marks exactly ONE
// square (4 lit corners) or none (blink-off). Decoding several consecutive
// real frames after a human e2-e4 showed the lit square is NOT constant --
// it alternates between exactly two squares over time (a couple of
// transient/settling frames first, then a clean repeating e7/e5 pattern,
// matching Black's real reply e7-e5 to the letter). So Phoenix time-
// multiplexes its move suggestion across successive frames instead of
// encoding both squares in one frame like King does. This tracks the last
// (up to) two *distinct* non-blank squares seen; once that pair has stayed
// the same for kLedMoveStableMs (no third square appearing in between,
// which would mean it's still animating/thinking), source vs. destination
// is resolved from board occupancy and the move is committed.
int alternatingSquareA = -1;
int alternatingSquareB = -1;
uint32_t alternatingPairStableSince = 0;

void clearAlternatingPair(const char* reason) {
  if (alternatingSquareA >= 0 && reason != nullptr) {
    Serial.printf("[CYNUS LED] alternating-square tracking reset: %s\r\n", reason);
  }
  alternatingSquareA = -1;
  alternatingSquareB = -1;
  alternatingPairStableSince = 0;
}

// Finds the single square whose 4 corners are all lit (the only per-square
// shape this hardware ever produces) and returns its board64 index, or -1
// if none. Same 7-file mirror as decodeKingLedFrame().
int findSingleLitSquareB64() {
  for (int file = 0; file < 8; ++file) {
    for (int rankTop = 0; rankTop < 8; ++rankTop) {
      if (ledValue(file, rankTop) != 0 && ledValue(file + 1, rankTop) != 0 &&
          ledValue(file, rankTop + 1) != 0 && ledValue(file + 1, rankTop + 1) != 0) {
        const int changedFile = 7 - file;
        const int changedRank = rankTop + 1;
        return (8 - changedRank) * 8 + changedFile;
      }
    }
  }
  return -1;
}

EngineSide sideOfPieceChar(char p) {
  if (p >= 'A' && p <= 'Z') return EngineSide::White;
  if (p >= 'a' && p <= 'z') return EngineSide::Black;
  return EngineSide::Unknown;
}

// Resolves which of the two alternating squares is the source (has the
// moving piece) and which is the destination, using board64 occupancy --
// same logic as extractMoveFromLCommand()'s occA!=occB / capture handling.
bool resolveAlternatingPair(int a, int b, int& source, int& destination) {
  const char pieceA = board64[a], pieceB = board64[b];
  const bool occA = pieceA != '.', occB = pieceB != '.';
  if (occA != occB) {
    source = occA ? a : b;
    destination = occA ? b : a;
    return true;
  }
  if (!occA && !occB) return false;  // both empty -- nonsensical, wait for a change
  // Both occupied (a capture): try both directions, keeping whichever is a
  // plausible move for the side actually to move.
  const bool aToB = plausibleBoardMove(board64, a, b, engineSide) &&
                     (engineSide == EngineSide::Unknown || sideOfPieceChar(pieceA) == engineSide);
  const bool bToA = plausibleBoardMove(board64, b, a, engineSide) &&
                     (engineSide == EngineSide::Unknown || sideOfPieceChar(pieceB) == engineSide);
  if (aToB == bToA) return false;  // ambiguous or neither -- wait for a change
  source = aToB ? a : b;
  destination = aToB ? b : a;
  return true;
}

void handleSingleSquareFrame() {
  const int candidateB64 = findSingleLitSquareB64();
  if (candidateB64 < 0) return;

  if (candidateB64 == alternatingSquareA || candidateB64 == alternatingSquareB) {
    // Still within the known pair -- nothing new, fall through to the
    // stability check below.
  } else if (alternatingSquareA < 0) {
    alternatingSquareA = candidateB64;
    alternatingPairStableSince = millis();
    return;
  } else if (alternatingSquareB < 0) {
    alternatingSquareB = candidateB64;
    alternatingPairStableSince = millis();
    return;
  } else {
    // A third distinct square appeared -- Phoenix is still animating/
    // recalculating. Restart tracking from this new square.
    alternatingSquareA = candidateB64;
    alternatingSquareB = -1;
    alternatingPairStableSince = millis();
    return;
  }

  if (alternatingSquareB < 0) return;  // only ever seen one square so far
  if (static_cast<int32_t>(millis() - alternatingPairStableSince) < static_cast<int32_t>(kLedMoveStableMs)) return;

  int source = -1, destination = -1;
  if (!resolveAlternatingPair(alternatingSquareA, alternatingSquareB, source, destination)) {
    return;  // ambiguous for now -- keep waiting, a later change will retry
  }
  if (engineSide == EngineSide::Unknown) engineSide = sideOfPieceChar(board64[source]);
  if (!plausibleBoardMove(board64, source, destination, engineSide)) {
    return;  // still ambiguous/stale -- keep waiting
  }

  const std::string uci = squareName(source % 8, source / 8) + squareName(destination % 8, destination / 8);
  commitMoveToRobot(uci);
  clearAlternatingPair(nullptr);
}

void clearPendingLedMove(const char* reason) {
  if (!pendingLedMove.empty() && reason != nullptr) {
    Serial.printf("[CYNUS LED] pending %s cancelled: %s\r\n", pendingLedMove.c_str(), reason);
  }
  pendingLedMove.clear();
  pendingLedFrame.clear();
  pendingLedMoveSince = 0;
}

void armPendingLedMove(const std::string& uci, const std::string& frameKey) {
  if (pendingLedMove == uci && pendingLedFrame == frameKey) return;
  pendingLedMove = uci;
  pendingLedFrame = frameKey;
  pendingLedMoveSince = millis();
  Serial.printf("[CYNUS LED] candidate %s armed; waiting for stable final LED state\r\n", uci.c_str());
}

uint8_t ledValue(int fileCorner, int rankCornerTop) {
  if (fileCorner < 0 || fileCorner > 8 || rankCornerTop < 0 || rankCornerTop > 8) return 0;
  return ledGrid[fileCorner * 9 + rankCornerTop];
}

// Of a square's 4 corners, returns whichever single byte value appears on
// the most of them (ties broken by lowest value) -- used only to
// disambiguate an occupied-occupied pair when engineSide is still unknown
// (see extractMoveFromLCommand()). Ported as-is from CynusLink.
uint8_t dominantSquarePattern(int file, int rankCornerTop) {
  const uint8_t corners[4] = {ledValue(file, rankCornerTop), ledValue(file + 1, rankCornerTop),
                               ledValue(file, rankCornerTop + 1), ledValue(file + 1, rankCornerTop + 1)};
  int counts[256] = {0};
  for (uint8_t v : corners) if (v != 0) counts[v]++;
  int best = 0;
  for (int v = 1; v < 256; ++v) if (counts[v] > counts[best]) best = v;
  return static_cast<uint8_t>(best);
}

// New Game / reset floods (and some mid-calculation intermediate frames)
// light far more than 2 squares at once. Ported as-is from CynusLink's
// extractPhasedMoveFromLCommand(): once engineSide is known, look for the
// engine's own piece whose 4 corners are purely low-nibble-marked (a "this
// one is still mid-animation, not the final source" pattern) together with
// the destination scoring highest on the high nibble -- the single best,
// unambiguous candidate is accepted.
bool extractPhasedMoveFromLCommand(std::string& uci) {
  if (engineSide == EngineSide::Unknown) return false;
  int bestSource = -1, bestDestination = -1, bestScore = -999, bestCount = 0;
  for (int source = 0; source < 64; ++source) {
    const char piece = board64[source];
    const EngineSide sourceSide = (piece >= 'A' && piece <= 'Z') ? EngineSide::White
                                   : (piece >= 'a' && piece <= 'z') ? EngineSide::Black
                                                                     : EngineSide::Unknown;
    if (sourceSide != engineSide) continue;
    const int sf = source % 8, sr = source / 8;
    const uint8_t sourceCorners[4] = {ledValue(sf, sr), ledValue(sf + 1, sr), ledValue(sf, sr + 1),
                                       ledValue(sf + 1, sr + 1)};
    int sourceLow = 0, sourceHigh = 0;
    for (uint8_t v : sourceCorners) {
      if (v & 0x0F) ++sourceLow;
      if (v & 0xF0) ++sourceHigh;
    }
    if (sourceLow != 4 || sourceHigh != 0) continue;

    for (int destination = 0; destination < 64; ++destination) {
      if (!plausibleBoardMove(board64, source, destination, engineSide)) continue;
      const int df = destination % 8, dr = destination / 8;
      if (static_cast<char>(tolower(static_cast<unsigned char>(piece))) == 'k' && sr == dr &&
          abs(df - sf) == 2) {
        continue;
      }
      const uint8_t destinationCorners[4] = {ledValue(df, dr), ledValue(df + 1, dr), ledValue(df, dr + 1),
                                              ledValue(df + 1, dr + 1)};
      int destinationLow = 0, destinationHigh = 0;
      for (uint8_t v : destinationCorners) {
        if (v & 0x0F) ++destinationLow;
        if (v & 0xF0) ++destinationHigh;
      }
      if (destinationHigh < 2) continue;
      const int score = destinationHigh * 4 - destinationLow;
      if (score > bestScore) {
        bestScore = score;
        bestSource = source;
        bestDestination = destination;
        bestCount = 1;
      } else if (score == bestScore) {
        ++bestCount;
      }
    }
  }
  if (bestSource < 0 || bestCount != 1) {
    if (bestSource >= 0) {
      Serial.printf("[CYNUS] phased LED pattern ambiguous: best score=%d candidates=%d\r\n", bestScore, bestCount);
    }
    return false;
  }
  uci = squareName(bestSource % 8, bestSource / 8) + squareName(bestDestination % 8, bestDestination / 8);
  Serial.printf("[CYNUS] phased LED fallback selected %s (score=%d)\r\n", uci.c_str(), bestScore);
  return true;
}

// Decodes the current ledGrid into a move suggestion. Ported as-is from
// CynusLink's extractMoveFromLCommand(): finds exactly one pair of squares
// whose corners match all lit corner cells (King's own 0x0F/0xF0 dual
// convention, or Mephisto Phoenix's uniform 0xFF marking -- either way,
// this only cares which squares are lit, not which specific bit pattern),
// then figures out which one is the source using board occupancy (and, if
// still ambiguous, dominantSquarePattern()). Fewer than 6 lit corners means
// no complete signal yet (Phoenix's single still-blinking square included);
// more than 8 falls back to extractPhasedMoveFromLCommand() for New Game/
// reset floods.
bool extractMoveFromLCommand(std::string& uci) {
  bool observed[81];
  int observedCount = 0;
  for (int i = 0; i < 81; ++i) {
    observed[i] = ledGrid[i] != 0;
    if (observed[i]) ++observedCount;
  }
  if (observedCount < 6) return false;
  if (observedCount > 8) return extractPhasedMoveFromLCommand(uci);

  auto addSquareCorners = [](bool mask[81], int file, int rankTop) {
    mask[file * 9 + rankTop] = true;
    mask[(file + 1) * 9 + rankTop] = true;
    mask[file * 9 + rankTop + 1] = true;
    mask[(file + 1) * 9 + rankTop + 1] = true;
  };
  struct EndpointPair { int a; int b; };
  EndpointPair pairs[16];
  int pairCount = 0;
  for (int a = 0; a < 64; ++a) {
    for (int b = a + 1; b < 64; ++b) {
      bool expected[81] = {false};
      addSquareCorners(expected, a % 8, a / 8);
      addSquareCorners(expected, b % 8, b / 8);
      bool same = true;
      for (int i = 0; i < 81 && same; ++i) same = expected[i] == observed[i];
      if (same && pairCount < 16) pairs[pairCount++] = {a, b};
    }
  }
  if (pairCount != 1) return false;

  const int a = pairs[0].a, b = pairs[0].b;
  const char pieceA = board64[a], pieceB = board64[b];
  const bool occA = pieceA != '.', occB = pieceB != '.';
  auto sideOfPiece = [](char p) -> EngineSide {
    if (p >= 'A' && p <= 'Z') return EngineSide::White;
    if (p >= 'a' && p <= 'z') return EngineSide::Black;
    return EngineSide::Unknown;
  };
  int source = -1, destination = -1;
  if (occA != occB) {
    source = occA ? a : b;
    destination = occA ? b : a;
    const EngineSide sourceSide = sideOfPiece(board64[source]);
    if (sourceSide == EngineSide::Unknown) return false;
    if (engineSide != EngineSide::Unknown && sourceSide != engineSide) {
      Serial.printf("[CYNUS] ignoring LED pattern %s-%s: source side does not match engine side\r\n",
                    squareName(source % 8, source / 8).c_str(), squareName(destination % 8, destination / 8).c_str());
      return false;
    }
    if (engineSide == EngineSide::Unknown) engineSide = sourceSide;
  } else if (occA && occB) {
    if (engineSide == EngineSide::Unknown) {
      const uint8_t patternA = dominantSquarePattern(a % 8, a / 8);
      const uint8_t patternB = dominantSquarePattern(b % 8, b / 8);
      if (patternA == 0x33 && patternB == 0xCC) {
        source = a; destination = b; engineSide = sideOfPiece(pieceA);
      } else if (patternA == 0xCC && patternB == 0x33) {
        source = b; destination = a; engineSide = sideOfPiece(pieceB);
      } else {
        return false;
      }
    } else {
      const bool aIsEngine = sideOfPiece(pieceA) == engineSide;
      const bool bIsEngine = sideOfPiece(pieceB) == engineSide;
      if (aIsEngine == bIsEngine) return false;
      source = aIsEngine ? a : b;
      destination = aIsEngine ? b : a;
    }
  } else {
    return false;
  }
  if (!plausibleBoardMove(board64, source, destination, engineSide)) {
    Serial.printf("[CYNUS] ignoring LED pattern %s-%s: not a plausible move for current board\r\n",
                  squareName(source % 8, source / 8).c_str(), squareName(destination % 8, destination / 8).c_str());
    return false;
  }
  uci = squareName(source % 8, source / 8) + squareName(destination % 8, destination / 8);
  return true;
}

// Only commits a move once moveCycle actually allows it (i.e. it's really
// King/Phoenix's turn) AND the same candidate has stayed stable for
// kLedMoveStableMs. Ported as-is from CynusLink's processPendingLedMove().
void processPendingLedMove() {
  if (pendingLedMove.empty()) return;
  const bool firstComputerMove = moveCycle == MoveCycle::WaitFirstMove && !firstMoveOrientationLocked;
  const bool cycleOk = moveCycle == MoveCycle::WaitEngineMove || moveCycle == MoveCycle::WaitFirstMove;
  if (syncState != SyncState::Ready || !cycleOk) {
    clearPendingLedMove("gateway state changed");
    return;
  }
  if (static_cast<int32_t>(millis() - pendingLedMoveSince) < static_cast<int32_t>(kLedMoveStableMs)) return;

  if (firstComputerMove && !lockFirstMoveOrientation(true)) {
    pendingLedMoveSince = millis();  // retry next tick rather than dropping the candidate
    return;
  }

  const std::string uci = pendingLedMove;
  clearPendingLedMove(nullptr);
  commitMoveToRobot(uci);
}

void enterFreeAnalysis() {
  experimentalMode = ExperimentalMode::FreeAnalysis;
  setPositionManualScanExpected = false;
  clearPendingLedMove(nullptr);
  nextFreeAnalysisScanAt = millis() + kFreeAnalysisScanMs;
  cynusDisplay("Freemode");
  Serial.println("[CYNUS] Free Analysis enabled; automatic scan every 5 seconds, moves not validated");
}

void leaveFreeAnalysis() {
  experimentalMode = ExperimentalMode::None;
  nextFreeAnalysisScanAt = 0;
  firstMoveOrientationLocked = true;
  engineSide = EngineSide::Black;
  fenPlacementToBoard(kStartFen, board64);
  lastSentMoveUci.clear();
  setMoveCycle(MoveCycle::WaitHumanMove);
  cynusDisplay("play");
  publishStatus();
  Serial.println("[CYNUS] Free Analysis disabled; normal play restored with human/White to move");
}

// Free Analysis forwards every scanned position directly to King/Phoenix
// without running it through inferMove()'s single-legal-move gate: King
// itself accepts arbitrary positions while in its own matching analysis
// mode. Simplification vs. CynusLink: real per-piece lift/place animation
// (its processFreeAnalysisTransition()) isn't reproduced -- a full-board
// status is sent directly on each change instead. Not yet needed for this
// project's actual bug; revisit if King's own analysis display looks wrong.
void handleFreeAnalysisFen(const char scanned[65]) {
  if (matchesOption(scanned, kAnalysisOffFen)) {
    leaveFreeAnalysis();
    return;
  }
  if (boardsEqual(scanned, board64)) {
    Serial.println("[CYNUS] Free Analysis: unchanged scanned position ignored");
    return;
  }
  memcpy(board64, scanned, sizeof(board64));
  publishStatus();
  Serial.println("[CYNUS] Free Analysis: scanned position forwarded without move validation");
}

void enterSetPosition() {
  experimentalMode = ExperimentalMode::SetPosition;
  setPositionManualScanExpected = false;
  clearPendingLedMove(nullptr);
  cynusDisplay("Set Pos");
  Serial.println("[CYNUS] Set Position enabled; waiting for a manual Clock scan with both kings");
}

void leaveSetPosition(bool accepted, const char acceptedBoard[65]) {
  experimentalMode = ExperimentalMode::None;
  setPositionManualScanExpected = false;
  firstMoveOrientationLocked = false;
  engineSide = EngineSide::Unknown;
  lastSentMoveUci.clear();
  setMoveCycle(MoveCycle::WaitFirstMove);
  if (accepted && acceptedBoard != nullptr) {
    memcpy(board64, acceptedBoard, sizeof(board64));
    publishStatus();
    Serial.println("[CYNUS] Set Position: complete position forwarded; waiting for first mover");
  } else {
    fenPlacementToBoard(kStartFen, board64);
    Serial.println("[CYNUS] Set Position: cancelled; internal start position restored");
  }
  cynusDisplay("play");
}

void handleSetPositionFen(const char scanned[65]) {
  if (!setPositionManualScanExpected) {
    Serial.println("[CYNUS] Set Position: camera FEN ignored; press Clock to accept a position");
    return;
  }
  setPositionManualScanExpected = false;
  if (matchesOption(scanned, kSetPositionOffFen)) {
    leaveSetPosition(false, nullptr);
    return;
  }
  if (!hasBothKings(scanned)) {
    cynusDisplay("Set Pos");
    Serial.println("[CYNUS] Set Position: manual scan ignored; both kings are required");
    return;
  }
  leaveSetPosition(true, scanned);
}

void handleFenLine(const std::string& fen) {
  char scanned[65];
  if (!fenPlacementToBoard(fen, scanned)) {
    Serial.printf("[CYNUS] fen line is not a valid 64-square placement, ignored: %s\r\n", fen.c_str());
    return;
  }

  if (syncState == SyncState::WaitingForStartPosition) {
    handleStartupBoard(scanned);
    return;
  }

  // Same manual override as handleStartupBoard(), now also usable mid-game
  // (2026-09-03 follow-up request): both kings lifted at any point arms it,
  // exactly like right after a reconnect. Checked ahead of every other
  // in-game branch since kings vanishing is never a legal move or a
  // recognized option-board gesture -- left unchecked, it would otherwise
  // just fall through to the one-legal-move gate below and get rejected as
  // a bad scan.
  if (handleManualOverrideScan(scanned)) return;

  if (moveCycle == MoveCycle::WaitRobotPosition) {
    // Cynus's arm just finished executing the move we sent; this fen is its
    // own confirmation, not a new human move -- accept directly, no
    // legality check (we already know exactly what move was made). Display
    // is deliberately left showing that move (not reset to "play") until
    // the human's own move overwrites it -- user's explicit preference.
    memcpy(board64, scanned, sizeof(board64));
    setMoveCycle(MoveCycle::WaitHumanMove);
    publishStatus();
    return;
  }

  if (experimentalMode == ExperimentalMode::FreeAnalysis) {
    handleFreeAnalysisFen(scanned);
    return;
  }
  if (experimentalMode == ExperimentalMode::SetPosition) {
    handleSetPositionFen(scanned);
    return;
  }

  if (handleOptionBoard(scanned)) return;

  // syncState == Ready is only ever reached via handleStartupBoard(), which
  // already sets board64 -- so a confirmed board is always available here.
  if (boardsEqual(scanned, board64)) return;  // unchanged, nothing to do

  // A scan showing the pristine starting position, seen at ANY point mid-
  // game (not just right after connect), means the human physically reset
  // the pieces to begin a fresh game -- recognized regardless of moveCycle,
  // ahead of the normal one-legal-move check (a full reset is never "one
  // legal move" away from a mid-game position). Reuses handleStartupBoard()
  // as-is: it already re-locks orientation, resets engineSide/moveCycle to
  // WaitFirstMove, and publishes the fresh status to Phoenix.
  {
    char normalStart[65], flippedStart[65];
    if (fenPlacementToBoard(kStartFen, normalStart) && fenPlacementToBoard(kFlippedStartFen, flippedStart) &&
        (boardsEqual(scanned, normalStart) || boardsEqual(scanned, flippedStart))) {
      Serial.println("[CYNUS] starting position detected mid-game; treating as New Game");
      handleStartupBoard(scanned);
      return;
    }
  }

  if (matchesOption(scanned, kKingLiftFen)) {
    Serial.println("[CYNUS] black king lifted; waiting for an option square, not an error");
    return;
  }

  if (moveCycle == MoveCycle::WaitEngineMove) {
    // It's the chess computer's turn, not the human's -- any fen arriving
    // now is either sensor noise or the human idly touching a piece.
    // Ignore it; the board's own known-good state (board64) is untouched.
    Serial.println("[CYNUS] fen arrived during the chess computer's turn; ignored");
    return;
  }

  std::string scanError;
  const std::string uci = inferMove(board64, scanned, &scanError);
  if (uci.empty()) {
    // Not exactly one legal move away from the last confirmed position --
    // likely a transient/partial scan. Just log and wait for the next scan;
    // never accepted without passing this check, no matter how many times
    // it repeats. Show the specific offending square(s) when there's a
    // single close-but-not-quite candidate (ported from CynusLink), a
    // generic "scan err" otherwise.
    cynusDisplay(scanError.empty() ? "scan err" : scanError);
    sendCynus("play audio error\n");
    Serial.println("[CYNUS] scanned position is not one legal move from the last confirmed one; ignored");
    return;
  }

  if (moveCycle == MoveCycle::WaitFirstMove && !firstMoveOrientationLocked) {
    if (!lockFirstMoveOrientation(false)) {
      Serial.println("[CYNUS FIRST MOVE] human move held; flip OFF must succeed before forwarding");
      return;
    }
  }
  setMoveCycle(MoveCycle::WaitEngineMove);
  const std::string humanDisplayMove = moveDisplayText(uci);
  if (!humanDisplayMove.empty()) cynusDisplay(humanDisplayMove);

  // Castling moves king AND rook at once -- board64 still holds the
  // PRE-move position here, so this checks the ORIGINAL piece on the
  // move's source square. See beginCastlingTransition()'s comment for why
  // this needs a 4-stage replay instead of one direct status jump.
  const int uciSourceB64 = (8 - (uci[1] - '0')) * 8 + (uci[0] - 'a');
  const int uciDestB64 = (8 - (uci[3] - '0')) * 8 + (uci[2] - 'a');
  const char movedPiece = board64[uciSourceB64];
  const bool isCastling = tolower(static_cast<unsigned char>(movedPiece)) == 'k' &&
                           (uciSourceB64 / 8) == (uciDestB64 / 8) &&
                           abs((uciSourceB64 % 8) - (uciDestB64 % 8)) == 2;
  // En passant: a pawn moving diagonally onto a square that was still
  // EMPTY in the pre-move board64 -- the only way that's a legal pawn move
  // at all. The captured enemy pawn sits on the source's rank, destination's
  // file (a third square, neither source nor destination).
  const bool isEnPassant = tolower(static_cast<unsigned char>(movedPiece)) == 'p' &&
                            (uciSourceB64 % 8) != (uciDestB64 % 8) && board64[uciDestB64] == '.';
  if (isCastling) {
    const int rankTopRow = uciSourceB64 / 8;
    const bool kingSide = (uciDestB64 % 8) > (uciSourceB64 % 8);
    const int rookSourceB64 = rankTopRow * 8 + (kingSide ? 7 : 0);
    const int rookDestB64 = rankTopRow * 8 + (kingSide ? uciDestB64 % 8 - 1 : uciDestB64 % 8 + 1);
    beginCastlingTransition(uciSourceB64, uciDestB64, rookSourceB64, rookDestB64, scanned);
  } else if (isEnPassant) {
    const int capturedSquareB64 = (uciSourceB64 / 8) * 8 + (uciDestB64 % 8);
    beginEnPassantTransition(uciSourceB64, uciDestB64, capturedSquareB64, scanned);
  } else {
    memcpy(board64, scanned, sizeof(board64));
    publishStatus();
  }
}

// Case-insensitive equality, matching CynusLink's own String::equalsIgnoreCase
// tolerance for the "get move" line.
bool equalsIgnoreCase(const std::string& a, const char* b) {
  size_t i = 0;
  for (; i < a.size() && b[i] != '\0'; ++i) {
    if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return i == a.size() && b[i] == '\0';
}

// Parses Cynus's own "get software version" reply -- confirmed on real
// hardware 2026-09-03: "App version: V1.4.2" (major.minor.patch after the
// "V"). Returns false if the line doesn't match this exact format.
bool parseCynusAppVersion(const std::string& line, int& major, int& minor, int& patch) {
  const char* kPrefix = "App version: V";
  const size_t prefixLen = strlen(kPrefix);
  if (line.rfind(kPrefix, 0) != 0) return false;
  const char* p = line.c_str() + prefixLen;
  char* end = nullptr;
  major = static_cast<int>(strtol(p, &end, 10));
  if (end == p || *end != '.') return false;
  p = end + 1;
  minor = static_cast<int>(strtol(p, &end, 10));
  if (end == p || *end != '.') return false;
  p = end + 1;
  patch = static_cast<int>(strtol(p, &end, 10));
  if (end == p) return false;
  return true;
}

void handleLine(const std::string& line) {
  Serial.printf("[CYNUS LINE] %s\n", line.c_str());
  {
    int major = 0, minor = 0, patch = 0;
    if (parseCynusAppVersion(line, major, minor, patch)) {
      // User's own feature request to the manufacturer: without this,
      // Cynus's own internal move-legality checking fights the gateway
      // whenever WE (not Cynus's own onboard engine) are driving the game
      // over BLE. Only send the toggle to a unit confirmed to understand
      // it -- an older unit's reaction to an unknown command is untested.
      const bool supportsIllegalMoveCheckToggle =
          (major > 1) || (major == 1 && minor > 4) || (major == 1 && minor == 4 && patch >= 2);
      Serial.printf("[CYNUS] app version %d.%d.%d%s\r\n", major, minor, patch,
                    supportsIllegalMoveCheckToggle
                        ? " (illegal-move-check toggle supported)"
                        : " (illegal-move-check toggle NOT supported -- need >= 1.4.2)");
      if (supportsIllegalMoveCheckToggle) {
        sendCynus("set illegal move check off\n");
      }
      return;
    }
  }
  if (line.rfind("fen:", 0) == 0) {
    std::string fen = line.substr(4);
    size_t start = fen.find_first_not_of(' ');
    if (start != std::string::npos) fen = fen.substr(start);
    handleFenLine(fen);
    return;
  }
  if (equalsIgnoreCase(line, "get move")) {
    if (experimentalMode == ExperimentalMode::FreeAnalysis) {
      Serial.println("[CYNUS] Free Analysis: get move ignored; periodic scans publish raw positions");
      return;
    }
    if (experimentalMode == ExperimentalMode::SetPosition) {
      Serial.println("[CYNUS] Set Position: get move ignored; press Clock to accept the setup");
      return;
    }
    // Cynus itself pings this whenever it's ready for the next step (after a
    // physical button press, or once its own arm finishes executing a move
    // we sent). Ported from CynusLink's own moveCycle gating: a rescan is
    // only requested while it's actually the human's/robot's turn to
    // report something (WaitHumanMove/WaitFirstMove/WaitRobotPosition).
    // While it's WaitEngineMove (the chess computer is still thinking),
    // Cynus's own idle "get move" heartbeat must NOT trigger a rescan --
    // doing so unconditionally (the bug before this fix) caused constant,
    // pointless "get fen" scans while nothing on the physical board had
    // changed at all.
    if (syncState != SyncState::Ready) return;
    if (moveCycle == MoveCycle::WaitEngineMove) return;
    // Deliberately no cynusDisplay("play") here even during WaitRobotPosition --
    // the last move's display text stays up until the human's own next move
    // overwrites it (user's explicit preference, differs from CynusLink).
    Serial.println("[CYNUS] get move -> requesting fresh fen");
    sendCynus("get fen\n");
    return;
  }
  if (line.rfind("promotions:", 0) == 0) {
    // Per the official Cynus BLE protocol doc (v3.7): Cynus always sends
    // this immediately before a "fen:" line, listing which pawns (if any)
    // are on a promotion rank -- "promotions: ----" means none. In Set
    // Position mode this line is also Cynus's only "Clock button pressed"
    // signal (ported from CynusLink); otherwise it's expected, normal
    // traffic and only logged. Pawn-promotion piece selection itself isn't
        // built yet (deferred).
    if (experimentalMode == ExperimentalMode::SetPosition) {
      setPositionManualScanExpected = true;
      Serial.println("[CYNUS] Set Position: manual Clock scan detected; next FEN may be accepted");
    }
    return;
  }
  // Any other line (get serial/battery replies, robot status, etc.) is not
  // handled yet -- we never request them, so none are expected.
}

// Only pushes raw bytes to a queue -- runs in the NimBLE host task's own
// context, which (per this project's earlier stack-overflow lesson with
// Millennium/Chessnut) is not a safe place to do the line-accumulation and
// move-inference work itself. cynusPoll() drains the queue on the main loop's
// own stack instead.
void onNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
  // Raw per-notification dump, matching CynusLink's own [CYNUS RX] logging
  // exactly (escaped \r\n, printable ASCII only) -- shows precisely what
  // arrives over BLE before any of our own line-accumulation touches it.
  Serial.print("[CYNUS RX] ");
  for (size_t i = 0; i < length; ++i) {
    const char c = static_cast<char>(data[i]);
    if (c == '\r') Serial.print("\\r");
    else if (c == '\n') Serial.print("\\n");
    else if (c >= 32 && c <= 126) Serial.write(c);
  }
  Serial.println();

  if (rxQueue == nullptr) return;
  while (length > 0) {
    RawPacket packet{};
    packet.length = static_cast<uint8_t>(min(length, kMaxNotifyChunk));
    memcpy(packet.data, data, packet.length);
    xQueueSend(rxQueue, &packet, 0);
    data += packet.length;
    length -= packet.length;
  }
}

void resetConnectionState() {
  syncState = SyncState::WaitingForStartPosition;
  moveCycle = MoveCycle::WaitHumanMove;
  engineSide = EngineSide::Unknown;
  firstMoveOrientationLocked = false;
  experimentalMode = ExperimentalMode::None;
  setPositionManualScanExpected = false;
  nextFreeAnalysisScanAt = 0;
  lastSentMoveUci.clear();
  nextStartupRescanAt = 0;
  initialScanAt = 0;
  startupManualOverrideArmed = false;
  clearPendingLedMove(nullptr);
  alternatingSquareA = -1;
  alternatingSquareB = -1;
  alternatingPairStableSince = 0;
  castlingTransitionActive = false;
  enPassantTransitionActive = false;
  lastStartupErrorDisplay.clear();
}

class ClientCallbacks final : public NimBLEClientCallbacks {
 public:
  void onConnect(NimBLEClient*) override {}
  void onDisconnect(NimBLEClient*, int) override {
    chr = nullptr;
    resetConnectionState();
    Serial.println("BLE disconnected; reconnecting automatically.");
  }
};

}  // namespace

bool cynusIsConnected() {
  return bleClient != nullptr && bleClient->isConnected() && chr != nullptr;
}

bool cynusConnect(const NimBLEAddress& address) {
  if (bleClient == nullptr) {
    bleClient = NimBLEDevice::createClient();
    bleClient->setClientCallbacks(new ClientCallbacks(), true);
    bleClient->setConnectTimeout(kConnectTimeoutMs);
  }
  if (rxQueue == nullptr) rxQueue = xQueueCreate(16, sizeof(RawPacket));

  Serial.printf("Connecting to %s ...\r\n", address.toString().c_str());
  if (!bleClient->connect(address)) {
    Serial.println("BLE connection failed.");
    return false;
  }

  NimBLERemoteService* service = bleClient->getService(kServiceUuid);
  if (service == nullptr) {
    Serial.println("Cynus BLE service not found.");
    bleClient->disconnect();
    return false;
  }
  chr = service->getCharacteristic(kCharUuid);
  if (chr == nullptr || !chr->canNotify() || !chr->subscribe(true, onNotify)) {
    Serial.println("Cynus BLE characteristic not usable.");
    chr = nullptr;
    bleClient->disconnect();
    return false;
  }

  Serial.println("Cynus BLE gateway connected.");
  rxLine.clear();
  resetConnectionState();

  // Tell Cynus we're driving it externally (King's own engine), not its
  // own onboard one -- matches CynusLink's proven connect handshake. Sent
  // first, right after the connection completes.
  sendCynus("set internal engine off\n");

  // "set illegal move check off" (see handleLine()'s "App version: V"
  // branch) only exists in app version 1.4.2+ -- ask for the version on
  // every connect so an older, not-yet-updated unit doesn't get sent a
  // command it doesn't understand. Reply format confirmed on real hardware
  // 2026-09-03: "App version: V1.4.2".
  sendCynus("get software version\n");

  // Matches CynusLink's requestBoardSync(BOARD_SYNC_STARTUP, 0), issued
  // right after the engine-off handshake: triggers the board's own startup
  // scan-for-start-position logic (scans, and forwards only if a valid
  // start position is found -- see handleStartupBoard()/nextStartupRescanAt
  // for the "wrong position -> rescan every 5s" half of this).
  initialScanAt = millis() + kInitialScanDelayMs;
  return true;
}

void cynusHandleLedFrame(const uint8_t frame167[167]) {
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
    if (hi < 0 || lo < 0) return;
    ledGrid[i] = static_cast<uint8_t>((hi << 4) | lo);
  }

  const bool cycleOk = moveCycle == MoveCycle::WaitEngineMove || moveCycle == MoveCycle::WaitFirstMove;
  if (syncState != SyncState::Ready || !cycleOk) {
    clearPendingLedMove("L outside engine wait");
    clearAlternatingPair("L outside engine wait");
    return;
  }

  int observedCount = 0;
  for (int i = 0; i < 81; ++i) if (ledGrid[i] != 0) ++observedCount;

  // Blink-off phase of Phoenix's own flash pattern -- not a state change,
  // ignore entirely so it doesn't reset the alternating-square tracking
  // (or the multi-square stability timer) on every single blink cycle.
  if (observedCount == 0) return;

  // 'L' + 2 slot digits + 162 LED hex digits = 165 bytes identify the LED
  // state itself; the trailing 2 checksum bytes don't add information.
  const std::string frameKey(reinterpret_cast<const char*>(frame167), 165);

  // Confirmed on real hardware 2026-08-29: this Mephisto Phoenix unit only
  // ever marks ONE square (4 lit corners) per frame -- never King's
  // simultaneous source+destination pair -- and alternates between the
  // move's two squares across successive frames. Route that case to the
  // alternating-pair handler instead of extractMoveFromLCommand(), which
  // requires >=6 lit corners and would just silently ignore every
  // single-square frame forever.
  if (observedCount == 4) {
    handleSingleSquareFrame();
    return;
  }
  clearAlternatingPair("LED pattern is no longer a single square");

  if (!pendingLedMove.empty() && pendingLedFrame != frameKey) {
    clearPendingLedMove("LED state changed before stabilization");
  }

  std::string uci;
  if (extractMoveFromLCommand(uci)) {
    armPendingLedMove(uci, frameKey);
  } else if (!pendingLedMove.empty() && pendingLedFrame == frameKey) {
    clearPendingLedMove("current LED state no longer yields a move candidate");
  }
}

void cynusClearLeds() {
  // No physical LEDs on Cynus itself to clear.
}

void cynusShowText(const char* text) {
  cynusDisplay(text);
}

void cynusPoll() {
  RawPacket packet;
  while (rxQueue != nullptr && xQueueReceive(rxQueue, &packet, 0) == pdTRUE) {
    for (uint8_t i = 0; i < packet.length; ++i) {
      const char c = static_cast<char>(packet.data[i]);
      if (c == '\n') {
        handleLine(rxLine);
        rxLine.clear();
      } else if (c != '\r') {
        rxLine += c;
        if (rxLine.size() > 256) rxLine.clear();  // runaway line, resync
      }
    }
  }

  processPendingLedMove();
  processCastlingTransition();
  processEnPassantTransition();

  if (initialScanAt != 0 &&
      static_cast<int32_t>(millis() - initialScanAt) >= 0) {
    initialScanAt = 0;
    Serial.println("[CYNUS] requesting initial board scan");
    sendCynus("scan board\n");
  }

  if (nextStartupRescanAt != 0 &&
      static_cast<int32_t>(millis() - nextStartupRescanAt) >= 0) {
    nextStartupRescanAt = 0;
    Serial.println("[CYNUS] automatic startup rescan");
    sendCynus("scan board\n");
  }

  if (experimentalMode == ExperimentalMode::FreeAnalysis && nextFreeAnalysisScanAt != 0 &&
      static_cast<int32_t>(millis() - nextFreeAnalysisScanAt) >= 0) {
    if (sendCynus("scan board\n")) {
      nextFreeAnalysisScanAt = millis() + kFreeAnalysisScanMs;
    } else {
      nextFreeAnalysisScanAt = millis() + 500;
    }
  }
}
