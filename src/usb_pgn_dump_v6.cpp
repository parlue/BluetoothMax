#include "usb_pgn_dump_v6.h"

#include <LittleFS.h>

#include <cstring>

#include "board_driver_v6.h"
#include "chessnut_board_v6.h"
#include "cynus_board_v6.h"
#include "millennium_board_v6.h"
#include "pgn_recorder_v6.h"

namespace {
// Delete-on-ack state -- see usbPgnDumpPoll()'s own header comment. Only
// ever armed by usbPgnDumpTrigger() when it actually dumped at least one
// game; a 0-game dump has nothing to delete and never arms this.
bool pendingAck = false;
int pendingAckCount = 0;
uint32_t pendingAckDeadlineMs = 0;
constexpr uint32_t kAckTimeoutMs = 5000;
constexpr const char* kAckLine = "###PGN_USB_DUMP_ACK###";
char ackLineBuf[40];
size_t ackLineLen = 0;
}  // namespace

void usbPgnDumpTrigger() {
  // 2026-09-02: the real board crashed/restarted repeatedly during a dump.
  // Root cause found in ESP32 Arduino's own HWCDC::write() (the native
  // USB-CDC Serial implementation this project uses,
  // ARDUINO_USB_CDC_ON_BOOT=1): if the USB host has the port open but isn't
  // actively reading, each write() call can block for up to its TX timeout
  // (100ms default) while waiting for ring-buffer space, retried multiple
  // times. This function used to call Serial.write() ONE BYTE AT A TIME in
  // a loop -- for a real multi-hundred-byte game, that's hundreds of
  // separate blocking-prone calls, easily adding up to seconds of loop()
  // being stuck here, during which chessnutPoll() never runs -- starving
  // the real board's live BLE connection long enough to make it give up
  // and reset. Fixed two ways: chunked reads/writes (one write() call per
  // ~128 bytes instead of per byte), and a short TX timeout for the
  // duration of the dump so an unread port fails fast instead of blocking
  // -- a client that misses a dump can just wait for the next queen-gesture
  // trigger, so dropping output here is an acceptable tradeoff against
  // stalling the whole gateway.
  constexpr uint32_t kDumpTxTimeoutMs = 20;
  const uint32_t previousTxTimeoutMs = 100;  // HWCDC's own compiled-in default
  Serial.setTxTimeoutMs(kDumpTxTimeoutMs);

  const int count = pgnRecorderSavedGameCount();
  Serial.printf("###PGN_USB_DUMP_BEGIN count=%d###\r\n", count);
  for (int i = 0; i < count; ++i) {
    char path[48];
    if (!pgnRecorderGamePgnPathByIndex(i, path, sizeof(path))) continue;
    File f = LittleFS.open(path, "r");
    if (!f) continue;
    Serial.printf("###PGN_FILE_BEGIN index=%d bytes=%u###\r\n", i,
                  static_cast<unsigned>(f.size()));
    uint8_t chunk[128];
    while (f.available()) {
      const int n = f.read(chunk, sizeof(chunk));
      if (n <= 0) break;
      Serial.write(chunk, static_cast<size_t>(n));
    }
    f.close();
    Serial.print("\r\n###PGN_FILE_END###\r\n");
  }
  Serial.println("###PGN_USB_DUMP_END###");

  Serial.setTxTimeoutMs(previousTxTimeoutMs);

  // Arm the delete-on-ack window -- see usbPgnDumpPoll(). Deliberately does
  // NOT wait here: blocking loop() on Serial input for even a couple of
  // seconds would starve chessnutPoll() exactly like the byte-by-byte write
  // bug this same file's own usbPgnDumpTrigger() comment above already
  // describes -- the real board doesn't tolerate loop() stalls like that.
  if (count > 0) {
    pendingAck = true;
    pendingAckCount = count;
    pendingAckDeadlineMs = millis() + kAckTimeoutMs;
    ackLineLen = 0;
  }
}

void usbPgnDumpPoll() {
  if (!pendingAck) return;

  if (static_cast<int32_t>(millis() - pendingAckDeadlineMs) >= 0) {
    Serial.println("[PGN] no delete-ack from USB client within timeout -- keeping games on device");
    pendingAck = false;
    return;
  }

  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n') {
      if (ackLineLen > 0 && ackLineBuf[ackLineLen - 1] == '\r') --ackLineLen;
      ackLineBuf[ackLineLen] = '\0';
      if (strcmp(ackLineBuf, kAckLine) == 0) {
        Serial.printf("[PGN] delete-ack received -- removing %d transferred game(s) from device\r\n",
                      pendingAckCount);
        // Highest index (chronologically newest of this dump) first: each
        // deletion only removes an entry that sorted AFTER every
        // still-pending lower index, so the lower indices' own chronological
        // positions (recomputed fresh from the directory on every call, see
        // chronologicalSlotByIndex()) stay valid throughout the loop.
        for (int i = pendingAckCount - 1; i >= 0; --i) {
          pgnRecorderDeleteGameByIndex(i);
        }
        pendingAck = false;
        return;
      }
      ackLineLen = 0;
    } else if (ackLineLen + 1 < sizeof(ackLineBuf)) {
      ackLineBuf[ackLineLen++] = c;
    } else {
      ackLineLen = 0;  // overflowed a line that was never going to match anyway
    }
  }
}

namespace {

// Lights (or clears) the 4 corner squares -- per-board-type dispatch, same
// pattern chessnut_server.cpp's relayLedCommandToBoard() already uses.
// Cynus has no per-square LEDs of its own, so it's simply a no-op there.
void setCornerLeds(bool on) {
  const uint8_t corners[4] = {
      static_cast<uint8_t>(boardSquareIndex('a', 1)),
      static_cast<uint8_t>(boardSquareIndex('a', 8)),
      static_cast<uint8_t>(boardSquareIndex('h', 1)),
      static_cast<uint8_t>(boardSquareIndex('h', 8)),
  };
  switch (currentBoardType()) {
    case BoardType::Millennium: {
      if (on) {
        uint8_t frame167[167];
        encodeLedFrame(corners, 4, frame167, /*useEncodedChecksum=*/false);
        millenniumRelayLedFrame(frame167, 167);
      } else {
        millenniumClearLeds();
      }
      break;
    }
    case BoardType::Chessnut: {
      SquareHighlight highlights[4];
      for (int i = 0; i < 4; ++i) highlights[i] = {corners[i], SquareHighlightRole::Generic};
      chessnutSetHighlightedSquares(on ? highlights : nullptr, on ? 4u : 0u);
      break;
    }
    default:
      break;
  }
}

}  // namespace

namespace {
bool ledActive = false;
}  // namespace

void usbPgnDumpSetBlinking(bool active) {
  // Steady on/off, not actually blinking -- changed 2026-09-02 after the
  // real board crashed/restarted repeatedly with the original 400ms
  // repeated-write blink. This project's own chessnutSetHighlightedSquares()
  // already carries a note about a *previous*, unrelated LED-write-
  // frequency issue hanging a real board on boot -- rapid repeated LED
  // writes are apparently fragile on this hardware, so a single write per
  // transition (matching the "steady/on, never blinking" convention this
  // file's own local-deviation LED fallback already uses elsewhere) is far
  // safer than a timer toggling it every 400ms. User's own request:
  // "dann lass die leds einfach nur an".
  if (active == ledActive) return;
  ledActive = active;
  // Cynus has no per-square LEDs -- its own display shows a static label
  // instead. "play" matches this project's own existing convention
  // elsewhere in cynus_board.cpp for "back to idle/normal".
  if (currentBoardType() == BoardType::Cynus) {
    cynusShowText(active ? "USBDATA" : "play");
  } else {
    setCornerLeds(active);
  }
}

void usbPgnDumpBlinkPoll() {
  // No longer does anything -- kept as a stable no-op so pgn_recorder.cpp's
  // existing call site doesn't need touching. See usbPgnDumpSetBlinking()'s
  // own comment for why this is steady on/off now, not a repeating toggle.
}
