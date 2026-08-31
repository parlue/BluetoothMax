#include "chessnut_server.h"

#include <NimBLEDevice.h>

#include <algorithm>
#include <cstring>

#include "chessnut_board.h"
#include "millennium_board.h"

// Ported from gkalab/cer2nut (https://github.com/gkalab/cer2nut, read
// directly as ground truth) -- an open-source bridge that masks a Certabo
// board as a real Chessnut board over BLE, i.e. plays exactly this module's
// role. Its bleuart.cpp gave the real GATT UUIDs and service layout;
// ChessnutConverter.cpp gave the exact board-frame encoding and main-channel
// command set. The board-frame encoding is cross-checked against this
// project's own already-proven client-side decode (chessnut_board.cpp),
// which has been confirmed against the user's real Chessnut GO -- this
// module's buildBoardFrame() below is the exact inverse of that decode.
//
// Unlike Mode-B (ChessLink), Chessnut has no shared framing/checksum layer
// at all: every command is a short, self-contained BLE write, and every
// reply is a short, self-contained BLE notify. There is no byte-stream
// reassembly needed here (contrast chesslink_server.cpp's processByte()) --
// each GATT write already is one complete command.

namespace {

constexpr char kName[] = "Chessnut Air";
constexpr char kServiceBoardUuid[] = "1b7e8261-2877-41c3-b46e-cf057c562023";
constexpr char kCharBoardReadUuid[] = "1b7e8262-2877-41c3-b46e-cf057c562023";
constexpr char kServiceMainUuid[] = "1b7e8271-2877-41c3-b46e-cf057c562023";
constexpr char kCharMainWriteUuid[] = "1b7e8272-2877-41c3-b46e-cf057c562023";
constexpr char kCharMainReadUuid[] = "1b7e8273-2877-41c3-b46e-cf057c562023";

// "upload" and "unknown" services -- present on a real Chessnut board /
// cer2nut's own masquerade but never exercised by this project's own
// client-side chessnut_board.cpp (a real board answers fine without a
// client ever touching them). Added here purely so a client that enumerates
// services to confirm board type sees the exact same profile a genuine
// board exposes -- content/semantics of "unknown" are, per its name, not
// actually known; writes are only logged, never acted on.
constexpr char kServiceUploadUuid[] = "1b7e8281-2877-41c3-b46e-cf057c562023";
constexpr char kCharUploadWriteUuid[] = "1b7e8282-2877-41c3-b46e-cf057c562023";
constexpr char kCharUploadReadUuid[] = "1b7e8283-2877-41c3-b46e-cf057c562023";
constexpr char kServiceUnknownUuid[] = "9e5d1e47-5c13-43a0-8635-82ad38a1386f";
constexpr char kCharUnknownIndicateNotifyWriteUuid[] = "e3dd50bf-f7a7-4e99-838e-570a086c666b";
constexpr char kCharUnknownWriteUuid[] = "92e86c7a-d961-4091-b74f-2409e72efe36";
constexpr char kCharUnknownReadUuid[] = "347f7608-2e2d-47eb-913b-75d4edc4de3b";

// Raw manufacturer-data bytes cer2nut's own real Chessnut-peripheral
// advertises (`main/bleuart.cpp`'s `bleuart_advertise()`) -- a possible
// secondary board-type signal some client apps check alongside the
// advertised name.
constexpr uint8_t kManufacturerData[] = {0x50, 0x44, 0x43, 0x53, 0x0D, 0x54, 0x27, 0x64,
                                          0x00, 0x00, 0x59, 0x95, 0x4f, 0x10, 0x1b, 0x00};

NimBLEServer* server = nullptr;
NimBLECharacteristic* boardReadChar = nullptr;
NimBLECharacteristic* mainWriteChar = nullptr;
NimBLECharacteristic* mainReadChar = nullptr;
bool initialized = false;
bool started = false;
bool connected = false;
uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE;
bool realTimeMode = false;
// Whether the client has actually enabled notifications (written the CCCD)
// on each NOTIFY characteristic -- chesslink_server.cpp gates its own
// sends on this (its proven-working TxCallbacks::onSubscribe); this module
// initially didn't, so a client that connects and issues commands before
// subscribing could have every reply/board-frame notify silently go
// nowhere. Tracked per characteristic since a client could subscribe to one
// before the other.
bool boardNotifyEnabled = false;
bool mainNotifyEnabled = false;

uint8_t cachedStatus[kModeBStatusFrameLength] = {};
bool haveCachedStatus = false;
uint8_t lastSentBoardFrame[38] = {};
bool haveSentBoardFrame = false;

constexpr size_t kMaxNotifyChunk = 64;
struct RawPacket { uint8_t length; uint8_t data[kMaxNotifyChunk]; };
QueueHandle_t rxQueue = nullptr;

// Inverse of chessnut_board.cpp's pieceFromChessnutCode() lookup table --
// independently cross-checked against cer2nut's own stoneToChessnutStone()
// map (identical code for every piece).
uint8_t chessnutCodeFromPiece(char c) {
  switch (c) {
    case 'q': return 1;
    case 'k': return 2;
    case 'b': return 3;
    case 'p': return 4;
    case 'n': return 5;
    case 'R': return 6;
    case 'P': return 7;
    case 'r': return 8;
    case 'B': return 9;
    case 'N': return 10;
    case 'Q': return 11;
    case 'K': return 12;
    default: return 0;
  }
}

uint8_t setLowerNibble(uint8_t orig, uint8_t nibble) { return (orig & 0xF0) | (nibble & 0x0F); }
uint8_t setUpperNibble(uint8_t orig, uint8_t nibble) { return (orig & 0x0F) | ((nibble & 0x0F) << 4); }

// Builds Chessnut's own 38-byte real-time board frame (0x01 0x24 + 32 bytes
// of nibble-packed piece codes + 4-byte timestamp) from the canonical
// Mode-B 's' status frame -- exact inverse of chessnut_board.cpp's
// handleBoardDataPacket(), which established the rawIndex <-> (file,rank)
// mapping against a real Chessnut GO.
void buildBoardFrame(const uint8_t modeBFrame[kModeBStatusFrameLength], uint8_t out[38]) {
  const uint8_t* board = modeBFrame + 1;  // 64 piece chars, modeBStatusWireIndex layout
  out[0] = 0x01;
  out[1] = 0x24;
  uint8_t raw[32] = {};
  for (int rawIndex = 0; rawIndex < 64; ++rawIndex) {
    const int byteIdx = rawIndex / 2;
    const int rank = 8 - (rawIndex / 8);
    const int file0 = 7 - (rawIndex % 8);
    const uint8_t code = chessnutCodeFromPiece(
        static_cast<char>(board[modeBStatusWireIndex(file0, rank)]));
    if (rawIndex % 2 == 0) {
      raw[byteIdx] = setLowerNibble(raw[byteIdx], code);
    } else {
      raw[byteIdx] = setUpperNibble(raw[byteIdx], code);
    }
  }
  memcpy(out + 2, raw, 32);
  // No real-time clock on this board -- a monotonic pseudo-timestamp is
  // enough, since real Chessnut client apps only display/log this field,
  // never validate it against wall-clock time.
  const uint32_t seconds = millis() / 1000;
  out[34] = static_cast<uint8_t>(seconds & 0xFF);
  out[35] = static_cast<uint8_t>((seconds >> 8) & 0xFF);
  out[36] = static_cast<uint8_t>((seconds >> 16) & 0xFF);
  out[37] = static_cast<uint8_t>((seconds >> 24) & 0xFF);
}

void sendBoardFrame() {
  // Every one of these guards used to live in the two callers below instead
  // (chessnutServerPublishStatus()/chessnutServerPoll(), each gating on
  // `connected && realTimeMode` before ever calling this function) -- which
  // meant a board change that arrived before the client had subscribed/
  // entered real-time mode produced NO log line at all, not even a
  // rejected-attempt one. That made "nothing happened" indistinguishable
  // from "we never even tried" from the log alone. Moving every guard in
  // here, each with its own log line, means every board change now leaves a
  // visible trail of exactly why it was or wasn't sent.
  if (!haveCachedStatus) return;  // nothing to send yet, not an error
  if (boardReadChar == nullptr) return;  // not initialized yet, not an error

  // Keep the characteristic's underlying value current unconditionally, not
  // just when about to notify() -- boardReadChar also declares READ (not
  // NOTIFY-only, unlike cer2nut's own board_read_uuid), so a client that
  // polls the current position with a plain GATT Read instead of
  // subscribing to notifications (per the user's contact "Lars" -- BearChess
  // is known to prefer working from whole-board-status reads over move-by-
  // move deltas, including for the ChessLink side) gets real data even
  // before real-time mode has ever been requested, rather than whatever
  // all-zero default the characteristic started with.
  {
    uint8_t frame[38];
    buildBoardFrame(cachedStatus, frame);
    boardReadChar->setValue(frame, sizeof(frame));
  }

  // Log only on an actual state transition, not on every call -- with the
  // guards moved in here, chessnutServerPoll() calls this on every loop()
  // tick (~1000/s) for as long as any precondition below stays unmet, since
  // haveSentBoardFrame never latches true until a frame actually goes out.
  // Logging every one of those calls flooded the serial log uselessly.
  enum class Reason { Ok, NotConnected, NoRealTimeMode, NotSubscribed };
  static Reason lastLoggedReason = Reason::Ok;
  auto logOnce = [](Reason reason, const char* message) {
    if (reason == lastLoggedReason) return;
    lastLoggedReason = reason;
    Serial.println(message);
  };

  if (!connected) {
    logOnce(Reason::NotConnected, "[CHESSNUT] board changed but no client connected -- not sent");
    return;
  }
  if (!realTimeMode) {
    logOnce(Reason::NoRealTimeMode,
            "[CHESSNUT] board changed but client has not entered real-time mode -- not sent");
    return;
  }
  if (!boardNotifyEnabled) {
    logOnce(Reason::NotSubscribed,
            "[CHESSNUT] board changed but client has not subscribed to notifications yet -- not sent");
    return;
  }
  lastLoggedReason = Reason::Ok;
  uint8_t frame[38];
  buildBoardFrame(cachedStatus, frame);  // value already set above; rebuilt here only to track what was sent
  boardReadChar->notify();
  memcpy(lastSentBoardFrame, frame, sizeof(frame));
  haveSentBoardFrame = true;
  Serial.println("[CHESSNUT] board frame notified to client");
}

void sendMainReply(const uint8_t* data, size_t length) {
  if (mainReadChar == nullptr || !connected) return;
  if (!mainNotifyEnabled) {
    Serial.println("[CHESSNUT] reply ready but client has not subscribed to main-read notifications yet -- not sent");
    return;
  }
  mainReadChar->setValue(data, length);
  mainReadChar->notify();
}

void logHex(const char* label, const uint8_t* data, size_t length) {
  Serial.printf("[CHESSNUT] %s (%u bytes):", label, static_cast<unsigned>(length));
  for (size_t i = 0; i < length; ++i) Serial.printf(" %02X", data[i]);
  Serial.println();
}

// Decodes Chessnut's own 8-byte-per-rank LED bitmask (byte 0 = rank 8 ...
// byte 7 = rank 1, MSB = file a, matching this project's own
// chessnutSetHighlightedSquares() encoding exactly) into SquareHighlight
// entries and relays them to whichever real board is currently connected --
// same per-board-type dispatch main.cpp's showBtBtSignal() already uses,
// not the Mode-B dispatchLedFrameToBoard() path (that one's input is King's
// own 167-byte 'L' frame shape, which doesn't apply here: a Chessnut LED
// command is a plain "light these squares" bitmask with no source/
// destination distinction, so SquareHighlightRole::Generic is the only
// correct choice, not a lossy simplification of richer input).
void relayLedCommandToBoard(const uint8_t bytes8[8]) {
  SquareHighlight highlights[64];
  size_t count = 0;
  for (int byteOffset = 0; byteOffset < 8; ++byteOffset) {
    const int rank = 8 - byteOffset;
    const uint8_t bits = bytes8[byteOffset];
    for (int file0 = 0; file0 < 8; ++file0) {
      const uint8_t fileBit = static_cast<uint8_t>(0x80 >> file0);
      if (bits & fileBit) {
        highlights[count++] = {
            static_cast<uint8_t>(boardSquareIndex(static_cast<char>('a' + file0), rank)),
            SquareHighlightRole::Generic};
      }
    }
  }

  switch (currentBoardType()) {
    case BoardType::Millennium: {
      uint8_t squareIndices[64];
      for (size_t i = 0; i < count; ++i) squareIndices[i] = highlights[i].squareIndex;
      uint8_t frame167[167];
      encodeLedFrame(squareIndices, count, frame167, /*useEncodedChecksum=*/false);
      millenniumRelayLedFrame(frame167, 167);
      break;
    }
    case BoardType::Chessnut:
      chessnutSetHighlightedSquares(highlights, count);
      break;
    default:
      // Cynus has no per-square LEDs of its own; nothing to relay.
      break;
  }
}

// Handles one complete command from the connected Chessnut client's main
// write characteristic -- ported from cer2nut's own
// chessnutToCertaboCommand(), replacing its Certabo-specific info replies
// (battery/version/files) with plausible fixed values (a real client only
// displays these, never validates their exact content) and its LED-command
// handling with relayLedCommandToBoard() above.
void handleMainWrite(const uint8_t* data, size_t length) {
  static constexpr uint8_t kAck[] = {0x23, 0x01, 0x00};
  logHex("received", data, length);
  if (length >= 3 && data[0] == 0x21 && data[1] == 0x01 && data[2] == 0x00) {
    // Enter real-time mode: ack, then push the current board immediately --
    // mirrors what a real board does right after this command, which this
    // project's own chessnut_board.cpp client already relies on.
    Serial.println("[CHESSNUT] client entered real-time mode");
    realTimeMode = true;
    sendMainReply(kAck, sizeof(kAck));
    haveSentBoardFrame = false;
    sendBoardFrame();
  } else if (length >= 3 && data[0] == 0x21 && data[1] == 0x01 && data[2] == 0x01) {
    Serial.println("[CHESSNUT] client left real-time mode (upload mode)");
    realTimeMode = false;
    sendMainReply(kAck, sizeof(kAck));
  } else if (length >= 3 && data[0] == 0x29 && data[1] == 0x01 && data[2] == 0x00) {
    static constexpr uint8_t kBattery[] = {0x2a, 0x02, 0x64, 0x00};  // full, not charging
    sendMainReply(kBattery, sizeof(kBattery));
  } else if (length >= 3 && data[0] == 0x31 && data[1] == 0x01 && data[2] == 0x00) {
    static constexpr uint8_t kFileCount[] = {0x32, 0x01, 0x00};  // zero saved games
    sendMainReply(kFileCount, sizeof(kFileCount));
  } else if (length >= 3 && data[0] == 0x26 && data[1] == 0x01 && data[2] == 0x00) {
    const uint32_t seconds = millis() / 1000;
    const uint8_t reply[6] = {0x2d, 0x04,
                               static_cast<uint8_t>(seconds & 0xFF),
                               static_cast<uint8_t>((seconds >> 8) & 0xFF),
                               static_cast<uint8_t>((seconds >> 16) & 0xFF),
                               static_cast<uint8_t>((seconds >> 24) & 0xFF)};
    sendMainReply(reply, sizeof(reply));
  } else if (length >= 3 && data[0] == 0x27 && data[1] == 0x01 && data[2] == 0x00) {
    static constexpr uint8_t kVersion[] = {0x28, 0x0D, 0x00, 'B', 'L', 'U', 'E', 'T',
                                            'O',  'O',  'T',  'H', 'M', 'A', 'X'};
    sendMainReply(kVersion, sizeof(kVersion));
  } else if (length >= 6 && data[0] == 0x0b && data[1] == 0x04) {
    sendMainReply(kAck, sizeof(kAck));  // sound command -- no sound hardware to drive
  } else if (length >= 10 && data[0] == 0x0a && data[1] == 0x08) {
    Serial.println("[CHESSNUT] LED command received, relaying to connected board");
    sendMainReply(kAck, sizeof(kAck));
    relayLedCommandToBoard(data + 2);
  } else {
    Serial.println("[CHESSNUT] unrecognized command, ignored");
  }
}

class ServerCallbacks final : public NimBLEServerCallbacks {
 public:
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    connected = true;
    connHandle = info.getConnHandle();
    realTimeMode = false;
    haveSentBoardFrame = false;
    boardNotifyEnabled = false;
    mainNotifyEnabled = false;
    Serial.printf("[CHESSNUT] client connected %s\r\n", info.getAddress().toString().c_str());
    // Deliberately NOT calling server->updateConnParams() here, unlike
    // chesslink_server.cpp -- that call is justified there by CynusLink's
    // own proven need for a fast interval (King's narrow probe-deadline
    // theory), which has no equivalent for a Chessnut client. cer2nut's own
    // real, proven Chessnut-peripheral onConnect handler (bleuart.cpp)
    // requests no connection-parameter change at all -- carrying this over
    // unreflected was exactly the kind of unrequested, unverified mechanism
    // this project's own workflow rules warn against (see
    // chesslink-workflow-rules memory: "don't add unrequested mechanisms,
    // even when porting a reference project").
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    Serial.printf("[CHESSNUT] client disconnected (%d)\r\n", reason);
    connected = false;
    realTimeMode = false;
    boardNotifyEnabled = false;
    mainNotifyEnabled = false;
    connHandle = BLE_HS_CONN_HANDLE_NONE;
    NimBLEDevice::startAdvertising();
  }

  // Every one of these fires (or doesn't) somewhere between "link connected"
  // and "client starts using GATT" -- none were logged before, so a client
  // that gets stuck in that window left literally no trace beyond the bare
  // onConnect() line. Logging all of them turns that whole window from a
  // black box into a visible sequence, whatever it turns out to contain.
  void onMTUChange(uint16_t mtu, NimBLEConnInfo& info) override {
    Serial.printf("[CHESSNUT] MTU changed to %u for %s\r\n", mtu,
                  info.getAddress().toString().c_str());
  }
  void onConnParamsUpdate(NimBLEConnInfo& info) override {
    Serial.printf("[CHESSNUT] connection parameters updated for %s\r\n",
                  info.getAddress().toString().c_str());
  }
  void onPhyUpdate(NimBLEConnInfo& info, uint8_t txPhy, uint8_t rxPhy) override {
    Serial.printf("[CHESSNUT] PHY updated for %s: tx=%u rx=%u\r\n",
                  info.getAddress().toString().c_str(), txPhy, rxPhy);
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    Serial.printf("[CHESSNUT] authentication complete for %s (encrypted=%d, bonded=%d)\r\n",
                  info.getAddress().toString().c_str(), info.isEncrypted(), info.isBonded());
  }
  void onIdentity(NimBLEConnInfo& info) override {
    Serial.printf("[CHESSNUT] identity resolved for %s\r\n", info.getAddress().toString().c_str());
  }
};

class MainWriteCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    // Only queues raw bytes -- runs on the BLE host task's own (small)
    // stack, matching every other board driver's established
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

// Tracks whether the client has actually written the CCCD to enable
// notifications on each NOTIFY characteristic -- see the notifyEnabled
// bools' own comment above for why this matters.
class BoardReadCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t subValue) override {
    boardNotifyEnabled = subValue != 0;
    Serial.printf("[CHESSNUT] client %s notifications on board-read characteristic\r\n",
                  boardNotifyEnabled ? "enabled" : "disabled");
    if (boardNotifyEnabled) haveSentBoardFrame = false;
  }
  // Board-read now also declares READ (see its own creation comment) --
  // this makes a plain GATT Read of the current position visible in the
  // log, since a client doing that instead of subscribing would otherwise
  // leave no trace at all in every log line added so far.
  void onRead(NimBLECharacteristic*, NimBLEConnInfo&) override {
    Serial.println("[CHESSNUT] client read the board-read characteristic directly (not via notify)");
  }
};

class MainReadCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t subValue) override {
    mainNotifyEnabled = subValue != 0;
    Serial.printf("[CHESSNUT] client %s notifications on main-read characteristic\r\n",
                  mainNotifyEnabled ? "enabled" : "disabled");
  }
};

ServerCallbacks serverCallbacks;
MainWriteCallbacks mainWriteCallbacks;
BoardReadCallbacks boardReadCallbacks;
MainReadCallbacks mainReadCallbacks;

}  // namespace

void chessnutServerInit() {
  if (initialized) return;
  rxQueue = xQueueCreate(16, sizeof(RawPacket));

  NimBLEDevice::setMTU(128);
  server = NimBLEDevice::createServer();  // singleton -- same server instance chesslink_server.cpp uses

  NimBLEService* boardService = server->createService(kServiceBoardUuid);
  // READ added in addition to cer2nut's own NOTIFY-only flags: a client
  // that polls the current position with a plain GATT Read instead of
  // subscribing (see sendBoardFrame()'s own comment) needs this to get
  // anything other than an error/empty response.
  boardReadChar = boardService->createCharacteristic(
      kCharBoardReadUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  boardReadChar->setCallbacks(&boardReadCallbacks);

  NimBLEService* mainService = server->createService(kServiceMainUuid);
  mainWriteChar = mainService->createCharacteristic(
      kCharMainWriteUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  mainReadChar = mainService->createCharacteristic(kCharMainReadUuid, NIMBLE_PROPERTY::NOTIFY);
  mainWriteChar->setCallbacks(&mainWriteCallbacks);
  mainReadChar->setCallbacks(&mainReadCallbacks);

  // "upload"/"unknown" services -- present so service discovery sees a
  // profile matching a genuine board (see the UUID block's comment above).
  // Any write here is just queued through the same path as the main write
  // characteristic -- handleMainWrite()'s existing "unrecognized command"
  // fallback already logs it, which is all we need: visibility into whether
  // a client touches these at all.
  NimBLEService* uploadService = server->createService(kServiceUploadUuid);
  NimBLECharacteristic* uploadWriteChar = uploadService->createCharacteristic(
      kCharUploadWriteUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  uploadService->createCharacteristic(kCharUploadReadUuid, NIMBLE_PROPERTY::NOTIFY);
  uploadWriteChar->setCallbacks(&mainWriteCallbacks);

  NimBLEService* unknownService = server->createService(kServiceUnknownUuid);
  NimBLECharacteristic* unknownIndicateNotifyWriteChar = unknownService->createCharacteristic(
      kCharUnknownIndicateNotifyWriteUuid,
      NIMBLE_PROPERTY::INDICATE | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::WRITE);
  NimBLECharacteristic* unknownWriteChar =
      unknownService->createCharacteristic(kCharUnknownWriteUuid, NIMBLE_PROPERTY::WRITE);
  unknownService->createCharacteristic(kCharUnknownReadUuid, NIMBLE_PROPERTY::READ);
  unknownIndicateNotifyWriteChar->setCallbacks(&mainWriteCallbacks);
  unknownWriteChar->setCallbacks(&mainWriteCallbacks);

  server->start();

  initialized = true;
}

void chessnutServerStart() {
  if (!initialized || started) return;
  // Claim the shared server-level callbacks and advertising fields only
  // now, not in chessnutServerInit() -- both this module and
  // chesslink_server.cpp's own Init() run unconditionally at boot (before
  // BT-BT mode selection is known), but NimBLEServer has only one callback
  // slot and NimBLEDevice has only one active advertising payload at a
  // time. Whichever masquerade mode is actually selected (this or
  // ChessLink) is the only one that ever calls its own Start(), so claiming
  // both here -- rather than at Init() -- is what keeps the two modes from
  // clobbering each other.
  server->setCallbacks(&serverCallbacks);

  // The advertised local name (set below) is a separate thing from the
  // standard GAP service's Device Name characteristic (0x2A00), which
  // NimBLEDevice::init("BluetoothMax") set once at boot and would otherwise
  // stay "BluetoothMax" for the rest of this connection. A client that reads
  // that characteristic to confirm the board type it just connected to
  // would see the wrong name -- cer2nut's own real Chessnut-peripheral code
  // sets this explicitly for the same reason (`ble_svc_gap_device_name_set`
  // in its BleUart::init()). Same shared-singleton-state reasoning as the
  // callbacks/advertising above: only set this once the mode is actually
  // chosen, not in Init().
  NimBLEDevice::setDeviceName(kName);

  // Switch to a random (not public/factory-MAC) advertising address for
  // this masquerade specifically. This device's public address has stayed
  // constant across every firmware iteration flashed this session -- if
  // Windows (or BearChess's underlying Bluetooth API) is silently reusing a
  // GATT service-table cache keyed by that address from an earlier, broken
  // build, no firmware fix would ever be visible to it. NimBLE generates a
  // fresh random address on each boot when this mode is selected, which
  // defeats any such per-address cache outright -- a firmware-only test
  // that needs no action in Windows. Scoped to just this function (not
  // Init()) so it can't affect the already-established BLE client
  // connection to the real board, or chesslink_server.cpp's own advertising
  // if that mode is chosen instead.
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);

  // Deliberately matching cer2nut's own primary-advertising-packet contents
  // exactly, byte for byte: flags (added automatically by the stack) + TX
  // power + the 16-byte manufacturer data = 24 bytes, well under the
  // 31-byte legacy advertising PDU limit. cer2nut puts the name ONLY in the
  // scan response, and never advertises a service UUID at all in either
  // packet. addServiceUUID() below (removed) was an addition of my own, not
  // present in cer2nut -- a 128-bit UUID is 18 bytes, which combined with
  // the manufacturer data alone already exceeds the 31-byte primary-packet
  // budget (3+18+18=39 bytes). An oversized/malformed primary packet could
  // easily explain a client that finds the device by name (scan response
  // still parses fine) and connects, but then never trusts it enough to
  // proceed with the actual protocol.
  //
  // Real bug found by re-reading NimBLEAdvertising::setName()'s own source:
  // it only puts the name into the scan response if `m_scanResp` is ALREADY
  // true at the moment it's called -- otherwise it tries the (by then
  // already-full) primary packet instead, and if that overflows too,
  // silently sets no name at all. enableScanResponse(true) MUST be called
  // BEFORE setName(), not after -- the previous ordering here very likely
  // caused the advertised name to be dropped entirely, which would explain
  // a scanning client finding no "Chessnut GO" whatsoever, not just one
  // that connects but stalls.
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->enableScanResponse(true);
  advertising->addTxPower();
  advertising->setManufacturerData(kManufacturerData, sizeof(kManufacturerData));
  advertising->setName(kName);
  NimBLEDevice::startAdvertising();

  started = true;
  Serial.printf("[CHESSNUT] advertising as Chessnut Air, our BLE address is %s "
                "(the real board's is 00:1b:10:51:0a:dd -- a client connecting to the "
                "wrong one would explain total silence despite a correct board-type "
                "selection)\r\n",
                NimBLEDevice::getAddress().toString().c_str());
}

bool chessnutServerConnected() { return connected; }

void chessnutServerPoll() {
  if (!started) return;
  RawPacket packet;
  while (rxQueue != nullptr && xQueueReceive(rxQueue, &packet, 0) == pdTRUE) {
    handleMainWrite(packet.data, packet.length);
  }

  if (haveCachedStatus) {
    uint8_t frame[38];
    buildBoardFrame(cachedStatus, frame);
    if (!haveSentBoardFrame || memcmp(lastSentBoardFrame, frame, sizeof(frame)) != 0) {
      sendBoardFrame();  // logs its own reason if it can't actually send yet
    }
  }
}

void chessnutServerPublishStatus(const uint8_t frame[kModeBStatusFrameLength]) {
  memcpy(cachedStatus, frame, kModeBStatusFrameLength);
  haveCachedStatus = true;
  sendBoardFrame();  // logs its own reason if it can't actually send yet
}
