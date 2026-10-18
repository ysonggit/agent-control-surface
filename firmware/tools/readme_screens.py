#!/usr/bin/env python3
"""Draw the device screens used in the README as SVG (docs/img/*.svg).

Mock-ups of the 280x456 AMOLED layout, not screenshots. The pixel cat comes from cat_sprite.py and the
provider marks from tools/marks (Lobe Icons, MIT, as bundled in Pulse).
"""
import math, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cat_sprite  # noqa: E402

OUT = os.path.join(HERE, "..", "..", "docs", "img")
W, H = 280, 456
FONT = "font-family=\"-apple-system, 'Helvetica Neue', Arial, sans-serif\""
MONO = "font-family=\"'SF Mono', Menlo, Consolas, monospace\""
CAT = {'.': None, 'K': '#1c1c1c', 'W': '#ffffff', 'O': '#f0a030', 'B': '#a0703a', 'P': '#f28ca0', 'R': '#e0302a', 'T': '#f59aaa'}


def mark(name, cx, cy, size):
    d = re.search(r' d="([^"]+)"', open(os.path.join(HERE, "marks", name + ".svg")).read()).group(1)
    s = size / 24
    return f'<path d="{d}" fill="#fff" fill-rule="evenodd" transform="translate({cx - size / 2},{cy - size / 2}) scale({s})"/>'


def cat(state, x, y, scale, tile=None):
    out = []
    if tile:
        out.append(f'<rect x="{x - 3}" y="{y - 3}" width="{18 * scale + 6}" height="{15 * scale + 6}" rx="5" fill="{tile}"/>')
    for r, row in enumerate(cat_sprite.grid(cat_sprite.STATES[state])):
        c = 0
        while c < len(row):
            run = 1
            while c + run < len(row) and row[c + run] == row[c]:
                run += 1
            if CAT[row[c]]:
                out.append(f'<rect x="{x + c * scale}" y="{y + r * scale}" width="{run * scale}" height="{scale}" fill="{CAT[row[c]]}"/>')
            c += run
    return "".join(out)


def ring(cx, cy, r, used, color, icon, dot=None):
    circ = 2 * math.pi * r
    out = [f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="none" stroke="#2a2a2a" stroke-width="12"/>',
           f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="none" stroke="{color}" stroke-width="12" stroke-linecap="round" '
           f'stroke-dasharray="{circ * used / 100:.1f} {circ:.1f}" transform="rotate(-90 {cx} {cy})"/>',
           mark(icon, cx, cy, 56)]
    if dot is not None:
        a = math.radians(dot)
        out.append(f'<circle cx="{cx + r * math.sin(a):.1f}" cy="{cy - r * math.cos(a):.1f}" r="7" fill="#fff"/>')
    return "".join(out)


def text(x, y, s, size=14, fill="#f2f2f2", weight=400, anchor="start", mono=False):
    return (f'<text x="{x}" y="{y}" font-size="{size}" font-weight="{weight}" fill="{fill}" text-anchor="{anchor}" '
            f'{MONO if mono else FONT}>{s}</text>')


def screen(name, body, bg="#000"):
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W + 16} {H + 16}" width="{W + 16}" height="{H + 16}">'
           f'<rect width="{W + 16}" height="{H + 16}" rx="26" fill="#22262c"/>'
           f'<g transform="translate(8,8)"><rect width="{W}" height="{H}" rx="20" fill="{bg}"/>{body}</g></svg>')
    open(os.path.join(OUT, name + ".svg"), "w").write(svg)


def button(y, h, fill, title, sub=None, size=21):
    s = f'<rect x="11" y="{y}" width="{W - 22}" height="{h}" rx="11" fill="{fill}"/>'
    s += text(W / 2, y + (h / 2 + 6 if not sub else h / 2 - 2), title, size, "#fff", 700, "middle")
    if sub:
        s += text(W / 2, y + h / 2 + 18, sub, 13, "#f2f2f2", 400, "middle")
    return s


# main screen: two usage rings (positions as in board_amoled164.h: rings at y 10 and 196, 128 px)
screen("main", "".join([
    ring(140, 74, 58, 29, "#d97757", "claude", dot=65),
    text(140, 172, "29%", 30, "#fff", 700, "middle"),
    text(140, 194, "5h resets 3h40m   7d 4%", 14, "#8a8f98", 400, "middle"),
    ring(140, 260, 58, 8, "#2bd98a", "openai"),
    text(140, 358, "8%", 30, "#fff", 700, "middle"),
    text(140, 380, "5h resets 3h27m   7d 1%", 14, "#8a8f98", 400, "middle"),
    cat("WORK", 9, 391, 3, tile="#f3eee6"),
    text(74, 404, "WORKING", 14, "#28c8f8", 700),
    text(272, 404, "web-app", 13, "#8a8f98", 400, "end"),
    text(74, 424, "Edit src/server.ts", 13, "#bdbdbd"),
    text(74, 446, "USB linked", 12, "#8a8f98"),
    text(272, 446, "USB", 12, "#8a8f98", 400, "end"),
]))

# permission request, high risk: can only be denied on the device
screen("permission", "".join([
    cat("MEOW", 9, 12, 4, tile="#f3eee6"),
    text(98, 28, "PERMISSION", 14, "#fd9f00", 700),
    text(272, 28, "HIGH", 14, "#f06a6e", 700, "end"),
    text(98, 48, "Claude / web-app", 13, "#8a8f98"),
    text(98, 76, "Bash", 22, "#fff", 700),
    text(14, 130, "git push origin", 26, "#e6e6e6", 400, mono=True),
    text(14, 164, "main", 26, "#e6e6e6", 400, mono=True),
    text(14, 272, "#a1b2  12s ago", 13, "#8a8f98"),
    text(266, 272, "&lt; 1/2 &gt;", 13, "#fff", 400, "end"),
    button(291, 71, "#2a2e34", "APPROVE AT KEYBOARD", None, 16),
    button(373, 48, "#b4151a", "DENY", None),
]))

# question at the end of a turn
screen("question", "".join([
    cat("MEOW", 9, 12, 4, tile="#f3eee6"),
    text(98, 28, "QUESTION", 14, "#fd9f00", 700),
    text(98, 48, "Claude / api-server", 13, "#8a8f98"),
    text(98, 76, "Question", 22, "#fff", 700),
    text(14, 126, "Should I push this", 19, "#e6e6e6", mono=True),
    text(14, 152, "to main?", 19, "#e6e6e6", mono=True),
    text(14, 272, "#c3d4  4s ago", 13, "#8a8f98"),
    text(266, 272, "1:56 left", 13, "#8a8f98", 400, "end"),
    button(291, 71, "#2a9128", "Yes", "hold here or BOOT"),
    button(373, 48, "#b4151a", "NO"),
]))

# your turn: a session finished
screen("your-turn", "".join([
    cat("DONE", 140 - 9 * 9, 92, 9),
    text(140, 270, "Your turn", 30, "#1c9e60", 700, "middle"),
    text(140, 302, "Claude / web-app", 15, "#6b6b6b", 400, "middle"),
    text(140, 324, "tap to dismiss", 15, "#6b6b6b", 400, "middle"),
]), bg="#f3eee6")

# boot / waiting for the daemon
screen("boot", "".join([
    cat("SLEEP", 140 - 9 * 9, 92, 9),
    text(206, 84, "z", 26, "#9a9aa4", 700),
    text(140, 270, "Hello Agent", 30, "#1c1c1c", 700, "middle"),
    text(140, 302, "waiting for acsd", 15, "#6b6b6b", 400, "middle"),
]), bg="#f3eee6")
print("wrote", sorted(f for f in os.listdir(OUT) if f.endswith(".svg")))
