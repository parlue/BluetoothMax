#pragma once

// Second, independent retrieval path for recorded games -- built 2026-09-02
// because the BLE-based Chess PGN Master download (chessnut_server.cpp's
// sendSavedGamesFileTransfer()) is blocked on an undocumented protocol step
// (see project memory), with no ETA on a fix. This path defines BOTH ends
// of its own protocol (this firmware + a standalone Windows client), so it
// doesn't depend on reverse-engineering anything.
//
// Trigger: the "queen gesture" -- standard starting position plus one extra
// white queen placed on c4, otherwise the board must match the starting
// position exactly. Reused from the earlier (reverted) WiFi-AP retrieval
// attempt's own boardMatchesStartExcept() helper in pgn_recorder.cpp, which
// is already proven (by construction: that exact combination can never
// occur during real play) to never trigger by accident.
//
// On trigger, every saved game is dumped to the native USB-CDC Serial link
// as plain text, framed by simple markers a Windows client parses:
//
//   ###PGN_USB_DUMP_BEGIN count=N###
//   ###PGN_FILE_BEGIN index=0 bytes=341###
//   <exact raw .pgn file content, `bytes` bytes>
//   ###PGN_FILE_END###
//   ... (repeated for each saved game, index 0..N-1)
//   ###PGN_USB_DUMP_END###
//
// This is the SAME Serial link this project's own debug logging already
// uses -- the markers are deliberately distinctive so a client can find its
// data even if other log lines happen to be interleaved, rather than this
// module trying to suppress all other logging while it runs.
void usbPgnDumpTrigger();

// Deletion-on-confirmed-transfer (added 2026-09-02, per the user's own
// long-standing plan -- see project memory): games are only ever removed
// from the device after the client sends back a single ack line,
//
//   ###PGN_USB_DUMP_ACK###
//
// which it does only once it has itself verified every file in the dump
// saved cleanly (byte count matched, no framing errors). usbPgnDumpTrigger()
// itself never blocks waiting for this -- it just remembers how many games
// were just dumped; usbPgnDumpPoll() (called every loop() iteration, same as
// the rest of this project's serviced-from-loop architecture) watches
// Serial non-blockingly for the ack line, with a bounded timeout. No ack
// within the timeout (bad cable, client crashed, older client that doesn't
// know this line, or a partial/failed transfer) leaves every game exactly
// as it was -- deletion is opt-in, never a side effect of merely dumping.
void usbPgnDumpPoll();

// Visual feedback while the queen gesture is being held (user's own
// request, 2026-09-02): the 4 corner squares (a1/a8/h1/h8) light up
// steadily for as long as the gesture stays recognized, off again the
// instant it's removed -- a single write per transition, NOT a repeating
// blink (an earlier blink version, toggling every 400ms, made a real board
// crash/restart repeatedly -- rapid repeated LED writes are apparently
// fragile on this hardware). pgn_recorder.cpp calls this once on every
// recognized/no-longer-recognized transition. usbPgnDumpBlinkPoll() is kept
// as a stable no-op call site for pgnRecorderPoll(); it doesn't need to do
// anything now that this isn't a repeating toggle.
void usbPgnDumpSetBlinking(bool active);
void usbPgnDumpBlinkPoll();
