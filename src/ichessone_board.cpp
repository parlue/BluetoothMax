#include "ichessone_board.h"

const char kIChessOneBoardName[] = "iChessOne";

namespace {

constexpr char kWriteUuid[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char kNotifyUuid[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr uint32_t kConnectTimeoutMs = 8000;
constexpr size_t kMaxNotifyPayload = 48;
constexpr size_t kPositionMessageLength = 34;  // '=' 'p' + 32 raw bytes

struct RawPacket {
  uint8_t length;
  uint8_t data[kMaxNotifyPayload];
};

NimBLEClient* bleClient = nullptr;
NimBLERemoteCharacteristic* writeChar = nullptr;
NimBLERemoteCharacteristic* notifyChar = nullptr;
QueueHandle_t notifyQueue = nullptr;

char currentBoard[64] = {};  // Mode-B wire order, same convention sendCableStatusFrame() expects

// Reassembly buffer for the split "=p<32 bytes>" position message -- see
// this file's own header comment for why this is needed (34 bytes exceeds
// the doc's own default 20-byte notify payload, so the board splits it).
uint8_t positionAssembly[kPositionMessageLength];
size_t positionAssemblyLen = 0;

// iChessOne's own piece codes (vendor doc's value table): 0=. 1=P 2=N 3=B
// 4=R 5=Q 6=K 7=p 8=n 9=b A=r B=q C=k -- already this project's own letter
// convention (uppercase white, lowercase black, '.' empty), pure lookup.
char pieceFromICode(uint8_t code) {
  static constexpr char kLut[] = {
      '.', 'P', 'N', 'B', 'R', 'Q', 'K', 'p', 'n', 'b', 'r', 'q', 'k'};
  return (code < sizeof(kLut)) ? kLut[code] : '.';
}

// Same pattern as chessnut_board.cpp's own sendCableStatusFrame(): plain
// (not cableHostUsesEncodedChecksum) convention -- a real Mode-B board
// always computes its own outgoing checksum over plain content.
void sendCableStatusFrame() {
  uint8_t frame[kModeBStatusFrameLength] = {};
  frame[0] = 's';
  memcpy(frame + 1, currentBoard, 64);
  computeModeBChecksumHex(frame + 65, frame, 65, /*useEncodedConvention=*/false);
  onBoardStatusFrame(frame);
}

// Decodes one complete 32-byte position payload (the "=p" prefix already
// stripped by the caller). 2 squares per byte, upper nibble = first square,
// lower nibble = second, in row-major order starting a8, b8, ... h8, a7,
// ... h1 -- confirmed directly against the doc's own worked starting-
// position example.
void handlePositionMessage(const uint8_t* payload32) {
  for (int byteIdx = 0; byteIdx < 32; ++byteIdx) {
    const uint8_t firstCode = payload32[byteIdx] >> 4;
    const uint8_t secondCode = payload32[byteIdx] & 0x0f;
    for (int half = 0; half < 2; ++half) {
      const int rawIndex = byteIdx * 2 + half;  // 0..63, a8,b8,...,h8,a7,...,h1
      const int rank = 8 - (rawIndex / 8);
      const int file0 = rawIndex % 8;  // 0-based a..h, ascending
      const uint8_t code = (half == 0) ? firstCode : secondCode;
      currentBoard[modeBStatusWireIndex(file0, rank)] = pieceFromICode(code);
    }
  }
  sendCableStatusFrame();
  armVerboseCableLog();
}

void handleNotifyPacket(const uint8_t* data, size_t length) {
  if (length == 0) return;

  // Mid-reassembly: further bytes are a raw continuation of the position
  // payload, not a new framed message -- the board doesn't interleave
  // another reply inside a still-incomplete "=p" transfer.
  if (positionAssemblyLen > 0) {
    const size_t toCopy = min(length, kPositionMessageLength - positionAssemblyLen);
    memcpy(positionAssembly + positionAssemblyLen, data, toCopy);
    positionAssemblyLen += toCopy;
    if (positionAssemblyLen >= kPositionMessageLength) {
      handlePositionMessage(positionAssembly + 2);  // skip '=' 'p'
      positionAssemblyLen = 0;
    }
    return;
  }

  if (length < 2 || data[0] != '=') return;  // not a reply we recognize

  if (data[1] == 'p') {
    const size_t toCopy = min(length, kPositionMessageLength);
    memcpy(positionAssembly, data, toCopy);
    positionAssemblyLen = toCopy;
    if (positionAssemblyLen >= kPositionMessageLength) {
      handlePositionMessage(positionAssembly + 2);
      positionAssemblyLen = 0;
    }
    return;
  }

  // =hxx.xx (hardware version), =bSV (battery), =sOFF (status-off ack) --
  // logged only, nothing currently acts on them.
  Serial.printf("[ICHESSONE] reply: %.*s\r\n", static_cast<int>(length), data);
}

void onNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
  if (notifyQueue == nullptr) return;
  RawPacket packet{};
  packet.length = static_cast<uint8_t>(min(length, kMaxNotifyPayload));
  memcpy(packet.data, data, packet.length);
  xQueueSend(notifyQueue, &packet, 0);
}

class ClientCallbacks final : public NimBLEClientCallbacks {
 public:
  void onConnect(NimBLEClient*) override {}
  void onDisconnect(NimBLEClient*, int) override {
    writeChar = nullptr;
    notifyChar = nullptr;
    positionAssemblyLen = 0;
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

}  // namespace

bool ichessoneIsConnected() {
  return bleClient != nullptr && bleClient->isConnected() && writeChar != nullptr;
}

bool ichessoneConnect(const NimBLEAddress& address) {
  if (bleClient == nullptr) {
    bleClient = NimBLEDevice::createClient();
    bleClient->setClientCallbacks(new ClientCallbacks(), true);
    bleClient->setConnectTimeout(kConnectTimeoutMs);
  }
  if (notifyQueue == nullptr) notifyQueue = xQueueCreate(16, sizeof(RawPacket));

  // Same fast connection interval this project's other BLE board drivers
  // all use (proven necessary for reliable, timely status delivery -- see
  // millennium_board.cpp/chessnut_board.cpp's own identical calls).
  bleClient->setConnectionParams(12, 24, 0, 200);

  Serial.printf("Connecting to %s ...\r\n", address.toString().c_str());
  if (!bleClient->connect(address)) {
    Serial.println("BLE connection failed.");
    return false;
  }

  if (bleClient->getServices(true).empty()) {
    Serial.println("iChessOne BLE service discovery returned nothing.");
    bleClient->disconnect();
    return false;
  }

  writeChar = findCharacteristicInAnyService(bleClient, kWriteUuid);
  notifyChar = findCharacteristicInAnyService(bleClient, kNotifyUuid);
  if (writeChar == nullptr || notifyChar == nullptr) {
    Serial.println("iChessOne BLE characteristics not found.");
    writeChar = nullptr;
    bleClient->disconnect();
    return false;
  }

  if (!notifyChar->canNotify() || !notifyChar->subscribe(true, onNotify)) {
    Serial.println("Could not enable iChessOne notifications.");
    writeChar = nullptr;
    bleClient->disconnect();
    return false;
  }

  // Vendor doc recommends requesting MTU 50 so the position data fits one
  // packet -- NimBLEClient has no per-client setMTU() in this library
  // version (only NimBLEDevice::setMTU(), a global default applied before
  // any connection). Not requesting it here isn't a correctness problem:
  // handleNotifyPacket()'s reassembly already handles the position data
  // arriving split across multiple packets at whatever MTU actually gets
  // negotiated, just with one extra notify round-trip of latency.
  Serial.printf("iChessOne negotiated MTU=%u.\r\n", bleClient->getMTU());

  positionAssemblyLen = 0;
  resetKingLedFrameBaseline();

  Serial.println("iChessOne BLE gateway connected.");
  // CPIRQ: stream position updates only when a piece actually moves, rather
  // than on a fixed timer -- this project is already fully event-driven
  // (every other board's own notify-triggered flow works the same way), so
  // there's no reason to prefer CPMxxxx's periodic-polling alternative.
  writeChar->writeValue(reinterpret_cast<const uint8_t*>("CPIRQ"), 5, true);
  return true;
}

void ichessonePoll() {
  if (!ichessoneIsConnected()) return;
  RawPacket packet{};
  while (xQueueReceive(notifyQueue, &packet, 0) == pdTRUE) {
    handleNotifyPacket(packet.data, packet.length);
  }
}

void ichessoneSetHighlightedSquares(const SquareHighlight* squares, size_t count) {
  if (!ichessoneIsConnected()) return;
  // Fixed steady red, full brightness, no flash -- user's own explicit
  // choice (2026-09-02): only one color is needed, always on rather than
  // blinking, matching this project's established "repeated/rapid LED
  // writes are fragile on real hardware, keep it to a single steady write"
  // convention used everywhere else (see usb_pgn_dump.cpp's own comment on
  // this exact point). i=F brightness, r=F red, g=0, b=0 -> color bytes
  // 0xFF, 0x00. fc=0x01 (no flash, clear previous each call -- so this
  // always fully replaces whatever was lit before, never accumulates).
  // tt=0xFF (on permanently until the next EL command).
  uint8_t command[16] = {'E', 'L', 0xFF, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0xFF};
  for (size_t i = 0; i < count; ++i) {
    const int file0 = squares[i].squareIndex % 8;  // 0-based a..h
    const int rank = squares[i].squareIndex / 8 + 1;
    if (rank < 1 || rank > 8) continue;
    // Row-byte order in the command is row9,row8,...,row1,row0 (command[4]
    // = row9/far bar .. command[13] = row0/near bar), so rank r lives at
    // command[4 + (9 - r)]. File-to-bit mapping (a=bit0 .. h=bit7) confirmed
    // directly from the doc's own worked example (highlighting a1+h8 -> row1
    // byte 0x01, row8 byte 0x80).
    command[4 + (9 - rank)] |= static_cast<uint8_t>(1 << file0);
  }
  writeChar->writeValue(command, sizeof(command), true);
}

void ichessoneClearLeds() { ichessoneSetHighlightedSquares(nullptr, 0); }
