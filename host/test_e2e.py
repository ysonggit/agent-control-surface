#!/usr/bin/env python3
"""End-to-end: real daemon + real hook subprocess + scripted fake device on a pty. Run: python3 host/test_e2e.py"""
import json, os, queue, subprocess, sys, tempfile, threading, time, tty, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
HOOK = [sys.executable, os.path.join(HERE, "acs_hook.py")]


class FakeDevice:
    """Speaks the device side of PROTOCOL.md: hello, ack every snap, record everything."""

    def __init__(self):
        self.master, slave = os.openpty()
        tty.setraw(slave)
        self.path = os.ttyname(slave)
        self._slave = slave            # keep open so the master never sees EIO before the daemon attaches
        self.acking = True
        self.snaps, self.other = queue.Queue(), queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()
        self.send({"t": "hello", "fw": "test", "w": 172, "h": 320})

    def send(self, obj):
        os.write(self.master, (json.dumps(obj) + "\n").encode())

    def _read(self):
        buf = b""
        while True:
            try:
                buf += os.read(self.master, 4096)
            except OSError:
                time.sleep(0.05)
                continue
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                msg = json.loads(line)
                if msg["t"] == "snap":
                    if self.acking:
                        self.send({"t": "ack", "rev": msg["rev"]})
                    self.snaps.put(msg)
                else:
                    self.other.put(msg)

    def wait_pending(self, timeout=5):
        end = time.time() + timeout
        while time.time() < end:
            try:
                snap = self.snaps.get(timeout=0.2)
            except queue.Empty:
                continue
            if snap["pending"]:
                return snap["pending"][0]
        raise AssertionError("no pending request reached the device")

    def wait_other(self, t, timeout=5):
        end = time.time() + timeout
        while time.time() < end:
            try:
                msg = self.other.get(timeout=0.2)
            except queue.Empty:
                continue
            if msg["t"] == t:
                return msg
        raise AssertionError(f"no {t} message")


def perm_event(command, cwd="/tmp/proj"):
    return {"session_id": "sess-1234abcd", "cwd": cwd, "hook_event_name": "PermissionRequest",
            "tool_name": "Bash", "tool_input": {"command": command}}


class E2E(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.sock = os.path.join(cls.tmp, "acsd.sock")
        cls.env = dict(os.environ, ACS_SOCK=cls.sock)
        cls.dev = FakeDevice()
        cls.daemon = subprocess.Popen([sys.executable, os.path.join(HERE, "acsd.py"), "--serial", cls.dev.path,
                                       "--sock", cls.sock], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for _ in range(50):
            if os.path.exists(cls.sock):
                break
            time.sleep(0.1)
        cls.dev.send({"t": "hello", "fw": "test", "w": 172, "h": 320})
        time.sleep(0.5)

    @classmethod
    def tearDownClass(cls):
        cls.daemon.terminate()
        print("\n--- daemon log ---\n" + cls.daemon.communicate(timeout=5)[0])

    def setUp(self):
        self.dev.acking = True
        for q in (self.dev.snaps, self.dev.other):
            while not q.empty():
                q.get()

    def start_hook(self, event):
        p = subprocess.Popen(HOOK, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=self.env)
        p.stdin.write(json.dumps(event))
        p.stdin.close()
        return p

    def decision(self, p):
        out = p.stdout.read().strip()
        p.wait(timeout=5)
        return json.loads(out)["hookSpecificOutput"]["decision"] if out else None

    def test_allow_normal(self):
        p = self.start_hook(perm_event("ls -la"))
        req = self.dev.wait_pending()
        self.assertEqual((req["text"], req["risk"], req["trunc"], req["label"]), ("ls -la", "normal", 0, "proj"))
        self.dev.send({"t": "decide", "id": req["id"], "d": "allow"})
        self.assertEqual(self.decision(p), {"behavior": "allow"})

    def test_high_risk_allow_refused_then_deny(self):
        p = self.start_hook(perm_event("git push origin main"))
        req = self.dev.wait_pending()
        self.assertEqual(req["risk"], "high")
        self.dev.send({"t": "decide", "id": req["id"], "d": "allow"})
        self.assertEqual(self.dev.wait_other("nack")["why"], "high_risk")
        self.dev.send({"t": "decide", "id": req["id"], "d": "deny"})
        self.assertEqual(self.decision(p)["behavior"], "deny")

    def test_deny_stop_sets_interrupt(self):
        p = self.start_hook(perm_event("touch x"))
        req = self.dev.wait_pending()
        self.dev.send({"t": "decide", "id": req["id"], "d": "deny_stop"})
        d = self.decision(p)
        self.assertEqual((d["behavior"], d["interrupt"]), ("deny", True))

    def test_codex_deny_stop_has_no_interrupt(self):
        p = subprocess.Popen(HOOK + ["--agent", "codex"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                             env=self.env)
        p.stdin.write(json.dumps(perm_event("touch c")))
        p.stdin.close()
        req = self.dev.wait_pending()
        self.assertEqual(req["agent"], "codex")
        self.dev.send({"t": "decide", "id": req["id"], "d": "deny_stop"})
        d = self.decision(p)
        self.assertEqual(d["behavior"], "deny")
        self.assertNotIn("interrupt", d)      # Codex fails closed on it

    def test_codex_apply_patch_text(self):
        sys.path.insert(0, HERE)
        from acsd import describe
        patch = "*** Begin Patch\n*** Update File: /tmp/proj/src/a.py\n@@\n-x\n+y\n*** End Patch"
        self.assertEqual(describe("apply_patch", {"command": patch}, "/tmp/proj"), ("patch src/a.py", 0, "normal"))
        outside = patch.replace("/tmp/proj/src/a.py", "/etc/hosts")
        self.assertEqual(describe("apply_patch", {"command": outside}, "/tmp/proj")[2], "high")

    def test_allow_session_adds_in_memory_rule(self):
        p = self.start_hook(perm_event("npm test"))
        req = self.dev.wait_pending()
        self.assertTrue(req["sess"])
        self.assertIn("created", req)
        self.dev.send({"t": "decide", "id": req["id"], "d": "allow_session"})
        d = self.decision(p)
        self.assertEqual(d["behavior"], "allow")
        self.assertEqual(d["updatedPermissions"], [{"type": "addRules", "rules": [{"toolName": "Bash", "ruleContent": "npm test"}],
                                                     "behavior": "allow", "destination": "session"}])

    def test_allow_session_refused_when_not_eligible(self):
        p = self.start_hook(perm_event("git push origin main"))          # high risk
        req = self.dev.wait_pending()
        self.assertFalse(req["sess"])
        self.dev.send({"t": "decide", "id": req["id"], "d": "allow_session"})
        self.assertEqual(self.dev.wait_other("nack")["why"], "high_risk")
        self.dev.send({"t": "decide", "id": req["id"], "d": "deny"})
        self.decision(p)
        p = subprocess.Popen(HOOK + ["--agent", "codex"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                             env=self.env)                                 # Codex fails closed on updatedPermissions
        p.stdin.write(json.dumps(dict(perm_event("npm test"), session_id="codex-sess")))
        p.stdin.close()
        req = self.dev.wait_pending()
        self.assertFalse(req["sess"])
        self.dev.send({"t": "decide", "id": req["id"], "d": "allow_session"})
        self.assertEqual(self.dev.wait_other("nack")["why"], "not_eligible")
        self.dev.send({"t": "decide", "id": req["id"], "d": "deny"})
        self.decision(p)

    def start_wait_hook(self, event, agent="claude"):
        p = subprocess.Popen(HOOK + ["--agent", agent, "--wait"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             text=True, env=self.env)
        p.stdin.write(json.dumps(event))
        p.stdin.close()
        return p

    def output(self, p):
        out = p.stdout.read().strip()
        p.wait(timeout=5)
        return json.loads(out) if out else None

    def stop_event(self, message, active=False, sid="sess-q1"):
        return {"session_id": sid, "cwd": "/tmp/proj", "hook_event_name": "Stop",
                "last_assistant_message": message, "stop_hook_active": active}

    def test_yesno_after_question(self):
        p = self.start_wait_hook(self.stop_event("I updated the tests.\nShould I push this to main?"))
        req = self.dev.wait_pending()
        self.assertEqual((req["kind"], req["text"], req["opts"]), ("yesno", "Should I push this to main?", ["Yes", "No"]))
        self.dev.send({"t": "decide", "id": req["id"], "d": "yes"})
        out = self.output(p)
        self.assertEqual(out["hookSpecificOutput"]["hookEventName"], "Stop")
        self.assertIn("Yes", out["hookSpecificOutput"]["additionalContext"])

    def test_stop_without_question_is_not_held(self):
        t = time.time()
        for ev in (self.stop_event("All done."), self.stop_event("Continue?", active=True)):
            self.assertIsNone(self.output(self.start_wait_hook(ev)))
        self.assertIsNone(self.output(self.start_wait_hook(self.stop_event("Push it?"), agent="codex")))
        self.assertLess(time.time() - t, 3)

    def test_ask_user_question_two_questions(self):
        inp = {"questions": [
            {"question": "Which database?", "header": "DB", "options": [{"label": "Postgres"}, {"label": "SQLite"}], "multiSelect": False},
            {"question": "Add tests?", "header": "Tests", "options": [{"label": "Yes"}, {"label": "Later"}], "multiSelect": False}]}
        p = self.start_wait_hook({"session_id": "sess-q2", "cwd": "/tmp/proj", "hook_event_name": "PreToolUse",
                                  "tool_name": "AskUserQuestion", "tool_input": inp})
        req = self.dev.wait_pending()
        self.assertEqual((req["kind"], req["text"], req["opts"], req["qpos"]), ("ask", "Which database?", ["Postgres", "SQLite"], "1/2"))
        self.dev.send({"t": "decide", "id": req["id"], "d": "opt", "opt": 1})
        req2 = self.dev.wait_pending()
        while req2["text"] != "Add tests?":
            req2 = self.dev.wait_pending()
        self.dev.send({"t": "decide", "id": req["id"], "d": "opt", "opt": 0})
        out = self.output(p)["hookSpecificOutput"]
        self.assertEqual(out["permissionDecision"], "allow")
        self.assertEqual(out["updatedInput"]["answers"], {"Which database?": "SQLite", "Add tests?": "Yes"})
        self.assertEqual(out["updatedInput"]["questions"], inp["questions"])

    def test_question_release_to_keyboard(self):
        p = self.start_wait_hook(self.stop_event("Want me to refactor it too?", sid="sess-q3"))
        req = self.dev.wait_pending()
        self.dev.send({"t": "decide", "id": req["id"], "d": "release"})
        self.assertIsNone(self.output(p))

    def test_single_use(self):
        p = self.start_hook(perm_event("touch y"))
        req = self.dev.wait_pending()
        self.dev.send({"t": "decide", "id": req["id"], "d": "deny"})
        self.decision(p)
        self.dev.send({"t": "decide", "id": req["id"], "d": "allow"})
        self.assertEqual(self.dev.wait_other("nack")["why"], "gone")

    def test_keyboard_answer_retracts(self):
        p = self.start_hook(perm_event("touch z"))
        req = self.dev.wait_pending()
        p.kill()                        # what Claude Code does when the terminal dialog is answered first
        p.wait()
        self.assertEqual(self.dev.wait_other("toast")["text"], "Answered at keyboard")
        self.dev.send({"t": "decide", "id": req["id"], "d": "allow"})
        self.assertEqual(self.dev.wait_other("nack")["why"], "gone")

    def test_answered_in_desktop_app_retracts(self):
        # The desktop app does not kill the hook; the PostToolUse for the same call shows it was approved there.
        p = self.start_hook(perm_event("touch d"))
        req = self.dev.wait_pending()
        post = dict(perm_event("touch d"), hook_event_name="PostToolUse")
        self.start_hook(post).wait(timeout=5)
        self.assertIsNone(self.decision(p))
        self.assertEqual(self.dev.wait_other("toast")["text"], "Answered elsewhere")
        self.dev.send({"t": "decide", "id": req["id"], "d": "allow"})
        self.assertEqual(self.dev.wait_other("nack")["why"], "gone")

    def test_truncated_cannot_be_allowed(self):
        p = self.start_hook(perm_event("echo " + "a" * 300))
        req = self.dev.wait_pending()
        self.assertGreater(req["trunc"], 0)
        self.dev.send({"t": "decide", "id": req["id"], "d": "allow"})
        self.assertEqual(self.dev.wait_other("nack")["why"], "high_risk")
        self.dev.send({"t": "decide", "id": req["id"], "d": "deny"})
        self.decision(p)

    def test_zz_device_not_live_passes_through(self):
        self.dev.acking = False
        time.sleep(13)                  # LIVE_WINDOW expires
        t = time.time()
        p = self.start_hook(perm_event("touch w"))
        self.assertIsNone(self.decision(p))
        self.assertLess(time.time() - t, 1.0)

    def test_daemon_down_passes_through(self):
        t = time.time()
        p = subprocess.Popen(HOOK, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                             env=dict(os.environ, ACS_SOCK=os.path.join(self.tmp, "nope.sock")))
        out, _ = p.communicate(json.dumps(perm_event("touch v")), timeout=5)
        self.assertEqual(out, "")
        self.assertLess(time.time() - t, 1.0)

    def test_status_events_update_session(self):
        p = self.start_hook({"session_id": "sess-5678", "cwd": "/tmp/other", "hook_event_name": "PreToolUse",
                             "tool_name": "Edit", "tool_input": {"file_path": "/tmp/other/src/a.c"}})
        p.wait(timeout=5)
        end = time.time() + 5
        while time.time() < end:
            snap = self.dev.snaps.get(timeout=1)
            match = [s for s in snap["sessions"] if s["label"] == "other"]
            if match:
                self.assertEqual((match[0]["st"], match[0]["tool"], match[0]["target"]), ("busy", "Edit", "src/a.c"))
                return
        self.fail("session not reported")


if __name__ == "__main__":
    unittest.main(verbosity=2)
