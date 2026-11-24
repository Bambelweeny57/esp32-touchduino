"""Draw images/c64-stripboard.png: stripboard layout for the C64 datasette adapter
(images/c64-interface.png), on a 9-strip x 24-hole board. The adapter sits at the C64 end:
a stereo lead from TouchDuino (tip IO17, ring IO18, sleeve GND) in, the C64 edge connector out.

The script also checks the layout: it works out which holes are joined (strips,
cuts, links, component pins) and compares the result with the schematic's nets.

Usage: python tools/draw_c64_stripboard.py images/c64-stripboard.png   (needs Pillow, Windows Arial)
"""
import sys
from PIL import Image, ImageDraw, ImageFont

COLS, ROWS = 24, 9

# ---- layout (column, row), 1-based; view from the component side ----------------
# Cuts: the copper is broken at this hole (the hole itself is drilled out).
CUTS = [(5, r) for r in (1, 2, 3, 4, 5, 7, 8)] + [(12, 5), (12, 8), (17, 8)]

# Wire links on the component side
LINKS = [((3, 2), (3, 5)), ((2, 5), (2, 6)), ((2, 8), (2, 9)),
         ((8, 3), (8, 4)), ((9, 4), (9, 6)), ((8, 6), (8, 7)), ((9, 7), (9, 9))]

# Components: name -> list of (pin label, (col, row))
U1 = [("1", (4, 2)), ("2", (4, 3)), ("3", (4, 4)), ("4", (4, 5)), ("5", (4, 6)), ("6", (4, 7)), ("7", (4, 8)),
      ("14", (7, 2)), ("13", (7, 3)), ("12", (7, 4)), ("11", (7, 5)), ("10", (7, 6)), ("9", (7, 7)), ("8", (7, 8))]
U2 = [("1", (16, 8)), ("2", (16, 9)), ("4", (19, 8)), ("3", (19, 9))]   # PC817: 1 anode, 2 cathode, 3 emitter, 4 collector
PARTS = {
    "R1": [(2, 1), (2, 3)],              # 1k: IO17 (strip 1) -> 1A (strip 3)
    "C1": [(10, 2), (10, 3)],
    "R2": [(14, 5), (14, 8)],
    "D1": [(15, 8), (15, 9)],            # cathode (band) at row 8 = LED anode
    "R3": [(21, 2), (21, 1)],            # 1k from +5 V (strip 2) to the LED anode (strip 1, right of the cut)
    "LED": [(23, 1), (23, 3)],           # power LED: anode strip 1, cathode (flat side) strip 3 = GND
}
WIRES = [  # off-board wires: (col, row), label, side
    ((1, 1), "from jack TIP (IO17)", "L"),
    ((1, 4), "to edge D-4 (READ)", "L"),
    ((1, 9), "from jack SLEEVE (GND)", "L"),
    ((11, 2), "from edge B-2 (+5 V from the C64)", "T"),
    ((24, 5), "from edge C-3 (MOTOR)", "R"),
    ((24, 8), "to jack RING (IO18)", "R"),
    ((24, 9), "to edge A-1 (GND), F-6 linked", "R"),
]

# ---- connectivity check -----------------------------------------------------------
parent = {}


def find(a):
    parent.setdefault(a, a)
    while parent[a] != a:
        parent[a] = parent[parent[a]]
        a = parent[a]
    return a


def union(a, b):
    parent[find(a)] = find(b)


cutset = set(CUTS)
for r in range(1, ROWS + 1):
    for c in range(1, COLS):
        if (c, r) in cutset or (c + 1, r) in cutset:
            continue
        union((c, r), (c + 1, r))
for a, b in LINKS:
    union(a, b)

EXPECT = {   # net -> holes that must be on it
    "IO17": [(1, 1), (2, 1)],
    "1A": [(2, 3), (4, 3)],
    "READ": [(4, 4), (1, 4)],
    "+5V": [(7, 2), (10, 2), (11, 2), (21, 2)],
    "GND": [(4, 2), (4, 5), (4, 6), (4, 8), (7, 3), (7, 4), (7, 6), (7, 7), (10, 3),
            (1, 9), (16, 9), (19, 9), (15, 9), (24, 9), (23, 3)],
    "MOTOR": [(14, 5), (24, 5)],
    "ANODE": [(16, 8), (14, 8), (15, 8)],
    "IO18": [(19, 8), (24, 8)],
    "LED_A": [(21, 1), (23, 1)],
    "U1_6": [(4, 7)], "U1_11": [(7, 5)], "U1_8": [(7, 8)],   # unused outputs: must stay alone
}
ok = True
roots = {}
for net, holes in EXPECT.items():
    rs = {find(h) for h in holes}
    if len(rs) != 1:
        print("NET SPLIT:", net, holes)
        ok = False
    roots[net] = find(holes[0])
if len(set(roots.values())) != len(roots):
    inv = {}
    for n, r in roots.items():
        inv.setdefault(r, []).append(n)
    for r, ns in inv.items():
        if len(ns) > 1:
            print("NETS SHORTED:", ns)
    ok = False
print("connectivity check:", "OK" if ok else "FAILED")
if not ok:
    sys.exit(1)

# ---- drawing ----------------------------------------------------------------------------
S = 2
PITCH = 40
X0, Y0 = 250, 150
W, H = X0 + COLS * PITCH + 300, Y0 + ROWS * PITCH + 230
img = Image.new("RGB", (W * S, H * S), "white")
d = ImageDraw.Draw(img)
INK = (20, 24, 32)
GREY = (110, 118, 130)
RED = (200, 30, 30)
COPPER = (226, 178, 120)
LINK = (20, 120, 60)


def F(size, bold=False):
    return ImageFont.truetype("C:/Windows/Fonts/" + ("arialbd.ttf" if bold else "arial.ttf"), size * S)


def xy(c, r):
    return X0 + (c - 1) * PITCH, Y0 + (r - 1) * PITCH


def text(x, y, s, size=13, bold=False, fill=INK, anchor="la"):
    d.text((x * S, y * S), s, font=F(size, bold), fill=fill, anchor=anchor)


def text_bg(x, y, s, size=11, bold=True, fill=INK, anchor="lm"):
    """Text on a white box, for labels that sit over holes or wires."""
    f = F(size, bold)
    l, t, r, b = d.textbbox((x * S, y * S), s, font=f, anchor=anchor)
    d.rectangle([l - 3 * S, t - 2 * S, r + 3 * S, b + 2 * S], fill=(255, 255, 255))
    d.text((x * S, y * S), s, font=f, fill=fill, anchor=anchor)


def L(p1, p2, fill=INK, w=3):
    d.line([(p1[0] * S, p1[1] * S), (p2[0] * S, p2[1] * S)], fill=fill, width=w * S)


text(20, 18, "C64 datasette adapter: stripboard layout", 20, True)
text(20, 46, "9 strips x 24 holes, viewed from the component side (copper strips shown underneath). "
     "Schematic: images/c64-interface.png", 12, False, GREY)

# board + strips
bx1, by1 = X0 - PITCH * 0.6, Y0 - PITCH * 0.6
bx2, by2 = X0 + (COLS - 0.4) * PITCH, Y0 + (ROWS - 0.4) * PITCH
d.rectangle([bx1 * S, by1 * S, bx2 * S, by2 * S], fill=(240, 232, 214), outline=GREY, width=2 * S)
for r in range(1, ROWS + 1):
    x1, y = xy(1, r)
    x2, _ = xy(COLS, r)
    d.rounded_rectangle([(x1 - 16) * S, (y - 13) * S, (x2 + 16) * S, (y + 13) * S], radius=6 * S, fill=COPPER)
for r in range(1, ROWS + 1):
    for c in range(1, COLS + 1):
        x, y = xy(c, r)
        d.ellipse([(x - 4) * S, (y - 4) * S, (x + 4) * S, (y + 4) * S], fill=(90, 70, 50))
# cuts: break the copper here (also drawn again on top later, so they show under the chips)
for c, r in CUTS:
    x, y = xy(c, r)
    d.rectangle([(x - 9) * S, (y - 14) * S, (x + 9) * S, (y + 14) * S], fill=(240, 232, 214))

# links
for a, b in LINKS:
    pa, pb = xy(*a), xy(*b)
    L(pa, pb, LINK, 4)
    for p in (pa, pb):
        d.ellipse([(p[0] - 6) * S, (p[1] - 6) * S, (p[0] + 6) * S, (p[1] + 6) * S], fill=LINK)


def chip(pins, label, sub, at=None):
    xs = [xy(*p)[0] for _, p in pins]
    ys = [xy(*p)[1] for _, p in pins]
    x1, x2, y1, y2 = min(xs) - 14, max(xs) + 14, min(ys) - 18, max(ys) + 18
    d.rectangle([x1 * S, y1 * S, x2 * S, y2 * S], fill=(48, 52, 60), outline=INK, width=2 * S)
    for lbl, p in pins:
        x, y = xy(*p)
        d.ellipse([(x - 6) * S, (y - 6) * S, (x + 6) * S, (y + 6) * S], fill=(200, 200, 200))
        text(x, y, lbl, 9, True, INK, "mm")
    cx, cy = ((x1 + x2) / 2, (y1 + y2) / 2) if at is None else xy(*at)
    text(cx, cy - 8, label, 12, True, (255, 255, 255), "mm")
    text(cx, cy + 9, sub, 10, False, (200, 200, 200), "mm")
    return x1, y1, x2, y2


x1, y1, x2, y2 = chip(U1, "74HCT125", "U1", at=(5.5, 6))   # strip 6 has no cut
d.pieslice([((x1 + x2) / 2 - 10) * S, (y1 - 10) * S, ((x1 + x2) / 2 + 10) * S, (y1 + 10) * S], 0, 180, fill=(240, 232, 214))
x1, y1, x2, y2 = chip(U2, "PC817", "U2", at=(18, 8.5))    # clear of the cut at column 17
text_bg((x1 + x2) / 2, y1 - 12, "dot = pin 1, top left", 10, False, GREY, "mm")


def part(name, a, b, band_at=None):
    pa, pb = xy(*a), xy(*b)
    L(pa, pb, (90, 90, 90), 3)
    mx, my = (pa[0] + pb[0]) / 2, (pa[1] + pb[1]) / 2
    vertical = pa[0] == pb[0]
    half = max(10, (abs(pb[1] - pa[1]) if vertical else abs(pb[0] - pa[0])) / 2 - 12)
    if vertical:
        box = [(mx - 9) * S, (my - half) * S, (mx + 9) * S, (my + half) * S]
    else:
        box = [(mx - half) * S, (my - 9) * S, (mx + half) * S, (my + 9) * S]
    col = (60, 110, 200) if name.startswith("R") else (230, 200, 60) if name.startswith("C") else (200, 80, 60)
    d.rounded_rectangle(box, radius=4 * S, fill=col, outline=INK, width=2 * S)
    if band_at is not None:
        bp = xy(*band_at)
        by = my - half + 3 if bp[1] < my else my + half - 6
        d.rectangle([(mx - 9) * S, by * S, (mx + 9) * S, (by + 4) * S], fill=(30, 30, 30))
    for p in (pa, pb):
        d.ellipse([(p[0] - 5) * S, (p[1] - 5) * S, (p[0] + 5) * S, (p[1] + 5) * S], fill=(120, 120, 120))
    return mx, my


labels = {}
for name, (a, b) in PARTS.items():
    labels[name] = part(name, a, b, band_at=a if name.startswith("D1") else None)
text_bg(labels["R1"][0] + 14, labels["R1"][1], "R1 1k")
text_bg(labels["C1"][0] + 14, labels["C1"][1], "C1 100n")
text_bg(labels["R2"][0] - 14, labels["R2"][1], "R2 1.5k", anchor="rm")
text_bg(labels["D1"][0] - 14, labels["D1"][1] + 2, "D1", anchor="rm")
text_bg(labels["R3"][0] - 14, labels["R3"][1], "R3 1k", anchor="rm")
text_bg(labels["LED"][0] + 14, labels["LED"][1], "power LED", anchor="lm")

# off-board wires
for (c, r), lbl, side in WIRES:
    x, y = xy(c, r)
    if side == "L":
        L((x, y), (bx1 - 40, y), (150, 40, 150), 3)
        text(bx1 - 46, y, lbl, 12, True, (150, 40, 150), "rm")
    elif side == "R":
        L((x, y), (bx2 + 40, y), (150, 40, 150), 3)
        text(bx2 + 46, y, lbl, 12, True, (150, 40, 150), "lm")
    else:
        L((x, y), (x, by1 - 40), (150, 40, 150), 3)
        text(x, by1 - 46, lbl, 12, True, (150, 40, 150), "mb")
    d.ellipse([(x - 6) * S, (y - 6) * S, (x + 6) * S, (y + 6) * S], fill=(150, 40, 150))

# cuts on top, so they show under the chips too
for c, r in CUTS:
    x, y = xy(c, r)
    L((x - 9, y - 9), (x + 9, y + 9), (255, 255, 255), 6)
    L((x - 9, y + 9), (x + 9, y - 9), (255, 255, 255), 6)
    L((x - 9, y - 9), (x + 9, y + 9), RED, 3)
    L((x - 9, y + 9), (x + 9, y - 9), RED, 3)

# strip and column numbers last, on white, so wires never cover them
for r in range(1, ROWS + 1):
    _, y = xy(1, r)
    text_bg(X0 - PITCH * 0.95, y, str(r), 12, True, GREY, "mm")
for c in range(1, COLS + 1):
    x, _ = xy(c, 1)
    text_bg(x, Y0 - PITCH * 0.95, str(c), 11, True, GREY, "mm")

# key + build notes
ky = by2 + 34
d.rectangle([20 * S, (ky - 6) * S, 34 * S, (ky + 6) * S], fill=COPPER)
text(42, ky, "copper strip (underneath)", 12, anchor="lm")
L((260, ky - 6), (272, ky + 6), RED, 3)
L((260, ky + 6), (272, ky - 6), RED, 3)
text(280, ky, "track cut (drill the hole out)", 12, anchor="lm")
L((520, ky), (556, ky), LINK, 4)
text(562, ky, "wire link", 12, anchor="lm")
L((660, ky), (696, ky), (150, 40, 150), 3)
text(702, ky, "wire off the board", 12, anchor="lm")
notes = [
    "Cuts: column 5 on strips 1, 2, 3, 4, 5, 7 and 8 (under U1; strip 6 stays whole), column 12 on strips 5 and 8, column 17 on strip 8.",
    "Links (column/strip): 3/2-3/5, 2/5-2/6, 2/8-2/9, 8/3-8/4, 9/4-9/6, 8/6-8/7, 9/7-9/9. Strip 9 is the GND bus; strips 4 (right) and 6 are GND too.",
    "U1 pin 1 at column 4 strip 2 (notch at the top). U2 (PC817) pin 1 (anode) at column 16 strip 8; its pins 2 and 3 share the GND strip 9.",
    "D1 stands upright: band (cathode) to strip 8, the opto's anode side. Use a DIP-14 socket for U1 if you like. At the edge connector, link F-6 (SENSE) to A-1 (GND).",
    "Power LED: long leg (anode) in strip 1, flat side (cathode) in strip 3; R3 1k stands upright from strip 2 to strip 1. The LED marks the top of the board.",
    "Test with a meter before fitting the chips: no short between +5 V (strip 2) and GND (strip 9), and MOTOR (strip 5, right) not joined to GND (strip 4, right).",
]
for i, n in enumerate(notes):
    text(20, ky + 34 + i * 22, "\u2022  " + n, 12, False, GREY)

img.save(sys.argv[1])
print("saved", sys.argv[1])
