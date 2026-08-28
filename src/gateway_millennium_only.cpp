// Frozen snapshot of the proven, single-board (Millennium/ChessLink-only)
// gateway, taken right before the multi-board (Chessnut and beyond) rework
// began. Built by the `esp32-c3-superminiv2` PlatformIO environment and
// never touched by any later work -- flash this to fall back to the exact
// behavior already confirmed working end-to-end against a real King and a
// real Millennium board.

#include <Arduino.h>
#include <BLEDevice.h>
#include <esp_gap_ble_api.h>

namespace {

#ifndef MILLENNIUM_BAUD
#define MILLENNIUM_BAUD 38400UL
#endif

#ifndef BOARD_ISOLATION_TEST_MODE
#define BOARD_ISOLATION_TEST_MODE 0
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
// Fallback report interval; overwritten by the board's own scan-time
// register (see queryBoardRegister()) once known.
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

  // Request a fast connection interval before connecting -- a slow/default
  // interval can silently defeat low-latency status forwarding regardless
  // of how fast the rest of this code reacts.
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
  queryBoardVersion();
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

// Reads one of the real board's own EEPROM-style registers. The reply is
// intercepted in sendBleDataToMillenniumComputer() and consumed internally,
// not forwarded to King (King itself never sends 'R' on the cable).
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

// Queries the board's firmware version on connect. The reply is intercepted
// and consumed internally, not forwarded to King.
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
          // The checksum of a bare 'l' ack is content-independent, so we can
          // answer instantly instead of waiting on a BLE round trip.
          static constexpr uint8_t localLedAck[] = {'l', '6', 'C'};
          writeFrameToKing(localLedAck, sizeof(localLedAck));
          suppressNextRealLedAck = true;

          // King may only accept a reply within a single probe cycle, which
          // a fresh BLE round trip can miss -- send the cached status
          // immediately alongside the ack, then also kick off a fresh fetch.
          if (haveCachedBoardStatus) {
            writeFrameToKing(cachedBoardStatus, sizeof(cachedBoardStatus));
          }
          requestBoardStatus();
          ledsAwaitingClear = true;

          static uint8_t lastLedFrame[167] = {};
          static bool haveLastLedFrame = false;
          if (!haveLastLedFrame || memcmp(lastLedFrame, uartFrame, 167) != 0) {
            memcpy(lastLedFrame, uartFrame, 167);
            haveLastLedFrame = true;
            size_t activeLedValues = 0;
            for (size_t i = 3; i < 165; i += 2) {
              if (uartFrame[i] != '0' || uartFrame[i + 1] != '0') ++activeLedValues;
            }
            Serial.printf("[L DIAG] slot=%c%c, active-values=%u, checksum=%c%c\r\n",
                          uartFrame[1], uartFrame[2],
                          static_cast<unsigned>(activeLedValues),
                          uartFrame[165], uartFrame[166]);
            Serial.print("[L RAW] ");
            Serial.write(uartFrame, expected);
            Serial.println();
          }
        }
        queueUartFrameForBle(uartFrame, expected);
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
  // Pre-encode odd parity into bit 7 and send as plain 8N1: this produces
  // the same 10-bit wire waveform as native 7O1 framing, which the ESP32-C3
  // UART hardware cannot generate directly.
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
            // Physically clear the board's setup LEDs once its position is
            // confirmed, so its own genuine 'x' ack flows to King normally.
            clearBoardLeds();
            ledsAwaitingClear = false;
          }
        }
        // King already got an instant local 'l' ack for its L command; the
        // real board's own genuine ack would be redundant.
        const bool redundantLedAck = bleFrame[0] == 'l' && suppressNextRealLedAck;
        if (redundantLedAck) {
          suppressNextRealLedAck = false;
        } else if (suppressAsOwnQueryReply) {
          // Consumed above; King never asked for this.
        } else {
          const size_t written = writeFrameToKing(bleFrame, expected);
          if (bleFrame[0] == 's') {
            // Keep the periodic resend in loop() in sync with what was just
            // sent, so it doesn't immediately resend the same frame.
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
// Drives the real board directly over BLE with no King/cable involved, to
// see what the board does on its own: connect, request status, show a move,
// then log everything it sends afterwards.
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
    static constexpr uint8_t moveFrame[] =
        "L0FFFFFFF000000FFFFFFFFFFFF000000FFFFFFFFFFFF000000FFFFFFFFFFFF"
        "000000FFFFFFFFFFFF000000FFFFFFFFFFFFF0F000FFFFFFFFFFFFF0F000FF"
        "FFFFFFFFFF000000FFFFFFFFFFFF000000FFFFFF3A";
    Serial.println("[ISOLATION TEST] Sending L frame (e2-e4 highlighted).");
    sendRawFrameToBoard(moveFrame, sizeof(moveFrame) - 1);
    testStep = 2;
    Serial.println("[ISOLATION TEST] Now make the move on the board; "
                    "logging everything it sends from here on.");
  }
}
#endif

}  // namespace

void setup() {
  // King sends its Mode-B identification immediately after power-up; start
  // its UART before USB logging and any BLE scan so those bytes aren't lost.
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

  Serial.println("\r\nChessL1nkWireless Mode-B gateway V3.20 setup-L filter");
  Serial.printf("Cable: %lu baud, explicit odd parity over 8N1, RX=GPIO%d, TX=GPIO%d\r\n",
                static_cast<unsigned long>(MILLENNIUM_BAUD),
                kMillenniumRxPin, kMillenniumTxPin);
  Serial.println("WARNING: cable GPIOs only through the 3.3 V MAX3232 TTL side.");

#if BOARD_ISOLATION_TEST_MODE
  Serial.println("MODE: BOARD ISOLATION TEST - driving the real board directly, no King involved.");
  BLEDevice::init("ChessL1nkWireless");
#else
  Serial.println("BLE: connection watchdog only; protocol idle until The King starts it.");
  BLEDevice::init("ChessL1nkWireless");
#endif
}

void loop() {
#if BOARD_ISOLATION_TEST_MODE
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

  // Status changes are already forwarded to King immediately on arrival;
  // this periodic tick only resends if the cache differs from what was
  // last actually put on the wire, to avoid flooding the cable with
  // identical frames at the board's own scan rate.
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
