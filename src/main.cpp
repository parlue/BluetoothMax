#include <Arduino.h>
#include <NimBLEDevice.h>

#include <algorithm>

#include "board_driver.h"
#include "chessnut_board.h"
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
        const uint8_t discarded = uartFrame[0];
        Serial.printf("[KING RX] unrecognized byte raw=%02X ascii=%02X '%c'\r\n",
                      discarded, discarded & 0x7f,
                      isprint(discarded & 0x7f) ? discarded & 0x7f : '.');
        memmove(uartFrame, uartFrame + 1, --uartFrameLength);
        continue;
      }
      if (uartFrameLength < expected) break;
      if (modeBValidBlock(uartFrame, expected)) {
        if (uartFrame[0] == 'L' && expected == 167) {
          // The checksum of a bare 'l' ack is content-independent, so we can
          // answer instantly instead of waiting on a BLE round trip.
          static constexpr uint8_t localLedAck[] = {'l', '6', 'C'};
          writeFrameToKing(localLedAck, sizeof(localLedAck));

          // King may only accept a reply within a single probe cycle, which
          // a fresh BLE round trip can miss -- send the cached status
          // immediately alongside the ack, then also kick off a fresh fetch.
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
            // Sized for a full New Game/reset frame (every square that
            // differs from the starting position at once), not just a
            // single move's source/destination pair.
            SquareHighlight squares[32];
            size_t count = decodeKingLedFrame(uartFrame, squares, 32);

            // Two generic (reset/error) squares sharing the same corner
            // value can make a square sandwiched between them appear lit
            // too, purely because it shares corners with both -- the raw
            // corner data alone can't tell a real generic highlight apart
            // from this geometric side effect. Disambiguate using what we
            // actually know: a generic-role square that already matches the
            // standard starting position has nothing wrong with it, so it
            // wasn't really meant to be highlighted.
            size_t kept = 0;
            for (size_t i = 0; i < count; ++i) {
              bool eliminate = false;
              if (squares[i].role == SquareHighlightRole::Generic && haveCachedBoardStatus) {
                const int file0 = squares[i].squareIndex % 8;
                const int rank = squares[i].squareIndex / 8 + 1;
                const int wireIndex = modeBStatusWireIndex(file0, rank);
                eliminate = cachedBoardStatus[1 + wireIndex] == kStartPositionWire[wireIndex];
              }
              if (!eliminate) squares[kept++] = squares[i];
            }
            count = kept;

            // Always update, even on count==0 (ambiguous/undecodable frame,
            // e.g. two adjacent suggested squares sharing corners): leaving
            // the PREVIOUS highlight lit instead of clearing it stuck the
            // physical board's LEDs on a stale suggestion, which then hid
            // the actual next move from the user.
            chessnutSetHighlightedSquares(squares, count);
          }
        } else if (activeBoardType == BoardType::Millennium) {
          // King never sends non-'L' commands in practice, but relay
          // anything else unchanged too, exactly as always.
          millenniumRelayCommand(uartFrame, expected);
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

}  // namespace

bool haveAnyBoardStatus() { return haveCachedBoardStatus; }

uint32_t autonomousStatusIntervalMs = kFallbackAutoReportIntervalMs;

size_t writeFrameToKing(const uint8_t* logicalFrame, size_t length) {
  // Pre-encode odd parity into bit 7 and send as plain 8N1: this produces
  // the same 10-bit wire waveform as native 7O1 framing, which the ESP32-C3
  // UART hardware cannot generate directly.
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
  // King sends its Mode-B identification immediately after power-up; start
  // its UART before USB logging and any BLE scan so those bytes aren't lost.
  MillenniumSerial.setRxBufferSize(4096);
  MillenniumSerial.begin(MILLENNIUM_BAUD, SERIAL_8N1, kMillenniumRxPin, kMillenniumTxPin);

  Serial.begin(kMonitorBaud);
  delay(1500);

  pinMode(kStatusLedPin, OUTPUT);
  digitalWrite(kStatusLedPin, HIGH);
  xTaskCreate(statusLedTask, "status-led", 1536, nullptr, 1, nullptr);

  Serial.println("\r\nBluetoothMax multi-board gateway");
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
