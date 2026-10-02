# Device design v0.3 — a hardware Pulse rail

The device is a physical companion to [Pulse](https://github.com/qunqin24/Pulse): the same vertical rail of
usage rings, on a 172×320 portrait screen next to the keyboard, plus the one thing a screen rail cannot do —
answering permission requests with a physical control.

## Principles (taken from Pulse)

- **Provider-reported figures only.** A missing reading shows as a grey ring and `--`, never as 0 %.
- **One ring per agent.** Dark track, arc from 12 o'clock clockwise, coloured by how much is used:
  green < 70 %, amber 70–89 %, red ≥ 90 %. Large white provider mark in the centre; bold percentage below.
- **Activity is shown on the ring, not in text.** A bright dot orbits the ring while that agent is working.
- **Details on demand.** Tapping a ring opens a card with every window as a bar and its reset time.
- **Black, quiet, glanceable.** No clock, no logs, no decoration.

## Screens

| Screen | When | Content |
|---|---|---|
| Rail | Default | Two rings, always Claude (top) and Codex (bottom); under each: `29%` and `5h · resets 3h40m`. Bottom-left: a small cat (cream tile) whose face follows the most urgent session, then that session (`WORKING Edit acs.ino`), then `USB linked` and battery. |
| Detail | Tap a ring, or USR2 short | Small ring + name; one bar per window (`5h 29% used · resets 3h40m`, `7d 4% used · resets 1d8h`); that agent's sessions with their state. Tap or 10 s idle returns to the rail. |
| Your turn | A session finished or waits for input, until any tap/press (that input does nothing else) | Cream card, the pleased "done" cat, `Your turn`, agent / project. |
| Cat mode | USR2 cycle after the detail cards; stays until tapped | Cream page, the cat's face follows the most urgent session: working (glancing down), needs you (meowing), done (^ ^), awake, asleep (no sessions). |
| Permission | A request is pending (always wins) | Meowing cat on a small cream tile; `PERMISSION`, project, tool; literal command; `#sid  Ns ago` and `< 1/3 >` (swipe or tap to switch); for eligible Claude Bash commands an extra teal "Always / this session" button (hold 1.5 s); ALLOW (hold here or USR1) and DENY (tap or USR2). High-risk or truncated: ALLOW replaced by `APPROVE AT KEYBOARD`. |
| Boot / waiting | No host for 12 s | Cream background, 18×15 calico pixel cat (6× scale) asleep with a drifting `z`, `Hello Agent`, `waiting for acsd`. When the host connects the cat opens its eyes and `linked` shows for 1.5 s, then the rail. Sprite: `firmware/tools/cat_sprite.py` → `firmware/acs/cat.h`. Style inspired by hand-drawn calico pixel art; the composition is original. |

## Controls

| Input | Request pending | Otherwise |
|---|---|---|
| USR1 | Hold 0.8 s: allow | Short: brightness 100/60/30 % |
| USR2 | Short: deny · hold 1.5 s: deny + stop | Short: rail → Claude detail → Codex detail → cat mode → rail |
| Touch | Hold ALLOW 0.8 s · tap DENY | Tap a ring: detail · tap detail: back |

When a session finishes or waits for input, its ring track pulses and the bottom line blinks `YOUR TURN` until any touch or press; the screen does not sleep meanwhile. Sleep after 90 s idle with nothing pending; wake on a new request, on a session becoming NEEDS YOU/DONE, or on
any touch/press (the waking press does nothing else).

## Rendering

Rings and marks are rendered into an off-screen RGB565 buffer (anti-aliased, round caps) and pushed as one
image, so animation does not flicker and does not depend on per-pixel SPI writes. Provider marks are 40×40
alpha maps generated from Pulse's bundled Lobe Icons SVGs (`firmware/tools/svg_alpha.swift`). An agent
without a mark gets its initial.

## Credits

Visual language from Pulse by qunqin24 (Apache-2.0). Provider marks from Lobe Icons (MIT); the marks belong to
their owners and identify compatible services only.
