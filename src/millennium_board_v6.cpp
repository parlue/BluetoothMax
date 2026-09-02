#include "millennium_board_v6.h"

const char kMillenniumBoardName[] = "MILLENNIUM CHESS";

namespace {

constexpr char kServiceUuid[] = "49535343-fe7d-4ae5-8fa9-9fafd205e455";
constexpr char kBoardTxUuid[] = "49535343-1e4d-4bd9-ba61-23c647249616";
constexpr char kBoardRxUuid[] = "49535343-8841-43f4-a8d4-ecbe34729bb3";
constexpr uint32_t kBleFrameSpacingMs = 100;
constexpr uint32_t kFallbackAutoReportIntervalMs = 41;
constexpr uint32_t kConnectTimeoutMs = 8000;

struct ProtocolFrame {
  uint16_t length;
  uint8_t data[kFrameBufferSize];
};

NimBLEClient* bleClient = nullptr;
NimBLERemoteCharacteristic* boardRx = nullptr;
QueueHandle_t bleToUartQueue = nullptr;
QueueHandle_t uartToBleQueue = nullptr;

uint8_t bleFrame[kFrameBufferSize];
size_t bleFrameLength = 0;
uint32_t lastBleFrameSentMs = 0;

bool suppressNextRealLedAck = false;
bool suppressNextXAck = false;
bool pendingRegisterQuery1 = false;
bool pendingRegisterQuery2 = false;
bool pendingVersionQuery = false;

// Single-slot "latest LED frame" instead of a FIFO queue entry -- added
// 2026-08-31. Phoenix blinks its suggestion (re-sends 'L' frequently,
// unlike King's one-shot steady display), and each new frame fully
// supersedes the last: there is no reason to ever display an intermediate
// blink phase once a newer one has arrived. Queuing every one (the old
// behavior, via queueFrameForBle()) let them pile up faster than the
// kBleFrameSpacingMs-throttled send rate could drain, so what the real
// board displayed lagged further and further behind Phoenix's actual
// current intent. A new 'L' frame simply overwrites this slot; at most one
// is ever in flight.
uint8_t pendingLedFrame[167] = {};
bool havePendingLedFrame = false;

void queueFrameForBle(const uint8_t* frame, size_t length) {
  if (length == 0 || uartToBleQueue == nullptr) return;
  ProtocolFrame pending{};
  pending.length = static_cast<uint16_t>(length);
  memcpy(pending.data, frame, length);
  const BaseType_t queued = xQueueSendToBack(uartToBleQueue, &pending, 0);
  if (queued != pdTRUE) {
    ProtocolFrame oldest{};
    xQueueReceive(uartToBleQueue, &oldest, 0);
    xQueueSendToBack(uartToBleQueue, &pending, 0);
  }
}

void onBoardNotification(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
  for (size_t i = 0; i < length; ++i) {
    const uint8_t ascii = data[i] & 0x7f;
    xQueueSend(bleToUartQueue, &ascii, 0);
  }
}

class ClientCallbacks final : public NimBLEClientCallbacks {
 public:
  void onConnect(NimBLEClient*) override {}
  void onDisconnect(NimBLEClient*, int) override {
    boardRx = nullptr;
    suppressNextRealLedAck = false;
    suppressNextXAck = false;
    havePendingLedFrame = false;
    pendingRegisterQuery1 = false;
    pendingRegisterQuery2 = false;
    pendingVersionQuery = false;
    autonomousStatusIntervalMs = kFallbackAutoReportIntervalMs;
    Serial.println("BLE disconnected; reconnecting automatically.");
  }
};

void requestBoardStatus() {
  if (!millenniumIsConnected()) return;
  static constexpr uint8_t statusRequest[] = {'S', '5', '3'};
  uint8_t encoded[sizeof(statusRequest)] = {};
  for (size_t i = 0; i < sizeof(statusRequest); ++i) encoded[i] = encodeOddParity(statusRequest[i]);
  boardRx->writeValue(encoded, sizeof(encoded), true);
  lastBleFrameSentMs = millis();
}

// Reads one of the real board's own EEPROM-style registers. The reply is
// intercepted in millenniumPoll() and consumed internally, not forwarded to
// King (King itself never sends 'R' on the cable).
void queryBoardRegister(uint8_t addr) {
  if (!millenniumIsConnected()) return;
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
  lastBleFrameSentMs = millis();
}

// Queries the board's firmware version on connect. The reply is intercepted
// and consumed internally, not forwarded to King.
void queryBoardVersion() {
  if (!millenniumIsConnected()) return;
  static constexpr char hex[] = "0123456789ABCDEF";
  uint8_t frame[3] = {'V', 0, 0};
  uint8_t checksum = frame[0] & 0x7f;
  frame[1] = static_cast<uint8_t>(hex[checksum >> 4]);
  frame[2] = static_cast<uint8_t>(hex[checksum & 0x0f]);
  uint8_t encoded[3];
  for (size_t i = 0; i < 3; ++i) encoded[i] = encodeOddParity(frame[i]);
  boardRx->writeValue(encoded, sizeof(encoded), true);
  lastBleFrameSentMs = millis();
}

void sendEncodedFrame(const uint8_t* frame, size_t length) {
  const uint8_t firstByte = frame[0] & 0x7f;
  uint8_t encoded[kFrameBufferSize];
  for (size_t i = 0; i < length; ++i) encoded[i] = encodeOddParity(frame[i]);
  const size_t mtuPayload = bleClient->getMTU() > 3 ? bleClient->getMTU() - 3 : 20;
  size_t offset = 0;
  while (offset < length && millenniumIsConnected()) {
    const size_t chunk = min(mtuPayload, length - offset);
    boardRx->writeValue(encoded + offset, chunk, true);
    offset += chunk;
  }
  lastBleFrameSentMs = millis();
  // Diagnostic added 2026-08-31 while chasing "T2's own LEDs never light" --
  // confirms whether an 'L' frame from King/Phoenix actually made it out
  // over BLE to the real board at all, since nothing else logs this send.
  if (firstByte == 'L') {
    Serial.printf("UART -> BLE: L frame relayed to real board, %u bytes\r\n",
                  static_cast<unsigned>(length));
  }
}

void transmitQueuedFrame() {
  if (!millenniumIsConnected()) return;
  if (lastBleFrameSentMs != 0 &&
      static_cast<uint32_t>(millis() - lastBleFrameSentMs) < kBleFrameSpacingMs) {
    return;
  }
  // The latest LED frame (see havePendingLedFrame's own comment) always
  // takes priority over the generic command queue -- there's nothing more
  // time-sensitive than what's currently being displayed.
  if (havePendingLedFrame) {
    havePendingLedFrame = false;
    sendEncodedFrame(pendingLedFrame, sizeof(pendingLedFrame));
    return;
  }
  if (uartToBleQueue == nullptr) return;
  ProtocolFrame pending{};
  if (xQueueReceive(uartToBleQueue, &pending, 0) != pdTRUE) return;
  sendEncodedFrame(pending.data, pending.length);
}

}  // namespace

bool millenniumIsConnected() {
  return bleClient != nullptr && bleClient->isConnected() && boardRx != nullptr;
}

bool millenniumConnect(const NimBLEAddress& address) {
  if (bleClient == nullptr) {
    bleClient = NimBLEDevice::createClient();
    bleClient->setClientCallbacks(new ClientCallbacks(), true);
    bleClient->setConnectTimeout(kConnectTimeoutMs);
  }
  if (bleToUartQueue == nullptr) bleToUartQueue = xQueueCreate(1024, sizeof(uint8_t));
  if (uartToBleQueue == nullptr) uartToBleQueue = xQueueCreate(256, sizeof(ProtocolFrame));

  // Request a fast connection interval before connecting -- a slow/default
  // interval can silently defeat low-latency status forwarding.
  bleClient->setConnectionParams(12, 24, 0, 200);

  Serial.printf("Connecting to %s ...\r\n", address.toString().c_str());
  if (!bleClient->connect(address)) {
    Serial.println("BLE connection failed.");
    return false;
  }

  NimBLERemoteService* service = bleClient->getService(kServiceUuid);
  if (service == nullptr) {
    Serial.println("ChessLink BLE service not found.");
    bleClient->disconnect();
    return false;
  }

  NimBLERemoteCharacteristic* boardTx = service->getCharacteristic(kBoardTxUuid);
  boardRx = service->getCharacteristic(kBoardRxUuid);
  if (boardTx == nullptr || boardRx == nullptr) {
    Serial.println("ChessLink BLE UART characteristics not found.");
    boardRx = nullptr;
    bleClient->disconnect();
    return false;
  }
  if (!boardTx->canNotify() || !boardTx->subscribe(true, onBoardNotification)) {
    Serial.println("Could not enable board notifications.");
    boardRx = nullptr;
    bleClient->disconnect();
    return false;
  }

  Serial.printf("BLE gateway connected; negotiated MTU=%u.\r\n", bleClient->getMTU());
  resetKingLedFrameBaseline();

  pendingVersionQuery = true;
  queryBoardVersion();
  pendingRegisterQuery1 = true;
  pendingRegisterQuery2 = true;
  queryBoardRegister(1);  // "board scan time" -- drives our auto-report interval.
  queryBoardRegister(2);  // auto-report mode -- logged only, for diagnostics.
  return true;
}

void millenniumRequestBoardStatus() {
  requestBoardStatus();
}

void millenniumRelayLedFrame(const uint8_t* frame167, size_t length) {
  if (length != sizeof(pendingLedFrame)) return;  // defensive: always 167 in practice
  memcpy(pendingLedFrame, frame167, length);
  havePendingLedFrame = true;
}

void millenniumRelayCommand(const uint8_t* frame, size_t length) {
  queueFrameForBle(frame, length);
}

void millenniumSuppressNextLedAck() {
  suppressNextRealLedAck = true;
}

void millenniumClearLeds() {
  if (!millenniumIsConnected()) return;
  static constexpr uint8_t offLedCommand[] = {'X', '5', '8'};
  uint8_t encoded[sizeof(offLedCommand)] = {};
  for (size_t i = 0; i < sizeof(offLedCommand); ++i) encoded[i] = encodeOddParity(offLedCommand[i]);
  boardRx->writeValue(encoded, sizeof(encoded), true);
  lastBleFrameSentMs = millis();
  // King/Phoenix never asked for this X -- it's purely internal housekeeping
  // (clearing a stale LED suggestion before the next one is shown). The
  // real board's own genuine 'x' ack must not be forwarded to the cable
  // host, which never sent a matching X request and has no way to make
  // sense of an unsolicited reply -- mirrors suppressNextRealLedAck for 'l'.
  suppressNextXAck = true;
}

void millenniumPoll() {
  transmitQueuedFrame();

  if (!millenniumIsConnected() || bleToUartQueue == nullptr) return;

  uint8_t ascii = 0;
  while (xQueueReceive(bleToUartQueue, &ascii, 0) == pdTRUE) {
    if (bleFrameLength == sizeof(bleFrame)) bleFrameLength = 0;
    bleFrame[bleFrameLength++] = ascii & 0x7f;
    while (bleFrameLength > 0) {
      const size_t expected = modeBReplyLength(bleFrame[0]);
      if (expected == 0) {
        memmove(bleFrame, bleFrame + 1, --bleFrameLength);
        continue;
      }
      if (bleFrameLength < expected) break;
      if (modeBValidBlock(bleFrame, expected)) {
        // 'v'/'r' replies here answer our own proactive queries on connect;
        // King never asks for these, so they're consumed internally.
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

        const bool redundantLedAck = bleFrame[0] == 'l' && suppressNextRealLedAck;
        const bool redundantXAck = bleFrame[0] == 'x' && suppressNextXAck;
        if (redundantLedAck) {
          // King already got an instant local 'l' ack for its L command;
          // the real board's own genuine ack would be redundant.
          suppressNextRealLedAck = false;
        } else if (redundantXAck) {
          // Ack for our own internal clear-LEDs housekeeping command (see
          // millenniumClearLeds()) -- King/Phoenix never sent a matching X,
          // so forwarding this would hand it an unsolicited reply it has no
          // request to pair it against.
          suppressNextXAck = false;
        } else if (suppressAsOwnQueryReply) {
          // Consumed above; King never asked for this.
        } else if (bleFrame[0] == 's') {
          onBoardStatusFrame(bleFrame);
        } else {
          const size_t written = writeFrameToKing(bleFrame, expected);
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

  static uint32_t lastStatusRequestAttemptMs = 0;
  if (!haveAnyBoardStatus() &&
      static_cast<uint32_t>(millis() - lastStatusRequestAttemptMs) >= 2000) {
    requestBoardStatus();
    lastStatusRequestAttemptMs = millis();
  }
}
