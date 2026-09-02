#include "chesslink_server_v6.h"

#include <NimBLEDevice.h>

#include <algorithm>
#include <cstring>

// Ported from CynusLink's own proven ChessLink-server role (real UUIDs,
// checksum/register handling, connection tuning) -- see chesslink_server.h.

namespace {

constexpr char kName[] = "MILLENNIUM CHESS";
constexpr char kServiceUuid[] = "49535343-fe7d-4ae5-8fa9-9fafd205e455";
constexpr char kTxUuid[] = "49535343-1e4d-4bd9-ba61-23c647249616";
constexpr char kRxUuid[] = "49535343-8841-43f4-a8d4-ecbe34729bb3";

NimBLEServer* server = nullptr;
NimBLECharacteristic* txChar = nullptr;
NimBLECharacteristic* rxChar = nullptr;
bool started = false;
bool connected = false;
bool notifyEnabled = false;
uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE;

// Small EEPROM-style register set a real Mode-B board exposes via 'R'/'W'.
// Defaults match CynusLink's own reset defaults (register[1]=0x14 is the
// "board scan time" register other Mode-B hosts read to learn our auto-
// report interval).
uint8_t registers[256] = {};

uint8_t cachedStatus[kModeBStatusFrameLength] = {};
bool haveCachedStatus = false;
uint8_t lastSentStatus[kModeBStatusFrameLength] = {};
bool haveSentStatus = false;

constexpr size_t kMaxNotifyChunk = 64;
struct RawPacket { uint8_t length; uint8_t data[kMaxNotifyChunk]; };
QueueHandle_t rxQueue = nullptr;

uint8_t rxFrame[kFrameBufferSize];
size_t rxFrameLength = 0;

void resetRegisters() {
  memset(registers, 0, sizeof(registers));
  registers[0] = 0x00;
  registers[1] = 0x14;
  registers[2] = 0x00;
  registers[4] = 0x0F;
}

// Sends a logical (plain 7-bit ASCII, no cable parity encoding -- that's a
// physical-RS232-only concern) Mode-B frame, checksummed with the plain
// convention real ChessLink software always expects over BLE, chunked to
// the negotiated peer MTU exactly like CynusLink's own sendCL().
size_t writeFrame(const uint8_t* logicalFrame, size_t length) {
  // Log the reason once per transition instead of silently no-op'ing --
  // chessnut_server.cpp's sendBoardFrame() already does this (its own
  // comment: "nothing happened" was indistinguishable from "we never even
  // tried" from the log alone). chesslink_server.cpp never got the same
  // treatment, which left the BearChess "no LED/status update ever arrives"
  // report (2026-08-31) with no way to tell whether a status change simply
  // never made it out because no client was connected/subscribed yet.
  enum class SendBlock { None, NotInitialized, NotConnected, NotSubscribed };
  static SendBlock lastLoggedBlock = SendBlock::None;
  auto logOnce = [](SendBlock block, const char* message) {
    if (block == lastLoggedBlock) return;
    lastLoggedBlock = block;
    Serial.println(message);
  };
  if (txChar == nullptr) {
    logOnce(SendBlock::NotInitialized, "[CHESSLINK] frame ready but server not initialized -- not sent");
    return 0;
  }
  if (!connected) {
    logOnce(SendBlock::NotConnected, "[CHESSLINK] frame ready but no client connected -- not sent");
    return 0;
  }
  if (!notifyEnabled) {
    logOnce(SendBlock::NotSubscribed,
            "[CHESSLINK] frame ready but client has not subscribed to notifications yet -- not sent");
    return 0;
  }
  lastLoggedBlock = SendBlock::None;

  uint8_t full[kFrameBufferSize];
  memcpy(full, logicalFrame, length);
  computeModeBChecksumHex(full + length, full, length, /*useEncodedConvention=*/false);
  const size_t total = length + 2;

  uint16_t mtu = 23;
  if (server != nullptr && connHandle != BLE_HS_CONN_HANDLE_NONE) {
    const uint16_t peerMtu = server->getPeerMTU(connHandle);
    if (peerMtu >= 23) mtu = peerMtu;
  }
  const size_t maxPayload = mtu > 3 ? static_cast<size_t>(mtu - 3) : 20;

  for (size_t offset = 0; offset < total; offset += maxPayload) {
    const size_t count = std::min(maxPayload, total - offset);
    txChar->setValue(full + offset, count);
    txChar->notify();
    if (offset + count < total) delay(8);
  }
  return total;
}

void sendStatus() {
  if (!haveCachedStatus) return;
  writeFrame(cachedStatus, kModeBStatusFrameLength);
  memcpy(lastSentStatus, cachedStatus, sizeof(lastSentStatus));
  haveSentStatus = true;
}

bool hexNibble(uint8_t c, int& value) {
  c &= 0x7f;
  if (c >= '0' && c <= '9') { value = c - '0'; return true; }
  if (c >= 'A' && c <= 'F') { value = c - 'A' + 10; return true; }
  if (c >= 'a' && c <= 'f') { value = c - 'a' + 10; return true; }
  return false;
}

bool hexByte(uint8_t hi, uint8_t lo, uint8_t& out) {
  int h, l;
  if (!hexNibble(hi, h) || !hexNibble(lo, l)) return false;
  out = static_cast<uint8_t>((h << 4) | l);
  return true;
}

void appendHex(uint8_t* out, size_t& pos, uint8_t value) {
  static constexpr char hex[] = "0123456789ABCDEF";
  out[pos++] = static_cast<uint8_t>(hex[value >> 4]);
  out[pos++] = static_cast<uint8_t>(hex[value & 0x0F]);
}

// Handles one complete, checksum-validated Mode-B command from the
// connected ChessLink client -- CynusLink's own handleCL(), adapted to
// this project's shared per-board dispatch (dispatchLedFrameToBoard()/
// clearBoardLeds()) instead of driving a single hardcoded board type.
void handleFrame(const uint8_t* frame, size_t length) {
  switch (frame[0]) {
    case 'S':
      sendStatus();
      break;
    case 'V':
      writeFrame(reinterpret_cast<const uint8_t*>("v0100"), 5);
      break;
    case 'X':
      clearBoardLeds(currentBoardType());
      writeFrame(reinterpret_cast<const uint8_t*>("x"), 1);
      break;
    case 'T':
      resetRegisters();
      haveSentStatus = false;
      Serial.println("[CHESSLINK] Magic Board reset command received; register defaults restored");
      break;
    case 'R': {
      uint8_t addr;
      if (!hexByte(frame[1], frame[2], addr)) break;
      uint8_t reply[7];
      size_t pos = 0;
      reply[pos++] = 'r';
      appendHex(reply, pos, addr);
      appendHex(reply, pos, registers[addr]);
      writeFrame(reply, pos);
      break;
    }
    case 'W': {
      uint8_t addr, value;
      if (!hexByte(frame[1], frame[2], addr) || !hexByte(frame[3], frame[4], value)) break;
      registers[addr] = value;
      if (addr == 1 || addr == 2 || addr == 3) haveSentStatus = false;
      uint8_t reply[7];
      size_t pos = 0;
      reply[pos++] = 'w';
      appendHex(reply, pos, addr);
      appendHex(reply, pos, value);
      writeFrame(reply, pos);
      break;
    }
    case 'L': {
      // The checksum of a bare 'l' ack is content-independent, so answer
      // instantly instead of waiting on anything else -- matches
      // CynusLink's own sendCL("l") and the cable path's instant l6C ack.
      writeFrame(reinterpret_cast<const uint8_t*>("l"), 1);
      if (length != 167) break;
      Serial.println("[CHESSLINK] L frame received from client, dispatching to connected board");
      dispatchLedFrameToBoard(currentBoardType(), frame);
      break;
    }
    default:
      break;
  }
}

class ServerCallbacks final : public NimBLEServerCallbacks {
 public:
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    connected = true;
    connHandle = info.getConnHandle();
    haveSentStatus = false;
    Serial.printf("[CHESSLINK] client connected %s\r\n", info.getAddress().toString().c_str());
    if (server != nullptr) {
      // Fast connection interval, matching CynusLink's own proven tuning --
      // King's/a real ChessLink host's acceptance window is narrow.
      server->updateConnParams(connHandle, 12, 24, 0, 200);
    }
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    Serial.printf("[CHESSLINK] client disconnected (%d)\r\n", reason);
    connected = false;
    notifyEnabled = false;
    connHandle = BLE_HS_CONN_HANDLE_NONE;
    rxFrameLength = 0;
    NimBLEDevice::startAdvertising();
  }
};

class TxCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t subValue) override {
    notifyEnabled = subValue != 0;
    Serial.printf("[CHESSLINK] client %s notifications on tx characteristic\r\n",
                  notifyEnabled ? "enabled" : "disabled");
    if (notifyEnabled) haveSentStatus = false;
  }
};

class RxCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    // Only pushes raw bytes into a queue -- runs on the BLE host task's own
    // (small) stack, matching every other board driver's own established
    // stack-overflow-avoidance pattern in this project.
    if (rxQueue == nullptr) return;
    const std::string value = characteristic->getValue();
    size_t offset = 0;
    while (offset < value.size()) {
      RawPacket packet{};
      packet.length = static_cast<uint8_t>(std::min(value.size() - offset, kMaxNotifyChunk));
      memcpy(packet.data, value.data() + offset, packet.length);
      xQueueSend(rxQueue, &packet, 0);
      offset += packet.length;
    }
  }
};

ServerCallbacks serverCallbacks;
TxCallbacks txCallbacks;
RxCallbacks rxCallbacks;

void processByte(uint8_t raw) {
  rxFrame[rxFrameLength++] = raw & 0x7f;
  while (rxFrameLength > 0) {
    const size_t expected = modeBCommandLength(rxFrame[0]);
    if (expected == 0) {
      memmove(rxFrame, rxFrame + 1, --rxFrameLength);
      continue;
    }
    if (rxFrameLength < expected) break;
    if (modeBValidBlock(rxFrame, expected)) {
      handleFrame(rxFrame, expected);
    } else {
      Serial.printf("[CHESSLINK] rejected %c frame: bad checksum\r\n", static_cast<char>(rxFrame[0] & 0x7f));
    }
    rxFrameLength -= expected;
    memmove(rxFrame, rxFrame + expected, rxFrameLength);
  }
  if (rxFrameLength == kFrameBufferSize) rxFrameLength = 0;
}

}  // namespace

bool initialized = false;

void chesslinkServerInit() {
  if (initialized) return;
  resetRegisters();
  rxQueue = xQueueCreate(32, sizeof(RawPacket));

  NimBLEDevice::setMTU(128);

  server = NimBLEDevice::createServer();
  NimBLEService* service = server->createService(kServiceUuid);
  txChar = service->createCharacteristic(kTxUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  rxChar = service->createCharacteristic(kRxUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  txChar->setCallbacks(&txCallbacks);
  rxChar->setCallbacks(&rxCallbacks);
  server->start();

  initialized = true;
}

void chesslinkServerStart() {
  if (!initialized || started) return;
  // Claim the shared server-level callbacks and advertising fields only
  // now, not in chesslinkServerInit() -- both this module and
  // chessnut_server.cpp's own Init() run unconditionally at boot (before
  // BT-BT mode selection is known), but NimBLEServer has only one callback
  // slot and NimBLEDevice has only one active advertising payload at a
  // time. Whichever masquerade mode is actually selected is the only one
  // that ever calls its own Start(), so claiming both here -- rather than
  // at Init() -- is what keeps the two modes from clobbering each other.
  server->setCallbacks(&serverCallbacks);

  // enableScanResponse(true) must come before setName() -- NimBLEAdvertising
  // ::setName() only routes the name into the scan response if scan-
  // response mode is already enabled at the moment it's called; called
  // afterwards (the previous order here), it instead tries to fit the name
  // into the primary packet alongside the service UUID, which can silently
  // drop the name if that overflows. See chessnut_server.cpp's own fix for
  // the same bug -- ported back here for consistency once found.
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->enableScanResponse(true);
  advertising->setName(kName);
  advertising->addServiceUUID(kServiceUuid);
  NimBLEDevice::startAdvertising();

  started = true;
  Serial.println("[CHESSLINK] advertising as MILLENNIUM CHESS");
}

bool chesslinkServerConnected() { return connected; }

void chesslinkServerPoll() {
  if (!started) return;
  RawPacket packet;
  while (rxQueue != nullptr && xQueueReceive(rxQueue, &packet, 0) == pdTRUE) {
    for (uint8_t i = 0; i < packet.length; ++i) processByte(packet.data[i]);
  }

  if (connected && notifyEnabled && haveCachedStatus &&
      (!haveSentStatus || memcmp(lastSentStatus, cachedStatus, sizeof(cachedStatus)) != 0)) {
    sendStatus();
  }
}

void chesslinkServerPublishStatus(const uint8_t frame[kModeBStatusFrameLength]) {
  memcpy(cachedStatus, frame, kModeBStatusFrameLength);
  haveCachedStatus = true;
  // Immediate forward, mirroring the cable path's own immediate-forward
  // behavior in onBoardStatusFrame() -- chesslinkServerPoll()'s own
  // resend-on-change check then has nothing new to do until the position
  // changes again.
  if (connected && notifyEnabled) sendStatus();
}

size_t chesslinkServerWriteFrame(const uint8_t* logicalFrame, size_t length) {
  return writeFrame(logicalFrame, length);
}
