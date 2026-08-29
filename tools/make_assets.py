#!/usr/bin/env python3
# Copyright (C) 2026 Solemn Scribe
# SPDX-License-Identifier: GPL-3.0-or-later
# This file is part of Phantom Fence (see LICENSE).
"""Generate MSIX/Store logo assets for PhantomFence.

Draws the mark (monitor with a red slash) at high resolution with
supersampling, then emits every size/scale variant the manifest needs.
"""
from PIL import Image, ImageDraw
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "store", "Assets")
os.makedirs(OUT, exist_ok=True)

BODY   = (56, 74, 100, 255)     # slate monitor body
SCREEN = (30, 41, 59, 255)      # darker screen
EDGE   = (148, 168, 194, 255)   # light edge
STAND  = (120, 132, 150, 255)
SLASH  = (222, 74, 74, 255)


def draw_mark(canvas_px: int, pad_frac: float = 0.10) -> Image.Image:
    """Render the mark on a transparent square canvas (supersampled 4x)."""
    ss = 4
    S = canvas_px * ss
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    pad = S * pad_frac
    w = S - 2 * pad
    # Monitor proportions inside the padded box
    mon_h = w * 0.62
    top = (S - (mon_h + w * 0.16)) / 2          # leave room for stand+base
    left = pad
    right = S - pad
    bottom = top + mon_h
    r = S * 0.045

    edge_w = max(2, int(S * 0.022))
    d.rounded_rectangle([left, top, right, bottom], radius=r,
                        fill=BODY, outline=EDGE, width=edge_w)
    inset = S * 0.045
    d.rounded_rectangle([left + inset, top + inset, right - inset, bottom - inset],
                        radius=r * 0.6, fill=SCREEN)

    # Stand + base
    cx = S / 2
    stand_w = w * 0.14
    stand_h = w * 0.09
    d.rectangle([cx - stand_w / 2, bottom, cx + stand_w / 2, bottom + stand_h],
                fill=STAND)
    base_w = w * 0.38
    base_h = max(2, S * 0.028)
    d.rounded_rectangle([cx - base_w / 2, bottom + stand_h,
                         cx + base_w / 2, bottom + stand_h + base_h],
                        radius=base_h / 2, fill=STAND)

    # Red slash corner-to-corner over the monitor
    lw = max(3, int(S * 0.055))
    d.line([left + inset * 0.6, bottom - inset * 0.2,
            right - inset * 0.6, top + inset * 0.2],
           fill=SLASH, width=lw)
    # round the slash ends
    for (x, y) in [(left + inset * 0.6, bottom - inset * 0.2),
                   (right - inset * 0.6, top + inset * 0.2)]:
        d.ellipse([x - lw / 2, y - lw / 2, x + lw / 2, y + lw / 2], fill=SLASH)

    return img.resize((canvas_px, canvas_px), Image.LANCZOS)


def save_square(name: str, size: int):
    draw_mark(size).save(os.path.join(OUT, name))


def save_wide(name: str, w: int, h: int):
    """Wide tile: mark centered on transparent wide canvas."""
    mark = draw_mark(h)
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    img.paste(mark, ((w - h) // 2, 0), mark)
    img.save(os.path.join(OUT, name))


# Square150x150Logo (medium tile)
for scale, px in [(100, 150), (200, 300), (400, 600)]:
    save_square(f"Square150x150Logo.scale-{scale}.png", px)
save_square("Square150x150Logo.png", 150)

# Wide310x150Logo (wide tile)
for scale, (w, h) in [(100, (310, 150)), (200, (620, 300)), (400, (1240, 600))]:
    save_wide(f"Wide310x150Logo.scale-{scale}.png", w, h)
save_wide("Wide310x150Logo.png", 310, 150)

# Square44x44Logo (app list / taskbar)
for scale, px in [(100, 44), (200, 88), (400, 176)]:
    save_square(f"Square44x44Logo.scale-{scale}.png", px)
save_square("Square44x44Logo.png", 44)
for ts in [16, 24, 32, 48, 256]:
    save_square(f"Square44x44Logo.targetsize-{ts}.png", ts)
    save_square(f"Square44x44Logo.targetsize-{ts}_altform-unplated.png", ts)

# StoreLogo
for scale, px in [(100, 50), (200, 100), (400, 200)]:
    save_square(f"StoreLogo.scale-{scale}.png", px)
save_square("StoreLogo.png", 50)

print("Wrote", len(os.listdir(OUT)), "assets to", OUT)
