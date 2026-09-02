#include "pgn_recorder.h"

#include <LittleFS.h>

#include <cstring>

#include "usb_pgn_dump.h"

namespace {

// Forward declaration: finalizeGame() (defined well before the LittleFS
// storage section further down) calls this; the actual definition lives
// next to the rest of the LittleFS/rolling-window logic below.
void saveGameToLittleFs(const char* movesText, const char* result);

// ---------------------------------------------------------------------------
// Board representation for this file only: board[64], boardSquareIndex()'s
// own convention (a1=0, h8=63, file fastest-varying). Independent of the
// Mode-B wire layout (modeBStatusWireIndex()) -- converted once on entry.
// ---------------------------------------------------------------------------

constexpr char kStartBoard[64] =
    // a1..h1                  a2..h2                  a3..h3
    {'R','N','B','Q','K','B','N','R', 'P','P','P','P','P','P','P','P',
     '.','.','.','.','.','.','.','.', '.','.','.','.','.','.','.','.',
     '.','.','.','.','.','.','.','.', '.','.','.','.','.','.','.','.',
     'p','p','p','p','p','p','p','p', 'r','n','b','q','k','b','n','r'};

inline int squareFile(int sq) { return sq % 8; }
inline int squareRank(int sq) { return sq / 8; }  // 0-based (rank 1 == 0)
inline bool onBoard(int file, int rank) { return file >= 0 && file < 8 && rank >= 0 && rank < 8; }
inline int makeSquare(int file, int rank) { return rank * 8 + file; }
inline bool isWhitePiece(char p) { return p >= 'A' && p <= 'Z'; }
inline bool isBlackPiece(char p) { return p >= 'a' && p <= 'z'; }
inline bool isOwnPiece(char p, bool white) { return white ? isWhitePiece(p) : isBlackPiece(p); }
inline bool isEnemyPiece(char p, bool white) { return white ? isBlackPiece(p) : isWhitePiece(p); }
inline char toUpperPiece(char p) { return isBlackPiece(p) ? static_cast<char>(p - 32) : p; }

struct ChessMove {
  int from = -1;
  int to = -1;
  char promotion = 0;  // 'Q'/'R'/'B'/'N' (always uppercase), or 0
  bool isCastleK = false;
  bool isCastleQ = false;
  bool isEnPassant = false;
};

struct GameState {
  char board[64];
  bool whiteToMove = true;
  bool whiteCastleK = true, whiteCastleQ = true, blackCastleK = true, blackCastleQ = true;
  int enPassantSquare = -1;  // square a capturing pawn would land on, or -1
  int fullmoveNumber = 1;
};

void resetGameState(GameState& gs) {
  memcpy(gs.board, kStartBoard, sizeof(gs.board));
  gs.whiteToMove = true;
  gs.whiteCastleK = gs.whiteCastleQ = gs.blackCastleK = gs.blackCastleQ = true;
  gs.enPassantSquare = -1;
  gs.fullmoveNumber = 1;
}

// ---------------------------------------------------------------------------
// Attack detection and pseudo-legal move generation. Deliberately simple,
// array-based (no dynamic allocation) -- fine for an occasional per-move
// call, not a hot loop.
// ---------------------------------------------------------------------------

constexpr int kRookDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
constexpr int kBishopDirs[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
constexpr int kKnightOffsets[8][2] = {{1, 2}, {2, 1}, {2, -1}, {1, -2}, {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
constexpr int kKingOffsets[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};

bool squareAttacked(const char board[64], int square, bool byWhite) {
  const int file = squareFile(square), rank = squareRank(square);

  // Pawns: attacked from behind the attacker's own forward direction.
  const int pawnRank = byWhite ? rank - 1 : rank + 1;
  const char pawnChar = byWhite ? 'P' : 'p';
  for (int df : {-1, 1}) {
    if (onBoard(file + df, pawnRank) && board[makeSquare(file + df, pawnRank)] == pawnChar) return true;
  }

  for (auto& o : kKnightOffsets) {
    if (onBoard(file + o[0], rank + o[1])) {
      const char p = board[makeSquare(file + o[0], rank + o[1])];
      if (p == (byWhite ? 'N' : 'n')) return true;
    }
  }

  for (auto& o : kKingOffsets) {
    if (onBoard(file + o[0], rank + o[1])) {
      const char p = board[makeSquare(file + o[0], rank + o[1])];
      if (p == (byWhite ? 'K' : 'k')) return true;
    }
  }

  for (auto& d : kRookDirs) {
    int f = file + d[0], r = rank + d[1];
    while (onBoard(f, r)) {
      const char p = board[makeSquare(f, r)];
      if (p != '.') {
        if (p == (byWhite ? 'R' : 'r') || p == (byWhite ? 'Q' : 'q')) return true;
        break;
      }
      f += d[0];
      r += d[1];
    }
  }

  for (auto& d : kBishopDirs) {
    int f = file + d[0], r = rank + d[1];
    while (onBoard(f, r)) {
      const char p = board[makeSquare(f, r)];
      if (p != '.') {
        if (p == (byWhite ? 'B' : 'b') || p == (byWhite ? 'Q' : 'q')) return true;
        break;
      }
      f += d[0];
      r += d[1];
    }
  }

  return false;
}

int findKing(const char board[64], bool white) {
  const char k = white ? 'K' : 'k';
  for (int sq = 0; sq < 64; ++sq) {
    if (board[sq] == k) return sq;
  }
  return -1;
}

// Appends pseudo-legal moves (not yet filtered for leaving own king in
// check) for the piece on `from` to out[], starting at *count.
void generatePseudoMovesFrom(const GameState& gs, int from, ChessMove* out, int maxOut, int* count) {
  const char piece = gs.board[from];
  if (piece == '.') return;
  const bool white = isWhitePiece(piece);
  const int file = squareFile(from), rank = squareRank(from);
  const char upper = toUpperPiece(piece);

  auto tryAdd = [&](int to, char promotion = 0) {
    if (*count >= maxOut) return;
    ChessMove m;
    m.from = from;
    m.to = to;
    m.promotion = promotion;
    out[(*count)++] = m;
  };

  if (upper == 'P') {
    const int dir = white ? 1 : -1;
    const int startRank = white ? 1 : 6;   // 0-based
    const int lastRank = white ? 7 : 0;    // 0-based rank reached by promotion
    // Single push.
    if (onBoard(file, rank + dir) && gs.board[makeSquare(file, rank + dir)] == '.') {
      if (rank + dir == lastRank) {
        for (char promo : {'Q', 'R', 'B', 'N'}) tryAdd(makeSquare(file, rank + dir), promo);
      } else {
        tryAdd(makeSquare(file, rank + dir));
      }
      // Double push, only possible if the single push square was empty.
      if (rank == startRank && gs.board[makeSquare(file, rank + 2 * dir)] == '.') {
        tryAdd(makeSquare(file, rank + 2 * dir));
      }
    }
    // Captures (including en passant).
    for (int df : {-1, 1}) {
      if (!onBoard(file + df, rank + dir)) continue;
      const int to = makeSquare(file + df, rank + dir);
      if (isEnemyPiece(gs.board[to], white)) {
        if (rank + dir == lastRank) {
          for (char promo : {'Q', 'R', 'B', 'N'}) tryAdd(to, promo);
        } else {
          tryAdd(to);
        }
      } else if (to == gs.enPassantSquare) {
        ChessMove m;
        m.from = from;
        m.to = to;
        m.isEnPassant = true;
        if (*count < maxOut) out[(*count)++] = m;
      }
    }
  } else if (upper == 'N') {
    for (auto& o : kKnightOffsets) {
      if (onBoard(file + o[0], rank + o[1])) {
        const int to = makeSquare(file + o[0], rank + o[1]);
        if (!isOwnPiece(gs.board[to], white)) tryAdd(to);
      }
    }
  } else if (upper == 'K') {
    for (auto& o : kKingOffsets) {
      if (onBoard(file + o[0], rank + o[1])) {
        const int to = makeSquare(file + o[0], rank + o[1]);
        if (!isOwnPiece(gs.board[to], white)) tryAdd(to);
      }
    }
    // Castling: rights + empty-between + not-through-check, all required.
    const int homeRank = white ? 0 : 7;
    if (rank == homeRank && file == 4) {
      const bool canK = white ? gs.whiteCastleK : gs.blackCastleK;
      const bool canQ = white ? gs.whiteCastleQ : gs.blackCastleQ;
      if (canK && gs.board[makeSquare(5, homeRank)] == '.' && gs.board[makeSquare(6, homeRank)] == '.' &&
          !squareAttacked(gs.board, makeSquare(4, homeRank), !white) &&
          !squareAttacked(gs.board, makeSquare(5, homeRank), !white) &&
          !squareAttacked(gs.board, makeSquare(6, homeRank), !white)) {
        ChessMove m;
        m.from = from;
        m.to = makeSquare(6, homeRank);
        m.isCastleK = true;
        if (*count < maxOut) out[(*count)++] = m;
      }
      if (canQ && gs.board[makeSquare(3, homeRank)] == '.' && gs.board[makeSquare(2, homeRank)] == '.' &&
          gs.board[makeSquare(1, homeRank)] == '.' &&
          !squareAttacked(gs.board, makeSquare(4, homeRank), !white) &&
          !squareAttacked(gs.board, makeSquare(3, homeRank), !white) &&
          !squareAttacked(gs.board, makeSquare(2, homeRank), !white)) {
        ChessMove m;
        m.from = from;
        m.to = makeSquare(2, homeRank);
        m.isCastleQ = true;
        if (*count < maxOut) out[(*count)++] = m;
      }
    }
  } else {
    const bool sliding = (upper == 'R' || upper == 'B' || upper == 'Q');
    if (!sliding) return;
    auto walk = [&](const int (*dirs)[2], int n) {
      for (int i = 0; i < n; ++i) {
        int f = file + dirs[i][0], r = rank + dirs[i][1];
        while (onBoard(f, r)) {
          const int to = makeSquare(f, r);
          if (isOwnPiece(gs.board[to], white)) break;
          tryAdd(to);
          if (gs.board[to] != '.') break;  // captured enemy piece, ray stops
          f += dirs[i][0];
          r += dirs[i][1];
        }
      }
    };
    if (upper == 'R' || upper == 'Q') walk(kRookDirs, 4);
    if (upper == 'B' || upper == 'Q') walk(kBishopDirs, 4);
  }
}

// Applies a move (already known pseudo-legal) to gs's board, updating
// castling rights and en-passant target. Does not touch whiteToMove or
// fullmoveNumber -- caller's responsibility, since inference/legality
// checks sometimes need to try a move without committing turn order yet.
void applyMoveToBoard(GameState& gs, const ChessMove& m) {
  const char piece = gs.board[m.from];
  const bool white = isWhitePiece(piece);
  gs.board[m.from] = '.';

  if (m.isEnPassant) {
    const int capturedSquare = makeSquare(squareFile(m.to), squareRank(m.from));
    gs.board[capturedSquare] = '.';
    gs.board[m.to] = piece;
  } else if (m.isCastleK || m.isCastleQ) {
    const int homeRank = squareRank(m.from);
    gs.board[m.to] = piece;
    if (m.isCastleK) {
      gs.board[makeSquare(7, homeRank)] = '.';
      gs.board[makeSquare(5, homeRank)] = white ? 'R' : 'r';
    } else {
      gs.board[makeSquare(0, homeRank)] = '.';
      gs.board[makeSquare(3, homeRank)] = white ? 'R' : 'r';
    }
  } else if (m.promotion != 0) {
    gs.board[m.to] = white ? m.promotion : static_cast<char>(m.promotion + 32);
  } else {
    gs.board[m.to] = piece;
  }

  gs.enPassantSquare = -1;
  if (toUpperPiece(piece) == 'P' && !m.isEnPassant && abs(squareRank(m.to) - squareRank(m.from)) == 2) {
    gs.enPassantSquare = makeSquare(squareFile(m.from), (squareRank(m.from) + squareRank(m.to)) / 2);
  }

  if (toUpperPiece(piece) == 'K') {
    if (white) { gs.whiteCastleK = gs.whiteCastleQ = false; }
    else { gs.blackCastleK = gs.blackCastleQ = false; }
  }
  auto clearRookRight = [&](int square) {
    if (square == makeSquare(0, 0)) gs.whiteCastleQ = false;
    else if (square == makeSquare(7, 0)) gs.whiteCastleK = false;
    else if (square == makeSquare(0, 7)) gs.blackCastleQ = false;
    else if (square == makeSquare(7, 7)) gs.blackCastleK = false;
  };
  clearRookRight(m.from);
  clearRookRight(m.to);  // covers a rook being captured on its home square
}

// Legal moves for the side to move: pseudo-legal, minus any that leave that
// side's own king in check.
int generateLegalMoves(const GameState& gs, ChessMove* out, int maxOut) {
  int pseudoCount = 0;
  ChessMove pseudo[128];
  for (int sq = 0; sq < 64; ++sq) {
    if (gs.board[sq] == '.' || !isOwnPiece(gs.board[sq], gs.whiteToMove)) continue;
    generatePseudoMovesFrom(gs, sq, pseudo, 128, &pseudoCount);
  }
  int count = 0;
  for (int i = 0; i < pseudoCount; ++i) {
    GameState trial = gs;
    applyMoveToBoard(trial, pseudo[i]);
    if (!squareAttacked(trial.board, findKing(trial.board, gs.whiteToMove), !gs.whiteToMove)) {
      if (count < maxOut) out[count++] = pseudo[i];
    }
  }
  return count;
}

bool inCheck(const GameState& gs, bool white) {
  const int kingSq = findKing(gs.board, white);
  return kingSq >= 0 && squareAttacked(gs.board, kingSq, !white);
}

// ---------------------------------------------------------------------------
// Move inference: diff the current settled board against gs.board, classify
// the change as one of the known patterns, and confirm it against the set
// of actually-legal moves for the side to move. Returns false (and leaves
// outMove untouched) if the diff doesn't correspond to any single legal
// move -- caller treats that as "irregular", not as a move to record.
// ---------------------------------------------------------------------------

bool inferMove(const GameState& gs, const char newBoard[64], ChessMove& outMove) {
  int changed[8];
  int changedCount = 0;
  for (int sq = 0; sq < 64 && changedCount < 8; ++sq) {
    if (gs.board[sq] != newBoard[sq]) changed[changedCount++] = sq;
  }
  if (changedCount < 2 || changedCount > 4) return false;

  ChessMove candidate;
  bool haveCandidate = false;

  if (changedCount == 2) {
    int from = -1, to = -1;
    for (int i = 0; i < 2; ++i) {
      const int sq = changed[i];
      if (newBoard[sq] == '.' && gs.board[sq] != '.') from = sq;
      else to = sq;
    }
    if (from < 0 || to < 0) return false;
    candidate.from = from;
    candidate.to = to;
    const char movedTo = newBoard[to];
    const char movedFrom = gs.board[from];
    if (toUpperPiece(movedFrom) == 'P' && toUpperPiece(movedTo) != 'P') {
      candidate.promotion = toUpperPiece(movedTo);
    }
    haveCandidate = true;
  } else if (changedCount == 3) {
    // En passant: source empties, destination gets a pawn, a third square
    // (same file as destination, same rank as source) loses the captured
    // pawn. Source and captured-pawn square are told apart by ownership
    // (the mover's own piece vs. the opponent's), not by which one happens
    // to have the lower square index -- a capture toward a lower file/rank
    // (e.g. e5xd6, where d5 < e5) previously got the two swapped, since the
    // old code just took "first empty-with-content square found = from,
    // second = captured" in ascending square-index order regardless of
    // whose piece was on which. Found via a standalone offline test of this
    // exact en passant direction, 2026-09-01.
    int from = -1, to = -1, capturedSq = -1;
    for (int i = 0; i < 3; ++i) {
      const int sq = changed[i];
      if (gs.board[sq] != '.' && newBoard[sq] == '.') {
        if (isOwnPiece(gs.board[sq], gs.whiteToMove)) from = sq; else capturedSq = sq;
      } else if (newBoard[sq] != '.') {
        to = sq;
      }
    }
    if (from < 0 || to < 0 || capturedSq < 0) return false;
    if (toUpperPiece(gs.board[from]) != 'P') return false;
    if (squareFile(to) != squareFile(capturedSq) || squareRank(from) != squareRank(capturedSq)) return false;
    candidate.from = from;
    candidate.to = to;
    candidate.isEnPassant = true;
    haveCandidate = true;
  } else if (changedCount == 4) {
    // Castling: king moves 2 files on its home rank, rook jumps to the
    // square the king passed through.
    int kingFrom = -1, kingTo = -1;
    for (int i = 0; i < 4; ++i) {
      const int sq = changed[i];
      if (toUpperPiece(gs.board[sq]) == 'K' && newBoard[sq] == '.') kingFrom = sq;
      if (toUpperPiece(newBoard[sq]) == 'K' && gs.board[sq] == '.') kingTo = sq;
    }
    if (kingFrom < 0 || kingTo < 0) return false;
    if (squareRank(kingFrom) != squareRank(kingTo) || squareFile(kingFrom) != 4) return false;
    const int df = squareFile(kingTo) - squareFile(kingFrom);
    if (df == 2) { candidate.isCastleK = true; }
    else if (df == -2) { candidate.isCastleQ = true; }
    else return false;
    candidate.from = kingFrom;
    candidate.to = kingTo;
    haveCandidate = true;
  }

  if (!haveCandidate) return false;

  ChessMove legal[128];
  const int legalCount = generateLegalMoves(gs, legal, 128);
  for (int i = 0; i < legalCount; ++i) {
    if (legal[i].from == candidate.from && legal[i].to == candidate.to &&
        legal[i].isCastleK == candidate.isCastleK && legal[i].isCastleQ == candidate.isCastleQ &&
        legal[i].isEnPassant == candidate.isEnPassant &&
        (candidate.promotion == 0 || legal[i].promotion == candidate.promotion)) {
      outMove = legal[i];
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// SAN notation.
// ---------------------------------------------------------------------------

void squareToAlgebraic(int sq, char out[3]) {
  out[0] = static_cast<char>('a' + squareFile(sq));
  out[1] = static_cast<char>('1' + squareRank(sq));
  out[2] = 0;
}

// gsBefore is the position BEFORE the move; used for disambiguation and to
// know whether the destination was a capture.
void moveToSan(const GameState& gsBefore, const ChessMove& m, bool isCheck, bool isMate, char* out) {
  if (m.isCastleK) { strcpy(out, isMate ? "O-O#" : isCheck ? "O-O+" : "O-O"); return; }
  if (m.isCastleQ) { strcpy(out, isMate ? "O-O-O#" : isCheck ? "O-O-O+" : "O-O-O"); return; }

  const char piece = gsBefore.board[m.from];
  const char upper = toUpperPiece(piece);
  const bool isCapture = m.isEnPassant || gsBefore.board[m.to] != '.';
  char dest[3];
  squareToAlgebraic(m.to, dest);

  size_t pos = 0;
  if (upper == 'P') {
    if (isCapture) {
      out[pos++] = static_cast<char>('a' + squareFile(m.from));
      out[pos++] = 'x';
    }
    out[pos++] = dest[0];
    out[pos++] = dest[1];
    if (m.promotion != 0) {
      out[pos++] = '=';
      out[pos++] = m.promotion;
    }
  } else {
    out[pos++] = upper;

    // Disambiguation: any OTHER friendly piece of the same type that could
    // also legally reach m.to.
    ChessMove legal[128];
    const int legalCount = generateLegalMoves(gsBefore, legal, 128);
    bool needFile = false, needRank = false, ambiguous = false;
    for (int i = 0; i < legalCount; ++i) {
      if (legal[i].to != m.to || legal[i].from == m.from) continue;
      if (toUpperPiece(gsBefore.board[legal[i].from]) != upper) continue;
      ambiguous = true;
      if (squareFile(legal[i].from) == squareFile(m.from)) needRank = true;
      else needFile = true;
    }
    if (ambiguous) {
      if (!needFile && !needRank) needFile = true;  // same file & rank clash impossible; default to file
      if (needFile) out[pos++] = static_cast<char>('a' + squareFile(m.from));
      if (needRank) out[pos++] = static_cast<char>('1' + squareRank(m.from));
    }

    if (isCapture) out[pos++] = 'x';
    out[pos++] = dest[0];
    out[pos++] = dest[1];
  }

  if (isMate) out[pos++] = '#';
  else if (isCheck) out[pos++] = '+';
  out[pos] = 0;
}

// ---------------------------------------------------------------------------
// Recording state machine.
// ---------------------------------------------------------------------------

enum class RecorderState { WaitingForStart, Recording };
RecorderState recorderState = RecorderState::WaitingForStart;
bool loggedNotRecordingYet = false;
GameState game;

// Edge-trigger latch for the USB-dump "queen gesture" (see usb_pgn_dump.h)
// -- fires usbPgnDumpTrigger() once when the gesture is newly recognized,
// not on every settled frame for as long as the player leaves the extra
// queen sitting on c4.
bool queenGestureActive = false;

// Tolerate a run of unrecognized settled positions before giving up --
// the very FIRST unrecognized snapshot after an illegal-move attempt IS
// the illegal position itself (before the player has had any chance to
// take it back), so bailing out immediately on that one occurrence never
// gave the "board reverted -- ignore" recovery below anything to catch.
// User's own real-hardware report, 2026-09-01: a single illegal attempt
// that WAS cleanly reverted still lost the game, because we'd already
// dropped out of Recording on the illegal position's own snapshot.
int consecutiveUnrecognized = 0;
uint32_t firstUnrecognizedMs = 0;
constexpr int kMaxConsecutiveUnrecognized = 4;
constexpr uint32_t kUnrecognizedGiveUpMs = 20000;

constexpr size_t kPgnBufferSize = 8192;  // ~room for a long game's move text
char pgnMoves[kPgnBufferSize];
size_t pgnMovesLen = 0;
int plyCount = 0;  // half-moves recorded so far in the current game

// One raw Mode-B status frame captured per successfully-recorded move,
// alongside the SAN text -- this is the exact wire format Chess PGN
// Master's own file-download protocol turns out to expect (see
// pgnRecorderGameSnapshotPathByIndex()'s own comment in the header): a
// sequence of position snapshots, not the SAN/PGN text itself. 128 slots
// comfortably covers a real game (128 plies = a 64-move game per side).
constexpr int kMaxSnapshots = 128;
uint8_t snapshotBuffer[kMaxSnapshots][kModeBStatusFrameLength];
int snapshotCount = 0;

// A game that never reached a decisive/drawn result (i.e. was abandoned --
// the board went back to the starting position, or an unparseable change
// occurred) is only worth keeping if it went at least this many full moves;
// shorter ones are almost always a test/setup fiddle, not a real game (user's
// own words: "10 Zuege ohne Ergebnis sehen nach einem Test aus"). A real
// decisive result (checkmate or the king-gesture signal below) is always
// kept regardless of length -- the player explicitly declared it over.
constexpr int kMinPliesForInconclusiveGame = 20;  // 10 full moves

// Raw board polling / settle timer, mirroring chessnut_board.cpp's own
// settle pattern -- avoids reacting to a piece mid-lift.
uint8_t latestFrame[kModeBStatusFrameLength];
bool haveLatestFrame = false;
bool frameConsumed = true;
uint32_t lastFrameChangeMs = 0;
uint8_t settledFrame[kModeBStatusFrameLength];
bool haveSettledFrame = false;
constexpr uint32_t kSettleMs = 600;

void frameToBoard(const uint8_t frame[kModeBStatusFrameLength], char board[64]) {
  for (int file0 = 0; file0 < 8; ++file0) {
    for (int rank = 1; rank <= 8; ++rank) {
      board[boardSquareIndex(static_cast<char>('a' + file0), rank)] =
          static_cast<char>(frame[1 + modeBStatusWireIndex(file0, rank)]);
    }
  }
}

// Bare board-only FEN field (ranks 8..1, '/'-separated, empty runs as a
// single digit 1-8 -- a board row is at most 8 squares so multi-digit runs
// never occur) -- no turn/castling/en-passant/move-count fields. This is
// the exact format documented for Chessnut's own C SDK's
// cl_get_file_and_delete(): "a sequence of FEN strings, separated by ;",
// each example FEN in that doc (e.g. "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/
// RNBQKBNR") is exactly this bare board field, confirmed 2026-09-01 after
// two earlier wrong guesses (raw 38-byte board frames, then full PGN text)
// both produced no visible result in Chess PGN Master at all.
void boardToBareFen(const char board[64], char* out, size_t outSize) {
  size_t pos = 0;
  for (int rank = 7; rank >= 0; --rank) {
    int emptyRun = 0;
    for (int file = 0; file < 8; ++file) {
      const char piece = board[makeSquare(file, rank)];
      if (piece == '.') {
        ++emptyRun;
      } else {
        if (emptyRun > 0) {
          if (pos + 1 < outSize) out[pos++] = static_cast<char>('0' + emptyRun);
          emptyRun = 0;
        }
        if (pos + 1 < outSize) out[pos++] = piece;
      }
    }
    if (emptyRun > 0 && pos + 1 < outSize) out[pos++] = static_cast<char>('0' + emptyRun);
    if (rank > 0 && pos + 1 < outSize) out[pos++] = '/';
  }
  if (pos < outSize) out[pos] = '\0';
}

bool boardMatchesStartExcept(const char board[64], int exceptSquare, char exceptExpected) {
  for (int sq = 0; sq < 64; ++sq) {
    const char expected = (sq == exceptSquare) ? exceptExpected : kStartBoard[sq];
    if (board[sq] != expected) return false;
  }
  return true;
}

void finalizeGame(const char* result) {
  if (pgnMovesLen == 0) return;  // nothing recorded, nothing to save
  if (strcmp(result, "*") == 0 && plyCount < kMinPliesForInconclusiveGame) {
    Serial.printf("[PGN] game discarded: only %d ply/plies and no result -- looks like a test, "
                  "not a real game\r\n", plyCount);
    pgnMovesLen = 0;
    return;
  }
  saveGameToLittleFs(pgnMoves, result);
  pgnMovesLen = 0;
}

void beginNewGame() {
  resetGameState(game);
  pgnMovesLen = 0;
  plyCount = 0;
  snapshotCount = 0;
  consecutiveUnrecognized = 0;
  recorderState = RecorderState::Recording;
  loggedNotRecordingYet = false;
  Serial.println("[PGN] starting position recognized -- recording a new game");
}

// Manual mid-game result signal: both kings placed on one of three special
// square pairs, independent of everything else on the board (this is a
// deliberate player gesture, not a legal chess move -- inferMove() would
// never match it, so it must be checked before normal move inference gets a
// chance to reject it as "irregular"). Square-pair choice is the user's own
// design: d5+e4 (light squares) = white wins, d4+e5 (dark squares) = black
// wins, d4+d5 or e4+e5 (adjacent, mixed color) = draw. Doesn't matter which
// specific king sits on which square within a pair, only that the two kings
// together occupy exactly that pair.
const char* detectKingGestureResult(const char board[64]) {
  int whiteKingSq = -1, blackKingSq = -1;
  for (int sq = 0; sq < 64; ++sq) {
    if (board[sq] == 'K') whiteKingSq = sq;
    else if (board[sq] == 'k') blackKingSq = sq;
  }
  if (whiteKingSq < 0 || blackKingSq < 0) return nullptr;
  auto isPair = [&](int a, int b) {
    return (whiteKingSq == a && blackKingSq == b) || (whiteKingSq == b && blackKingSq == a);
  };
  const int d4 = boardSquareIndex('d', 4), d5 = boardSquareIndex('d', 5);
  const int e4 = boardSquareIndex('e', 4), e5 = boardSquareIndex('e', 5);
  if (isPair(d5, e4)) return "1-0";
  if (isPair(d4, e5)) return "0-1";
  if (isPair(d4, d5) || isPair(e4, e5)) return "1/2-1/2";
  return nullptr;
}

void appendMoveText(bool whiteMoved, int moveNumber, const char* san) {
  size_t pos = pgnMovesLen;
  const size_t remaining = kPgnBufferSize - pos;
  if (remaining < 16) return;  // out of room; drop silently rather than overflow
  int written;
  if (whiteMoved) {
    written = snprintf(pgnMoves + pos, remaining, "%d. %s ", moveNumber, san);
  } else {
    written = snprintf(pgnMoves + pos, remaining, "%s ", san);
  }
  if (written > 0) pgnMovesLen += static_cast<size_t>(written);
}

void processSettledBoard(const uint8_t frame[kModeBStatusFrameLength]) {
  char board[64];
  frameToBoard(frame, board);

  // USB-dump "queen gesture" -- checked before anything else (including the
  // "recognized as a new game" branch right below, which this board would
  // otherwise fail to match anyway since it has one extra piece) so it
  // works regardless of current recorder state. Deliberately does NOT
  // interact with recorderState/game at all -- purely a side-channel
  // trigger, same "passive observer" spirit as the rest of this file.
  const bool queenGestureNow =
      boardMatchesStartExcept(board, boardSquareIndex('c', 4), 'Q');
  if (queenGestureNow) {
    if (!queenGestureActive) {
      queenGestureActive = true;
      usbPgnDumpSetBlinking(true);
      Serial.println("[PGN] queen gesture recognized -- dumping saved games over USB");
      usbPgnDumpTrigger();
    }
    return;
  }
  if (queenGestureActive) usbPgnDumpSetBlinking(false);
  queenGestureActive = false;

  if (memcmp(board, kStartBoard, sizeof(board)) == 0) {
    if (recorderState == RecorderState::Recording && pgnMovesLen > 0) {
      finalizeGame("*");
    }
    beginNewGame();
    return;
  }

  if (recorderState != RecorderState::Recording) {
    // Log once per waiting period, not on every settled frame while idle
    // (chessnut_board.cpp-style debounce would spam this whenever the
    // player is mid-setup) -- but visible enough to diagnose "why did
    // nothing get recorded" without needing a full session's log.
    // loggedNotRecordingYet is reset back to false in beginNewGame(), so
    // this fires again the next time recording pauses.
    if (!loggedNotRecordingYet) {
      loggedNotRecordingYet = true;
      Serial.println("[PGN] board changed but not in a recognized starting position yet -- "
                      "not recording (waiting for a settled standard starting position first)");
    }
    return;
  }

  if (const char* gestureResult = detectKingGestureResult(board)) {
    Serial.printf("[PGN] king-gesture result signal: %s\r\n", gestureResult);
    finalizeGame(gestureResult);
    recorderState = RecorderState::WaitingForStart;
    return;
  }

  ChessMove move;
  if (!inferMove(game, board, move)) {
    // Board reverted back to exactly the last recorded position -- almost
    // certainly an illegal-move attempt that got corrected (e.g. a pinned
    // piece "capturing" into check, rejected by a cable chess computer,
    // then physically taken back), not a genuine desync. Nothing actually
    // changed net, so just keep recording from here instead of tearing
    // down the whole game -- user's own real-hardware report, 2026-09-01.
    if (memcmp(board, game.board, sizeof(game.board)) == 0) {
      Serial.println("[PGN] board reverted to the last recorded position -- ignoring "
                      "(likely an illegal move attempt that was corrected)");
      consecutiveUnrecognized = 0;
      return;
    }

    // Don't give up on the very first unrecognized snapshot -- that one IS
    // typically the illegal position itself, seen BEFORE the player has had
    // any chance to take it back. Giving up immediately here meant the
    // "reverted -- ignore" check above never got a second snapshot to
    // compare against, since we'd already dropped out of Recording. Instead
    // tolerate a short run of unrecognized snapshots (or a short time
    // window) and keep `game` unchanged, so a revert on any of the next few
    // settled frames still gets caught above. Only truly give up once that
    // patience runs out. User's own real-hardware report, 2026-09-01: a
    // clean single illegal-attempt-then-revert still lost the game before
    // this fix, because the illegal position's own snapshot already
    // triggered the old immediate bail-out below.
    if (consecutiveUnrecognized == 0) firstUnrecognizedMs = millis();
    ++consecutiveUnrecognized;
    if (consecutiveUnrecognized < kMaxConsecutiveUnrecognized &&
        static_cast<uint32_t>(millis() - firstUnrecognizedMs) < kUnrecognizedGiveUpMs) {
      Serial.printf("[PGN] unrecognized board change (%d in a row) -- waiting to see if it "
                    "resolves before giving up\r\n", consecutiveUnrecognized);
      return;
    }

    Serial.println("[PGN] unrecognized board change persisted -- saving partial game and "
                    "pausing until starting position is seen again");
    finalizeGame("*");
    recorderState = RecorderState::WaitingForStart;
    consecutiveUnrecognized = 0;
    return;
  }
  consecutiveUnrecognized = 0;

  // Captured before mutating game -- SAN disambiguation/capture-detection
  // need the position as it was immediately before this move.
  const GameState preMove = game;
  const bool whiteMoved = game.whiteToMove;
  const int moveNumber = game.fullmoveNumber;

  applyMoveToBoard(game, move);
  game.whiteToMove = !whiteMoved;
  if (!whiteMoved) ++game.fullmoveNumber;  // black just moved -> full move complete

  const bool giveCheck = inCheck(game, game.whiteToMove);
  ChessMove replies[128];
  const bool isMate = giveCheck && generateLegalMoves(game, replies, 128) == 0;

  char san[16];
  moveToSan(preMove, move, giveCheck, isMate, san);
  appendMoveText(whiteMoved, moveNumber, san);
  ++plyCount;
  if (snapshotCount < kMaxSnapshots) {
    memcpy(snapshotBuffer[snapshotCount], frame, kModeBStatusFrameLength);
    ++snapshotCount;
  }
  Serial.printf("[PGN] move: %s\r\n", san);

  if (isMate) {
    finalizeGame(whiteMoved ? "1-0" : "0-1");
    recorderState = RecorderState::WaitingForStart;
  }
}

// ---------------------------------------------------------------------------
// LittleFS storage: rolling window of the 10 most recent games.
// ---------------------------------------------------------------------------

constexpr const char* kGamesDir = "/games";
// 20 slots, round-robin, no deletion -- per the user's explicit instruction
// 2026-09-01 ("20 Partien alle im FEN format Round robbery ohne löschen").
// nextGameNumber is now the slot to write NEXT (1..kMaxSavedGames, wrapping),
// not an ever-incrementing game count -- saving simply overwrites
// game_<slot>.pgn/.snap via LittleFS.open(path, "w") (which truncates any
// existing content on its own), so there is no separate prune/delete step
// at all once the 20 slots have all been used once.
constexpr int kMaxSavedGames = 20;
constexpr const char* kSlotCursorFile = "/games/.slot";
int nextGameNumber = 1;
bool littleFsReady = false;

// Inverse of frameToBoard() above -- builds a synthetic Mode-B status frame
// from a reconstructed board, for regenerateSnapshotForGame()'s backfill
// use (there's no real incoming frame to reuse when replaying an old game).
void boardToFrame(const char board[64], uint8_t frame[kModeBStatusFrameLength]) {
  frame[0] = 's';
  for (int file0 = 0; file0 < 8; ++file0) {
    for (int rank = 1; rank <= 8; ++rank) {
      frame[1 + modeBStatusWireIndex(file0, rank)] =
          static_cast<uint8_t>(board[boardSquareIndex(static_cast<char>('a' + file0), rank)]);
    }
  }
  computeModeBChecksumHex(frame + 65, frame, 65, /*useEncodedConvention=*/false);
}

// Backfill for a .pgn saved before position-snapshot capture existed (or
// whose .snap is otherwise missing) -- replays its own saved SAN move text
// back through this file's own chess engine to reconstruct the position
// after each move, without needing a separate SAN grammar parser: at each
// step, every actually-legal move's own SAN is computed via moveToSan() (the
// same function real-time recording already uses) and compared against the
// next token in the saved text, reusing the encoder as a decoder. Best-
// effort -- stops early (keeping whatever was reconstructed so far) if a
// token can't be matched, e.g. because of hand-edited or malformed text.
void regenerateSnapshotForGame(int gameNumber) {
  char pgnPath[48];
  snprintf(pgnPath, sizeof(pgnPath), "%s/game_%d.pgn", kGamesDir, gameNumber);
  File f = LittleFS.open(pgnPath, "r");
  if (!f) return;
  String moveText;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.length() > 0 && line[0] == '[') continue;  // skip PGN headers
    moveText += line;
    moveText += ' ';
  }
  f.close();

  GameState replay;
  resetGameState(replay);
  static uint8_t snapshots[kMaxSnapshots][kModeBStatusFrameLength];
  int count = 0;

  const int len = moveText.length();
  int pos = 0;
  while (pos < len && count < kMaxSnapshots) {
    while (pos < len && isspace(static_cast<unsigned char>(moveText[pos]))) ++pos;
    const int start = pos;
    while (pos < len && !isspace(static_cast<unsigned char>(moveText[pos]))) ++pos;
    if (pos == start) break;
    String token = moveText.substring(start, pos);
    if (token.length() == 0) continue;
    // Skip move-number tokens ("18.") and the trailing result token.
    if (isdigit(static_cast<unsigned char>(token[0])) && token.indexOf('.') >= 0) continue;
    if (token == "1-0" || token == "0-1" || token == "1/2-1/2" || token == "*") continue;

    ChessMove legal[128];
    const int legalCount = generateLegalMoves(replay, legal, 128);
    const bool whiteMoved = replay.whiteToMove;
    bool matched = false;
    for (int i = 0; i < legalCount; ++i) {
      GameState trial = replay;
      applyMoveToBoard(trial, legal[i]);
      trial.whiteToMove = !whiteMoved;
      if (!whiteMoved) ++trial.fullmoveNumber;
      const bool giveCheck = inCheck(trial, trial.whiteToMove);
      ChessMove replies[128];
      const bool isMate = giveCheck && generateLegalMoves(trial, replies, 128) == 0;
      char san[16];
      moveToSan(replay, legal[i], giveCheck, isMate, san);
      if (token == san) {
        replay = trial;
        boardToFrame(replay.board, snapshots[count]);
        ++count;
        matched = true;
        break;
      }
    }
    if (!matched) {
      Serial.printf("[PGN] backfill: could not match token \"%s\" in game_%d -- stopping early\r\n",
                    token.c_str(), gameNumber);
      break;
    }
  }

  char snapPath[48];
  snprintf(snapPath, sizeof(snapPath), "%s/game_%d.snap", kGamesDir, gameNumber);
  File snapFile = LittleFS.open(snapPath, "w");
  if (snapFile) {
    if (count > 0) {
      snapFile.write(reinterpret_cast<const uint8_t*>(snapshots),
                     static_cast<size_t>(count) * kModeBStatusFrameLength);
    }
    snapFile.close();
  }
  Serial.printf("[PGN] backfilled %d position snapshot(s) for game_%d\r\n", count, gameNumber);
}

// sscanf(name, "game_%d.pgn", &n) == 1 alone is NOT a reliable "does this
// filename end in .pgn" check -- sscanf's return value only counts
// successful %d conversions, so it stays 1 even when the trailing literal
// ".pgn" fails to match. E.g. for "game_1.snap": "game_" matches, %d
// converts 1, the literal "." also matches -- but then format "pgn" vs
// input "snap" mismatches, and sscanf simply stops there without undoing
// the already-completed %d conversion, so it still returns 1. Real bug
// found 2026-09-01: once .snap companion files existed alongside .pgn files
// sharing the same "game_N." prefix, this made every count/index function
// using the naive check double-count each game. %n only fires once
// everything before it in the format string has matched, so comparing it
// against the full filename length catches the trailing-mismatch case.
bool isPgnGameFile(const char* name, int* outNumber) {
  int n = 0;
  int consumed = -1;
  sscanf(name, "game_%d.pgn%n", &n, &consumed);
  if (consumed != static_cast<int>(strlen(name))) return false;
  *outNumber = n;
  return true;
}

// Persists nextGameNumber (the slot to write next) across reboots -- a
// single small text file, not derived from scanning game_N.pgn filenames,
// since after round-robin wraparound the existing slot NUMBERS no longer
// indicate write order (e.g. slot 1 can hold the newest game and slot 20
// the oldest) the way an ever-incrementing scheme's filenames used to.
void saveSlotCursor() {
  File f = LittleFS.open(kSlotCursorFile, "w");
  if (!f) return;
  f.printf("%d", nextGameNumber);
  f.close();
}

// Loads the persisted cursor; falls back to scanning existing game_N.pgn
// files (highest N + 1, capped into range) if the cursor file doesn't
// exist yet -- covers the very first boot after this round-robin scheme
// was introduced, when only files from the old ever-incrementing scheme
// exist on disk (e.g. this project's own pre-existing game_1.pgn).
void loadSlotCursor() {
  nextGameNumber = 1;
  File cursorFile = LittleFS.open(kSlotCursorFile, "r");
  if (cursorFile) {
    const int n = cursorFile.parseInt();
    cursorFile.close();
    if (n >= 1 && n <= kMaxSavedGames) {
      nextGameNumber = n;
      return;
    }
  }
  File dir = LittleFS.open(kGamesDir);
  if (!dir) return;
  int highest = 0;
  File entry = dir.openNextFile();
  while (entry) {
    int n = 0;
    if (isPgnGameFile(entry.name(), &n) && n > highest) highest = n;
    entry = dir.openNextFile();
  }
  dir.close();
  nextGameNumber = (highest % kMaxSavedGames) + 1;
}

void saveGameToLittleFs(const char* movesText, const char* result) {
  if (!littleFsReady) return;
  char path[48];
  snprintf(path, sizeof(path), "%s/game_%d.pgn", kGamesDir, nextGameNumber);
  File f = LittleFS.open(path, "w");
  if (!f) {
    Serial.printf("[PGN] failed to open %s for writing\r\n", path);
    return;
  }
  f.print("[Event \"BluetoothMax\"]\r\n[Site \"?\"]\r\n[Date \"????.??.??\"]\r\n"
          "[Round \"?\"]\r\n[White \"?\"]\r\n[Black \"?\"]\r\n");
  f.printf("[Result \"%s\"]\r\n\r\n", result);
  f.print(movesText);
  f.print(result);
  f.print("\r\n");
  f.close();

  // Companion position-snapshot file -- see snapshotBuffer's own comment.
  // Written even for a game with 0 captured snapshots (e.g. the king-gesture
  // result signal fires before any move was recorded) as an empty file, so
  // pgnRecorderGameSnapshotPathByIndex() always finds a file matching every
  // .pgn that pgnRecorderSavedGameCount() counts.
  snprintf(path, sizeof(path), "%s/game_%d.snap", kGamesDir, nextGameNumber);
  File snapFile = LittleFS.open(path, "w");
  if (snapFile) {
    if (snapshotCount > 0) {
      snapFile.write(reinterpret_cast<const uint8_t*>(snapshotBuffer),
                     static_cast<size_t>(snapshotCount) * kModeBStatusFrameLength);
    }
    snapFile.close();
  }

  Serial.printf("[PGN] game saved: %s (result %s, %d position snapshot(s))\r\n", path, result,
                snapshotCount);
  // Round-robin: wrap back to slot 1 after kMaxSavedGames, no deletion --
  // the next save's own LittleFS.open(path, "w") overwrites whatever was
  // in that slot directly.
  nextGameNumber = (nextGameNumber % kMaxSavedGames) + 1;
  saveSlotCursor();
}

}  // namespace

// Retrieval: decided 2026-09-01 to be Chess PGN Master connecting over the
// existing BT-BT Chessnut ("Chessnut Air") masquerade -- chessnut_server.cpp
// calls pgnRecorderSavedGameCount() below to answer its "how many saved
// games?" query truthfully instead of the old hardcoded 0, which is what
// makes PGN Master's own download button appear at all. An earlier WiFi AP +
// captive-portal download page was built and confirmed working here, but
// cost ~555KB of flash (WiFi/WebServer/DNSServer stack), pushing this app
// partition from 43.8% to 86.1% used -- rejected as too expensive. Its
// boardMatchesStartExcept() gesture helper is kept (currently unused) in
// case a c4-style gesture is ever needed again for something else.

void pgnRecorderInit() {
  littleFsReady = LittleFS.begin(/*formatOnFail=*/true);
  if (!littleFsReady) {
    Serial.println("[PGN] LittleFS mount failed -- game recording disabled");
    return;
  }
  if (!LittleFS.exists(kGamesDir)) LittleFS.mkdir(kGamesDir);
  File dir = LittleFS.open(kGamesDir);
  if (dir) {
    File entry = dir.openNextFile();
    while (entry) {
      // Temporary diagnostic: list every raw directory entry exactly once
      // at boot, to check for duplicate/unexpected entries -- chasing a
      // real discrepancy where pgnRecorderGameSnapshotPathByIndex() reports
      // 2 games but both resolve to the same game_1.snap, 2026-09-01.
      Serial.printf("[PGN] directory entry: \"%s\" (%u bytes)\r\n", entry.name(),
                    static_cast<unsigned>(entry.size()));
      entry = dir.openNextFile();
    }
    dir.close();
  }
  loadSlotCursor();
  Serial.printf("[PGN] LittleFS ready, next slot %d/%d\r\n", nextGameNumber, kMaxSavedGames);
  Serial.printf("[PGN] %d saved game(s) found in %s\r\n", pgnRecorderSavedGameCount(), kGamesDir);

  // Backfill: a .pgn saved before position-snapshot capture existed (e.g.
  // game_1 from earlier this session, 2026-09-01) has no matching .snap,
  // which made it download as empty content via Chess PGN Master. Rebuild
  // it once at boot by replaying its own saved move text -- see
  // regenerateSnapshotForGame()'s own comment. Runs once per boot, only for
  // files that need it (an existing non-empty .snap is left alone).
  {
    File backfillDir = LittleFS.open(kGamesDir);
    if (backfillDir) {
      File entry = backfillDir.openNextFile();
      while (entry) {
        int n = 0;
        if (isPgnGameFile(entry.name(), &n)) {
          char snapPath[48];
          snprintf(snapPath, sizeof(snapPath), "%s/game_%d.snap", kGamesDir, n);
          const bool needsBackfill =
              !LittleFS.exists(snapPath) || LittleFS.open(snapPath, "r").size() == 0;
          if (needsBackfill) regenerateSnapshotForGame(n);
        }
        entry = backfillDir.openNextFile();
      }
      backfillDir.close();
    }
  }

  // Debug-dump every saved game's actual file content to Serial at boot --
  // requested directly by the user to verify what actually landed on
  // LittleFS, not just that a file exists. Temporary/diagnostic, not part
  // of the retrieval mechanism itself.
  File dumpDir = LittleFS.open(kGamesDir);
  if (dumpDir) {
    File dumpEntry = dumpDir.openNextFile();
    while (dumpEntry) {
      Serial.printf("[PGN] --- content of %s (%u bytes) ---\r\n", dumpEntry.name(),
                    static_cast<unsigned>(dumpEntry.size()));
      while (dumpEntry.available()) Serial.write(dumpEntry.read());
      Serial.println();
      Serial.println("[PGN] --- end ---");
      dumpEntry = dumpDir.openNextFile();
    }
    dumpDir.close();
  }
}

void pgnRecorderOnBoardStatus(const uint8_t frame[kModeBStatusFrameLength]) {
  memcpy(latestFrame, frame, kModeBStatusFrameLength);
  haveLatestFrame = true;
  frameConsumed = false;
  lastFrameChangeMs = millis();
}

void pgnRecorderPoll() {
  if (!littleFsReady) return;

  usbPgnDumpBlinkPoll();
  usbPgnDumpPoll();

  if (haveLatestFrame && !frameConsumed &&
      static_cast<uint32_t>(millis() - lastFrameChangeMs) >= kSettleMs) {
    frameConsumed = true;
    if (!haveSettledFrame || memcmp(settledFrame, latestFrame, kModeBStatusFrameLength) != 0) {
      memcpy(settledFrame, latestFrame, kModeBStatusFrameLength);
      haveSettledFrame = true;
      processSettledBoard(settledFrame);
    }
  }

  // Retrieval itself is passive from this module's point of view -- see the
  // comment above pgnRecorderInit(): chessnut_server.cpp calls
  // pgnRecorderSavedGameCount() and, eventually, a per-game read function
  // directly, driven by whatever PGN Master sends, not by anything polled
  // here.
}

int pgnRecorderSavedGameCount() {
  if (!littleFsReady) return 0;
  File dir = LittleFS.open(kGamesDir);
  if (!dir) return 0;
  int count = 0;
  File entry = dir.openNextFile();
  while (entry) {
    // Only count the .pgn files -- each game also has a companion .snap
    // file now (position snapshots for the retrieval protocol), which
    // would otherwise double this count.
    int n = 0;
    if (isPgnGameFile(entry.name(), &n)) ++count;
    entry = dir.openNextFile();
  }
  dir.close();
  return count;
}

// Chronological (oldest-first) slot lookup for round-robin storage: unlike
// the old ever-incrementing scheme, a slot's own NUMBER no longer indicates
// write order once wraparound has happened (slot 1 can hold the newest
// game, slot kMaxSavedGames the oldest) -- nextGameNumber (the slot about
// to be overwritten next) is the one reliable anchor: it's either about to
// replace the single oldest surviving game, or -- before the slots have
// ever wrapped -- points at the next EMPTY slot, in which case walking
// forward from it just never finds an existing file until wrapping back to
// slot 1, which is exactly the true oldest game in that case too.
// Real bug found 2026-09-02: the original version of this function probed
// each of the kMaxSavedGames possible slots individually via
// LittleFS.exists(game_N.pgn), one snprintf+exists() call per slot. Every
// miss (a slot with no file, the common case once only a few games exist
// out of 20 possible) makes the underlying ESP-IDF VFS layer log its own
// "open(): ... does not exist" line -- up to 19 of them, each its own
// blocking Serial write -- for a single lookup. That's exactly the kind of
// loop()-stalling work usbPgnDumpTrigger()'s own fix (chunked writes, short
// TX timeout) was trying to avoid, and very likely a real contributor to
// the "real board resets during a dump" symptom this was found while
// chasing. Fixed by listing the directory ONCE (same pattern
// pgnRecorderSavedGameCount() already uses) into a small existence table,
// then computing chronological order purely in memory -- no further
// filesystem probing per slot at all.
bool chronologicalSlotByIndex(int index, int* outSlot) {
  if (!littleFsReady || index < 0) return false;
  bool slotExists[kMaxSavedGames + 1] = {};
  File dir = LittleFS.open(kGamesDir);
  if (!dir) return false;
  File entry = dir.openNextFile();
  while (entry) {
    int n = 0;
    if (isPgnGameFile(entry.name(), &n) && n >= 1 && n <= kMaxSavedGames) slotExists[n] = true;
    entry = dir.openNextFile();
  }
  dir.close();

  int found = -1;
  int seen = 0;
  for (int offset = 0; offset < kMaxSavedGames; ++offset) {
    const int slot = ((nextGameNumber - 1 + offset) % kMaxSavedGames) + 1;
    if (!slotExists[slot]) continue;
    if (seen == index) {
      found = slot;
      break;
    }
    ++seen;
  }
  if (found < 0) return false;
  *outSlot = found;
  return true;
}

bool pgnRecorderGameSnapshotPathByIndex(int index, char* outPath, size_t outPathSize) {
  int slot = 0;
  if (!chronologicalSlotByIndex(index, &slot)) return false;
  snprintf(outPath, outPathSize, "%s/game_%d.snap", kGamesDir, slot);
  return true;
}

bool pgnRecorderGamePgnPathByIndex(int index, char* outPath, size_t outPathSize) {
  int slot = 0;
  if (!chronologicalSlotByIndex(index, &slot)) return false;
  snprintf(outPath, outPathSize, "%s/game_%d.pgn", kGamesDir, slot);
  return true;
}

bool pgnRecorderDeleteGameByIndex(int index) {
  int slot = 0;
  if (!chronologicalSlotByIndex(index, &slot)) return false;
  char path[48];
  snprintf(path, sizeof(path), "%s/game_%d.pgn", kGamesDir, slot);
  const bool pgnRemoved = LittleFS.remove(path);
  snprintf(path, sizeof(path), "%s/game_%d.snap", kGamesDir, slot);
  LittleFS.remove(path);
  if (pgnRemoved) {
    Serial.printf("[PGN] deleted game_%d (slot freed after confirmed USB transfer)\r\n", slot);
  }
  return pgnRemoved;
}

size_t pgnRecorderGameFenSequenceByIndex(int index, char* outBuffer, size_t outBufferSize) {
  if (outBufferSize == 0) return 0;
  outBuffer[0] = '\0';
  char snapPath[48];
  if (!pgnRecorderGameSnapshotPathByIndex(index, snapPath, sizeof(snapPath))) return 0;
  File snapFile = LittleFS.open(snapPath, "r");
  if (!snapFile) return 0;

  size_t pos = 0;
  auto appendFen = [&](const char boardToAppend[64]) {
    if (pos > 0 && pos + 1 < outBufferSize) outBuffer[pos++] = ';';
    char fen[72];
    boardToBareFen(boardToAppend, fen, sizeof(fen));
    size_t fenLen = strlen(fen);
    if (pos + fenLen >= outBufferSize) fenLen = outBufferSize - pos - 1;
    memcpy(outBuffer + pos, fen, fenLen);
    pos += fenLen;
  };

  // The .snap file only holds positions *after* each recorded move -- the
  // starting position itself, which the real protocol's own documented
  // example always includes as the first FEN, is never stored there.
  appendFen(kStartBoard);

  uint8_t modeBFrame[kModeBStatusFrameLength];
  while (pos + 1 < outBufferSize &&
         snapFile.available() >= static_cast<int>(kModeBStatusFrameLength)) {
    snapFile.read(modeBFrame, kModeBStatusFrameLength);
    char board[64];
    frameToBoard(modeBFrame, board);
    appendFen(board);
  }
  snapFile.close();

  outBuffer[pos < outBufferSize ? pos : outBufferSize - 1] = '\0';
  return pos;
}
