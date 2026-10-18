#!/usr/bin/env python3
"""acsd — agent control surface daemon.

Hooks connect on a Unix socket; the device is a USB CDC serial port speaking NDJSON (docs/PROTOCOL.md).
Standard library only.
"""
import argparse, asyncio, calendar, glob, json, os, re, secrets, termios, time, tty

TTL = 290            # seconds a request stays answerable on the device (hook timeout is 300)
LIVE_WINDOW = 12     # device counts as live while an ack arrived this recently
HEARTBEAT = 5
MAX_TEXT = 200
SESSION_GC = 2 * 3600
MAX_SESSIONS = 6     # the device shows at most 6; more would overflow its line buffer
SNAP_LIMIT = 2000    # firmware line buffer is 2100 bytes
QUESTION_TTL = 120   # a held question blocks the terminal's own UI, so keep it short (hook timeout: 150 s)
QUESTION_END = re.compile(r"[?？]\s*[*_`)\]]*\s*$")
PULSE = "/Applications/Pulse.app/Contents/MacOS/Pulse"
USAGE_EVERY = 60          # `Pulse --json` only reads Pulse's cache, so polling is cheap
USAGE_STALE = 3600        # older readings are not sent: the device shows nothing rather than old numbers
PULSE_AGENTS = {"claudeCode": "claude", "codex": "codex"}
WINDOW_KEYS = {"fiveHour": "5h", "weekly": "7d", "daily": "1d", "monthly": "30d"}

HIGH_RISK_BASH = re.compile(
    r"\b(rm|sudo|su|dd|mkfs\w*|chmod|chown|kill|killall|pkill|shutdown|reboot|curl|wget|ssh|scp|rsync|nc|"
    r"git\s+push|git\s+reset\s+--hard|git\s+clean|npm\s+publish|docker|kubectl|terraform|aws|gcloud|az)\b"
    r"|\|\s*(sh|bash|zsh|python3?)\b|\$\(|`")
SENSITIVE_PATH = re.compile(r"(^|/)(\.ssh|\.aws|\.gnupg|\.env[^/]*|\.claude|\.codex)(/|$)")


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def describe(tool, inp, cwd):
    """Literal text for the device and its risk. Never summarised."""
    if tool == "apply_patch":                         # Codex: the patch arrives in tool_input.command
        files = re.findall(r"^\*\*\* (?:Add|Update|Delete) File: (.+)$", str(inp.get("command", "")), re.M)
        text = " ".join(os.path.relpath(f, cwd) if cwd and f.startswith(cwd.rstrip("/") + "/") else f for f in files)
        text = "patch " + (text or "?")
        risk = "high" if any(SENSITIVE_PATH.search(f) or (f.startswith("/") and not f.startswith(cwd.rstrip("/") + "/"))
                             for f in files) else "normal"
    elif tool == "Bash":
        cmd = inp.get("command", "")
        text = " ".join(cmd) if isinstance(cmd, list) else str(cmd)
        risk = "high" if HIGH_RISK_BASH.search(text) else "normal"
    elif "file_path" in inp or "notebook_path" in inp:
        path = inp.get("file_path") or inp.get("notebook_path") or ""
        inside = cwd and path.startswith(cwd.rstrip("/") + "/")
        text = os.path.relpath(path, cwd) if inside else path
        risk = "normal" if inside and not SENSITIVE_PATH.search(path) else "high"
    elif "url" in inp:
        text, risk = inp["url"], "normal"
    else:
        text, risk = json.dumps(inp, separators=(",", ":")), "normal"
    trunc = max(0, len(text) - MAX_TEXT)
    return text[:MAX_TEXT], trunc, risk


class Daemon:
    def __init__(self, serial_arg):
        self.serial_arg = serial_arg
        self.sessions = {}   # sid -> dict
        self.pending = {}    # id -> dict (insertion order = queue order)
        self.rev = 0
        self.fd = None
        self.rbuf = b""
        self.last_ack = 0.0
        self.obuf, self.writer_on = b"", False
        self.usage = {}      # agent -> {"age": s, "win": [{"k", "used", "resets"}]}

    # ── usage (from Pulse, which reads each provider's own figures) ─────────
    async def poll_usage(self):
        while True:
            usage = {}
            if os.path.exists(PULSE):
                try:
                    proc = await asyncio.create_subprocess_exec(PULSE, "--json", stdout=asyncio.subprocess.PIPE,
                                                                stderr=asyncio.subprocess.DEVNULL)
                    out, _ = await asyncio.wait_for(proc.communicate(), 10)
                    usage = parse_pulse(json.loads(out))
                except (OSError, ValueError, asyncio.TimeoutError) as e:
                    log("usage unavailable:", e)
            if usage != self.usage:
                self.usage = usage
                self.changed()
            await asyncio.sleep(USAGE_EVERY)

    # ── device ──────────────────────────────────────────────────────────────
    def device_live(self):
        return self.fd is not None and time.time() - self.last_ack < LIVE_WINDOW

    def open_serial(self):
        paths = [self.serial_arg] if self.serial_arg != "auto" else sorted(glob.glob("/dev/cu.usbmodem*"))
        for path in paths:
            try:
                fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                tty.setraw(fd)
                attrs = termios.tcgetattr(fd)
                attrs[4] = attrs[5] = termios.B115200
                termios.tcsetattr(fd, termios.TCSANOW, attrs)
            except OSError:
                continue
            self.fd, self.rbuf = fd, b""
            asyncio.get_running_loop().add_reader(fd, self.on_serial)
            log("device opened", path)
            self.send_snap()
            return

    def close_serial(self, why):
        if self.fd is None:
            return
        loop = asyncio.get_running_loop()
        loop.remove_reader(self.fd)
        if self.writer_on:
            loop.remove_writer(self.fd)
        self.obuf, self.writer_on = b"", False
        try:
            os.close(self.fd)
        except OSError:
            pass
        self.fd, self.last_ack = None, 0.0
        log("device closed:", why)

    def write(self, obj):
        """Queue a line for the device; a full port is drained when writable instead of dropping state."""
        if self.fd is None:
            return
        line = (json.dumps(obj, separators=(",", ":")) + "\n").encode()
        if len(self.obuf) > 32768:      # device not reading: keep only the newest message
            self.obuf = b""
        self.obuf += line
        self.flush()

    def flush(self):
        if self.fd is None:
            return
        try:
            while self.obuf:
                n = os.write(self.fd, self.obuf)
                self.obuf = self.obuf[n:]
        except BlockingIOError:
            pass
        except OSError as e:
            return self.close_serial(e)
        loop = asyncio.get_running_loop()
        if self.obuf and not self.writer_on:
            loop.add_writer(self.fd, self.flush)
            self.writer_on = True
        elif not self.obuf and self.writer_on:
            loop.remove_writer(self.fd)
            self.writer_on = False

    def on_serial(self):
        try:
            chunk = os.read(self.fd, 4096)
        except BlockingIOError:
            return
        except OSError as e:
            return self.close_serial(e)
        if not chunk:
            return self.close_serial("eof")
        self.rbuf += chunk
        while b"\n" in self.rbuf:
            line, self.rbuf = self.rbuf.split(b"\n", 1)
            try:
                msg = json.loads(line)
            except ValueError:
                continue                # firmware debug prints share the port
            self.on_device(msg)

    def on_device(self, msg):
        t = msg.get("t")
        if t == "ack":
            self.last_ack = time.time()
        elif t == "hello":
            log("device hello", msg)
            self.last_ack = time.time()
            self.send_snap()
        elif t == "decide":
            self.decide(msg.get("id"), msg.get("d"), msg.get("opt"))

    def send_snap(self):
        now = time.time()
        snap = {
            "t": "snap", "rev": self.rev, "now": int(now),
            "sessions": [{k: s[k] for k in ("sid", "agent", "label", "st", "tool", "target")} | {"age": int(now - s["last"])}
                         for s in sorted(self.sessions.values(), key=lambda s: -s["last"])[:MAX_SESSIONS]],
            "pending": [{k: p[k] for k in ("id", "sid", "agent", "label", "tool", "text", "trunc", "risk", "exp",
                                           "created", "sess", "kind", "opts", "qpos")}
                        for p in list(self.pending.values())[:4]],
            "usage": self.usage,
        }
        line = json.dumps(snap, separators=(",", ":"))
        while len(line) > SNAP_LIMIT and snap["sessions"]:   # never let the device drop a whole snapshot
            snap["sessions"].pop()
            line = json.dumps(snap, separators=(",", ":"))
        self.write(snap)

    def changed(self):
        self.rev += 1
        self.send_snap()

    # ── decisions ───────────────────────────────────────────────────────────
    def decide(self, rid, d, opt=None):
        p = self.pending.get(rid)
        if p is None or time.time() > p["exp"]:
            return self.write({"t": "nack", "id": rid, "why": "gone"})
        if p["kind"] != "perm":
            return self.answer(p, d, opt)
        if d in ("allow", "allow_session") and (p["risk"] == "high" or p["trunc"]):
            return self.write({"t": "nack", "id": rid, "why": "high_risk"})
        if d == "allow_session" and not p["sess"]:
            return self.write({"t": "nack", "id": rid, "why": "not_eligible"})
        if d == "allow":
            decision = {"behavior": "allow"}
        elif d == "allow_session":
            # In-memory rule for this Claude session only (never written to a settings file), exact command match.
            decision = {"behavior": "allow", "updatedPermissions": [{
                "type": "addRules", "rules": [{"toolName": "Bash", "ruleContent": p["raw"]}],
                "behavior": "allow", "destination": "session"}]}
        elif d == "deny":
            decision = {"behavior": "deny", "message": "Denied from the control surface."}
        elif d == "deny_stop" and p["agent"] == "codex":
            # Codex fails closed on `interrupt` in PermissionRequest output, so it gets a plain deny.
            decision = {"behavior": "deny", "message": "Denied from the control surface. Stop and wait for the user."}
        elif d == "deny_stop":
            decision = {"behavior": "deny", "message": "Denied and stopped from the control surface.", "interrupt": True}
        else:
            return self.write({"t": "nack", "id": rid, "why": "gone"})
        self.resolve(p, {"decision": decision})
        log("decided", rid, d)

    def resolve(self, p, reply):
        """Single use: take the request off the queue and hand `reply` to the waiting hook ({} = no answer)."""
        self.pending.pop(p["id"], None)
        if not p["fut"].done():
            p["fut"].set_result(reply)

    def answer(self, p, d, opt):
        """Yes/no after a turn that ended in a question, or an AskUserQuestion option."""
        if d == "release":                              # "answer at keyboard"
            log("released", p["id"])
            return self.resolve(p, {})
        if p["kind"] == "yesno" and d in ("yes", "no"):
            log("answered", p["id"], d)
            ctx = f"The user answered from the control surface: {'Yes' if d == 'yes' else 'No'}."
            return self.resolve(p, {"output": {"hookSpecificOutput": {"hookEventName": "Stop", "additionalContext": ctx}}})
        if p["kind"] == "ask" and d == "opt" and isinstance(opt, int) and 0 <= opt < len(p["opts"]):
            q = p["questions"][p["qi"]]
            p["answers"][q["question"]] = p["opts"][opt]
            log("answered", p["id"], f"q{p['qi'] + 1}", repr(p["opts"][opt]))
            p["qi"] += 1
            if p["qi"] < len(p["questions"]):              # next question in the same tool call
                self.load_question(p)
                return self.changed()
            updated = dict(p["tool_input"], answers=p["answers"])
            return self.resolve(p, {"output": {"hookSpecificOutput": {
                "hookEventName": "PreToolUse", "permissionDecision": "allow", "updatedInput": updated}}})
        self.write({"t": "nack", "id": p["id"], "why": "gone"})

    @staticmethod
    def load_question(p):
        q = p["questions"][p["qi"]]
        p["text"] = str(q.get("question", ""))[:MAX_TEXT]
        p["tool"] = str(q.get("header", "Question"))[:15]
        p["opts"] = [str(o.get("label", ""))[:28] for o in q.get("options", [])][:4]
        p["qpos"] = f"{p['qi'] + 1}/{len(p['questions'])}" if len(p["questions"]) > 1 else ""

    # ── hooks ───────────────────────────────────────────────────────────────
    def session_for(self, agent, ev):
        sid = (ev.get("session_id") or "unknown")[:8]
        s = self.sessions.get(sid)
        if s is None:
            s = self.sessions[sid] = {"sid": sid, "agent": agent, "st": "idle", "tool": "", "target": "",
                                      "label": os.path.basename((ev.get("cwd") or "").rstrip("/")) or "?"}
        s["last"] = time.time()
        s["agent"] = agent               # the hook that sent this event decides the output format
        return s

    def reduce(self, s, ev):
        name = ev.get("hook_event_name")
        if name == "SessionEnd":
            self.sessions.pop(s["sid"], None)
        elif name == "SessionStart":
            s.update(st="idle", tool="", target="")
        elif name == "UserPromptSubmit":
            s.update(st="busy", tool="", target="")
        elif name == "PreToolUse":
            text, _, _ = describe(ev.get("tool_name", ""), ev.get("tool_input") or {}, ev.get("cwd", ""))
            s.update(st="busy", tool=ev.get("tool_name", ""), target=text[:40])
        elif name in ("PostToolUse", "PostToolUseFailure"):
            if s["st"] == "needs_you":
                s["st"] = "busy"
        elif name == "PermissionRequest":
            s["st"] = "needs_you"
        elif name == "Notification" and ev.get("notification_type") == "idle_prompt":
            s["st"] = "done"
        elif name == "Stop":
            s.update(st="done", tool="", target="")
        elif name == "StopFailure":
            s["st"] = "error"
        elif name == "Interrupt":                     # Codex: the user interrupted the turn
            s.update(st="idle", tool="", target="")

    def settle_elsewhere(self, s, ev):
        """Retract requests this event shows were already answered somewhere else.

        The terminal CLI kills the hook when its dialog is answered; the desktop app does not, so without this a
        request approved in the app would linger on the device until it expired.
        """
        name = ev.get("hook_event_name")
        if name in ("PostToolUse", "PostToolUseFailure", "PermissionDenied"):
            key = call_key(ev)
            gone = [p for p in self.pending.values() if p["sid"] == s["sid"] and p["key"] == key]
        elif name in ("Stop", "StopFailure", "UserPromptSubmit", "SessionEnd", "Interrupt"):
            gone = [p for p in self.pending.values() if p["sid"] == s["sid"]]
        else:
            return
        for p in gone:
            del self.pending[p["id"]]
            if not p["fut"].done():
                p["fut"].set_result(None)    # release the hook with no decision

    async def on_hook(self, reader, writer):
        try:
            line = await reader.readline()
            msg = json.loads(line)
            agent, ev = msg.get("agent", "claude"), msg["event"]
        except (ValueError, KeyError, ConnectionError):
            writer.close()
            return
        s = self.session_for(agent, ev)
        self.settle_elsewhere(s, ev)
        self.reduce(s, ev)
        name = ev.get("hook_event_name")
        question = self.question_for(agent, ev) if msg.get("wait") and self.device_live() else None
        if name != "PermissionRequest" and not msg.get("wait"):
            self.changed()
            writer.close()
            return
        if not self.device_live() or (name != "PermissionRequest" and not question):
            self.changed()
            writer.write(b"{}\n")      # nothing to hold: the agent's own UI is the only path
            await writer.drain()
            writer.close()
            return
        p = question or self.permission_for(s, ev)
        p["key"] = call_key(ev)
        await self.hold(s, p, reader, writer)

    @staticmethod
    def question_for(agent, ev):
        """A question the device can answer, or None. Claude only (Codex has no equivalent)."""
        if agent != "claude":
            return None
        name, inp = ev.get("hook_event_name"), ev.get("tool_input") or {}
        if name == "PreToolUse" and ev.get("tool_name") == "AskUserQuestion":
            qs = [q for q in inp.get("questions", []) if isinstance(q, dict) and q.get("options")]
            if not qs or len(qs) != len(inp.get("questions", [])):
                return None
            p = {"kind": "ask", "questions": qs, "qi": 0, "answers": {}, "tool_input": inp,
                 "risk": "normal", "trunc": 0}
            Daemon.load_question(p)
            return p
        if name == "Stop" and not ev.get("stop_hook_active"):
            msg = (ev.get("last_assistant_message") or "").strip()
            last = msg.splitlines()[-1].strip() if msg else ""
            if not QUESTION_END.search(last):
                return None
            text = last.lstrip("#>*- ").strip()
            trunc = max(0, len(text) - MAX_TEXT)
            return {"kind": "yesno", "tool": "Question", "text": text[-MAX_TEXT:] if trunc else text,
                    "trunc": trunc, "risk": "normal", "opts": ["Yes", "No"], "qpos": ""}
        return None

    @staticmethod
    def permission_for(s, ev):
        text, trunc, risk = describe(ev.get("tool_name", ""), ev.get("tool_input") or {}, ev.get("cwd", ""))
        return {"kind": "perm", "tool": ev.get("tool_name", ""), "text": text, "trunc": trunc, "risk": risk,
                "raw": (ev.get("tool_input") or {}).get("command"), "opts": [], "qpos": "",
                # "allow for this session" only for an exact, fully shown, normal-risk Claude Bash command
                "sess": s["agent"] == "claude" and ev.get("tool_name") == "Bash" and risk == "normal"
                        and not trunc and isinstance((ev.get("tool_input") or {}).get("command"), str)}

    async def hold(self, s, p, reader, writer):
        ttl = TTL if p["kind"] == "perm" else QUESTION_TTL
        rid = "p_" + secrets.token_hex(4)
        fut = asyncio.get_running_loop().create_future()
        p.update(id=rid, sid=s["sid"], agent=s["agent"], label=s["label"], fut=fut, key=p.get("key", ""),
                 exp=int(time.time() + ttl), created=int(time.time()))
        p.setdefault("sess", False)
        self.pending[rid] = p
        s["st"] = "needs_you"
        log("pending", rid, p["kind"], s["label"], p["tool"], repr(p["text"][:60]), p["risk"])
        self.changed()
        eof = asyncio.ensure_future(reader.read(1))   # hook killed => answered at the keyboard (or Esc)
        done, _ = await asyncio.wait({fut, eof}, timeout=ttl, return_when=asyncio.FIRST_COMPLETED)
        eof.cancel()
        reply = fut.result() if fut in done else None
        if fut in done and reply is None:              # answered elsewhere (settle_elsewhere)
            log("retracted", rid, "(answered elsewhere)")
            self.write({"t": "toast", "text": "Answered elsewhere"})
            reply = {}
        elif fut not in done:
            self.pending.pop(rid, None)
            log("retracted", rid, "(answered at keyboard)" if eof in done else "(expired)")
            self.write({"t": "toast", "text": "Answered at keyboard" if eof in done else "Request expired"})
            reply = {}
        try:
            writer.write((json.dumps(reply) + "\n").encode())
            await writer.drain()
        except ConnectionError:
            if reply:
                log("hook gone before the answer was delivered", rid)
        stopped = reply.get("decision", {}).get("interrupt")
        s.update(st="idle", tool="", target="") if stopped else s.update(st="done" if p["kind"] == "yesno" and not reply else "busy")
        writer.close()
        self.changed()

    # ── main ────────────────────────────────────────────────────────────────
    async def housekeeping(self):
        while True:
            await asyncio.sleep(HEARTBEAT)
            if self.fd is None:
                self.open_serial()
            cutoff = time.time() - SESSION_GC
            for sid in [k for k, s in self.sessions.items() if s["last"] < cutoff]:
                del self.sessions[sid]
            self.send_snap()

    async def run(self, sock_path):
        if len(sock_path.encode()) > 103:
            raise SystemExit(f"socket path too long for macOS AF_UNIX (max 103 bytes): {sock_path}")
        os.makedirs(os.path.dirname(sock_path), mode=0o700, exist_ok=True)
        if os.path.exists(sock_path):
            os.unlink(sock_path)
        server = await asyncio.start_unix_server(self.on_hook, path=sock_path)
        os.chmod(sock_path, 0o600)
        log("listening", sock_path, "serial", self.serial_arg)
        self.open_serial()
        asyncio.ensure_future(self.poll_usage())
        async with server:
            await self.housekeeping()


def call_key(ev):
    """Identity of a tool call across PermissionRequest and PostToolUse (PermissionRequest has no tool_use_id)."""
    return ev.get("tool_name", "") + "|" + json.dumps(ev.get("tool_input") or {}, sort_keys=True)


def parse_pulse(report):
    """Pulse --json -> {agent: {"age", "win": [{"k", "used", "resets"}]}}. Only fresh, provider-reported windows."""
    usage = {}
    for acc in report.get("accounts", []):
        agent = PULSE_AGENTS.get(acc.get("id"))        # first account per provider only
        if not agent or acc.get("ageSeconds", USAGE_STALE) >= USAGE_STALE:
            continue
        wins = []
        for w in acc.get("windows", []):
            k = WINDOW_KEYS.get(w.get("kind"))
            if not k or w.get("estimated") or w.get("usedPercent") is None:
                continue
            resets = w.get("resetsAt")
            wins.append({"k": k, "used": int(w["usedPercent"]),
                         "resets": calendar.timegm(time.strptime(resets, "%Y-%m-%dT%H:%M:%SZ")) if resets else 0})
        if wins:
            usage[agent] = {"age": int(acc["ageSeconds"]), "win": wins[:2]}
    return usage


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--serial", default="auto", help="device path, or 'auto' to scan /dev/cu.usbmodem*")
    ap.add_argument("--sock", default=os.environ.get("ACS_SOCK", os.path.expanduser("~/.acs/acsd.sock")))
    args = ap.parse_args()
    try:
        asyncio.run(Daemon(args.serial).run(args.sock))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
