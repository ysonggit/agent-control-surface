#!/usr/bin/env python3
"""Animated mock-up of the MP3 page for the README (docs/img/mp3.gif).

Draws the 280x456 AMOLED layout frame by frame: spectrum bars, progress, a "next" tap and a scrolling CJK title.
Chinese is drawn at 16 px with a system font and doubled without smoothing, close to the device's 2x Unifont.
Example tracks only. Needs Pillow.
"""
import os, random
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "docs", "img", "mp3.gif")
W, H, BEZEL = 280, 456, 8
GREEN, LCD, DIM, TRACK = (29, 185, 84), (3, 38, 35), (138, 143, 152), (42, 42, 42)
CJK = ImageFont.truetype("/System/Library/Fonts/Hiragino Sans GB.ttc", 16)
UI = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 22)
SMALL = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 12)
TRACKS = [("晴天", "周杰伦", "叶惠美", 269, 63), ("认真的雪（温柔版）", "藤柒吖", "", 247, 0)]


def pixel_text(text, color):
    """Text drawn at 16 px, then doubled with nearest-neighbour scaling: the device's pixel look."""
    w = int(CJK.getlength(text)) + 2
    im = Image.new("RGBA", (max(w, 1), 18), (0, 0, 0, 0))
    ImageDraw.Draw(im).text((0, 0), text, font=CJK, fill=color)
    return im.resize((im.width * 2, im.height * 2), Image.NEAREST)


def row(img, text, y, color, offset):
    if not text:
        return
    t = pixel_text(text, color)
    x0, avail = 14, W - 28
    if t.width <= avail:
        img.alpha_composite(t, (x0, y))
    else:                                            # scrolls, with a 40 px gap before the next copy
        for x in (x0 - offset, x0 - offset + t.width + 40):
            img.alpha_composite(t, (x, y))


def frame(track, pos, playing, eq, offsets, toast):
    title, artist, album, dur, _ = track
    img = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    d = ImageDraw.Draw(img)
    d.text((14, 8), "MP3", font=UI, fill=(255, 255, 255))
    st = "Spotify  PLAYING" if playing else "Spotify  PAUSED"
    d.text((W - 14 - d.textlength(st, font=SMALL), 14), st, font=SMALL, fill=GREEN)
    d.rounded_rectangle((11, 43, W - 11, 160), 14, fill=LCD)
    bx, bw, base, hmax, gap = 26, W - 52, 148, 91, 4
    bar_w = (bw - 13 * gap) / 14
    for i, e in enumerate(eq):
        h = max(3, int(hmax * e))
        x = bx + i * (bar_w + gap)
        d.rectangle((x, base - h, x + bar_w - 1, base), fill=GREEN)
    row(img, title, 168, (255, 255, 255, 255), offsets[0])
    row(img, artist, 204, (195, 195, 195, 255), offsets[1])
    row(img, album, 238, (138, 143, 152, 255), offsets[2])
    d.rounded_rectangle((14, 279, W - 14, 284), 3, fill=TRACK)
    d.rounded_rectangle((14, 279, 14 + max(5, int((W - 28) * pos / dur)), 284), 3, fill=GREEN)
    fmt = lambda s: f"{int(s) // 60}:{int(s) % 60:02d}"
    d.text((14, 292), fmt(pos), font=SMALL, fill=DIM)
    d.text((W - 14 - d.textlength(fmt(dur), font=SMALL), 292), fmt(dur), font=SMALL, fill=DIM)
    cy, s = 348, 13
    for c, kind in ((W // 6, "prev"), (W // 2, "play"), (5 * W // 6, "next")):
        if kind == "prev":
            d.polygon([(c - s, cy), (c, cy - s), (c, cy + s)], fill="white")
            d.polygon([(c, cy), (c + s, cy - s), (c + s, cy + s)], fill="white")
        elif kind == "next":
            d.polygon([(c + s, cy), (c, cy - s), (c, cy + s)], fill="white")
            d.polygon([(c, cy), (c - s, cy - s), (c - s, cy + s)], fill="white")
        elif playing:
            d.rectangle((c - s, cy - s, c - s + 7, cy + s), fill="white")
            d.rectangle((c + s - 7, cy - s, c + s, cy + s), fill="white")
        else:
            d.polygon([(c - s + 2, cy - s - 2), (c - s + 2, cy + s + 2), (c + s + 4, cy)], fill="white")
    hint = "tap  prev / play / next   swipe = page"
    d.text(((W - d.textlength(hint, font=SMALL)) / 2, 380), hint, font=SMALL, fill=DIM)
    if toast:
        d.text((14, H - 22), toast, font=SMALL, fill=(253, 159, 0))
    out = Image.new("RGB", (W + 2 * BEZEL, H + 2 * BEZEL), (255, 255, 255))
    ImageDraw.Draw(out).rounded_rectangle((0, 0, W + 2 * BEZEL - 1, H + 2 * BEZEL - 1), 26, fill=(34, 38, 44))
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, W - 1, H - 1), 20, fill=255)
    out.paste(img.convert("RGB"), (BEZEL, BEZEL), mask)
    return out


def main():
    random.seed(7)
    eq = [0.1] * 14
    frames = []
    for idx, (n_frames, toast_at) in enumerate(((26, None), (60, 0))):
        track = TRACKS[idx]
        pos, offsets = track[4], [0, 0, 0]
        for f in range(n_frames):
            for i in range(14):
                target = 0.15 + random.random() * 0.85 * (0.7 if i < 4 or i > 10 else 1.0)
                eq[i] += (target - eq[i]) * 0.45
            toast = "Next" if toast_at is not None and f < 8 else None
            frames.append(frame(track, pos, True, eq, offsets, toast))
            pos += 0.12
            offsets[0] += 3
            title_w = pixel_text(track[0], (0, 0, 0, 255)).width
            offsets[0] %= title_w + 40
    frames[0].save(OUT, save_all=True, append_images=frames[1:], duration=120, loop=0, optimize=True, disposal=2)
    print("wrote", OUT, len(frames), "frames", os.path.getsize(OUT) // 1024, "KB")


if __name__ == "__main__":
    main()
