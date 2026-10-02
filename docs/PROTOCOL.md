# Device protocol v1

Transport: USB CDC serial (`/dev/cu.usbmodem*` on macOS). Newline-delimited JSON (NDJSON), UTF-8, one object per line, max 2048 bytes per line. Baud rate is ignored by CDC; use 115200.

The host (daemon `acsd`) is the only authority. The device renders the latest snapshot and emits input events. It holds no agent state machine.

## Host → device

### `snap` — full state, sent on every change and at least every 5 s (heartbeat)

```json
{"t":"snap","rev":412,"now":1790000000,
 "sessions":[{"sid":"a1b2","agent":"claude","label":"pad-fw","st":"busy","tool":"Edit","target":"src/ui.c","age":12}],
 "pending":[{"id":"p_9f2c1d","sid":"a1b2","agent":"claude","label":"pad-fw","tool":"Bash",
             "text":"git push origin main","trunc":0,"risk":"high","exp":1790000290}],
 "usage":{}}
```

| Field | Meaning |
|---|---|
| `rev` | Increases on every change. Device echoes it in `ack`. |
| `now` | Host epoch seconds. Device computes countdowns as `exp - now` plus its own elapsed time since the snap arrived. No RTC needed. |
| `sessions[].st` | `idle` · `busy` · `needs_you` · `done` · `error` |
| `sessions[].agent` | `claude` · `codex` — selects the icon. |
| `sessions[].age` | Seconds since the last event for that session. |
| `pending` | Oldest first. The device shows `pending[0]`; the count shows the queue. |
| `pending[].text` | Literal text (Bash command, file path, URL). Never an LLM summary. |
| `pending[].trunc` | Characters cut from `text`. `>0` means the device must render a truncation marker. |
| `pending[].kind` | `perm` (tool permission) · `yesno` (Claude ended its turn with a question) · `ask` (Claude's AskUserQuestion; one question at a time). |
| `pending[].opts`, `qpos` | Option labels (`ask`: 2–4; `yesno`: `["Yes","No"]`) and `"1/2"` when one tool call carries several questions. For questions `tool` is the question's header and `text` the question. |
| `pending[].created` | Epoch seconds the request arrived; the device shows "Ns ago". |
| `pending[].sess` | `true` when "allow for this session" is offered: Claude, Bash, normal risk, not truncated. |
| `pending[].risk` | `normal` or `high`. For `high`, the device shows no ALLOW control ("Approve at keyboard"). The host also rejects `allow` for `high`. |
| `usage` | `{"claude":{"age":20,"win":[{"k":"5h","used":29,"resets":1790520000},{"k":"7d","used":4,"resets":1790636400}]},"codex":{…}}`. From `Pulse --json` (provider-reported figures only; readings older than 1 h and estimated windows are dropped). A missing agent means unavailable: the device says so and never shows zero. `used` is percent used; `resets` is epoch seconds. |

### `toast` — transient message, shown ~2 s

```json
{"t":"toast","text":"Answered at keyboard"}
```

### `nack` — a decision was rejected

```json
{"t":"nack","id":"p_9f2c1d","why":"gone"}
```

`why`: `gone` (already answered at keyboard, expired, or unknown id) · `high_risk` (allow refused for a high-risk request) · `not_eligible` (`allow_session` for a request with `sess: false`).

## Device → host

```json
{"t":"hello","fw":"0.1.0","w":172,"h":320}
{"t":"ack","rev":412}
{"t":"decide","id":"p_9f2c1d","d":"allow"}
```

| Message | When |
|---|---|
| `hello` | On boot and whenever the host's first `snap` arrives after a gap. The host replies with a fresh `snap`. |
| `ack` | After rendering each `snap`. The host treats the device as **live** only while an `ack` arrived in the last 12 s. |
| `decide` | Permission: `d` is `allow` · `allow_session` · `deny` · `deny_stop`. `yesno`: `yes` · `no` · `release`. `ask`: `opt` with `"opt": <index>`, or `release`. `release` hands the question back to the terminal. Must name a pending `id`. `allow_session` adds an in-memory Claude rule for this exact Bash command (`destination: "session"`, never written to a settings file). |

## Input mapping (firmware)

| Input | Action |
|---|---|
| Hold "Always / this session" 1.5 s (only when `sess`) | `decide allow_session` |
| Swipe the command area, or tap `<` / `>` | Show the previous / next queued request; decisions apply to the one shown |
| Touch-and-hold ALLOW 800 ms, or hold USR1 800 ms | `decide allow` (only when `risk=normal` and `trunc=0`; otherwise "Approve at keyboard") |
| Tap DENY, or short-press USR2 | `decide deny` on `pending[0]` |
| Hold USR2 1.5 s | `decide deny_stop` on `pending[0]` |
| USR1 short, nothing pending | Cycle brightness 100/60/30 % |
| USR2 short, nothing pending | Cycle the focused session on the status screen |
| Any touch/button while the screen sleeps (90 s idle, nothing pending) | Wakes the screen only; the press does nothing else |

## Questions (Claude only)

- `yesno` comes from the `Stop` hook (installed with `--wait`, timeout 150 s) when the last line of Claude's reply ends in `?` and the turn is not already a stop-hook continuation. `yes`/`no` reach Claude as `additionalContext` ("The user answered from the control surface: Yes.") and the turn continues.
- `ask` comes from a `PreToolUse` hook on `AskUserQuestion` (`--wait`). When every question is answered the hook returns `permissionDecision: allow` with the original `questions` plus `answers`.
- While a question is held, the terminal cannot show it, so the host releases it after 120 s. `release`, the device's "answer at keyboard", or Esc in the terminal ends the hold immediately.

## Host guarantees

- A decision is accepted once per `id` (single use); a second one gets `nack gone`.
- When the keyboard answers first, Claude Code kills the hook. The host removes the request, sends a new `snap` and a `toast`.
- When no device is live, hooks return immediately with no decision, so the agent's normal terminal dialog is the only path.
