#!/usr/bin/env python3
"""Make DeckReader's icon and Steam library artwork (SVG + PNG).

    tools/make-artwork.py

Same visual language as DeckVideoPlayer's artwork (dark gradient, ring,
glow, Inter), in DeckReader's teal instead of its blue (D20), and with an
open book in the ring instead of a play triangle: one family in the Steam
library, but easy to tell apart. PNGs are rendered with
ffmpeg's librsvg decoder, using the bundled Inter (run scripts/dev-build.sh
first so .dev/app/share/deckreader/fonts exists).
"""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STEAM = os.path.join(ROOT, "packaging", "steam")
ICONS = os.path.join(ROOT, "packaging", "icons")
APP_ID = "io.github.nicocarbone.DeckReader"
FONTS = os.path.join(ROOT, ".dev", "app", "share", "deckreader", "fonts")

DEFS = """<defs><linearGradient id="bg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#18222E"/>\
<stop offset="1" stop-color="#090C10"/></linearGradient><linearGradient id="ring" x1="0" y1="0" x2="1" y2="1">\
<stop offset="0" stop-color="#5EE6C8"/><stop offset="1" stop-color="#0B8C79"/></linearGradient>\
<radialGradient id="glow" cx="0.5" cy="0.5" r="0.5"><stop offset="0" stop-color="#10A38F" stop-opacity="0.45"/>\
<stop offset="1" stop-color="#10A38F" stop-opacity="0"/></radialGradient></defs>"""


def book(cx, cy, r, ink="#0E1217"):
    """An open book centred in a ring of radius r."""
    s = r / 120.0

    def p(x, y):
        return "%.2f %.2f" % (cx + x * s, cy + y * s)

    left = "M%s L%s L%s L%s Z" % (p(-6, -34), p(-70, -46), p(-70, 40), p(-6, 52))
    right = "M%s L%s L%s L%s Z" % (p(6, -34), p(70, -46), p(70, 40), p(6, 52))
    out = ['<path d="%s" fill="#fff" stroke="#fff" stroke-width="%.2f" stroke-linejoin="round"/>' % (d, 10 * s)
           for d in (left, right)]
    # Lines of text on both pages.
    for i, y in enumerate((-24, -10, 4, 18, 32)):
        w = 40 if i % 2 == 0 else 32
        for side in (-1, 1):
            x0 = side * 16
            x1 = side * (16 + w)
            dy = -abs(x1) * 0.12  # follows the page's slant
            out.append('<line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f" stroke="%s" stroke-width="%.2f" '
                       'stroke-linecap="round" stroke-opacity="0.55"/>'
                       % (cx + x0 * s, cy + (y - abs(x0) * 0.12) * s, cx + x1 * s, cy + (y + dy) * s, ink, 5 * s))
    return "".join(out)


def ring(cx, cy, r, width):
    return ('<circle cx="%g" cy="%g" r="%g" fill="none" stroke="url(#ring)" stroke-width="%.1f"/>' % (cx, cy, r, width)
            + book(cx, cy, r))


def text(x, y, s, size, weight=800, fill="#fff", anchor="start", spacing=0):
    extra = ' letter-spacing="%g"' % spacing if spacing else ""
    return ('<text x="%g" y="%g" text-anchor="%s" font-family="Inter" font-weight="%d" font-size="%g" fill="%s"%s>%s</text>'
            % (x, y, anchor, weight, size, fill, extra, s))


TAGLINE = "EPUB · OLED · CONTROLLER"


def svg(w, h, body, background=True):
    bg = '<rect width="%d" height="%d" fill="url(#bg)"/>' % (w, h) if background else ""
    return '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d">%s\n%s\n%s\n</svg>\n' % (w, h, DEFS, bg, body)


def artwork():
    yield "steam/capsule", svg(600, 900, "".join([
        '<circle cx="300" cy="360" r="300" fill="url(#glow)"/>',
        ring(300, 360, 120, 24),
        text(300, 610, "Deck", 64, anchor="middle"),
        text(300, 684, "Reader", 64, anchor="middle"),
        text(300, 760, TAGLINE, 26, 600, "#5EE6C8", "middle", 6),
    ]))
    yield "steam/header", svg(920, 430, "".join([
        '<circle cx="220" cy="215" r="260" fill="url(#glow)"/>',
        ring(220, 215, 110, 22),
        text(400, 200, "Deck", 66),
        text(400, 272, "Reader", 66),
        text(402, 330, TAGLINE, 22, 600, "#5EE6C8", spacing=5),
    ]))
    yield "steam/hero", svg(1920, 620, "".join([
        '<circle cx="1500" cy="310" r="560" fill="url(#glow)"/>',
        ring(1500, 310, 190, 34),
    ]))
    yield "steam/logo", svg(1440, 320, "".join([
        ring(160, 160, 120, 24),
        text(320, 197, "DeckReader", 120),
    ]), background=False)
    icon = ('<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">%s\n'
            '<rect x="8" y="8" width="240" height="240" rx="56" fill="url(#bg)"/>\n%s\n</svg>\n'
            % (DEFS, '<circle cx="128" cy="128" r="78" fill="none" stroke="url(#ring)" stroke-width="16"/>'
               + book(128, 128, 78)))
    yield "icons/" + APP_ID, icon


def render(svg_path, png_path, env):
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-c:v", "librsvg", "-i", svg_path,
                    "-frames:v", "1", png_path], check=True, env=env)


def main():
    if not os.path.isdir(FONTS):
        print("Inter not found in %s: run scripts/dev-build.sh first" % FONTS, file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        conf = os.path.join(tmp, "fonts.conf")
        with open(conf, "w") as f:
            f.write('<?xml version="1.0"?><fontconfig><dir>%s</dir><cachedir>%s</cachedir>'
                    '<include ignore_missing="yes">/etc/fonts/fonts.conf</include></fontconfig>' % (FONTS, tmp))
        env = dict(os.environ, FONTCONFIG_FILE=conf)
        for name, content in artwork():
            base = os.path.join(ROOT, "packaging", name)
            with open(base + ".svg", "w", encoding="utf-8") as f:
                f.write(content)
            png = base + ("-256.png" if name.startswith("icons/") else ".png")
            render(base + ".svg", png, env)
            print("wrote", os.path.relpath(png, ROOT))
        # Steam's icon is the app icon.
        render(os.path.join(ICONS, APP_ID + ".svg"), os.path.join(STEAM, "icon.png"), env)
        print("wrote packaging/steam/icon.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
