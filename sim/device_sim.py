#!/usr/bin/env python3
"""Standard-library PTY device: run interactively or with --auto allow|deny|deny_stop.

The host owns all state. A 12-second snapshot gap (the protocol's liveness
window), or a PTY disconnect, causes hello to be repeated on the next snap.
Long literal permission text is shown in rotating seven-line pages.
"""

import argparse
import errno
import json
import math
import os
import select
import sys
import termios
import time
import tty


MAX_LINE = 2048
GAP = 12.0
WIDTH = 24
TEXT_ROWS = 7


def encode(message):
    return (json.dumps(message, ensure_ascii=False, separators=(",", ":"))
            + "\n").encode("utf-8")


def literal_lines(value):
    """Wrap without collapsing whitespace; escape controls to protect the terminal."""
    safe = "".join(c if c == "\n" or 32 <= ord(c) < 127
                   else c.encode("unicode_escape").decode("ascii")
                   for c in str(value))
    return [part[i:i + WIDTH] for part in safe.split("\n")
            for i in range(0, max(1, len(part)), WIDTH)]


class Device:
    def __init__(self, auto=None, output=None):
        self.master, slave = os.openpty()
        tty.setraw(slave)
        self.path = os.ttyname(slave)
        os.close(slave)
        os.set_blocking(self.master, False)
        self.auto = auto
        self.output = output if output is not None else sys.stdout
        self.snapshot = {"sessions": [], "pending": [], "now": 0}
        self.received = None
        self.focus = None
        self.disconnected = False
        self.retry_at = 0.0
        self.incoming = bytearray()
        self.outgoing = bytearray()
        self.discard_line = False
        self.seen = set()
        self.due = {}
        self.message = ""
        self.message_until = 0.0
        self.last_frame = None

    def close(self):
        os.close(self.master)

    def emit(self, message):
        self.outgoing.extend(encode(message))

    def hello(self):
        self.emit({"t": "hello", "fw": "sim-0.1", "w": 172, "h": 320})

    def log(self, event, **fields):
        if self.auto:
            print(json.dumps({"event": event, **fields}, ensure_ascii=True),
                  file=self.output, flush=True)

    def notice(self, text):
        self.message = text
        self.message_until = time.monotonic() + 2.0
        self.log("message", text=text)
        self.render(force=True)

    def render(self, force=False):
        if self.auto:
            return
        now = time.monotonic()
        pending = self.snapshot["pending"]
        if pending:
            p = pending[0]
            lines = literal_lines(p["text"])
            pages = math.ceil(len(lines) / TEXT_ROWS)
            page = int((now - self.received) / 3) % pages
            text = lines[page * TEXT_ROWS:(page + 1) * TEXT_ROWS]
            seconds = max(0, math.ceil(p["exp"] - self.snapshot["now"]
                                       - (now - self.received)))
            blocked = p["risk"] != "normal" or p["trunc"] > 0
            rows = ["PERMISSION", "Agent: " + p["agent"],
                    "Label: " + p["label"], "Tool: " + p["tool"],
                    "Text page %d/%d" % (page + 1, pages)]
            rows += text + [""] * (TEXT_ROWS - len(text))
            rows += [("…+%s" % p["trunc"]) if p["trunc"] > 0 else "",
                     "Risk: " + p["risk"], "Seconds left: %d" % seconds,
                     "Queue: %d" % len(pending),
                     "APPROVE AT KEYBOARD" if blocked else "[a] ALLOW",
                     "[d] DENY  [s] DENY_STOP", "[n] Next  [q] Quit"]
        else:
            sessions = self.snapshot["sessions"]
            session = next((s for s in sessions if s["sid"] == self.focus), None)
            rows = ["STATUS", "Sessions: %d" % len(sessions)]
            if session:
                rows += ["Agent: " + session["agent"],
                         "Label: " + session["label"], "State: " + session["st"],
                         "Tool: " + session["tool"], "Target:"]
                rows += literal_lines(session["target"])[:10]
            else:
                rows += ["No sessions"]
            rows = rows[:18] + [""] * max(0, 18 - len(rows))
            rows += ["[n] Next  [q] Quit"]
        message = self.message if now < self.message_until else ""
        # Scroll long messages so nack reasons and local refusals remain readable.
        message_lines = literal_lines(message)
        index = min(len(message_lines) - 1,
                    int(max(0, now - (self.message_until - 2)) * len(message_lines) / 2))
        rows += [message_lines[index]]
        border = "+" + "-" * WIDTH + "+"
        frame = "\n".join([border] + ["|" + r[:WIDTH].ljust(WIDTH) + "|"
                                             for r in rows] + [border])
        if force or frame != self.last_frame:
            print("\x1b[2J\x1b[H" + frame, file=self.output, flush=True)
            self.last_frame = frame

    def handle(self, msg):
        kind = msg.get("t")
        now = time.monotonic()
        if kind == "snap":
            if self.disconnected or (self.received is not None
                                     and now - self.received >= GAP):
                self.hello()
            self.disconnected = False
            self.received = now
            self.snapshot = msg
            sessions = msg["sessions"]
            if not any(s["sid"] == self.focus for s in sessions):
                self.focus = sessions[0]["sid"] if sessions else None
            ids = {p["id"] for p in msg["pending"]}
            self.due = {pid: due for pid, due in self.due.items() if pid in ids}
            if self.auto:
                for p in msg["pending"]:
                    if p["id"] not in self.seen:
                        self.seen.add(p["id"])
                        self.due[p["id"]] = now + 1.0
            self.log("snap", snapshot=msg)
            self.render(force=True)
            self.emit({"t": "ack", "rev": msg["rev"]})
        elif kind == "toast":
            self.notice(msg["text"])
        elif kind == "nack":
            self.notice("nack %s: %s" % (msg["id"], msg["why"]))

    def decide(self, p, decision):
        if decision == "allow" and (p["risk"] != "normal" or p["trunc"] > 0):
            self.notice("Allow refused: APPROVE AT KEYBOARD")
            return
        msg = {"t": "decide", "id": p["id"], "d": decision}
        self.emit(msg)
        self.log("decide", id=p["id"], d=decision)

    def key(self, key):
        if key == "q":
            return False
        if key in "ads":
            if self.snapshot["pending"]:
                self.decide(self.snapshot["pending"][0],
                            {"a": "allow", "d": "deny", "s": "deny_stop"}[key])
            else:
                self.notice("No pending request")
        elif key == "n" and not self.snapshot["pending"]:
            ids = [s["sid"] for s in self.snapshot["sessions"]]
            if ids:
                self.focus = ids[(ids.index(self.focus) + 1) % len(ids)]
                self.render(force=True)
        return True

    def tick(self):
        now = time.monotonic()
        for p in self.snapshot["pending"]:
            if p["id"] in self.due and now >= self.due[p["id"]]:
                del self.due[p["id"]]
                self.decide(p, self.auto)
        self.render()

    def read(self):
        try:
            data = os.read(self.master, 8192)
        except BlockingIOError:
            return
        except OSError as exc:
            if exc.errno != errno.EIO:
                raise
            data = b""
        if not data:
            self.disconnected = True
            self.incoming.clear()
            self.discard_line = False
            self.retry_at = time.monotonic() + 0.1
            return
        for byte in data:
            if byte == 10:
                if not self.discard_line:
                    try:
                        msg = json.loads(self.incoming.decode("utf-8"))
                        if isinstance(msg, dict):
                            self.handle(msg)
                    except (ValueError, UnicodeError):
                        self.notice("Invalid JSON from host")
                self.incoming.clear()
                self.discard_line = False
            elif not self.discard_line:
                self.incoming.append(byte)
                if len(self.incoming) >= MAX_LINE:
                    self.incoming.clear()
                    self.discard_line = True
                    self.notice("Host line exceeds 2048 B")

    def flush(self):
        if not self.outgoing:
            return
        try:
            count = os.write(self.master, self.outgoing)
            del self.outgoing[:count]
        except BlockingIOError:
            pass
        except OSError as exc:
            if exc.errno != errno.EIO:
                raise
            self.disconnected = True
            self.retry_at = time.monotonic() + 0.1

    def step(self, stdin=None, timeout=0.1):
        ready = time.monotonic() >= self.retry_at
        reads = ([self.master] if ready else []) + ([stdin] if stdin is not None else [])
        writes = [self.master] if ready and self.outgoing else []
        readable, writable, _ = select.select(reads, writes, [], timeout)
        if self.master in readable:
            self.read()
        if stdin is not None and stdin in readable:
            data = os.read(stdin, 64)
            if not data or not all(self.key(chr(k)) for k in data):
                return False
        self.tick()
        if self.master in writable or time.monotonic() >= self.retry_at:
            self.flush()
        return True


def selftest():
    import io

    device = Device(auto="allow", output=io.StringIO())
    peer = os.open(device.path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    buffer = bytearray()
    messages = []

    def pump(seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            device.step(timeout=min(0.02, max(0, deadline - time.monotonic())))
            try:
                buffer.extend(os.read(peer, 8192))
            except BlockingIOError:
                pass
            while b"\n" in buffer:
                line, _, rest = buffer.partition(b"\n")
                buffer[:] = rest
                messages.append(json.loads(line))

    p = {"id": "selftest-1", "sid": "s1", "agent": "codex", "label": "test",
         "tool": "Bash", "text": "echo hello", "trunc": 0, "risk": "normal",
         "exp": 1790000030}
    snap = {"t": "snap", "rev": 42, "now": 1790000000,
            "sessions": [], "pending": [p], "usage": {}}
    try:
        device.hello()
        os.write(peer, encode(snap))
        pump(0.2)
        assert {"t": "hello", "fw": "sim-0.1", "w": 172, "h": 320} in messages
        assert {"t": "ack", "rev": 42} in messages
        assert not any(m["t"] == "decide" for m in messages), "auto fired early"
        pump(1.0)
        decision = {"t": "decide", "id": p["id"], "d": "allow"}
        assert messages.count(decision) == 1, messages
        os.write(peer, encode(snap))
        pump(1.1)
        assert messages.count(decision) == 1, "duplicate auto decision"
        for changes in ({"risk": "high"}, {"trunc": 12}):
            request = dict(p, id="blocked-" + next(iter(changes)), **changes)
            os.write(peer, encode(dict(snap, pending=[request])))
            pump(1.1)
            assert not any(m.get("id") == request["id"] for m in messages)
        os.close(peer)
        peer = None
        device.read()  # EIO on Linux; zero bytes on some other PTY implementations.
        assert device.disconnected
        peer = os.open(device.path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        os.write(peer, encode(dict(snap, rev=43, pending=[])))
        pump(0.3)
        assert {"t": "ack", "rev": 43} in messages
        assert sum(m["t"] == "hello" for m in messages) == 2
        print("SELFTEST PASS: hello, ack, delayed auto allow, deduplication, "
              "local refusal, slave reconnect")
    finally:
        if peer is not None:
            os.close(peer)
        device.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--auto", choices=("allow", "deny", "deny_stop"))
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        selftest()
        return
    if not args.auto and not sys.stdin.isatty():
        parser.error("interactive mode requires a TTY on stdin; use --auto")
    device = Device(auto=args.auto)
    saved = None
    stdin = None if args.auto else sys.stdin.fileno()
    try:
        print("SERIAL " + device.path, flush=True)
        if stdin is not None:
            saved = termios.tcgetattr(stdin)
            tty.setcbreak(stdin)
        device.hello()
        device.flush()
        device.log("hello", fw="sim-0.1", w=172, h=320)
        while device.step(stdin):
            pass
    except KeyboardInterrupt:
        pass
    finally:
        if saved is not None:
            termios.tcsetattr(stdin, termios.TCSADRAIN, saved)
        device.close()


if __name__ == "__main__":
    main()
