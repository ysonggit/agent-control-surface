#!/usr/bin/env python3
"""Add (or with --uninstall remove) the acs hooks in ~/.claude/settings.json and ~/.codex/hooks.json.

Existing hooks are left untouched; ours are recognised by the path to acs_hook.py. A timestamped backup of
each file is written before any change.
"""
import json, os, shutil, sys, time

HOOK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "acs_hook.py")
TARGETS = {
    "claude": (os.path.expanduser("~/.claude/settings.json"),
               ["SessionStart", "SessionEnd", "UserPromptSubmit", "PreToolUse", "PostToolUse", "PermissionRequest",
                "Notification", "Stop", "StopFailure"]),
    "codex": (os.path.expanduser("~/.codex/hooks.json"),
              ["SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "PermissionRequest", "Stop",
               "Interrupt"]),
}
TOOL_EVENTS = {"PreToolUse", "PostToolUse", "PermissionRequest"}


def entry(agent, event, matcher=None, wait=False):
    timeout = 300 if event == "PermissionRequest" else 150 if wait else (1 if event == "Interrupt" else 2)
    hook = {"type": "command", "command": f"/usr/bin/env python3 {HOOK} --agent {agent}" + (" --wait" if wait else ""),
            "timeout": timeout}
    e = {"hooks": [hook]}
    if matcher or event in TOOL_EVENTS or event == "Notification":
        e["matcher"] = matcher or "*"
    return e


def entries(agent, event):
    """Claude's end-of-turn and multiple-choice questions can be answered on the device (--wait)."""
    if agent == "claude" and event == "Stop":
        return [entry(agent, event, wait=True)]
    if agent == "claude" and event == "PreToolUse":
        return [entry(agent, event), entry(agent, event, matcher="AskUserQuestion", wait=True)]
    return [entry(agent, event)]


def ours(e):
    return any(HOOK in h.get("command", "") for h in e.get("hooks", []))


def main():
    uninstall = "--uninstall" in sys.argv
    for agent, (path, events) in TARGETS.items():
        data = json.load(open(path)) if os.path.exists(path) else {}
        backup = f"{path}.acs-backup-{time.strftime('%Y%m%d-%H%M%S')}"
        if os.path.exists(path):
            shutil.copy2(path, backup)
        hooks = data.setdefault("hooks", {})
        for event in events:
            kept = [e for e in hooks.get(event, []) if not ours(e)]
            hooks[event] = kept if uninstall else kept + entries(agent, event)
            if not hooks[event]:
                del hooks[event]
        tmp = path + ".acs-tmp"
        with open(tmp, "w") as f:
            json.dump(data, f, indent=2)
            f.write("\n")
        os.replace(tmp, path)
        print(f"{'removed from' if uninstall else 'installed in'} {path} (backup: {backup})")
    if not uninstall:
        print("Codex: open `/hooks` in the Codex CLI once and trust the new acs hooks; Codex skips untrusted hooks.")


if __name__ == "__main__":
    main()
