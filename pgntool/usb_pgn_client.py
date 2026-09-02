"""
BluetoothMax USB PGN client.

Second, independent retrieval path for recorded games (see
include/usb_pgn_dump.h in the firmware) -- built because the BLE-based
Chess PGN Master download is blocked on an undocumented protocol step, with
no ETA on a fix. This client and the firmware side define both ends of
their own simple protocol, so there's nothing to reverse-engineer here.

Usage: plug the gateway into this PC via USB (it's powered from the same
cable), place a standard starting position on the board with one extra
white queen on c4 ("queen gesture"). The firmware dumps every saved game
over the serial link; this client saves each one as its own gameN.pgn file
in the directory this script/exe lives in, auto-numbered from whatever
gameN.pgn files are already there.

Protocol (plain text over the same serial link the firmware's normal debug
log already uses -- other lines are simply ignored):

    ###PGN_USB_DUMP_BEGIN count=N###
    ###PGN_FILE_BEGIN index=0 bytes=341###
    <exact raw .pgn file content, `bytes` bytes>
    ###PGN_FILE_END###
    ... (repeated for each saved game, index 0..N-1)
    ###PGN_USB_DUMP_END###

If, and only if, every one of the N games above was saved with its exact
expected byte count (no framing errors), this client writes one line back:

    ###PGN_USB_DUMP_ACK###

which tells the gateway it's safe to delete those N games from its own
storage (added 2026-09-02, per the user's own plan -- previously the
gateway never deleted anything and games only wrapped around after 20 new
ones). A partial/failed transfer sends no ack at all, so the gateway's own
timeout just leaves everything in place for the next attempt.
"""

import contextlib
import glob
import logging
import os
import re
import sys
import time

import serial
import serial.tools.list_ports

BAUD_RATE = 115200
PORT_PROBE_TIMEOUT_S = 2.0
PORT_PROBE_SIGNATURES = (b"BluetoothMax", b"Gateway: BLE=", b"[PGN]", b"[CHESSNUT]")
RECONNECT_DELAY_S = 3.0

FILE_BEGIN_RE = re.compile(rb"###PGN_FILE_BEGIN index=(\d+) bytes=(\d+)###")
DUMP_BEGIN_RE = re.compile(rb"###PGN_USB_DUMP_BEGIN count=(\d+)###")

# Logging is deliberately verbose for now -- there's no visibility into the
# firmware side once it's mid-dump, so this logfile is the only window into
# what actually happened during development. User's own plan: turn this
# down (or off) again once the whole path is confirmed reliable.
LOG_ENABLED = True


def script_dir():
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))


def setup_logging():
    handlers = [logging.StreamHandler(sys.stdout)]
    if LOG_ENABLED:
        handlers.append(
            logging.FileHandler(os.path.join(script_dir(), "usb_pgn_client.log"), encoding="utf-8")
        )
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        handlers=handlers,
    )


def next_game_number(directory):
    """Highest existing game<N>.pgn in `directory`, plus 1 -- or 1 if none exist."""
    highest = 0
    for path in glob.glob(os.path.join(directory, "game*.pgn")):
        match = re.fullmatch(r"game(\d+)\.pgn", os.path.basename(path))
        if match:
            highest = max(highest, int(match.group(1)))
    return highest + 1


def count_saved_pgns(directory):
    """Total game<N>.pgn files actually present in `directory` right now --
    user's own request 2026-09-02: show this on exit as a quick "how many
    games have I collected here so far" summary, not just this session's own
    save count. Counts real files (not next_game_number()'s highest-N-plus-1,
    which would be wrong if a file were ever manually removed)."""
    count = 0
    for path in glob.glob(os.path.join(directory, "game*.pgn")):
        if re.fullmatch(r"game(\d+)\.pgn", os.path.basename(path)):
            count += 1
    return count


@contextlib.contextmanager
def open_serial_no_reset(port, timeout):
    """Opens a serial port the same way pio device monitor/esptool
    deliberately DO trigger a reset (by toggling DTR/RTS, the standard
    ESP32 dev-board auto-reset circuit) -- but this client must NOT, since
    it isn't flashing anything and a reset here just drops the gateway's
    live BLE connection to the real board mid-game. Constructing Serial()
    with the port already set performs an implicit open() immediately,
    before DTR/RTS could be set low -- so port/baudrate are set on an
    unopened Serial object first, DTR/RTS forced low, and only then is
    open() called explicitly. Found 2026-09-02 after every single one of
    this client's own restarts during testing was reported by the user as
    "the real board keeps restarting" -- it was this client resetting the
    gateway each time it opened the port, not anything about the queen
    gesture or file dump itself.

    Also explicitly re-asserts DTR/RTS low again right before closing, not
    just before opening -- found 2026-09-02 that a restart of this client
    while the board was in its *normal* (non-gesture) idle state left the
    board unresponsive to the next client instance, matching a second,
    close-time reset (some drivers reset control lines to a default state
    on the last handle closing, independent of whatever they were set to
    while open). A plain `with serial.Serial(...) as ser:` only calls
    close(), it doesn't touch dtr/rts first -- this context manager does,
    covering both edges of the connection's lifetime."""
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD_RATE
    ser.timeout = timeout
    ser.dtr = False
    ser.rts = False
    ser.open()
    try:
        yield ser
    finally:
        try:
            ser.dtr = False
            ser.rts = False
        except (serial.SerialException, OSError):
            pass
        ser.close()


def find_gateway_port():
    """Tries every available COM port briefly, looking for one of this
    project's own known log signatures, rather than guessing a specific
    USB VID/PID (which can vary by board/driver)."""
    for info in serial.tools.list_ports.comports():
        try:
            with open_serial_no_reset(info.device, timeout=0.5) as probe:
                deadline = time.monotonic() + PORT_PROBE_TIMEOUT_S
                buffer = b""
                while time.monotonic() < deadline:
                    chunk = probe.read(256)
                    if chunk:
                        buffer += chunk
                        if any(sig in buffer for sig in PORT_PROBE_SIGNATURES):
                            return info.device
        except (serial.SerialException, OSError):
            continue
    return None


def read_line(ser):
    """Reads one CRLF-terminated line, returns it without the line ending,
    or b"" if the port timed out without one -- used for the marker lines,
    not for the raw PGN content itself (that's read by exact byte count via
    read_exact() instead, since PGN text could in principle contain
    anything except the literal marker strings)."""
    line = ser.readline()
    return line.rstrip(b"\r\n")


def read_exact(ser, count, deadline_s=10.0):
    data = b""
    deadline = time.monotonic() + deadline_s
    while len(data) < count and time.monotonic() < deadline:
        chunk = ser.read(count - len(data))
        if chunk:
            data += chunk
    return data


def save_game(directory, content_bytes):
    number = next_game_number(directory)
    path = os.path.join(directory, f"game{number}.pgn")
    with open(path, "wb") as f:
        f.write(content_bytes)
    logging.info("saved %s (%d bytes)", path, len(content_bytes))
    return path


def process_dump(ser, directory, expected_count):
    """Called once a ###PGN_USB_DUMP_BEGIN...### line has already been
    seen -- reads and saves each file until the matching END marker.
    Returns the number of games actually saved.

    Sends the ###PGN_USB_DUMP_ACK### line back (see this module's own
    docstring) only if every single expected game was saved cleanly --
    `saved` only ever increments on a byte-exact file, so `saved ==
    expected_count` alone is sufficient proof nothing was dropped."""
    saved = 0
    while True:
        line = read_line(ser)
        if not line:
            continue
        if line == b"###PGN_USB_DUMP_END###":
            if expected_count > 0 and saved == expected_count:
                try:
                    ser.write(b"###PGN_USB_DUMP_ACK###\r\n")
                    logging.info(
                        "acknowledged transfer -- gateway will delete these games from its own storage"
                    )
                except (serial.SerialException, OSError) as exc:
                    logging.warning(
                        "could not send delete-ack (%s) -- games stay on the gateway", exc
                    )
            elif expected_count > 0:
                logging.warning(
                    "only %d of %d game(s) saved cleanly -- not acknowledging, "
                    "games stay on the gateway for a retry", saved, expected_count
                )
            return saved
        match = FILE_BEGIN_RE.fullmatch(line)
        if not match:
            logging.warning("unexpected line while expecting a file marker: %r", line)
            continue
        index = int(match.group(1))
        byte_count = int(match.group(2))
        content = read_exact(ser, byte_count)
        if len(content) != byte_count:
            logging.error(
                "file index %d: expected %d bytes, got %d (timed out) -- skipping",
                index, byte_count, len(content),
            )
            continue
        # The firmware's own trailing "\r\n" before the marker plus the
        # saved .pgn content's own trailing newline can leave one blank
        # line here -- skip a few blank lines rather than treating this
        # framing detail as an error (the content itself was already read
        # by the exact `bytes` count above, so this is just a sanity-check
        # line). Bounded, not a `while True`, so a genuinely dropped
        # connection (readline() timing out with b"" forever) doesn't hang.
        end_line = b""
        for _ in range(5):
            end_line = read_line(ser)
            if end_line:
                break
        if end_line != b"###PGN_FILE_END###":
            logging.warning("file index %d: expected end marker, got %r", index, end_line)
        save_game(directory, content)
        saved += 1
    return saved


def wait_for_one_dump(port, directory):
    """Waits on an already-found port for exactly one queen-gesture dump
    cycle, then returns the number of games saved (0 if the gesture fired
    with nothing stored on the gateway -- a real, expected case once this
    project starts deleting games after a successful download, per the
    user's own future plan; not something to treat as an error).

    User's own request 2026-09-02: this used to run forever, catching
    however many dumps happened to come by during the process's lifetime.
    Changed to one-shot -- run once, get the one dump, done -- since
    re-running the exe for the next retrieval is simpler to reason about
    than a background watcher that silently keeps consuming gestures."""
    # See usbPgnDumpSetBlinking()'s own comment in the firmware for why
    # this line matters: the port-finding scan itself can take several
    # seconds (probing other ports first), so placing the queen gesture
    # before this exact point sends its one-shot dump into a port nobody
    # is listening to yet. This is the real "go" signal.
    # User's own feedback 2026-09-02 ("die Leute die Englisch nicht gut
    # sprechen vertun sich"): the two instructions the user actually has to
    # act on need to be unambiguous for non-fluent-English speakers too --
    # German alongside English, not English-only.
    logging.info(
        "ready on %s -- please place the second white queen on c4 now / "
        "bitte jetzt die zweite weisse Dame auf c4 stellen",
        port,
    )
    with open_serial_no_reset(port, timeout=1.0) as ser:
        while True:
            line = read_line(ser)
            if not line:
                continue
            match = DUMP_BEGIN_RE.fullmatch(line)
            if match:
                expected_count = int(match.group(1))
                logging.info("dump starting, %d game(s) expected", expected_count)
                return process_dump(ser, directory, expected_count)


def main():
    setup_logging()
    logging.info("BluetoothMax PGN Tool by Dirk D. Sommerfeld")
    directory = script_dir()
    logging.info("saving games into: %s", directory)
    while True:
        port = find_gateway_port()
        if port is None:
            # User's own feedback 2026-09-02: "no gateway found" read as an
            # alarming failure message even though this is just the normal,
            # expected state while waiting for the board to be plugged in/
            # ready -- reworded to sound like ongoing progress, not an error.
            logging.info("looking for an active COM port, please wait...")
            time.sleep(RECONNECT_DELAY_S)
            continue
        try:
            saved = wait_for_one_dump(port, directory)
        except (serial.SerialException, OSError) as exc:
            logging.warning("lost connection to %s (%s), looking again...", port, exc)
            time.sleep(RECONNECT_DELAY_S)
            continue
        if saved > 0:
            logging.info("done -- %d game(s) saved", saved)
        else:
            # Real, expected case (not an error): the gateway now deletes a
            # game from its own storage once this client confirms it saved
            # cleanly (see process_dump()'s ack), so an empty dump just means
            # everything was already collected on a previous run.
            logging.info("done -- no games were stored on the gateway")
        logging.info("total games in this folder: %d", count_saved_pgns(directory))
        return


def wait_for_keypress(prompt):
    """When double-clicked directly (not run from an already-open
    terminal), a console app's window closes the instant the process
    exits -- both on a normal finish and, worse, on a crash (the
    traceback would flash by unreadable). Used after every exit path so
    the window always stays open until the user is ready."""
    print(f"\n{prompt}")
    try:
        import msvcrt
        msvcrt.getch()
    except ImportError:
        input()


if __name__ == "__main__":
    try:
        main()
        wait_for_keypress(
            "Please remove the second white queen and press any key to close this window... / "
            "Bitte die zweite weisse Dame wieder wegnehmen und eine beliebige Taste druecken, um dieses Fenster zu schliessen..."
        )
    except KeyboardInterrupt:
        pass
    except Exception:
        logging.exception("unexpected error, client is stopping")
        wait_for_keypress("Press any key to close this window...")
