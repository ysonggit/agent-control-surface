# Agent Control Surface

A small USB device next to the keyboard that shows coding-agent status and lets you answer permission requests (Claude Code first; Codex later).

- `host/acsd.py` — daemon. Hooks connect on `~/.acs/acsd.sock`; the device is USB CDC serial (NDJSON, see [docs/PROTOCOL.md](docs/PROTOCOL.md)). Python 3 standard library only.
- `host/acs_hook.py` — the hook Claude Code runs. Exits instantly with no output when the daemon or device is absent.
- `host/claude-hooks.json` — hook config to merge into `~/.claude/settings.json` (or a project's `.claude/settings.json`).
- `sim/device_sim.py` — terminal stand-in for the device.
- `firmware/` — device firmware for a Seeed XIAO nRF52840 Plus with a 1.47" touch display. `firmware/build.sh` builds it (`firmware/build.sh upload` also flashes it); needs `arduino-cli` with the Seeed nRF52 core and ArduinoJson.

## Daemon at login

`host/com.agentcontrolsurface.acsd.plist` is installed in `~/Library/LaunchAgents` (starts at login, restarts on crash; log: `~/Library/Logs/acsd.log`). `firmware/build.sh upload` pauses it while flashing.

```bash
launchctl bootout gui/$(id -u)/com.agentcontrolsurface.acsd                                        # stop
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.agentcontrolsurface.acsd.plist         # start
```

## Run

```bash
python3 host/acsd.py --serial auto        # or --serial <path printed by the simulator>
python3 sim/device_sim.py                 # in another terminal, for testing without hardware
python3 host/test_e2e.py                  # end-to-end tests with a scripted fake device
```

## Behaviour verified against Claude Code 2.1.185

- The terminal permission dialog and the `PermissionRequest` hook run in parallel; whichever answers first wins. When the keyboard wins, Claude Code kills the hook and the daemon retracts the request from the device.
- `PermissionRequest` does not fire in headless `claude -p` runs.
- `deny` with `interrupt: true` stops the agent. `continue: false` from `PreToolUse` is not a reliable interrupt.

## Safety rules

- The host refuses `allow` for high-risk requests (network, `rm`, `sudo`, `git push`, shell substitution, writes outside the project or to credential paths) and for truncated text. Those can only be denied on the device.
- Decisions are single-use and expire after 290 s.
- The device shows literal command text, never a summary.

## Credits

- UI direction inspired by [Pulse](https://github.com/qunqin24/Pulse) by qunqin24 (Apache-2.0).
- Claude and OpenAI marks from [Lobe Icons](https://github.com/lobehub/lobe-icons) (MIT), as bundled in Pulse. The marks remain the property of their owners; their use identifies compatible services and does not imply endorsement.
