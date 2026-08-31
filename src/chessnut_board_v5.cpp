#include "chessnut_board_v5.h"

const char kChessnutBoardName[] = "chessnut";
const char kChessnutBoardNameAlt[] = "smart chess";

bool phoenixHasActiveLedSuggestion = false;

namespace {

constexpr char kWriteUuid[] = "1b7e8272-2877-41c3-b46e-cf057c562023";
constexpr char kReadMiscUuid[] = "1b7e8273-2877-41c3-b46e-cf057c562023";
constexpr char kReadBoardUuid[] = "1b7e8262-2877-41c3-b46e-cf057c562023";
constexpr size_t kMaxNotifyPayload = 48;
constexpr uint32_t kConnectTimeoutMs = 8000;

struct RawPacket {
  uint8_t length;
  uint8_t data[kMaxNotifyPayload];
};

NimBLEClient* bleClient = nullptr;
NimBLERemoteCharacteristic* writeChar = nullptr;
NimBLERemoteCharacteristic* miscChar = nullptr;
NimBLERemoteCharacteristic* boardChar = nullptr;
QueueHandle_t boardDataQueue = nullptr;
QueueHandle_t miscDataQueue = nullptr;

uint8_t lastRawBoardData[32] = {};
bool haveLastRawBoardData = false;

// Local "show whatever currently differs from the last settled position"
// LED fallback -- added 2026-08-31 at the user's explicit direction, after
// finding Chessnut has no such feature of its own (unlike, per the user's
// direct observation, a real Millennium/Supreme T2 board, which circles
// every lifted piece with its own local LEDs regardless of what Phoenix's
// 'L' frame currently contains). Purely a physical-board display aid for
// the player -- does NOT touch anything sent to Phoenix over the cable.
//
// Priority note (confirmed on real hardware 2026-08-31): this used to defer
// to Phoenix's own last-decoded 'L' frame whenever that had something to
// show. That's backwards -- during a real capture, Phoenix can still be
// blinking a stale suggestion from before the capture started, and that
// silently ate the "piece missing" indicator during exactly the moment it
// mattered most. A real physical discrepancy must always win; see
// chessnutHasLocalDeviation() in board_driver.cpp's dispatchLedFrameToBoard().
// Steady/on, never blinking -- Chessnut's own LED command has no time-based
// flash pattern support (unlike Mode-B's 'L' slot-time field).
char currentBoard[64] = {};
char settledBoard[64] = {};
bool haveSettledBoard = false;
uint32_t lastBoardChangeMs = 0;
bool ledSettlePending = false;
// A short debounce, not a multi-second one -- this is a real-time board,
// per the user's explicit correction. 500ms is enough to bridge a piece
// being physically slid across adjacent squares (each square's sensor
// triggering its own brief transient along the path) without accumulating
// a whole multi-step move; it is NOT meant to hold captures together the
// way earlier (reverted) attempts at delaying the actual cable status did.
constexpr uint32_t kLedSettleMs = 500;

void sendCableStatusFrame() {
  uint8_t frame[kModeBStatusFrameLength] = {};
  frame[0] = 's';
  memcpy(frame + 1, currentBoard, 64);
  // Plain checksum, not cableHostUsesEncodedChecksum -- found 2026-08-31 by
  // reading Elfacun/Diablillo's own proven-working source directly
  // (github.com/inmbolmie/elfacun, board_b.cpp): a real/reference Mode-B
  // board ALWAYS computes its own outgoing checksum over the plain 7-bit
  // content, then applies odd-parity encoding to the whole frame
  // (including the checksum digits) as a separate final step -- it never
  // computes the checksum over already-parity-encoded bytes, regardless of
  // what convention the connected host's own frames validate under.
  // cableHostUsesEncodedChecksum only describes how to interpret Phoenix's
  // OWN incoming frames (modeBValidBlock()'s dual check); it was being
  // reused here to select the outgoing convention too, which is wrong.
  // This exactly explains the 16/32-piece mirror-symmetric registration
  // pattern found this session: '.' and most pieces need their own parity
  // bit for odd-parity encoding, but the queen and rook characters happen
  // not to (verified by hand for every piece letter) -- computing the
  // checksum over the already-encoded bytes silently flips the checksum's
  // own top bit exactly when a queen/rook-vs-everything-else transition
  // changes the frame's total parity-bit count by one, producing a
  // checksum Phoenix correctly rejects as corrupt. A paired transition
  // (a normal move) always nets zero change here, which is also exactly
  // why two-square updates never showed this symptom.
  computeModeBChecksumHex(frame + 65, frame, 65, /*useEncodedConvention=*/false);
  onBoardStatusFrame(frame);
}

size_t collectLocalDeviations(SquareHighlight* deviations, size_t maxOut) {
  size_t count = 0;
  for (int wireIndex = 0; wireIndex < 64 && count < maxOut; ++wireIndex) {
    if (currentBoard[wireIndex] == settledBoard[wireIndex]) continue;
    const int rank = wireIndex / 8 + 1;
    const int file0 = 7 - (wireIndex % 8);
    deviations[count].squareIndex =
        static_cast<uint8_t>(boardSquareIndex(static_cast<char>('a' + file0), rank));
    deviations[count].role = SquareHighlightRole::Generic;
    ++count;
  }
  return count;
}

void showLocalBoardDeviations() {
  SquareHighlight deviations[32];
  const size_t count = collectLocalDeviations(deviations, 32);
  chessnutSetHighlightedSquares(count > 0 ? deviations : nullptr, count);
}

// Chessnut's own piece codes: empty square = 0, white uppercase, black
// lowercase. Confirmed against two independent reverse-engineering projects.
char pieceFromChessnutCode(uint8_t code) {
  static constexpr char kLut[] = {
      '.', 'q', 'k', 'b', 'p', 'n', 'R', 'P', 'r', 'B', 'N', 'Q', 'K'};
  return (code < sizeof(kLut)) ? kLut[code] : '.';
}

// Counts BLE board-data notifications lost because boardDataQueue was full
// when they arrived -- added 2026-08-31 to test the theory that "sometimes
// yes, sometimes no" status updates for a given square (confirmed on real
// hardware not to be a Chessnut sensor problem -- the user's own words:
// "das Board funktioniert mit allen 100%") are actually being silently
// dropped here rather than lost upstream. xQueueSend()'s return value was
// previously discarded entirely, so a full queue failed with zero trace.
uint32_t droppedBoardPackets = 0;

void pushToQueue(QueueHandle_t queue, uint8_t* data, size_t length) {
  if (queue == nullptr || length == 0) return;
  RawPacket packet{};
  packet.length = static_cast<uint8_t>(min(length, kMaxNotifyPayload));
  memcpy(packet.data, data, packet.length);
  if (xQueueSend(queue, &packet, 0) != pdTRUE && queue == boardDataQueue) {
    ++droppedBoardPackets;
  }
}

void onBoardNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
  pushToQueue(boardDataQueue, data, length);
}

void onMiscNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
  pushToQueue(miscDataQueue, data, length);
}

class ClientCallbacks final : public NimBLEClientCallbacks {
 public:
  void onConnect(NimBLEClient*) override {}
  void onDisconnect(NimBLEClient*, int) override {
    writeChar = nullptr;
    miscChar = nullptr;
    boardChar = nullptr;
    haveLastRawBoardData = false;
    Serial.println("BLE disconnected; reconnecting automatically.");
  }
};

NimBLERemoteCharacteristic* findCharacteristicInAnyService(NimBLEClient* client, const char* uuid) {
  for (NimBLERemoteService* service : client->getServices()) {
    NimBLERemoteCharacteristic* characteristic = service->getCharacteristic(uuid);
    if (characteristic != nullptr) return characteristic;
  }
  return nullptr;
}

void handleBoardDataPacket(const uint8_t* data, size_t length) {
  if (length < 34 || data[0] != 0x01 || data[1] != 0x24) return;
  const uint8_t* raw = data + 2;  // 32 bytes, 2 squares per byte
  if (haveLastRawBoardData && memcmp(lastRawBoardData, raw, 32) == 0) return;
  memcpy(lastRawBoardData, raw, 32);
  haveLastRawBoardData = true;

  memset(currentBoard, '.', sizeof(currentBoard));
  for (int byteIdx = 0; byteIdx < 32; ++byteIdx) {
    const uint8_t lowerCode = raw[byteIdx] & 0x0f;
    const uint8_t upperCode = raw[byteIdx] >> 4;
    for (int half = 0; half < 2; ++half) {
      const int rawIndex = byteIdx * 2 + half;  // 0..63, H8..A8,...,H1..A1
      const int rank = 8 - (rawIndex / 8);
      const int file0 = 7 - (rawIndex % 8);
      const uint8_t code = (half == 0) ? lowerCode : upperCode;
      currentBoard[modeBStatusWireIndex(file0, rank)] = pieceFromChessnutCode(code);
    }
  }

  sendCableStatusFrame();
  armVerboseCableLog();

  if (!haveSettledBoard) {
    memcpy(settledBoard, currentBoard, sizeof(settledBoard));
    haveSettledBoard = true;
  }
  lastBoardChangeMs = millis();
  ledSettlePending = true;
  showLocalBoardDeviations();
}

void handleMiscPacket(const uint8_t* data, size_t length) {
  if (length < 3) return;
  if (data[0] == 0x0f && data[1] == 0x01) {
    Serial.printf("[CHESSNUT] button pressed: %u\r\n", data[2]);
  }
}

}  // namespace

void chessnutShowLocalBoardDeviations() { showLocalBoardDeviations(); }

bool chessnutHasLocalDeviation() {
  SquareHighlight deviations[1];
  return collectLocalDeviations(deviations, 1) > 0;
}

uint32_t chessnutDroppedBoardPackets() { return droppedBoardPackets; }

bool chessnutIsConnected() {
  return bleClient != nullptr && bleClient->isConnected() && writeChar != nullptr;
}

bool chessnutConnect(const NimBLEAddress& address) {
  if (bleClient == nullptr) {
    bleClient = NimBLEDevice::createClient();
    bleClient->setClientCallbacks(new ClientCallbacks(), true);
    bleClient->setConnectTimeout(kConnectTimeoutMs);
  }
  if (boardDataQueue == nullptr) boardDataQueue = xQueueCreate(16, sizeof(RawPacket));
  if (miscDataQueue == nullptr) miscDataQueue = xQueueCreate(16, sizeof(RawPacket));

  // Request a fast connection interval before connecting, matching
  // millennium_board.cpp's own client-side setConnectionParams() -- added
  // 2026-08-31, never previously ported here. This exact fix (15-30ms
  // interval, zero slave latency, 2s supervision timeout) was proven
  // critical for King/T2's reliable status delivery earlier in this
  // project's history (see project memory: "BREAKTHROUGH: King progressed
  // to a third move for the first time ever", 2026-08-27) -- a slower
  // default interval let notifications queue up and arrive coalesced/
  // delayed rather than as separate timely updates. Chessnut never got this
  // fix; worth testing given T2 (which has it) captures correctly and
  // Chessnut (which doesn't) has never once registered a capture with
  // Phoenix despite every content/timing variant tried on the cable side.
  bleClient->setConnectionParams(12, 24, 0, 200);

  Serial.printf("Connecting to %s ...\r\n", address.toString().c_str());
  if (!bleClient->connect(address)) {
    Serial.println("BLE connection failed.");
    return false;
  }

  if (bleClient->getServices(true).empty()) {
    Serial.println("Chessnut BLE service discovery returned nothing.");
    bleClient->disconnect();
    return false;
  }

  writeChar = findCharacteristicInAnyService(bleClient, kWriteUuid);
  boardChar = findCharacteristicInAnyService(bleClient, kReadBoardUuid);
  miscChar = findCharacteristicInAnyService(bleClient, kReadMiscUuid);
  if (writeChar == nullptr || boardChar == nullptr) {
    Serial.println("Chessnut BLE characteristics not found.");
    writeChar = nullptr;
    bleClient->disconnect();
    return false;
  }

  if (!boardChar->canNotify() || !boardChar->subscribe(true, onBoardNotify)) {
    Serial.println("Could not enable Chessnut board notifications.");
    writeChar = nullptr;
    bleClient->disconnect();
    return false;
  }
  if (miscChar != nullptr && miscChar->canNotify()) {
    miscChar->subscribe(true, onMiscNotify);
  }

  Serial.println("Chessnut BLE gateway connected.");
  resetKingLedFrameBaseline();
  haveLastRawBoardData = false;
  haveSettledBoard = false;
  ledSettlePending = false;

  static constexpr uint8_t initCommand[] = {0x21, 0x01, 0x00};
  writeChar->writeValue(initCommand, sizeof(initCommand), true);
  return true;
}

void chessnutSetHighlightedSquares(const SquareHighlight* squares, size_t count) {
  if (!chessnutIsConnected()) return;
  uint8_t command[10] = {0x0a, 0x08, 0, 0, 0, 0, 0, 0, 0, 0};
  for (size_t i = 0; i < count; ++i) {
    const int file0 = squares[i].squareIndex % 8;  // 0-based a..h
    const int rank = squares[i].squareIndex / 8 + 1;
    const uint8_t fileBit = static_cast<uint8_t>(0x80 >> file0);  // a=0x80 .. h=0x01
    command[2 + (8 - rank)] |= fileBit;
  }
  // Write-with-response. A write-without-response variant was tried
  // 2026-08-31 (theory: the blocking round-trip here starves the cable's
  // incoming UART read of CPU time, risking corrupted bytes from Phoenix)
  // but the very next real-hardware test hung the whole device on boot --
  // no serial output at all, persisting through a full USB unplug/replug,
  // unrelated to the earlier 2026-08-30 attempt's own regression. Reverted
  // immediately without a confirmed root cause; do not retry without
  // isolating why it hung first.
  writeChar->writeValue(command, sizeof(command), true);
}

void chessnutPoll() {
  if (!chessnutIsConnected()) return;

  RawPacket packet{};
  while (xQueueReceive(boardDataQueue, &packet, 0) == pdTRUE) {
    handleBoardDataPacket(packet.data, packet.length);
  }
  while (xQueueReceive(miscDataQueue, &packet, 0) == pdTRUE) {
    handleMiscPacket(packet.data, packet.length);
  }

  // Once nothing has changed for kLedSettleMs, decide whether to accept the
  // current position as the new confirmed baseline. Accept it ONLY if at
  // least one square shows a piece that wasn't there in the old baseline
  // (an "arrival" -- covers both empty->occupied and a piece swap sampled
  // in one snapshot). That single event resolves the whole outstanding
  // diff at once: a normal move's origin (now empty) clears together with
  // its destination (now occupied) the moment the destination settles --
  // matching "the piece is back or stands elsewhere" (the user's own
  // words). If NOTHING arrived -- a piece was only ever removed, nothing
  // placed anywhere -- do NOT accept, no matter how long it's been quiet:
  // confirmed on real hardware 2026-08-31 that unconditionally accepting
  // after the timeout made a still-missing piece's warning self-clear
  // after 500ms ("blinkt einmal, dann nichts"), which is exactly the
  // moment a real capture needs it to keep showing.
  if (ledSettlePending &&
      static_cast<uint32_t>(millis() - lastBoardChangeMs) >= kLedSettleMs) {
    bool pieceArrived = false;
    for (int wireIndex = 0; wireIndex < 64; ++wireIndex) {
      if (currentBoard[wireIndex] != settledBoard[wireIndex] &&
          currentBoard[wireIndex] != '.') {
        pieceArrived = true;
        break;
      }
    }
    if (pieceArrived) {
      memcpy(settledBoard, currentBoard, sizeof(settledBoard));
      showLocalBoardDeviations();
    }
    ledSettlePending = false;
  }
}
