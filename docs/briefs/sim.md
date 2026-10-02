Goal: write `sim/device_sim.py`, a terminal stand-in for the USB control-surface device, so the host daemon can be tested without hardware. Done = the verification steps below pass.

Contract: `docs/PROTOCOL.md` in this repo. Read it first; implement the device side exactly.

Requirements:
- Python 3 standard library only (no pip installs). Single file.
- Creates a pseudo-terminal pair with `os.openpty()`, puts the slave in raw mode, prints `SERIAL <slave path>` as its first stdout line, and speaks NDJSON on the master fd. The daemon will open the slave path as if it were `/dev/cu.usbmodem*`.
- On start: send `{"t":"hello","fw":"sim-0.1","w":172,"h":320}`. After every `snap`: send `{"t":"ack","rev":<rev>}`.
- Render the current screen as a fixed-width ASCII panel (about 24 columns x 20 rows) redrawn on each snap/toast/nack: a status screen (focused session: agent, label, st, tool, target; count of sessions) or, when `pending` is non-empty, a permission screen for `pending[0]` (agent, label, tool, wrapped literal `text`, a `…+N` marker when `trunc>0`, risk, seconds left from `exp - now`, queue count). For `risk=high` or `trunc>0` show "APPROVE AT KEYBOARD" instead of the ALLOW control.
- Keys (stdin in cbreak mode): `a` = allow (refused locally with a message when risk is high or trunc>0), `d` = deny, `s` = deny_stop, `n` = cycle focused session, `q` = quit. Show toasts and nacks on a message line.
- Flag `--auto allow|deny|deny_stop`: answer every new pending id automatically once, 1 s after it appears (respect the same local refusal for allow), for unattended demos. With `--auto`, do not require a TTY on stdin and print one plain log line per event instead of redrawing.
- Handle the daemon closing/reopening the slave without crashing (EIO on read means no peer; keep waiting).

Constraints:
- Do not create or modify files outside `sim/`. Do not touch `host/`, `docs/`, or any config in the home directory.
- No network access needed; do not add dependencies.

Verify (run these yourself before finishing):
1. `python3 -m py_compile sim/device_sim.py`
2. A self-test: `python3 sim/device_sim.py --selftest` that, in-process, writes a sample `snap` with one pending normal-risk request to the master side's peer, checks that an `ack` with the same rev comes back, then checks `--auto allow` logic produces a `decide` for that id. Exit 0 on pass. Implement `--selftest` and run it.

Report: what you built, the exact commands you ran and their output.
