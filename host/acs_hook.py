#!/usr/bin/env python3
"""Claude Code / Codex hook: forward the event to acsd, and for PermissionRequest wait for the device.

Never blocks the agent when the daemon or device is absent: any failure exits 0 with no output,
which leaves the agent's own terminal dialog in charge.
"""
import json, os, socket, sys

SOCK = os.environ.get("ACS_SOCK", os.path.expanduser("~/.acs/acsd.sock"))


def main():
    agent = sys.argv[sys.argv.index("--agent") + 1] if "--agent" in sys.argv else "claude"
    try:
        event = json.load(sys.stdin)
    except ValueError:
        return
    # PermissionRequest always waits for the device; --wait also lets Stop / AskUserQuestion be answered there
    blocking = event.get("hook_event_name") == "PermissionRequest" or "--wait" in sys.argv
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(0.3)
        s.connect(SOCK)
        s.sendall((json.dumps({"agent": agent, "event": event, "wait": blocking}) + "\n").encode())
        if not blocking:
            return
        s.settimeout(None)  # Claude Code kills us if the keyboard answers first
        reply = json.loads(s.makefile().readline() or "{}")
    except (OSError, ValueError):
        return
    if reply.get("output"):                 # full hook output (Stop / PreToolUse answers)
        print(json.dumps(reply["output"]))
        return
    decision = reply.get("decision")
    if decision:
        print(json.dumps({"hookSpecificOutput": {"hookEventName": "PermissionRequest", "decision": decision}}))


if __name__ == "__main__":
    main()
