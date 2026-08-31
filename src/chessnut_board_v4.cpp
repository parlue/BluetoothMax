#include "chessnut_board_v4.h"

const char kChessnutBoardName[] = "chessnut";
const char kChessnutBoardNameAlt[] = "smart chess";

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

// Chessnut's own piece codes: empty square = 0, white uppercase, black
// lowercase. Confirmed against two independent reverse-engineering projects.
char pieceFromChessnutCode(uint8_t code) {
  static constexpr char kLut[] = {
      '.', 'q', 'k', 'b', 'p', 'n', 'R', 'P', 'r', 'B', 'N', 'Q', 'K'};
  return (code < sizeof(kLut)) ? kLut[code] : '.';
}

void pushToQueue(QueueHandle_t queue, uint8_t* data, size_t length) {
  if (queue == nullptr || length == 0) return;
  RawPacket packet{};
  packet.length = static_cast<uint8_t>(min(length, kMaxNotifyPayload));
  memcpy(packet.data, data, packet.length);
  xQueueSend(queue, &packet, 0);
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

  char board[64];
  memset(board, '.', sizeof(board));
  for (int byteIdx = 0; byteIdx < 32; ++byteIdx) {
    const uint8_t lowerCode = raw[byteIdx] & 0x0f;
    const uint8_t upperCode = raw[byteIdx] >> 4;
    for (int half = 0; half < 2; ++half) {
      const int rawIndex = byteIdx * 2 + half;  // 0..63, H8..A8,...,H1..A1
      const int rank = 8 - (rawIndex / 8);
      const int file0 = 7 - (rawIndex % 8);
      const uint8_t code = (half == 0) ? lowerCode : upperCode;
      board[modeBStatusWireIndex(file0, rank)] = pieceFromChessnutCode(code);
    }
  }

  uint8_t frame[kModeBStatusFrameLength] = {};
  frame[0] = 's';
  memcpy(frame + 1, board, 64);
  computeModeBChecksumHex(frame + 65, frame, 65, cableHostUsesEncodedChecksum);
  onBoardStatusFrame(frame);
}

void handleMiscPacket(const uint8_t* data, size_t length) {
  if (length < 3) return;
  if (data[0] == 0x0f && data[1] == 0x01) {
    Serial.printf("[CHESSNUT] button pressed: %u\r\n", data[2]);
  }
}

}  // namespace

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
}
