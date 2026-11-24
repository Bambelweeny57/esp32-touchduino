"""Draw images/c64-interface.png (TouchDuino -> Commodore 64 datasette port adapter).

TouchDuino only has a 3.5 mm stereo socket (tip IO17, ring IO18, sleeve GND);
the adapter board sits at the C64 end and is powered by the C64's +5 V.

Usage: python tools/draw_c64.py images/c64-interface.png   (needs Pillow and Windows Arial fonts)
"""
import sys
from PIL import Image, ImageDraw, ImageFont

S = 2
W, H = 1240, 806
img = Image.new("RGB", (W * S, H * S), "white")
d = ImageDraw.Draw(img)
INK = (20, 24, 32)
GREY = (110, 118, 130)
RED = (190, 40, 40)
BLUE = (30, 90, 190)
LW = 2 * S


def F(size, bold=False):
    return ImageFont.truetype("C:/Windows/Fonts/" + ("arialbd.ttf" if bold else "arial.ttf"), size * S)


def P(*pts):
    return [(x * S, y * S) for x, y in pts]


def line(*pts, fill=INK, w=LW):
    d.line(P(*pts), fill=fill, width=w, joint="curve")


def dashed_box(x1, y1, x2, y2, col=GREY):
    for (a, b) in (((x1, y1), (x2, y1)), ((x2, y1), (x2, y2)), ((x2, y2), (x1, y2)), ((x1, y2), (x1, y1))):
        n = int(max(abs(b[0] - a[0]), abs(b[1] - a[1])) / 12)
        for i in range(0, n, 2):
            t0, t1 = i / n, min(1, (i + 1) / n)
            line((a[0] + (b[0] - a[0]) * t0, a[1] + (b[1] - a[1]) * t0),
                 (a[0] + (b[0] - a[0]) * t1, a[1] + (b[1] - a[1]) * t1), fill=col)


def dot(x, y):
    r = 4 * S
    d.ellipse([x * S - r, y * S - r, x * S + r, y * S + r], fill=INK)


def text(x, y, s, size=14, bold=False, fill=INK, anchor="la"):
    d.text((x * S, y * S), s, font=F(size, bold), fill=fill, anchor=anchor)


def box(x1, y1, x2, y2, r=6):
    d.rounded_rectangle(P((x1, y1), (x2, y2)), radius=r * S, outline=INK, width=LW)


def gnd(x, y):
    line((x, y), (x, y + 10))
    for i, half in enumerate((12, 8, 4)):
        line((x - half, y + 10 + i * 5), (x + half, y + 10 + i * 5))


def zigzag_h(x1, x2, y, body=46):
    a = (x1 + x2 - body) / 2
    pts = [(x1, y), (a, y)]
    for i in range(7):
        pts.append((a + body / 7 * (i + 0.5), y + (-7 if i % 2 == 0 else 7)))
    pts += [(a + body, y), (x2, y)]
    line(*pts)


def zigzag_v(x, y1, y2, body=40):
    a = (y1 + y2 - body) / 2
    pts = [(x, y1), (x, a)]
    for i in range(7):
        pts.append((x + (-7 if i % 2 == 0 else 7), a + body / 7 * (i + 0.5)))
    pts += [(x, a + body), (x, y2)]
    line(*pts)


def led_v(x, y1, y2):
    """LED from y1 (anode, top) to y2 (cathode, bottom), light arrows outwards."""
    m = (y1 + y2) / 2
    line((x, y1), (x, m - 10))
    d.polygon(P((x - 11, m - 10), (x + 11, m - 10), (x, m + 9)), outline=INK, width=LW)
    line((x - 11, m + 9), (x + 11, m + 9), w=3 * S)
    line((x, m + 9), (x, y2))
    for k in (0, 1):
        line((x + 14, m - 6 + k * 10), (x + 28, m - 14 + k * 10))
        d.polygon(P((x + 28, m - 14 + k * 10), (x + 22, m - 13 + k * 10), (x + 26, m - 8 + k * 10)), fill=INK)


def cap_v(x, y1, y2):
    m = (y1 + y2) / 2
    line((x, y1), (x, m - 5))
    line((x - 14, m - 5), (x + 14, m - 5), w=3 * S)
    line((x - 14, m + 5), (x + 14, m + 5), w=3 * S)
    line((x, m + 5), (x, y2))


text(20, 16, "TouchDuino \u2192 Commodore 64 datasette port adapter", 20, True)
text(20, 44, "The C64 has no audio input: this adapter makes the digital datasette signals. "
     "TouchDuino needs only a 3.5 mm stereo socket; the adapter is powered by the C64.", 12, False, GREY)

TIPY, RINGY, SLY = 190, 330, 520

# ---- TouchDuino + socket ---------------------------------------------------------
box(30, 130, 160, 560)
text(95, 116, "TouchDuino", 12, False, GREY, "mm")
text(150, TIPY, "IO17", 14, True, anchor="rm")
text(150, RINGY, "IO18", 14, True, anchor="rm")
text(150, SLY, "GND", 14, True, anchor="rm")
text(40, 150, "1.0 mm 4-pin IO", 11, False, GREY)
text(40, 165, "connector", 11, False, GREY)
box(200, 150, 280, 560)
text(240, 122, "3.5 mm stereo", 11, False, GREY, "mm")
text(240, 136, "socket (in the case)", 11, False, GREY, "mm")
for y, lbl in ((TIPY, "TIP"), (RINGY, "RING"), (SLY, "SLEEVE")):
    line((160, y), (200, y))
    text(240, y, lbl, 12, True, anchor="mm")
text(240, 590, "label it \u201cC64\u201d", 11, False, RED, "mm")

# stereo lead
for y in (TIPY, RINGY, SLY):
    line((280, y), (360, y), fill=(150, 40, 150), w=3 * S)
text(320, 104, "stereo lead", 11, True, (150, 40, 150), "mm")

# ---- adapter board ------------------------------------------------------------------
AX1, AX2, AY1, AY2 = 360, 960, 90, 620
dashed_box(AX1, AY1, AX2, AY2)
text(AX1 + 10, AY1 + 8, "Adapter board, at the C64 end (powered by the C64's +5 V)", 12, True, GREY)

# +5 V rail from the edge connector B-2
V5Y = 130
line((560, V5Y), (1010, V5Y), fill=RED)
text(565, V5Y - 16, "+5 V from the C64", 12, True, RED)

# R1 + 74HCT125
zigzag_h(360, 470, TIPY)
text(415, TIPY - 24, "R1 1 k\u03a9", 12, True, anchor="mm")
text(415, TIPY + 24, "(input protection)", 10, False, GREY, "mm")
U1x1, U1x2, U1y1, U1y2 = 500, 650, 150, 260
box(U1x1, U1y1, U1x2, U1y2, 4)
text((U1x1 + U1x2) / 2, U1y1 + 16, "74HCT125  U1", 13, True, anchor="mm")
tx = (U1x1 + U1x2) / 2
BY = TIPY + 20
d.polygon(P((tx - 22, BY - 23), (tx - 22, BY + 23), (tx + 20, BY)), outline=INK, width=LW)
line((470, TIPY), (U1x1, TIPY), (U1x1, BY), (tx - 22, BY))
text(U1x1 - 4, TIPY - 12, "1A (2)", 10, False, GREY, "rs")
line((tx + 20, BY), (U1x2, BY))
text(U1x2 + 6, BY - 14, "1Y (3)", 10, False, GREY)
line((U1x2, BY), (1010, BY))
text(800, BY - 12, "READ", 13, True, BLUE)
# VCC + C1
line((620, V5Y), (620, U1y1))
dot(620, V5Y)
text(626, U1y1 - 12, "14", 10, False, GREY)
dot(720, V5Y)
line((720, V5Y), (720, 140))
cap_v(720, 140, 164)
text(736, 144, "C1", 12, True)
text(736, 158, "100 nF", 11, False, GREY)
gnd(720, 164)
# OE to GND, notes
line((540, U1y2), (540, 276))
text(546, U1y2 + 4, "1OE (1)", 10, False, GREY)
gnd(540, 276)
text(570, 292, "pin 7 \u2192 GND; unused gates:", 10, False, GREY)
text(570, 306, "pins 4, 5, 9, 10, 12, 13 \u2192 GND", 10, False, GREY)

# ---- PC817 ---------------------------------------------------------------------------
Ox1, Ox2, Oy1, Oy2 = 560, 740, 360, 470
P1Y, P2Y = 385, 445
box(Ox1, Oy1, Ox2, Oy2, 4)
text((Ox1 + Ox2) / 2, Oy2 + 16, "PC817  U2", 13, True, anchor="mm")
LX = 700
line((Ox2, P1Y), (LX, P1Y), (LX, 400))
line((LX, 424), (LX, P2Y), (Ox2, P2Y))
d.polygon(P((LX - 12, 402), (LX + 12, 402), (LX, 424)), outline=INK, width=LW)
line((LX - 12, 424), (LX + 12, 424), w=3 * S)
for k in (0, 1):
    line((LX - 18, 406 + k * 12), (LX - 34, 398 + k * 12))
text(Ox2 + 4, P1Y - 4, "1", 10, False, GREY, "lb")
text(Ox2 + 4, P2Y + 14, "2", 10, False, GREY, "lb")
QX = 610
line((Ox1, P1Y), (QX, P1Y), (QX, 397))
line((QX, 433), (QX, P2Y), (Ox1, P2Y))
line((QX, 395), (QX, 435), w=3 * S)
text(Ox1 - 4, P1Y - 4, "4", 10, False, GREY, "rb")
text(Ox1 - 4, P2Y + 14, "3", 10, False, GREY, "rb")

# IO18 (ring) -> collector
line((360, RINGY), (500, RINGY), (500, P1Y), (Ox1, P1Y))
text(420, RINGY - 12, "IO18", 12, True, BLUE)
# emitter -> GND bus
GBY = 560
line((Ox1, P2Y), (520, P2Y), (520, GBY))
dot(520, GBY)

# MOTOR -> R2 -> anode, D1 reverse across the LED
MY = P1Y
line((1010, MY), (900, MY))
zigzag_h(800, 900, MY)
text(850, MY - 24, "R2 1.5 k\u03a9", 12, True, anchor="mm")
line((800, MY), (Ox2, MY))
text(930, MY - 12, "MOTOR", 13, True, BLUE)
DX = 780
dot(DX, P1Y)
dot(DX, P2Y)
line((DX, P1Y), (DX, 405))
line((DX, 427), (DX, P2Y))
d.polygon(P((DX - 9, 427), (DX + 9, 427), (DX, 413)), outline=INK, width=LW)
line((DX - 10, 411), (DX + 10, 411), w=3 * S)
text(794, 410, "D1 1N4148", 11, True)
line((Ox2, P2Y), (840, P2Y), (840, GBY))
dot(840, GBY)

# Power LED: +5 V -> R3 -> LED -> GND (shows the adapter is powered; fit it on the top side)
PLX = 925
line((PLX - 12, 424), (PLX + 12, 424), fill=RED, w=3 * S)
text(PLX, 416, "+5 V", 11, True, RED, "mb")
zigzag_v(PLX, 424, 474, body=34)
text(PLX - 14, 449, "R3 1 k\u03a9", 11, True, anchor="rm")
led_v(PLX, 476, 530)
text(PLX - 16, 503, "power LED", 11, True, anchor="rm")
line((PLX, 530), (PLX, 560))
dot(PLX, 560)

# GND bus: sleeve, emitter, cathode, edge A-1, SENSE
line((360, SLY), (400, SLY), (400, GBY), (1010, GBY))
gnd(660, GBY)
text(410, GBY + 18, "GND", 12, True)

# ---- edge connector ------------------------------------------------------------------
EX = 1010
box(EX, 100, EX + 80, 620, 4)
text(EX + 40, 72, "C64 datasette", 11, False, GREY, "mm")
text(EX + 40, 86, "edge connector", 11, False, GREY, "mm")
pins = [(V5Y, "B-2", "+5 V", RED), (BY, "D-4", "READ", BLUE), (MY, "C-3", "MOTOR", BLUE),
        (GBY, "A-1", "GND", INK)]
for y, pin, sig, col in pins:
    text(EX + 8, y - 9, pin, 12, True)
    text(EX + 8, y + 8, sig, 11, False, col)
SNY = 500
line((EX, SNY), (980, SNY), (980, GBY))
dot(980, GBY)
text(EX + 8, SNY - 9, "F-6", 12, True)
text(EX + 8, SNY + 8, "SENSE", 11, False, RED)
text(EX + 8, 588, "E-5", 12, True)
text(EX + 8, 604, "WRITE n/c", 10, False, GREY)
text(EX + 96, 130, "pins 1-6 on top,", 10, False, GREY)
text(EX + 96, 144, "A-F underneath:", 10, False, GREY)
text(EX + 96, 158, "check yours", 10, False, GREY)

# ---- notes -------------------------------------------------------------------------------
notes = [
    "TouchDuino only needs a 3.5 mm stereo socket: tip IO17, ring IO18, sleeve GND (3.3 V logic). Everything C64-specific is on the adapter.",
    "READ: the 74HCT125 runs from the C64's +5 V and accepts IO17's 3.3 V signal. R1 limits the current if the C64 is off, or if the lead goes into the audio jack.",
    "MOTOR: the C64 puts out about 6-9 V to run the motor. The PC817 then pulls IO18 low = play (Auto REM on, REM invert off). IO18's own pull-up does the rest.",
    "SENSE linked to GND tells the C64 that PLAY is pressed. Plug the lead in with the tape stopped (a stereo plug shorts its contacts as it goes in).",
    "Power LED (R3 + LED from the C64's +5 V): lights when the adapter is plugged into a C64 that is on. Fit it on the top of the board, so it also shows which way up the plug goes.",
    "WARNING: the plug must only fit one way round. Upside down reverses the pins and puts MOTOR (6-9 V) on the READ output. Use a polarising key; check before powering on.",
    "Files: C64 .tap only (.t64 / .prg / .d64 are not tape recordings). TouchDuino flips the signal for C64 TAP itself: leave Invert output off.",
]
for i, n in enumerate(notes):
    text(20, 642 + i * 22, "\u2022  " + n, 12, False, GREY)

img.save(sys.argv[1])
print("saved", sys.argv[1])
