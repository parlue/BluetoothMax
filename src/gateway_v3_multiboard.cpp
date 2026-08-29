// Frozen v3 snapshot of the multi-board gateway (main.cpp and friends),
// taken right before Cynus (third board type) integration began. Confirmed
// working end-to-end on real hardware: MILLENNIUM Supreme T2 BT, Chessnut
// GO, and a Mephisto Phoenix (as the cable-side host, alongside King) --
// connection, board status, moves incl. castling, LED move suggestions,
// New Game reset, and cross-host checksum-convention detection all
// confirmed. Built by the `esp32-c3-superminiv3` PlatformIO environment and
// never touched by any later work -- flash this to fall back to this exact,
// already-proven multi-board behavior. Self-contained: uses only the _v3
// copies of board_driver/millennium_board/chessnut_board, never the live
// (non-_v3) ones, so ongoing Cynus work can never affect this build.

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <algorithm>

#include "board_driver_v3.h"
#include "chessnut_board_v3.h"
#include "millennium_board_v3.h"

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

// Standard starting position in Mode-B wire order (h..a per rank), used only
// to filter Chessnut LED highlights -- see the comment at its use site.
constexpr char kStartPositionWire[] =
    "RNBKQBNRPPPPPPPP................................pppppppprnbkqbnr";

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
    default: return false;
  }
}

void clearActiveBoardLeds() {
  switch (activeBoardType) {
    case BoardType::Millennium: millenniumClearLeds(); break;
    case BoardType::Chessnut: chessnutSetHighlightedSquares(nullptr, 0); break;
    default: break;
  }
}

struct KnownBoard {
  const char* name;
  BoardType type;
};

const KnownBoard kKnownBoards[] = {
    {kMillenniumBoardName, BoardType::Millennium},
    {kChessnutBoardName, BoardType::Chessnut},
    {kChessnutBoardNameAlt, BoardType::Chessnut},
};

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

void connectTask(void*) {
  connectToBoard();
  connectInProgress = false;
  vTaskDelete(nullptr);
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
          uint8_t localLedAck[3] = {'l', 0, 0};
          computeModeBChecksumHex(localLedAck + 1, localLedAck, 1, cableHostUsesEncodedChecksum);
          writeFrameToKing(localLedAck, sizeof(localLedAck));

          if (haveCachedBoardStatus) {
            writeFrameToKing(cachedBoardStatus, sizeof(cachedBoardStatus));
          }
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

          if (activeBoardType == BoardType::Millennium) {
            millenniumSuppressNextLedAck();
            millenniumRequestBoardStatus();
            millenniumRelayLedFrame(uartFrame, expected);
          } else if (activeBoardType == BoardType::Chessnut) {
            SquareHighlight squares[32];
            size_t count = decodeKingLedFrame(uartFrame, squares, 32);

            size_t kept = 0;
            for (size_t i = 0; i < count; ++i) {
              bool eliminate = false;
              if (count >= 3 && squares[i].role == SquareHighlightRole::Generic &&
                  haveCachedBoardStatus) {
                const int file0 = squares[i].squareIndex % 8;
                const int rank = squares[i].squareIndex / 8 + 1;
                const int wireIndex = modeBStatusWireIndex(file0, rank);
                eliminate = cachedBoardStatus[1 + wireIndex] == kStartPositionWire[wireIndex];
              }
              if (!eliminate) squares[kept++] = squares[i];
            }
            count = kept;

            chessnutSetHighlightedSquares(squares, count);
          }
        } else if (activeBoardType == BoardType::Millennium) {
          millenniumRelayCommand(uartFrame, expected);
        }
      } else {
        ++discardedUartRxBytes;
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

uint32_t autonomousStatusIntervalMs = kFallbackAutoReportIntervalMs;
bool cableHostUsesEncodedChecksum = false;

size_t writeFrameToKing(const uint8_t* logicalFrame, size_t length) {
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

  const size_t written = writeFrameToKing(frame, kModeBStatusFrameLength);
  memcpy(lastStatusSentToKing, frame, kModeBStatusFrameLength);
  haveSentStatusToKing = true;
  Serial.printf("BLE -> UART: s frame, %u ASCII bytes\r\n", static_cast<unsigned>(written));

  if (wasFirstStatus || ledsAwaitingClear) {
    clearActiveBoardLeds();
    ledsAwaitingClear = false;
  }
}

void setup() {
  MillenniumSerial.setRxBufferSize(4096);
  MillenniumSerial.begin(MILLENNIUM_BAUD, SERIAL_8N1, kMillenniumRxPin, kMillenniumTxPin);

  Serial.begin(kMonitorBaud);
  delay(1500);

  pinMode(kStatusLedPin, OUTPUT);
  digitalWrite(kStatusLedPin, HIGH);
  xTaskCreate(statusLedTask, "status-led", 1536, nullptr, 1, nullptr);

  Serial.println("\r\nBluetoothMax multi-board gateway (v3 frozen snapshot)");
  Serial.printf("Cable: %lu baud, explicit odd parity over 8N1, RX=GPIO%d, TX=GPIO%d\r\n",
                static_cast<unsigned long>(MILLENNIUM_BAUD), kMillenniumRxPin, kMillenniumTxPin);
  Serial.println("WARNING: cable GPIOs only through the 3.3 V MAX3232 TTL side.");
  Serial.println("BLE: connection watchdog only; protocol idle until King starts it.");

  NimBLEDevice::init("BluetoothMax");
}

void loop() {
  receiveFromMillenniumComputer();

  if (anyBoardConnected()) {
    switch (activeBoardType) {
      case BoardType::Millennium: millenniumPoll(); break;
      case BoardType::Chessnut: chessnutPoll(); break;
      default: break;
    }

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
                                                             : "none",
                  static_cast<unsigned long>(rawUartRxBytes),
                  static_cast<unsigned long>(discardedUartRxBytes));
    lastStatusMs = nowMs;
  }

  delay(1);
}
