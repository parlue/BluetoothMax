#include <Arduino.h>
#include <BLEDevice.h>
#include <esp_gap_ble_api.h>

namespace {

#ifndef MILLENNIUM_BAUD
#define MILLENNIUM_BAUD 38400UL
#endif

#ifndef KING_SIMULATOR_MODE
#define KING_SIMULATOR_MODE 0
#endif

#ifndef BOARD_ISOLATION_TEST_MODE
#define BOARD_ISOLATION_TEST_MODE 0
#endif

// King's own 'L' frame is normally NOT relayed to the BLE peer at all (see
// receiveFromMillenniumComputer): against the real board, relaying it
// re-lights the board's physical LEDs right after our own clearBoardLeds()
// turns them off, which real testing showed blocks King from reacting to
// anything further. But DiablilloSniffer (a fake board used to isolate our
// own cable-facing code, no physical LEDs to re-light) needs to actually
// see King's 'L' content to decode and confirm King's suggested move --
// that's how the original sniffer breakthrough worked. Set to 1 only when
// pointing this gateway's BLE scan at the sniffer instead of the real board.
#ifndef RELAY_L_FRAME_TO_BOARD
#define RELAY_L_FRAME_TO_BOARD 0
#endif

#define RAW_TRANSPARENT_GATEWAY 0

constexpr int kMillenniumRxPin = 20;
constexpr int kMillenniumTxPin = 21;
constexpr int kStatusLedPin = 8;
constexpr uint32_t kMonitorBaud = 115200;
constexpr uint32_t kReconnectIntervalMs = 3000;
constexpr uint32_t kBleFrameSpacingMs = 100;
constexpr size_t kFrameBufferSize = 256;

constexpr char kBoardName[] = "MILLENNIUM CHESS";
constexpr char kServiceUuid[] = "49535343-fe7d-4ae5-8fa9-9fafd205e455";
constexpr char kBoardTxUuid[] = "49535343-1e4d-4bd9-ba61-23c647249616";
constexpr char kBoardRxUuid[] = "49535343-8841-43f4-a8d4-ecbe34729bb3";

HardwareSerial MillenniumSerial(1);
BLEClient* bleClient = nullptr;
BLERemoteCharacteristic* boardRx = nullptr;
QueueHandle_t bleToUartQueue = nullptr;
QueueHandle_t uartToBleQueue = nullptr;

struct ProtocolFrame {
  uint16_t length;
  uint8_t data[kFrameBufferSize];
};

uint8_t uartFrame[kFrameBufferSize];
size_t uartFrameLength = 0;
uint8_t bleFrame[kFrameBufferSize];
size_t bleFrameLength = 0;
uint32_t lastConnectAttemptMs = 0;
uint32_t lastBleFrameSentMs = 0;
uint32_t uartToBleBytes = 0;
uint32_t bleToUartBytes = 0;
uint32_t rawUartRxBytes = 0;
uint32_t discardedUartRxBytes = 0;
uint8_t lastLoggedStatus[67] = {};
bool haveLoggedStatus = false;
bool suppressNextRealLedAck = false;
bool ledsAwaitingClear = false;
uint8_t cachedBoardStatus[67] = {};
bool haveCachedBoardStatus = false;
uint32_t lastAutonomousStatusMs = 0;
uint8_t lastStatusSentToKing[67] = {};
bool haveSentStatusToKing = false;
// Matches a real board's own scan-rate-driven auto-report cadence: register 1
// ("board scan time") defaults to 0x14 (20); 20*2048/1000 ~= 41ms is the real
// interval a genuine Mode-B board reports on in "update every scan" mode --
// not once a second, which is 24x slower than King likely expects. This is
// only a fallback now -- see queryBoardRegister()/pendingRegisterQuery1: we
// proactively read the *real* board's own register 1 on connect (mirroring
// what CynusLink/Diablillo do to any board they talk to) and use its actual
// value instead of assuming this default is correct for this specific board.
constexpr uint32_t kAutonomousStatusIntervalMs = 41;
uint32_t autonomousStatusIntervalMs = kAutonomousStatusIntervalMs;
bool pendingRegisterQuery1 = false;
bool pendingRegisterQuery2 = false;
bool pendingVersionQuery = false;
uint8_t rawGatewayFrame[kFrameBufferSize] = {};
size_t rawGatewayFrameLength = 0;
uint32_t rawGatewayLastByteUs = 0;

enum class LedState : uint8_t { Searching, Connected };
volatile LedState ledState = LedState::Searching;

void statusLedTask(void*) {
  bool ledOn = false;
  uint32_t lastChangeMs = 0;
  for (;;) {
    const uint32_t nowMs = millis();
    const LedState state = ledState;
    if (state == LedState::Searching) {
      if (static_cast<uint32_t>(nowMs - lastChangeMs) >= 250) {
        ledOn = !ledOn;
        lastChangeMs = nowMs;
      }
    } else {
      ledOn = true;
    }
    digitalWrite(kStatusLedPin, ledOn ? LOW : HIGH);  // onboard LED is active low
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

bool bleConnected() {
  return bleClient != nullptr && bleClient->isConnected() && boardRx != nullptr;
}

void queueUartFrameForBle(const uint8_t* frame, size_t length);
size_t writeFrameToKing(const uint8_t* logicalFrame, size_t length);
void queryBoardRegister(uint8_t addr);
void queryBoardVersion();

uint8_t encodeOddParity(uint8_t ascii) {
  ascii &= 0x7f;
  uint8_t ones = 0;
  for (uint8_t value = ascii; value != 0; value >>= 1) ones += value & 1;
  return (ones % 2 == 0) ? static_cast<uint8_t>(ascii | 0x80) : ascii;
}

size_t commandLength(uint8_t first) {
  switch (first & 0x7f) {
    case 'S': case 'X': case 'T': case 'V': return 3;
    case 'R': return 5;
    case 'W': return 7;
    case 'L': return 167;
    default: return 0;
  }
}

size_t replyLength(uint8_t first) {
  switch (first & 0x7f) {
    case 's': return 67;
    case 'x': case 'l': return 3;
    // 'r' is 'r' + 2 hex addr + 2 hex value + 2 hex checksum = 7 bytes, not
    // 5 -- confirmed against CynusLink's own real, King-proven source
    // (`sendCL("r" + hx(a) + hx(ee[a]))`, checksum appended automatically)
    // and cross-checked against our own DiablilloSniffer test project's
    // identical reply format. Never exercised before since King itself
    // never sends 'R' directly, so this bug had no visible effect until we
    // started issuing our own R queries to the real board below.
    case 'r': return 7;
    case 'v': case 'w': return 7;
    default: return 0;
  }
}

bool validBlock(const uint8_t* frame, size_t length) {
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

void handleKingWriteLocally(const uint8_t* command) {
  uint8_t reply[7] = {'w', command[1], command[2], command[3], command[4], 0, 0};
  uint8_t checksum = 0;
  for (size_t i = 0; i < 5; ++i) checksum ^= reply[i] & 0x7f;
  static constexpr char hex[] = "0123456789ABCDEF";
  reply[5] = hex[checksum >> 4];
  reply[6] = hex[checksum & 0x0f];

  delayMicroseconds(750);
  writeFrameToKing(reply, sizeof(reply));

  static bool firstWriteLogged = false;
  if (!firstWriteLogged) {
    Serial.print("Gateway-local EEPROM write acknowledged: ");
    Serial.write(reply, sizeof(reply));
    Serial.println();
    firstWriteLogged = true;
  }
}

#if KING_SIMULATOR_MODE

// Simulates a perfectly well-behaved Mode-B board directly on King's own
// cable -- no BLE, no real board at all. This is a direct port of
// DiablilloSniffer's proven LED-decode/move-confirm logic (which got King
// to play multiple real moves -- but with Diablillo, not our own code, on
// the cable). Running that exact same logic through OUR OWN cable-facing
// UART code isolates whether our own King-facing code is the reason King
// never reacts against the real board, using only this one board -- no
// second ESP32 needed.
//
// Wire format note: this uses the *raw*, unmirrored file order (h..a) --
// exactly what DiablilloSniffer sends directly to Diablillo/King, proven
// three times independently (g1-f3, e2-e4, d2-d4) via wireIndex() below.
// This deliberately does NOT use the per-rank mirror applied to the real
// board's status in sendBleDataToMillenniumComputer() -- that mirror was
// derived from how to correctly *read* the real board's raw output for our
// own understanding, but was never independently confirmed as what King
// itself wants on the wire. If this simulator gets King to react while
// using the raw/unmirrored convention, that would suggest the real-board
// mirror step direction should be revisited too.
constexpr char kStartPosition[] =
    "RNBKQBNRPPPPPPPP................................pppppppprnbkqbnr";
char currentBoard[65];
char lastSentBoard[65];

constexpr int wireIndex(char file, int rank) {
  return (rank - 1) * 8 + (7 - (file - 'a'));
}
int wireIndexFR(int file0, int rank) { return (rank - 1) * 8 + (7 - file0); }
void wireIndexToFileRank(int idx, int& file0, int& rank) {
  rank = idx / 8 + 1;
  file0 = 7 - (idx % 8);
}

enum class MoveSimStage { kIdle, kArmed, kWhiteLifted, kBlackArmed, kBlackLifted, kDone };
MoveSimStage moveSimStage = MoveSimStage::kIdle;
uint32_t moveSimStageAt = 0;
char moveSimPiece = '.';
int decodedWhiteSource = -1;
int decodedWhiteDest = -1;
constexpr int kBlackLiftedSquare = wireIndex('e', 7);  // scripted Black reply: e7-e5
constexpr int kBlackPlacedSquare = wireIndex('e', 5);

uint8_t ledGrid[81] = {};
uint8_t lastLoggedLedGrid[81] = {};
bool haveLoggedLedGrid = false;
uint8_t baselineLedGrid[81] = {};
bool haveBaselineLedGrid = false;

void sendKingFrame(const uint8_t* data, size_t dataLength, const char* reason) {
  uint8_t frame[80] = {};
  uint8_t wireFrame[80] = {};
  if (dataLength + 2 > sizeof(frame)) return;
  memcpy(frame, data, dataLength);
  uint8_t checksum = 0;
  for (size_t i = 0; i < dataLength; ++i) checksum ^= frame[i] & 0x7f;
  static constexpr char hex[] = "0123456789ABCDEF";
  frame[dataLength] = hex[checksum >> 4];
  frame[dataLength + 1] = hex[checksum & 0x0f];
  const size_t total = dataLength + 2;
  for (size_t i = 0; i < total; ++i) wireFrame[i] = encodeOddParity(frame[i]);
  const size_t written = MillenniumSerial.write(wireFrame, total);
  Serial.printf("[SIM -> KING] %s, %u/%u bytes: ", reason,
                static_cast<unsigned>(written), static_cast<unsigned>(total));
  Serial.write(frame, total);
  Serial.println();
}

void sendKingAck(const uint8_t* commandFrame, size_t commandFrameLength) {
  const uint8_t command = commandFrame[0] & 0x7f;
  uint8_t reply[7] = {};
  size_t replyLength = 1;
  reply[0] = static_cast<uint8_t>(tolower(command & 0x7f));
  if (command == 'V') {
    static constexpr uint8_t version[] = {'v', '0', '1', '0', '0'};
    memcpy(reply, version, sizeof(version));
    replyLength = sizeof(version);
  } else if ((command == 'W' || command == 'R') && commandFrameLength >= 3) {
    replyLength = commandFrameLength - 2;
    memcpy(reply + 1, commandFrame + 1, replyLength - 1);
  }
  sendKingFrame(reply, replyLength, "command acknowledgement");
}

void sendStatusFrame(const char board[64], const char* reason) {
  uint8_t status[65] = {};
  status[0] = 's';
  memcpy(status + 1, board, 64);
  sendKingFrame(status, sizeof(status), reason);
  memcpy(lastSentBoard, board, 64);
}

void logLedGridIfChanged() {
  if (haveLoggedLedGrid && memcmp(ledGrid, lastLoggedLedGrid, sizeof(ledGrid)) == 0) return;
  memcpy(lastLoggedLedGrid, ledGrid, sizeof(ledGrid));
  haveLoggedLedGrid = true;
  Serial.println("[LED] grid changed (rows=fileCorner 0-8, cols=rankCorner 0-8):");
  for (int file = 0; file < 9; ++file) {
    char row[32];
    int pos = 0;
    for (int rankTop = 0; rankTop < 9; ++rankTop) {
      pos += snprintf(row + pos, sizeof(row) - pos, "%02X ", ledGrid[file * 9 + rankTop]);
    }
    Serial.printf("[LED]   %s\r\n", row);
  }
}

// Finds squares whose 4 corners all changed, uniformly, relative to the
// baseline (generic, no-highlight) grid. Source marker = 0x0F, destination
// marker = 0xF0 -- confirmed three times against real King captures.
int findChangedSquares(int outIdx[2], uint8_t outValue[2]) {
  if (!haveBaselineLedGrid) return 0;
  int count = 0;
  for (int file = 0; file < 8; ++file) {
    for (int rankTop = 0; rankTop < 8; ++rankTop) {
      const uint8_t c00 = ledGrid[file * 9 + rankTop];
      const uint8_t c10 = ledGrid[(file + 1) * 9 + rankTop];
      const uint8_t c01 = ledGrid[file * 9 + rankTop + 1];
      const uint8_t c11 = ledGrid[(file + 1) * 9 + rankTop + 1];
      const uint8_t b00 = baselineLedGrid[file * 9 + rankTop];
      const uint8_t b10 = baselineLedGrid[(file + 1) * 9 + rankTop];
      const uint8_t b01 = baselineLedGrid[file * 9 + rankTop + 1];
      const uint8_t b11 = baselineLedGrid[(file + 1) * 9 + rankTop + 1];
      const bool allChanged = c00 != b00 && c10 != b10 && c01 != b01 && c11 != b11;
      const bool allSameNewValue = c00 == c10 && c10 == c01 && c01 == c11;
      if (allChanged && allSameNewValue) {
        if (count < 2) {
          const int trueFile = 7 - file;
          const int trueRank = rankTop + 1;
          outIdx[count] = wireIndexFR(trueFile, trueRank);
          outValue[count] = c00;
        }
        ++count;
      }
    }
  }
  return count;
}

// Given a suggested (empty) destination square, finds a White pawn or
// knight on currentBoard that could plausibly reach it -- King's first move
// is always one of these two piece types. Sliding pieces not handled.
int findPawnOrKnightSource(int destIdx) {
  if (currentBoard[destIdx] != '.') return -1;
  int destFile, destRank;
  wireIndexToFileRank(destIdx, destFile, destRank);
  for (int idx = 0; idx < 64; ++idx) {
    const char piece = currentBoard[idx];
    if (piece < 'A' || piece > 'Z') continue;
    int file0, rank;
    wireIndexToFileRank(idx, file0, rank);
    if (piece == 'P') {
      if (file0 != destFile) continue;
      if (destRank == rank + 1) return idx;
      if (rank == 2 && destRank == rank + 2 &&
          currentBoard[wireIndexFR(file0, rank + 1)] == '.') {
        return idx;
      }
    } else if (piece == 'N') {
      const int df = abs(destFile - file0), dr = abs(destRank - rank);
      if ((df == 1 && dr == 2) || (df == 2 && dr == 1)) return idx;
    }
  }
  return -1;
}

void handleKingLFrame(const uint8_t* frame) {
  bool ok = true;
  for (int i = 0; i < 81 && ok; ++i) {
    auto hexNibble = [](uint8_t c) -> int {
      c &= 0x7f;
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return -1;
    };
    const int hi = hexNibble(frame[3 + i * 2]);
    const int lo = hexNibble(frame[4 + i * 2]);
    if (hi < 0 || lo < 0) { ok = false; break; }
    ledGrid[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  if (!ok) return;
  logLedGridIfChanged();
  if (!haveBaselineLedGrid) {
    memcpy(baselineLedGrid, ledGrid, sizeof(baselineLedGrid));
    haveBaselineLedGrid = true;
    Serial.println("[SIM] first L frame captured as baseline (generic/no-highlight pattern)");
    return;
  }
  if (moveSimStage != MoveSimStage::kIdle) return;
  int changedIdx[2];
  uint8_t changedValue[2];
  const int changedCount = findChangedSquares(changedIdx, changedValue);
  int sourceIdx = -1, destIdx = -1;
  if (changedCount == 2) {
    for (int k = 0; k < 2; ++k) {
      const bool lowSet = (changedValue[k] & 0x0F) != 0;
      const bool highSet = (changedValue[k] & 0xF0) != 0;
      if (lowSet && !highSet) sourceIdx = changedIdx[k];
      else if (highSet && !lowSet) destIdx = changedIdx[k];
    }
  } else if (changedCount == 1) {
    destIdx = changedIdx[0];
    sourceIdx = findPawnOrKnightSource(destIdx);
  }
  if (sourceIdx >= 0 && destIdx >= 0) {
    decodedWhiteSource = sourceIdx;
    decodedWhiteDest = destIdx;
    int sf, sr, df, dr;
    wireIndexToFileRank(sourceIdx, sf, sr);
    wireIndexToFileRank(destIdx, df, dr);
    Serial.printf("[SIM] decoded King's suggested move: %c%d-%c%d -- will confirm "
                  "in 2s (lift, then place 2s later)\r\n",
                  'a' + sf, sr, 'a' + df, dr);
    moveSimStage = MoveSimStage::kArmed;
    moveSimStageAt = millis() + 2000;
  } else if (changedCount >= 1) {
    Serial.printf("[LED] %d changed square(s) found but couldn't decode a clean "
                  "source+destination pair -- not confirming\r\n", changedCount);
  }
}

void receiveFromKingSimulator() {
  while (MillenniumSerial.available() > 0) {
    const uint8_t raw = static_cast<uint8_t>(MillenniumSerial.read());
    ++rawUartRxBytes;
    if (uartFrameLength == sizeof(uartFrame)) uartFrameLength = 0;
    uartFrame[uartFrameLength++] = raw & 0x7f;

    while (uartFrameLength > 0) {
      const size_t expected = commandLength(uartFrame[0]);
      if (expected == 0) {
        Serial.printf("[KING RX] unknown start byte %02X discarded\r\n", uartFrame[0]);
        memmove(uartFrame, uartFrame + 1, --uartFrameLength);
        continue;
      }
      if (uartFrameLength < expected) break;

      if (uartFrame[0] != 'L') {
        Serial.printf("[KING RX FRAME] %c, %u bytes, checksum=%s: ", uartFrame[0],
                      static_cast<unsigned>(expected),
                      validBlock(uartFrame, expected) ? "OK" : "BAD");
        Serial.write(uartFrame, expected);
        Serial.println();
      }

      if (validBlock(uartFrame, expected)) {
        if (uartFrame[0] == 'S') {
          sendStatusFrame(currentBoard, "replying to explicit S request");
        } else {
          // Checksum of a bare 'l' ack is content-independent -- ack first,
          // instantly, exactly like the proven sniffer/Elfacun behavior.
          sendKingAck(uartFrame, expected);
          if (uartFrame[0] == 'L') handleKingLFrame(uartFrame);
        }
      }
      uartFrameLength -= expected;
      memmove(uartFrame, uartFrame + expected, uartFrameLength);
    }
  }
}

void runMoveSimStage() {
  const uint32_t nowMs = millis();
  if (moveSimStage == MoveSimStage::kArmed &&
      static_cast<int32_t>(nowMs - moveSimStageAt) >= 0) {
    moveSimPiece = currentBoard[decodedWhiteSource];
    currentBoard[decodedWhiteSource] = '.';
    sendStatusFrame(currentBoard, "source square lifted (confirming King's own decoded move)");
    moveSimStage = MoveSimStage::kWhiteLifted;
    moveSimStageAt = nowMs + 2000;
  } else if (moveSimStage == MoveSimStage::kWhiteLifted &&
             static_cast<int32_t>(nowMs - moveSimStageAt) >= 0) {
    currentBoard[decodedWhiteDest] = moveSimPiece;
    sendStatusFrame(currentBoard, "destination square placed (White move confirmed)");
    moveSimStage = MoveSimStage::kBlackArmed;
    moveSimStageAt = nowMs + 3000;
  } else if (moveSimStage == MoveSimStage::kBlackArmed &&
             static_cast<int32_t>(nowMs - moveSimStageAt) >= 0) {
    moveSimPiece = currentBoard[kBlackLiftedSquare];
    currentBoard[kBlackLiftedSquare] = '.';
    sendStatusFrame(currentBoard, "e7 lifted (scripted Black reply)");
    moveSimStage = MoveSimStage::kBlackLifted;
    moveSimStageAt = nowMs + 2000;
  } else if (moveSimStage == MoveSimStage::kBlackLifted &&
             static_cast<int32_t>(nowMs - moveSimStageAt) >= 0) {
    currentBoard[kBlackPlacedSquare] = moveSimPiece;
    sendStatusFrame(currentBoard, "e5 placed (e7-e5 confirmed)");
    moveSimStage = MoveSimStage::kDone;
  }
}

#endif

void onBoardNotification(BLERemoteCharacteristic*, uint8_t* data,
                         size_t length, bool) {
#if RAW_TRANSPARENT_GATEWAY
  uint8_t logical[kFrameBufferSize] = {};
  size_t offset = 0;
  while (offset < length) {
    const size_t chunk = min(sizeof(logical), length - offset);
    for (size_t i = 0; i < chunk; ++i) logical[i] = data[offset + i] & 0x7f;
    MillenniumSerial.write(logical, chunk);
    bleToUartBytes += chunk;
    offset += chunk;
  }
#else
  for (size_t i = 0; i < length; ++i) {
    const uint8_t ascii = data[i] & 0x7f;
    xQueueSend(bleToUartQueue, &ascii, 0);
  }
#endif
}

class ClientCallbacks final : public BLEClientCallbacks {
 public:
  void onConnect(BLEClient*) override {}

  void onDisconnect(BLEClient*) override {
    boardRx = nullptr;
    suppressNextRealLedAck = false;
    haveCachedBoardStatus = false;
    pendingRegisterQuery1 = false;
    pendingRegisterQuery2 = false;
    pendingVersionQuery = false;
    haveSentStatusToKing = false;
    autonomousStatusIntervalMs = kAutonomousStatusIntervalMs;
    ledState = LedState::Searching;
    Serial.println("BLE disconnected; reconnecting automatically.");
  }
};

bool findBoard(BLEAddress& address) {
  Serial.println("BLE scan: looking for MILLENNIUM CHESS ...");
  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  BLEScanResults results = scan->start(5, false);

  for (int i = 0; i < results.getCount(); ++i) {
    BLEAdvertisedDevice device = results.getDevice(i);
    if (device.haveName() && device.getName().find(kBoardName) != std::string::npos) {
      address = device.getAddress();
      Serial.printf("BLE board found: %s (%s)\r\n",
                    device.getName().c_str(), address.toString().c_str());
      scan->clearResults();
      return true;
    }
  }

  scan->clearResults();
  Serial.println("No matching BLE board found.");
  return false;
}

bool connectBoard() {
  ledState = LedState::Searching;
  BLEAddress address("00:00:00:00:00:00");
  if (!findBoard(address)) return false;

  if (bleClient == nullptr) {
    bleClient = BLEDevice::createClient();
    bleClient->setClientCallbacks(new ClientCallbacks());
  }

  // CynusLink (proven to feed King moves successfully end-to-end) explicitly
  // requests a fast connection (15-30ms interval, no slave latency, 2s
  // supervision timeout) as soon as it knows the peer address, before ever
  // connecting -- confirmed via DiablilloSniffer testing that a slow/default
  // connection interval alone can silently defeat an otherwise-correct
  // low-latency status-forwarding architecture, regardless of how fast our
  // own software reacts. `esp_ble_gap_set_prefer_conn_params` is this
  // project's master-role equivalent of CynusLink's peripheral-role
  // `updateConnParams` (this project connects TO the real board as a BLE
  // client, the opposite role from CynusLink/DiablilloSniffer).
  esp_ble_gap_set_prefer_conn_params(*address.getNative(), 12, 24, 0, 200);

  Serial.printf("Connecting to %s ...\r\n", address.toString().c_str());
  if (!bleClient->connect(address)) {
    Serial.println("BLE connection failed.");
    return false;
  }

  bleClient->setMTU(247);
  BLERemoteService* service = bleClient->getService(BLEUUID(kServiceUuid));
  if (service == nullptr) {
    Serial.println("ChessLink BLE service not found.");
    bleClient->disconnect();
    return false;
  }

  BLERemoteCharacteristic* boardTx = service->getCharacteristic(BLEUUID(kBoardTxUuid));
  boardRx = service->getCharacteristic(BLEUUID(kBoardRxUuid));
  if (boardTx == nullptr || boardRx == nullptr) {
    Serial.println("ChessLink BLE UART characteristics not found.");
    boardRx = nullptr;
    bleClient->disconnect();
    return false;
  }

  if (!boardTx->canNotify()) {
    Serial.println("Could not enable board notifications.");
    boardRx = nullptr;
    bleClient->disconnect();
    return false;
  }
  boardTx->registerForNotify(onBoardNotification);

  Serial.printf("BLE gateway connected; negotiated MTU=%u.\r\n", bleClient->getMTU());
  Serial.println("UART startup frames preserved; forwarding them in original order.");
  ledState = LedState::Connected;

  pendingVersionQuery = true;
  queryBoardVersion();    // matches Diablillo's own observed connect order: V, then S/R.
  pendingRegisterQuery1 = true;
  pendingRegisterQuery2 = true;
  queryBoardRegister(1);  // "board scan time" -- drives our auto-report interval.
  queryBoardRegister(2);  // auto-report mode -- logged only, for diagnostics.
  return true;
}

void queueUartFrameForBle(const uint8_t* frame, size_t length) {
  if (length == 0) return;
  ProtocolFrame pending{};
  pending.length = static_cast<uint16_t>(length);
  memcpy(pending.data, frame, length);
  const BaseType_t queued = xQueueSendToBack(uartToBleQueue, &pending, 0);
  if (queued != pdTRUE) {
    ProtocolFrame oldest{};
    xQueueReceive(uartToBleQueue, &oldest, 0);
    xQueueSendToBack(uartToBleQueue, &pending, 0);
    static uint32_t lastQueueFullLogMs = 0;
    const uint32_t nowMs = millis();
    if (lastQueueFullLogMs == 0 ||
        static_cast<uint32_t>(nowMs - lastQueueFullLogMs) >= 1000) {
      Serial.println("UART -> BLE queue full; oldest frame replaced (log throttled).");
      lastQueueFullLogMs = nowMs;
    }
  }
}

void requestBoardStatus() {
  if (!bleConnected()) return;
  static constexpr uint8_t statusRequest[] = {'S', '5', '3'};
  uint8_t encoded[sizeof(statusRequest)] = {};
  for (size_t i = 0; i < sizeof(statusRequest); ++i) {
    encoded[i] = encodeOddParity(statusRequest[i]);
  }
  boardRx->writeValue(encoded, sizeof(encoded), true);
  uartToBleBytes += sizeof(encoded);
  lastBleFrameSentMs = millis();
}

// Reads one of the real board's own registers (mirroring CynusLink's/
// Diablillo's own proactive R-query behavior toward any board they connect
// to -- confirmed via DiablilloSniffer testing that a proven-working bridge
// does this unconditionally on connect, independent of anything King asks).
// The reply is intercepted and consumed in sendBleDataToMillenniumComputer()
// (via pendingRegisterQuery1/2), not forwarded to King -- it's our own
// diagnostic query, not something King requested.
void queryBoardRegister(uint8_t addr) {
  if (!bleConnected()) return;
  static constexpr char hex[] = "0123456789ABCDEF";
  uint8_t frame[5] = {'R', static_cast<uint8_t>(hex[addr >> 4]),
                       static_cast<uint8_t>(hex[addr & 0x0f]), 0, 0};
  uint8_t checksum = 0;
  for (size_t i = 0; i < 3; ++i) checksum ^= frame[i] & 0x7f;
  frame[3] = static_cast<uint8_t>(hex[checksum >> 4]);
  frame[4] = static_cast<uint8_t>(hex[checksum & 0x0f]);
  uint8_t encoded[5];
  for (size_t i = 0; i < 5; ++i) encoded[i] = encodeOddParity(frame[i]);
  boardRx->writeValue(encoded, sizeof(encoded), true);
  uartToBleBytes += sizeof(encoded);
  lastBleFrameSentMs = millis();
}

// Diablillo (proven working) queries the board's version ('V') immediately
// on connect, before even its first status request -- confirmed via
// DiablilloSniffer's first real capture ("V56 then S53, in that order,
// immediately"). This project never sent V at all before, a real behavioral
// gap versus a working bridge. The reply is intercepted and consumed in
// sendBleDataToMillenniumComputer() (via pendingVersionQuery), not forwarded
// to King -- King itself never sends V on the cable in any capture so far.
void queryBoardVersion() {
  if (!bleConnected()) return;
  static constexpr char hex[] = "0123456789ABCDEF";
  uint8_t frame[3] = {'V', 0, 0};
  uint8_t checksum = frame[0] & 0x7f;
  frame[1] = static_cast<uint8_t>(hex[checksum >> 4]);
  frame[2] = static_cast<uint8_t>(hex[checksum & 0x0f]);
  uint8_t encoded[3];
  for (size_t i = 0; i < 3; ++i) encoded[i] = encodeOddParity(frame[i]);
  boardRx->writeValue(encoded, sizeof(encoded), true);
  uartToBleBytes += sizeof(encoded);
  lastBleFrameSentMs = millis();
}

void clearBoardLeds() {
  if (!bleConnected()) return;
  static constexpr uint8_t offLedCommand[] = {'X', '5', '8'};
  uint8_t encoded[sizeof(offLedCommand)] = {};
  for (size_t i = 0; i < sizeof(offLedCommand); ++i) {
    encoded[i] = encodeOddParity(offLedCommand[i]);
  }
  boardRx->writeValue(encoded, sizeof(encoded), true);
  uartToBleBytes += sizeof(encoded);
  lastBleFrameSentMs = millis();
  Serial.println("[LED] X58 sent to the real board to physically clear its setup LEDs.");
}

void transmitQueuedUartFrame() {
  if (!bleConnected() || uartToBleQueue == nullptr) return;

  ProtocolFrame pending{};
  if (xQueueReceive(uartToBleQueue, &pending, 0) != pdTRUE) return;
  const size_t length = pending.length;
  uint8_t* frame = pending.data;
  if (lastBleFrameSentMs != 0 &&
      static_cast<uint32_t>(millis() - lastBleFrameSentMs) < kBleFrameSpacingMs) {
    xQueueSendToFront(uartToBleQueue, &pending, 0);
    return;
  }

  uint8_t encoded[kFrameBufferSize];
  for (size_t i = 0; i < length; ++i) encoded[i] = encodeOddParity(frame[i]);

  const size_t mtuPayload = bleClient->getMTU() > 3 ? bleClient->getMTU() - 3 : 20;
  size_t offset = 0;
  while (offset < length && bleConnected()) {
    const size_t chunk = min(mtuPayload, length - offset);
    boardRx->writeValue(encoded + offset, chunk, true);
    offset += chunk;
  }

  uartToBleBytes += offset;
  lastBleFrameSentMs = millis();
  if ((frame[0] & 0x7f) == 'W') {
    static uint8_t lastWrite[7] = {};
    static bool haveLastWrite = false;
    if (!haveLastWrite || length != sizeof(lastWrite) ||
        memcmp(lastWrite, frame, sizeof(lastWrite)) != 0) {
      memcpy(lastWrite, frame, sizeof(lastWrite));
      haveLastWrite = true;
      Serial.print("[MODE-B CONFIG] ");
      Serial.write(frame, length);
      Serial.println();
    }
  } else {
    Serial.printf("UART -> BLE: %c frame, %u ASCII bytes\r\n",
                  static_cast<char>(frame[0] & 0x7f),
                  static_cast<unsigned>(offset));
  }
}

#if RAW_TRANSPARENT_GATEWAY
void runRawTransparentGateway() {
  while (MillenniumSerial.available() > 0) {
    const uint8_t raw = static_cast<uint8_t>(MillenniumSerial.read());
    ++rawUartRxBytes;
    if (rawGatewayFrameLength < sizeof(rawGatewayFrame)) {
      rawGatewayFrame[rawGatewayFrameLength++] = raw & 0x7f;
    } else {
      rawGatewayFrameLength = 0;
    }
    rawGatewayLastByteUs = micros();
  }

  if (rawGatewayFrameLength == 0 || rawGatewayLastByteUs == 0 ||
      static_cast<uint32_t>(micros() - rawGatewayLastByteUs) < 3000) return;

  if (!bleConnected()) {
    rawGatewayFrameLength = 0;
    return;
  }

  uint8_t encoded[kFrameBufferSize] = {};
  for (size_t i = 0; i < rawGatewayFrameLength; ++i) {
    encoded[i] = encodeOddParity(rawGatewayFrame[i]);
  }
  const size_t mtuPayload = bleClient->getMTU() > 3 ? bleClient->getMTU() - 3 : 20;
  size_t offset = 0;
  while (offset < rawGatewayFrameLength && bleConnected()) {
    const size_t chunk = min(mtuPayload, rawGatewayFrameLength - offset);
    boardRx->writeValue(encoded + offset, chunk, true);
    offset += chunk;
  }
  uartToBleBytes += offset;
  Serial.printf("RAW UART -> BLE: %u bytes\r\n", static_cast<unsigned>(offset));
  rawGatewayFrameLength = 0;
}
#endif

void receiveFromMillenniumComputer() {
  size_t completedFrames = 0;
  while (MillenniumSerial.available() > 0) {
    const uint8_t raw = static_cast<uint8_t>(MillenniumSerial.read());
    ++rawUartRxBytes;
    uartFrame[uartFrameLength++] = raw & 0x7f;
    while (uartFrameLength > 0) {
      const size_t expected = commandLength(uartFrame[0]);
      if (expected == 0) {
        ++discardedUartRxBytes;
        const uint8_t discarded = uartFrame[0];
        Serial.printf("[KING RX] unrecognized byte raw=%02X ascii=%02X '%c'\r\n",
                      discarded, discarded & 0x7f,
                      isprint(discarded & 0x7f) ? discarded & 0x7f : '.');
        memmove(uartFrame, uartFrame + 1, --uartFrameLength);
        continue;
      }
      if (uartFrameLength < expected) break;
      if (validBlock(uartFrame, expected)) {
        if (uartFrame[0] == 'L' && expected == 167) {
          // Elfacun's own bridge (Diablillo) proves this: the checksum of a
          // handshake ack like a bare 'l' never depends on the LED payload,
          // so a proven Mode-B bridge answers L commands locally and
          // instantly instead of waiting on a round trip to the real board.
          // Elfacun's board_b.cpp explicitly skips its own BLE-relayed ack
          // once it recognizes a Diablillo bridge, trusting the bridge to
          // have already answered King directly over the cable.
          static constexpr uint8_t localLedAck[] = {'l', '6', 'C'};
          writeFrameToKing(localLedAck, sizeof(localLedAck));
          suppressNextRealLedAck = true;

          // King appears to probe repeatedly on its own (observed sending
          // fresh L frames over and over with nothing pressed) and may only
          // accept a reply that completes within a single probe cycle. A
          // fresh BLE round trip to the real board is too slow for that --
          // send whatever we already have cached instantly, right alongside
          // the l-ack, so it has a chance to land within the same cycle.
          // Still also kick off a fresh fetch to keep the cache current.
          if (haveCachedBoardStatus) {
            writeFrameToKing(cachedBoardStatus, sizeof(cachedBoardStatus));
          }
          requestBoardStatus();
          ledsAwaitingClear = true;

          // Deliberately NOT relayed to the real board (see below): a live
          // test showed the board's LEDs going on, briefly off (our own
          // clearBoardLeds() X58, once a confirmed status arrives), then
          // straight back on again -- because this L frame's own content
          // (King's generic "everything lit" New-Game pattern, unchanged
          // across every capture in this whole investigation) was queued
          // and relayed to the board right after, re-lighting it. User's
          // own experience: King only proceeds once the board's LEDs
          // actually stay off. We already drive the board's LEDs
          // independently via clearBoardLeds() whenever a confirmed status
          // arrives, so relaying King's own (generic, non-move) guess back
          // to the board only fights that and keeps the board lit forever.
          static uint8_t lastLedFrame[167] = {};
          static bool haveLastLedFrame = false;
          if (!haveLastLedFrame || memcmp(lastLedFrame, uartFrame, 167) != 0) {
            memcpy(lastLedFrame, uartFrame, 167);
            haveLastLedFrame = true;
            size_t activeLedValues = 0;
            for (size_t i = 3; i < 165; i += 2) {
              if (uartFrame[i] != '0' || uartFrame[i + 1] != '0') ++activeLedValues;
            }
            Serial.printf("[L DIAG] slot=%c%c, active-values=%u, checksum=%c%c%s\r\n",
                          uartFrame[1], uartFrame[2],
                          static_cast<unsigned>(activeLedValues),
                          uartFrame[165], uartFrame[166],
                          RELAY_L_FRAME_TO_BOARD ? "" : " (not relayed to board)");
            Serial.print("[L RAW] ");
            Serial.write(uartFrame, expected);
            Serial.println();
          }
#if RELAY_L_FRAME_TO_BOARD
          queueUartFrameForBle(uartFrame, expected);
#endif
        } else {
          // Preserve every other valid Mode-B command and its original
          // order so the real board's LEDs and state still stay in sync.
          queueUartFrameForBle(uartFrame, expected);
        }
      } else {
        ++discardedUartRxBytes;
        Serial.printf("[KING RX] bad checksum on %c frame (%u bytes), discarding first byte\r\n",
                      static_cast<char>(uartFrame[0] & 0x7f),
                      static_cast<unsigned>(expected));
        memmove(uartFrame, uartFrame + 1, --uartFrameLength);
        continue;
      }
      uartFrameLength -= expected;
      memmove(uartFrame, uartFrame + expected, uartFrameLength);
      ++completedFrames;
      if (completedFrames >= 1) return;
    }
    if (uartFrameLength == kFrameBufferSize) uartFrameLength = 0;
  }
}

size_t writeFrameToKing(const uint8_t* logicalFrame, size_t length) {
  // Emit the exact Mode-B wire image explicitly: seven data bits followed by
  // the odd-parity bit.  Sending that pre-encoded byte as 8N1 produces the
  // same 10-bit waveform while avoiding the ESP32-C3 hardware 7O1 TX path.
  uint8_t encoded[kFrameBufferSize] = {};
  for (size_t i = 0; i < length; ++i) {
    encoded[i] = encodeOddParity(logicalFrame[i]);
  }
  const size_t written = MillenniumSerial.write(encoded, length);
  bleToUartBytes += written;
  return written;
}

void sendBleDataToMillenniumComputer() {
  uint8_t ascii = 0;
  while (xQueueReceive(bleToUartQueue, &ascii, 0) == pdTRUE) {
    if (bleFrameLength == sizeof(bleFrame)) bleFrameLength = 0;
    bleFrame[bleFrameLength++] = ascii & 0x7f;
    while (bleFrameLength > 0) {
      const size_t expected = replyLength(bleFrame[0]);
      if (expected == 0) {
        memmove(bleFrame, bleFrame + 1, --bleFrameLength);
        continue;
      }
      if (bleFrameLength < expected) break;
      if (validBlock(bleFrame, expected)) {
        // 'r' replies here are answers to our own queryBoardRegister() calls
        // (King itself never sends 'R' on the cable, per extensive earlier
        // testing) -- consume them internally and don't forward to King.
        bool suppressAsOwnQueryReply = false;
        if (bleFrame[0] == 'v' && pendingVersionQuery) {
          pendingVersionQuery = false;
          suppressAsOwnQueryReply = true;
          Serial.print("[VERSION] real board replied: ");
          Serial.write(bleFrame, expected);
          Serial.println();
        } else if (bleFrame[0] == 'r' && (pendingRegisterQuery1 || pendingRegisterQuery2)) {
          auto hexNibble = [](uint8_t c) -> int {
            c &= 0x7f;
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
          };
          const int addrHi = hexNibble(bleFrame[1]), addrLo = hexNibble(bleFrame[2]);
          const int valHi = hexNibble(bleFrame[3]), valLo = hexNibble(bleFrame[4]);
          if (addrHi >= 0 && addrLo >= 0 && valHi >= 0 && valLo >= 0) {
            const uint8_t addr = static_cast<uint8_t>((addrHi << 4) | addrLo);
            const uint8_t value = static_cast<uint8_t>((valHi << 4) | valLo);
            if (addr == 1 && pendingRegisterQuery1) {
              pendingRegisterQuery1 = false;
              suppressAsOwnQueryReply = true;
              const uint32_t scan = (static_cast<uint32_t>(value) * 2048UL + 999UL) / 1000UL;
              if (scan > 0) autonomousStatusIntervalMs = scan;
              Serial.printf("[REGISTER] real board scan-time register[1]=0x%02X -> "
                            "auto-report interval %lums\r\n",
                            value, static_cast<unsigned long>(autonomousStatusIntervalMs));
            } else if (addr == 2 && pendingRegisterQuery2) {
              pendingRegisterQuery2 = false;
              suppressAsOwnQueryReply = true;
              Serial.printf("[REGISTER] real board auto-report mode register[2]=0x%02X "
                            "(mode=%d)\r\n", value, value & 7);
            }
          }
        }
        // NOTE: previously mirrored each rank's 8 files here (h..a -> a..h)
        // before forwarding to King. That mirror was derived from reading
        // the real board's raw output for our OWN understanding (isolation
        // test: a move the board displayed as c2-c4 showed up in raw data
        // at the position an a..h reading would call f2-f4) -- but it was
        // never independently confirmed that King actually wants the
        // mirrored version. DiablilloSniffer's proven wireIndex() (matched
        // 3 real King captures: g1-f3, e2-e4, d2-d4) sends the *raw*,
        // unmirrored h..a order directly to King/Diablillo, unmodified, and
        // that's what got King to actually play multiple moves. So: forward
        // the real board's status to King exactly as received, unmirrored.
        if (bleFrame[0] == 's' &&
            (!haveLoggedStatus || memcmp(lastLoggedStatus, bleFrame, 67) != 0)) {
          memcpy(lastLoggedStatus, bleFrame, 67);
          haveLoggedStatus = true;
          Serial.print("[BOARD STATUS] ");
          Serial.write(bleFrame, 67);
          Serial.println();
        }
        if (bleFrame[0] == 's') {
          const bool wasFirstStatus = !haveCachedBoardStatus;
          memcpy(cachedBoardStatus, bleFrame, 67);
          haveCachedBoardStatus = true;
          lastAutonomousStatusMs = millis();
          if (wasFirstStatus || ledsAwaitingClear) {
            // A real cabled board physically clears its own setup LEDs once
            // it has confirmed the position -- Diablillo visibly does this
            // to the real board too. Actually clear them (not just fake the
            // ack to King) so the board's own genuine 'x' reply, carrying
            // real confirmation, flows to King through the normal path.
            // Re-fires after every King L, not just the very first connect.
            clearBoardLeds();
            ledsAwaitingClear = false;
          }
        }
        // King already got an instant local 'l' ack when it sent the L
        // command (see receiveFromMillenniumComputer); the real board's own
        // genuine ack is redundant and must not be sent a second time.
        const bool redundantLedAck = bleFrame[0] == 'l' && suppressNextRealLedAck;
        if (redundantLedAck) {
          suppressNextRealLedAck = false;
        } else if (suppressAsOwnQueryReply) {
          // Consumed above; King never asked for this, so it doesn't get it.
        } else {
          // Elfacun's proven Mode-B board sends the native status order
          // (white home rank first). Forward every reply unchanged and
          // unprompted by us -- King drives all requests, we only relay.
          const size_t written = writeFrameToKing(bleFrame, expected);
          if (bleFrame[0] == 's') {
            // Keep the periodic resend below (loop()) in sync with what was
            // just put on the wire, so it doesn't immediately re-send the
            // exact same frame again on its next tick.
            memcpy(lastStatusSentToKing, bleFrame, sizeof(lastStatusSentToKing));
            haveSentStatusToKing = true;
          }
          if (bleFrame[0] != 'w') {
            Serial.printf("BLE -> UART: %c frame, %u ASCII bytes\r\n",
                          static_cast<char>(bleFrame[0]),
                          static_cast<unsigned>(written));
          }
        }
      } else {
        Serial.printf("BLE reply %c rejected: bad checksum\r\n",
                      static_cast<char>(bleFrame[0]));
      }
      bleFrameLength -= expected;
      memmove(bleFrame, bleFrame + expected, bleFrameLength);
    }
  }
}

#if BOARD_ISOLATION_TEST_MODE
// Drives the real board directly over BLE with no King/cable involved at
// all, to see exactly what the board does on its own: connect, ask for
// status, show a move (the exact L frame King itself sent for e2-e4 in an
// earlier session), then log everything the board sends afterwards so we
// can see if it produces anything beyond what we already forward to King.
void sendRawFrameToBoard(const uint8_t* frame, size_t length) {
  if (!bleConnected()) return;
  uint8_t encoded[kFrameBufferSize];
  for (size_t i = 0; i < length; ++i) encoded[i] = encodeOddParity(frame[i]);
  const size_t mtuPayload = bleClient->getMTU() > 3 ? bleClient->getMTU() - 3 : 20;
  size_t offset = 0;
  while (offset < length && bleConnected()) {
    const size_t chunk = min(mtuPayload, length - offset);
    boardRx->writeValue(encoded + offset, chunk, true);
    offset += chunk;
  }
}

void logBoardRepliesForIsolationTest() {
  uint8_t ascii = 0;
  while (xQueueReceive(bleToUartQueue, &ascii, 0) == pdTRUE) {
    if (bleFrameLength == sizeof(bleFrame)) bleFrameLength = 0;
    bleFrame[bleFrameLength++] = ascii & 0x7f;
    while (bleFrameLength > 0) {
      const size_t expected = replyLength(bleFrame[0]);
      if (expected == 0) {
        memmove(bleFrame, bleFrame + 1, --bleFrameLength);
        continue;
      }
      if (bleFrameLength < expected) break;
      if (validBlock(bleFrame, expected)) {
        Serial.print("[ISOLATION TEST] board -> us: ");
        Serial.write(bleFrame, expected);
        Serial.println();
      } else {
        Serial.printf("[ISOLATION TEST] bad checksum on %c frame\r\n",
                      static_cast<char>(bleFrame[0]));
      }
      bleFrameLength -= expected;
      memmove(bleFrame, bleFrame + expected, bleFrameLength);
    }
  }
}

void runBoardIsolationTest() {
  const uint32_t nowMs = millis();
  if (!bleConnected()) {
    if (static_cast<uint32_t>(nowMs - lastConnectAttemptMs) >= kReconnectIntervalMs) {
      lastConnectAttemptMs = nowMs;
      connectBoard();
    }
    return;
  }

  logBoardRepliesForIsolationTest();

  static bool testStarted = false;
  static int testStep = 0;
  static uint32_t testStepAt = 0;
  if (!testStarted) {
    testStarted = true;
    testStep = 0;
    testStepAt = nowMs + 1000;
    Serial.println("[ISOLATION TEST] Board connected. Requesting status in 1s...");
  }

  if (testStep == 0 && static_cast<int32_t>(nowMs - testStepAt) >= 0) {
    Serial.println("[ISOLATION TEST] Sending S53 (status request).");
    static constexpr uint8_t statusRequest[] = {'S', '5', '3'};
    sendRawFrameToBoard(statusRequest, sizeof(statusRequest));
    testStep = 1;
    testStepAt = nowMs + 2000;
  } else if (testStep == 1 && static_cast<int32_t>(nowMs - testStepAt) >= 0) {
    // Exact 167-byte L frame captured from a real King session showing
    // e2-e4 highlighted (active-values=58 in our own diagnostic log).
    static constexpr uint8_t moveFrame[] =
        "L0FFFFFFF000000FFFFFFFFFFFF000000FFFFFFFFFFFF000000FFFFFFFFFFFF"
        "000000FFFFFFFFFFFF000000FFFFFFFFFFFFF0F000FFFFFFFFFFFFF0F000FF"
        "FFFFFFFFFF000000FFFFFFFFFFFF000000FFFFFF3A";
    Serial.println("[ISOLATION TEST] Sending L frame (e2-e4 highlighted, "
                    "captured verbatim from a real King session).");
    sendRawFrameToBoard(moveFrame, sizeof(moveFrame) - 1);
    testStep = 2;
    Serial.println("[ISOLATION TEST] Now make the move on the board; "
                    "logging everything it sends from here on.");
  }
}
#endif

}  // namespace

void setup() {
  // The King sends its Mode-B identification/configuration immediately after
  // power-up. Start its UART before USB logging and before any BLE scan so
  // those first V/W frames remain in the hardware RX buffer.
  MillenniumSerial.setRxBufferSize(4096);
  MillenniumSerial.begin(MILLENNIUM_BAUD, SERIAL_8N1,
                          kMillenniumRxPin, kMillenniumTxPin);

  Serial.begin(kMonitorBaud);
  delay(1500);

  pinMode(kStatusLedPin, OUTPUT);
  digitalWrite(kStatusLedPin, HIGH);
  xTaskCreate(statusLedTask, "status-led", 1536, nullptr, 1, nullptr);

  bleToUartQueue = xQueueCreate(1024, sizeof(uint8_t));
  uartToBleQueue = xQueueCreate(256, sizeof(ProtocolFrame));

#if KING_SIMULATOR_MODE
  memcpy(currentBoard, kStartPosition, 65);
  memcpy(lastSentBoard, kStartPosition, 65);
  Serial.println("\r\nChessL1nkWireless King simulator V3.0 (sniffer-logic port)");
#else
  Serial.println("\r\nChessL1nkWireless Mode-B gateway V3.20 setup-L filter");
#endif
  Serial.printf("Cable: %lu baud, explicit odd parity over 8N1, RX=GPIO%d, TX=GPIO%d\r\n",
                static_cast<unsigned long>(MILLENNIUM_BAUD),
                kMillenniumRxPin, kMillenniumTxPin);
  Serial.println("WARNING: cable GPIOs only through the 3.3 V MAX3232 TTL side.");

#if KING_SIMULATOR_MODE
  Serial.println("MODE: KING SIMULATOR - BLE and the real chessboard are disabled.");
  Serial.println("Set King to play White (not just New Game) so it suggests its own");
  Serial.println("first move; this simulator decodes and confirms it automatically,");
  Serial.println("then plays a scripted e7-e5 as Black. No board/second ESP32 needed.");
#elif BOARD_ISOLATION_TEST_MODE
  Serial.println("MODE: BOARD ISOLATION TEST - driving the real board directly, no King involved.");
  BLEDevice::init("ChessL1nkWireless");
#else
  Serial.println("BLE: connection watchdog only; protocol idle until The King starts it.");
  BLEDevice::init("ChessL1nkWireless");
#endif
}

void loop() {
#if KING_SIMULATOR_MODE
  receiveFromKingSimulator();
  runMoveSimStage();

  static uint32_t lastSimulatorStatusMs = 0;
  const uint32_t nowMs = millis();
  if (static_cast<uint32_t>(nowMs - lastSimulatorStatusMs) >= 5000) {
    static constexpr const char* kStageNames[] = {
        "idle", "armed", "white-lifted", "black-armed", "black-lifted", "done"};
    Serial.printf("[SIM STATUS] KING UART raw=%lu, buffered=%u, stage=%s\r\n",
                  static_cast<unsigned long>(rawUartRxBytes),
                  static_cast<unsigned>(uartFrameLength),
                  kStageNames[static_cast<int>(moveSimStage)]);
    lastSimulatorStatusMs = nowMs;
  }
#elif BOARD_ISOLATION_TEST_MODE
  runBoardIsolationTest();
#else
#if RAW_TRANSPARENT_GATEWAY
  runRawTransparentGateway();

  const uint32_t nowMs = millis();
  if (!bleConnected() &&
      static_cast<uint32_t>(nowMs - lastConnectAttemptMs) >= kReconnectIntervalMs) {
    lastConnectAttemptMs = nowMs;
    connectBoard();
  }

  static uint32_t lastRawStatusMs = 0;
  if (static_cast<uint32_t>(nowMs - lastRawStatusMs) >= 10000) {
    Serial.printf("RAW gateway: BLE=%s, UART->BLE=%lu, BLE->UART=%lu bytes\r\n",
                  bleConnected() ? "connected" : "offline",
                  static_cast<unsigned long>(uartToBleBytes),
                  static_cast<unsigned long>(bleToUartBytes));
    lastRawStatusMs = nowMs;
  }
#else
  receiveFromMillenniumComputer();
  sendBleDataToMillenniumComputer();
  transmitQueuedUartFrame();

  // Every real status CHANGE is already forwarded to King immediately, the
  // instant it arrives from the real board (see sendBleDataToMillenniumComputer).
  // This periodic check used to unconditionally re-blast the same cached
  // status to King every interval tick regardless of whether anything
  // changed -- at the real board's actual ~62ms scan rate that's a
  // continuous flood of identical 67-byte frames, and a live capture showed
  // King's own cable RX line picking up stray bytes matching the flood's
  // repeated 'p' (pawn) characters -- almost certainly crosstalk/overrun
  // from this needless traffic, not anything King itself sent. CynusLink
  // (the proven-working reference) only ever sends on an actual state
  // change, never unconditionally -- match that: only resend here if the
  // cache differs from what we last actually put on the wire to King.
  if (haveCachedBoardStatus &&
      static_cast<uint32_t>(millis() - lastAutonomousStatusMs) >= autonomousStatusIntervalMs) {
    lastAutonomousStatusMs = millis();
    if (!haveSentStatusToKing ||
        memcmp(lastStatusSentToKing, cachedBoardStatus, sizeof(cachedBoardStatus)) != 0) {
      writeFrameToKing(cachedBoardStatus, sizeof(cachedBoardStatus));
      memcpy(lastStatusSentToKing, cachedBoardStatus, sizeof(lastStatusSentToKing));
      haveSentStatusToKing = true;
    }
  }

  // The real board doesn't always volunteer its position on connect -- keep
  // asking it directly until we have at least one status to work with.
  static uint32_t lastStatusRequestAttemptMs = 0;
  if (bleConnected() && !haveCachedBoardStatus &&
      static_cast<uint32_t>(millis() - lastStatusRequestAttemptMs) >= 2000) {
    requestBoardStatus();
    lastStatusRequestAttemptMs = millis();
  }

  const uint32_t nowMs = millis();
  if (!bleConnected() &&
      static_cast<uint32_t>(nowMs - lastConnectAttemptMs) >= kReconnectIntervalMs) {
    lastConnectAttemptMs = nowMs;
    connectBoard();
  }

  static uint32_t lastStatusMs = 0;
  if (static_cast<uint32_t>(nowMs - lastStatusMs) >= 10000) {
    Serial.printf("Gateway: BLE=%s, UART-RX(raw)=%lu, discarded=%lu, UART->BLE=%lu, BLE->UART=%lu bytes\r\n",
                  bleConnected() ? "connected" : "offline",
                  static_cast<unsigned long>(rawUartRxBytes),
                  static_cast<unsigned long>(discardedUartRxBytes),
                  static_cast<unsigned long>(uartToBleBytes),
                  static_cast<unsigned long>(bleToUartBytes));
    lastStatusMs = nowMs;
  }
#endif
#endif

  delay(1);
}
