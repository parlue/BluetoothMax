#include <Arduino.h>
#include <NimBLEDevice.h>

#include <algorithm>

#include "board_driver.h"
#include "chesslink_server.h"
#include "chessnut_board.h"
#include "chessnut_server.h"
#include "cynus_board.h"
#include "millennium_board.h"

namespace {

#ifndef MILLENNIUM_BAUD
#define MILLENNIUM_BAUD 38400UL
#endif

constexpr int kMillenniumRxPin = 20;
constexpr int kMillenniumTxPin = 21;
constexpr int kStatusLedPin = 8;
constexpr uint32_t kMonitorBaud = 115200;
constexpr uint32_t kReconnectIntervalMs = 3000;
constexpr uint32_t kFallbackAutoReportIntervalMs = 41;

HardwareSerial MillenniumSerial(1);

uint8_t uartFrame[kFrameBufferSize];
size_t uartFrameLength = 0;
uint32_t rawUartRxBytes = 0;
uint32_t discardedUartRxBytes = 0;

uint8_t cachedBoardStatus[kModeBStatusFrameLength] = {};
bool haveCachedBoardStatus = false;
uint32_t lastAutonomousStatusMs = 0;
uint8_t lastStatusSentToKing[kModeBStatusFrameLength] = {};
bool haveSentStatusToKing = false;
uint8_t lastLoggedStatus[kModeBStatusFrameLength] = {};
bool haveLoggedStatus = false;
bool ledsAwaitingClear = false;

uint32_t lastConnectAttemptMs = 0;
BoardType activeBoardType = BoardType::Unknown;
volatile bool connectInProgress = false;

// --- BT-BT mode: dual-BLE operation when no cable-side host is present ----
//
// If the module is plugged into King/Phoenix, bytes arrive on the cable
// almost immediately (King probes automatically within ~1-2s of a cable
// connection, confirmed elsewhere in this project). If nothing at all
// arrives within kCableDetectTimeoutMs, the module is instead running
// standalone (USB/powerbank only) -- switch to BT-BT mode: keep the
// existing BLE-client connection to a real e-board exactly as today, and
// additionally advertise a second BLE role that masquerades as a real
// ChessLink board so external chess software can connect wirelessly. This
// mode, once entered, can only be left by a full power cycle.
enum class HostTransport : uint8_t { Cable, ChesslinkBle, ChessnutBle };
HostTransport activeHostTransport = HostTransport::Cable;

enum class BtBtStage : uint8_t {
  Inactive,           // cable mode chosen, or BT-BT flow already finished
  WaitingForBoard,    // BT-BT chosen; waiting for a connected board's first status
  ShowingReady,       // "ready" signal (center 4 squares / Cynus "OK") on screen
  WaitingForModeSelect,  // waiting for the second-white-queen mode selection
  ShowingConfirm,     // "confirmed" signal (corner squares / Cynus mode text) on screen
};

uint32_t bootMs = 0;
bool btBtModeChosen = false;
BtBtStage btBtStage = BtBtStage::Inactive;
uint32_t btBtStageAt = 0;
int btBtSelectedMode = -1;  // 0 = ChessLink, 1 = Chessnut (selected via 2nd white queen)
constexpr uint32_t kCableDetectTimeoutMs = 5000;
constexpr uint32_t kBtBtSignalHoldMs = 2000;

constexpr uint8_t kCenterSquares[4] = {
    boardSquareIndex('d', 4), boardSquareIndex('d', 5),
    boardSquareIndex('e', 4), boardSquareIndex('e', 5)};
constexpr uint8_t kCornerSquares[4] = {
    boardSquareIndex('a', 1), boardSquareIndex('a', 8),
    boardSquareIndex('h', 1), boardSquareIndex('h', 8)};

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

bool anyBoardConnected() {
  switch (activeBoardType) {
    case BoardType::Millennium: return millenniumIsConnected();
    case BoardType::Chessnut: return chessnutIsConnected();
    case BoardType::Cynus: return cynusIsConnected();
    default: return false;
  }
}

void clearActiveBoardLeds() { clearBoardLeds(activeBoardType); }

struct KnownBoard {
  const char* name;
  BoardType type;
};

const KnownBoard kKnownBoards[] = {
    {kMillenniumBoardName, BoardType::Millennium},
    {kChessnutBoardName, BoardType::Chessnut},
    {kChessnutBoardNameAlt, BoardType::Chessnut},
    {kCynusBoardName, BoardType::Cynus},
};

// Case-insensitive substring search -- BLE devices vary the exact advertised
// name (model suffix, MAC-derived suffix, etc.) so an exact/case-sensitive
// match is too brittle to rely on.
bool containsCaseInsensitive(const std::string& haystack, const char* needle) {
  std::string lowerHaystack = haystack;
  std::transform(lowerHaystack.begin(), lowerHaystack.end(), lowerHaystack.begin(), ::tolower);
  std::string lowerNeedle = needle;
  std::transform(lowerNeedle.begin(), lowerNeedle.end(), lowerNeedle.begin(), ::tolower);
  return lowerHaystack.find(lowerNeedle) != std::string::npos;
}

bool scanForKnownBoard(NimBLEAddress& address, BoardType& type) {
  Serial.println("BLE scan: looking for a known board ...");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults results = scan->getResults(5000, false);

  for (int i = 0; i < results.getCount(); ++i) {
    const NimBLEAdvertisedDevice* device = results.getDevice(i);
    if (!device->haveName()) continue;
    for (const KnownBoard& known : kKnownBoards) {
      if (containsCaseInsensitive(device->getName(), known.name)) {
        address = device->getAddress();
        type = known.type;
        Serial.printf("BLE board found: %s (%s, addr type %d)\r\n",
                      device->getName().c_str(), address.toString().c_str(), address.getType());
        scan->clearResults();
        return true;
      }
    }
  }

  scan->clearResults();
  Serial.println("No matching BLE board found.");
  return false;
}

bool connectToBoard() {
  ledState = LedState::Searching;
  NimBLEAddress address;
  BoardType type = BoardType::Unknown;
  if (!scanForKnownBoard(address, type)) return false;

  const bool connected = (type == BoardType::Millennium) ? millenniumConnect(address)
                          : (type == BoardType::Chessnut) ? chessnutConnect(address)
                          : (type == BoardType::Cynus)    ? cynusConnect(address)
                                                           : false;
  if (!connected) return false;

  activeBoardType = type;
  ledState = LedState::Connected;
  haveCachedBoardStatus = false;
  haveSentStatusToKing = false;
  haveLoggedStatus = false;
  ledsAwaitingClear = false;
  autonomousStatusIntervalMs = kFallbackAutoReportIntervalMs;
  return true;
}

// The underlying BLE library's connect() call has no internal timeout and
// can block forever if a peer accepts the link at the radio level but never
// completes the GATT-open handshake (observed with some non-Millennium
// boards). Running each attempt on its own task keeps a stuck connect from
// freezing King's cable-facing loop() -- the one board interface that must
// never be affected by anything on the BLE side.
void connectTask(void*) {
  connectToBoard();
  connectInProgress = false;
  vTaskDelete(nullptr);
}

// Shows a fixed, board-type-independent signal during the BT-BT mode-
// selection handshake: LED boards light exactly the given 4 squares (the
// same pattern regardless of which mode is ultimately chosen -- only the
// square SET differs between the "ready" and "confirmed" stages, not the
// board type); Cynus has no LEDs, so it shows the given text instead.
void showBtBtSignal(const uint8_t* squares, size_t count, const char* cynusText) {
  switch (activeBoardType) {
    case BoardType::Millennium: {
      uint8_t frame[kFrameBufferSize] = {};
      // Plain checksum, not cableHostUsesEncodedChecksum: that flag
      // describes King/Phoenix's own cable quirk, never the real BLE
      // board's convention, which is always plain Mode-B.
      encodeLedFrame(squares, count, frame, /*useEncodedChecksum=*/false);
      millenniumRelayLedFrame(frame, 167);
      break;
    }
    case BoardType::Chessnut: {
      SquareHighlight highlights[4];
      for (size_t i = 0; i < count && i < 4; ++i) {
        highlights[i] = {squares[i], SquareHighlightRole::Generic};
      }
      chessnutSetHighlightedSquares(highlights, count);
      break;
    }
    case BoardType::Cynus:
      cynusShowText(cynusText);
      break;
    default:
      break;
  }
}

// Every one of these boards' physical piece sets includes a spare white
// queen (needed for promotion anyway), which the human places on a4 (in
// addition to the normal starting position) to select ChessLink mode, or
// b4 to select Chessnut mode -- user's own design, not from any reference
// project. Returns 0/1 for the two modes, or -1 if not yet selected.
int detectBtBtModeSelection() {
  const uint8_t* status = cachedBoardStatusBytes();
  if (status == nullptr) return -1;
  int whiteQueens = 0;
  for (int i = 0; i < 64; ++i) {
    if (status[1 + i] == 'Q') ++whiteQueens;
  }
  if (whiteQueens != 2) return -1;
  const int a4Wire = modeBStatusWireIndex(/*file0=*/0, /*rank=*/4);
  const int b4Wire = modeBStatusWireIndex(/*file0=*/1, /*rank=*/4);
  if (status[1 + a4Wire] == 'Q') return 0;
  if (status[1 + b4Wire] == 'Q') return 1;
  return -1;
}

// Drives the whole BT-BT sequence: cable-presence timeout -> wait for a
// connected board's first status -> "ready" signal -> wait for the
// second-white-queen mode selection -> "confirmed" signal -> start the
// selected masquerade server. A no-op for as long as the cable is actually
// present (the overwhelmingly common case).
void processBtBtStateMachine() {
  const uint32_t nowMs = millis();

  if (!btBtModeChosen) {
    if (rawUartRxBytes > 0) {
      btBtModeChosen = true;  // cable host present -- normal cable mode, forever
      return;
    }
    if (static_cast<uint32_t>(nowMs - bootMs) < kCableDetectTimeoutMs) return;
    btBtModeChosen = true;
    btBtStage = BtBtStage::WaitingForBoard;
    Serial.println("BT-BT mode: no cable host detected within 5s -- switching to dual-BLE "
                    "operation (BLE board client + ChessLink BLE masquerade server).");
  }

  switch (btBtStage) {
    case BtBtStage::Inactive:
      break;
    case BtBtStage::WaitingForBoard:
      if (anyBoardConnected() && haveCachedBoardStatus) {
        Serial.println("BT-BT mode: board ready -- showing ready signal");
        showBtBtSignal(kCenterSquares, 4, "OK");
        btBtStage = BtBtStage::ShowingReady;
        btBtStageAt = nowMs;
      }
      break;
    case BtBtStage::ShowingReady:
      if (static_cast<uint32_t>(nowMs - btBtStageAt) >= kBtBtSignalHoldMs) {
        clearActiveBoardLeds();  // the 2s hold is over; don't leave it lit forever
        btBtStage = BtBtStage::WaitingForModeSelect;
      }
      break;
    case BtBtStage::WaitingForModeSelect: {
      const int mode = detectBtBtModeSelection();
      if (mode < 0) break;
      btBtSelectedMode = mode;
      Serial.printf("BT-BT mode: %s selected via second white queen\r\n",
                    mode == 0 ? "ChessLink" : "Chessnut");
      showBtBtSignal(kCornerSquares, 4, mode == 0 ? "CSLMode" : "NutMode");
      btBtStage = BtBtStage::ShowingConfirm;
      btBtStageAt = nowMs;
      break;
    }
    case BtBtStage::ShowingConfirm:
      if (static_cast<uint32_t>(nowMs - btBtStageAt) >= kBtBtSignalHoldMs) {
        clearActiveBoardLeds();  // the 2s hold is over; don't leave it lit forever
        btBtStage = BtBtStage::Inactive;
        if (btBtSelectedMode == 0) {
          activeHostTransport = HostTransport::ChesslinkBle;
          if (haveCachedBoardStatus) chesslinkServerPublishStatus(cachedBoardStatus);
          chesslinkServerStart();
          Serial.println("BT-BT mode: ChessLink masquerade server started");
        } else {
          activeHostTransport = HostTransport::ChessnutBle;
          if (haveCachedBoardStatus) chessnutServerPublishStatus(cachedBoardStatus);
          chessnutServerStart();
          Serial.println("BT-BT mode: Chessnut masquerade server started");
        }
      }
      break;
  }
}

void receiveFromMillenniumComputer() {
  size_t completedFrames = 0;
  while (MillenniumSerial.available() > 0) {
    const uint8_t raw = static_cast<uint8_t>(MillenniumSerial.read());
    ++rawUartRxBytes;
    uartFrame[uartFrameLength++] = raw & 0x7f;
    while (uartFrameLength > 0) {
      const size_t expected = modeBCommandLength(uartFrame[0]);
      if (expected == 0) {
        ++discardedUartRxBytes;
        // Resyncing after a bad frame (e.g. a rejected checksum) can discard
        // dozens of bytes one at a time -- log only the first byte of each
        // run plus a one-line summary once resynced, instead of one line per
        // byte, which used to flood the log to the point of being unusable.
        static uint32_t resyncRunLength = 0;
        static uint8_t resyncFirstByte = 0;
        if (resyncRunLength == 0) {
          resyncFirstByte = uartFrame[0];
          Serial.printf("[KING RX] resync: unrecognized byte raw=%02X ascii=%02X '%c'\r\n",
                        resyncFirstByte, resyncFirstByte & 0x7f,
                        isprint(resyncFirstByte & 0x7f) ? resyncFirstByte & 0x7f : '.');
        }
        ++resyncRunLength;
        memmove(uartFrame, uartFrame + 1, --uartFrameLength);
        if (uartFrameLength == 0 || modeBCommandLength(uartFrame[0]) != 0) {
          if (resyncRunLength > 1) {
            Serial.printf("[KING RX] resync: discarded %lu bytes total, resumed\r\n",
                          static_cast<unsigned long>(resyncRunLength));
          }
          resyncRunLength = 0;
        }
        continue;
      }
      if (uartFrameLength < expected) break;
      bool frameUsedEncodedChecksum = false;
      if (modeBValidBlock(uartFrame, expected, &frameUsedEncodedChecksum)) {
        if (frameUsedEncodedChecksum && !cableHostUsesEncodedChecksum) {
          cableHostUsesEncodedChecksum = true;
          Serial.println("Cable host uses the odd-parity-encoded checksum "
                          "convention (confirmed: Mephisto Phoenix) -- "
                          "matching it in our own replies from now on.");
        }
        if (uartFrame[0] == 'L' && expected == 167) {
          // The checksum of a bare 'l' ack is content-independent, so we can
          // answer instantly instead of waiting on a BLE round trip.
          uint8_t localLedAck[3] = {'l', 0, 0};
          computeModeBChecksumHex(localLedAck + 1, localLedAck, 1, cableHostUsesEncodedChecksum);
          writeFrameToKing(localLedAck, sizeof(localLedAck));

          // King may only accept a reply within a single probe cycle, which
          // a fresh BLE round trip can miss -- send the cached status
          // immediately alongside the ack, then also kick off a fresh fetch.
          if (haveCachedBoardStatus) {
            writeFrameToKing(cachedBoardStatus, sizeof(cachedBoardStatus));
          }
          ledsAwaitingClear = true;

          // Mephisto Phoenix legitimately cycles through many distinct
          // frames while suggesting one move (blink-off, plus each square
          // of a multi-square animation in turn) -- comparing against even
          // a handful of recently-seen frames still logged most of them as
          // "new", flooding the log enough to cut off the more useful lines
          // (fen/status/move messages) in a pasted capture. A hard time
          // throttle is simpler and actually bounds the log rate: at most
          // one [L DIAG]/[L RAW] pair per second, always showing whatever
          // the current frame is at that tick. This is diagnostic-only
          // output (no decoding logic reads it) so throttling it changes
          // nothing about move detection.
          static uint32_t lastLedDiagLogAt = 0;
          if (static_cast<uint32_t>(millis() - lastLedDiagLogAt) >= 1000) {
            lastLedDiagLogAt = millis();
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

          // Shared with the BT-BT ChessLink masquerade path
          // (chesslink_server.cpp) so King/Phoenix on the cable and
          // external ChessLink software over BLE get identical per-board
          // LED handling -- see dispatchLedFrameToBoard()'s own comment.
          dispatchLedFrameToBoard(activeBoardType, uartFrame);
        } else if (activeBoardType == BoardType::Millennium) {
          // King never sends non-'L' commands in practice, but relay
          // anything else unchanged too, exactly as always.
          millenniumRelayCommand(uartFrame, expected);
        }
      } else {
        ++discardedUartRxBytes;
        // Dump the full raw frame as one line (mirrors [L RAW] for a valid
        // L frame) plus the computed-vs-received checksum, since the
        // byte-by-byte resync log below is too fragmented to diagnose a
        // consistently-wrong checksum from a new/unproven host device.
        uint8_t computedChecksum = 0;
        for (size_t i = 0; i + 2 < expected; ++i) computedChecksum ^= uartFrame[i] & 0x7f;
        Serial.print("[BAD CHECKSUM RAW] ");
        Serial.write(uartFrame, expected);
        Serial.println();
        Serial.printf("[BAD CHECKSUM CALC] computed=%02X received=%c%c\r\n", computedChecksum,
                      uartFrame[expected - 2], uartFrame[expected - 1]);
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

}  // namespace

bool haveAnyBoardStatus() { return haveCachedBoardStatus; }

const uint8_t* cachedBoardStatusBytes() { return haveCachedBoardStatus ? cachedBoardStatus : nullptr; }

BoardType currentBoardType() { return activeBoardType; }

uint32_t autonomousStatusIntervalMs = kFallbackAutoReportIntervalMs;
bool cableHostUsesEncodedChecksum = false;

size_t writeFrameToKing(const uint8_t* logicalFrame, size_t length) {
  // Routes to whichever host transport is actually active -- the King/
  // Phoenix cable (default), or the ChessLink BLE masquerade server once
  // BT-BT mode has started it. The name is historical (this function
  // predates BT-BT mode); despite it, this is no longer cable-only.
  if (activeHostTransport == HostTransport::ChesslinkBle) {
    return chesslinkServerWriteFrame(logicalFrame, length);
  }
  // Pre-encode odd parity into bit 7 and send as plain 8N1: this produces
  // the same 10-bit wire waveform as native 7O1 framing, which the ESP32-C3
  // UART hardware cannot generate directly. BLE carries plain bytes, so
  // this encoding step only ever applies to the cable case above.
  uint8_t encoded[kFrameBufferSize] = {};
  for (size_t i = 0; i < length; ++i) encoded[i] = encodeOddParity(logicalFrame[i]);
  return MillenniumSerial.write(encoded, length);
}

void onBoardStatusFrame(const uint8_t frame[kModeBStatusFrameLength]) {
  if (!haveLoggedStatus || memcmp(lastLoggedStatus, frame, kModeBStatusFrameLength) != 0) {
    memcpy(lastLoggedStatus, frame, kModeBStatusFrameLength);
    haveLoggedStatus = true;
    Serial.print("[BOARD STATUS] ");
    Serial.write(frame, kModeBStatusFrameLength);
    Serial.println();
  }

  const bool wasFirstStatus = !haveCachedBoardStatus;
  memcpy(cachedBoardStatus, frame, kModeBStatusFrameLength);
  haveCachedBoardStatus = true;
  lastAutonomousStatusMs = millis();

  // Always safe to call (a no-op cache update before either server ever
  // starts); once BT-BT mode is active and has started its selected
  // masquerade server, this also immediately forwards to a connected
  // client, mirroring the cable path's own immediate-forward behavior
  // below.
  chesslinkServerPublishStatus(frame);
  chessnutServerPublishStatus(frame);

  if (activeHostTransport == HostTransport::Cable) {
    const size_t written = writeFrameToKing(frame, kModeBStatusFrameLength);
    memcpy(lastStatusSentToKing, frame, kModeBStatusFrameLength);
    haveSentStatusToKing = true;
    Serial.printf("BLE -> UART: s frame, %u ASCII bytes\r\n", static_cast<unsigned>(written));
  }

  if (wasFirstStatus || ledsAwaitingClear) {
    clearActiveBoardLeds();
    ledsAwaitingClear = false;
  }
}

void setup() {
  // King sends its Mode-B identification immediately after power-up; start
  // its UART before USB logging and any BLE scan so those bytes aren't lost.
  MillenniumSerial.setRxBufferSize(4096);
  MillenniumSerial.begin(MILLENNIUM_BAUD, SERIAL_8N1, kMillenniumRxPin, kMillenniumTxPin);

  Serial.begin(kMonitorBaud);
  delay(1500);

  // Starting point for the BT-BT cable-presence timeout (see
  // processBtBtStateMachine()) -- set right after the boot delay/log setup,
  // i.e. from here on is what's actually visible in the serial monitor
  // (matches how the user times it when watching the log, rather than the
  // ~1.5s of silent boot-delay before the first log line even appears).
  bootMs = millis();

  pinMode(kStatusLedPin, OUTPUT);
  digitalWrite(kStatusLedPin, HIGH);
  xTaskCreate(statusLedTask, "status-led", 1536, nullptr, 1, nullptr);

  Serial.println("\r\nBluetoothMax multi-board gateway");
  Serial.printf("Cable: %lu baud, explicit odd parity over 8N1, RX=GPIO%d, TX=GPIO%d\r\n",
                static_cast<unsigned long>(MILLENNIUM_BAUD), kMillenniumRxPin, kMillenniumTxPin);
  Serial.println("WARNING: cable GPIOs only through the 3.3 V MAX3232 TTL side.");
  Serial.println("BLE: connection watchdog only; protocol idle until King starts it.");

  NimBLEDevice::init("BluetoothMax");

  // Create both BT-BT masquerade servers' GATT structure (service/
  // characteristics) now, at boot, before the BLE client role ever starts
  // scanning/connecting -- creating one much later (once BT-BT mode is
  // confirmed) crashed with "assert failed: ble_svc_gap_init" on real
  // hardware, apparently from registering a new GATT server while the
  // client role is already active. Advertising itself is still deferred to
  // each server's own Start() function, once BT-BT mode selection actually
  // picks one -- these init steps alone do not advertise anything or affect
  // normal cable operation.
  chesslinkServerInit();
  chessnutServerInit();
}

void loop() {
  receiveFromMillenniumComputer();
  processBtBtStateMachine();
  chesslinkServerPoll();  // no-op until chesslinkServerStart() has run
  chessnutServerPoll();   // no-op until chessnutServerStart() has run

  if (anyBoardConnected()) {
    switch (activeBoardType) {
      case BoardType::Millennium: millenniumPoll(); break;
      case BoardType::Chessnut: chessnutPoll(); break;
      case BoardType::Cynus: cynusPoll(); break;
      default: break;
    }

    // Status changes are already forwarded immediately on arrival; this
    // periodic tick only resends if the cache differs from what was last
    // actually put on the wire, to avoid flooding the cable with identical
    // frames at the board's own scan rate. Cable-only: the ChessLink BLE
    // masquerade path has its own equivalent resend-on-change check inside
    // chesslinkServerPoll(), using its own send-tracking.
    if (activeHostTransport == HostTransport::Cable && haveCachedBoardStatus &&
        static_cast<uint32_t>(millis() - lastAutonomousStatusMs) >= autonomousStatusIntervalMs) {
      lastAutonomousStatusMs = millis();
      if (!haveSentStatusToKing ||
          memcmp(lastStatusSentToKing, cachedBoardStatus, sizeof(cachedBoardStatus)) != 0) {
        writeFrameToKing(cachedBoardStatus, sizeof(cachedBoardStatus));
        memcpy(lastStatusSentToKing, cachedBoardStatus, sizeof(lastStatusSentToKing));
        haveSentStatusToKing = true;
      }
    }
  }

  const uint32_t nowMs = millis();
  if (!anyBoardConnected() && !connectInProgress &&
      static_cast<uint32_t>(nowMs - lastConnectAttemptMs) >= kReconnectIntervalMs) {
    lastConnectAttemptMs = nowMs;
    connectInProgress = true;
    xTaskCreate(connectTask, "board-connect", 8192, nullptr, 1, nullptr);
  }

  static uint32_t lastStatusMs = 0;
  if (static_cast<uint32_t>(nowMs - lastStatusMs) >= 10000) {
    Serial.printf("Gateway: BLE=%s, board=%s, UART-RX(raw)=%lu, discarded=%lu\r\n",
                  anyBoardConnected() ? "connected" : "offline",
                  activeBoardType == BoardType::Millennium ? "millennium"
                  : activeBoardType == BoardType::Chessnut  ? "chessnut"
                  : activeBoardType == BoardType::Cynus     ? "cynus"
                                                             : "none",
                  static_cast<unsigned long>(rawUartRxBytes),
                  static_cast<unsigned long>(discardedUartRxBytes));
    lastStatusMs = nowMs;
  }

  delay(1);
}
