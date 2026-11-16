"""Draw images/line-out-lm386.png (TouchDuino line out with an LM386 amplifier).

Usage: python tools/draw_lm386.py images/line-out-lm386.png   (needs Pillow and Windows Arial fonts)
"""
import sys
from PIL import Image, ImageDraw, ImageFont

S = 2
W, H = 1040, 640
img = Image.new("RGB", (W * S, H * S), "white")
d = ImageDraw.Draw(img)
INK = (20, 24, 32)
GREY = (110, 118, 130)
RED = (190, 40, 40)
LW = 2 * S


def F(size, bold=False):
    return ImageFont.truetype("C:/Windows/Fonts/" + ("arialbd.ttf" if bold else "arial.ttf"), size * S)


def P(*pts):
    return [(x * S, y * S) for x, y in pts]


def line(*pts, fill=INK, w=LW):
    d.line(P(*pts), fill=fill, width=w, joint="curve")


def dot(x, y):
    r = 4 * S
    d.ellipse([x * S - r, y * S - r, x * S + r, y * S + r], fill=INK)


def text(x, y, s, size=14, bold=False, fill=INK, anchor="la"):
    d.text((x * S, y * S), s, font=F(size, bold), fill=fill, anchor=anchor)


def gnd(x, y):
    """Ground symbol hanging from (x, y)."""
    line((x, y), (x, y + 10))
    for i, half in enumerate((12, 8, 4)):
        line((x - half, y + 10 + i * 5), (x + half, y + 10 + i * 5))


def zigzag_h(x1, x2, y, body=50):
    a = (x1 + x2 - body) / 2
    pts = [(x1, y), (a, y)]
    for i in range(7):
        pts.append((a + body / 7 * (i + 0.5), y + (-7 if i % 2 == 0 else 7)))
    pts += [(a + body, y), (x2, y)]
    line(*pts)


def zigzag_v(x, y1, y2, body=50):
    a = (y1 + y2 - body) / 2
    pts = [(x, y1), (x, a)]
    for i in range(7):
        pts.append((x + (-7 if i % 2 == 0 else 7), a + body / 7 * (i + 0.5)))
    pts += [(x, a + body), (x, y2)]
    line(*pts)


def cap_v(x, y1, y2, polar=False):
    m = (y1 + y2) / 2
    line((x, y1), (x, m - 5))
    line((x - 14, m - 5), (x + 14, m - 5), w=3 * S)
    if polar:
        d.arc(P((x - 16, m + 3), (x + 16, m + 23)), 200, 340, fill=INK, width=3 * S)
        line((x, m + 4), (x, y2))
        text(x - 24, m - 20, "+", 14, True)
    else:
        line((x - 14, m + 5), (x + 14, m + 5), w=3 * S)
        line((x, m + 5), (x, y2))


def cap_h_polar(x1, x2, y):
    m = (x1 + x2) / 2
    line((x1, y), (m - 6, y))
    line((m - 6, y - 14), (m - 6, y + 14), w=3 * S)
    d.arc(P((m + 2, y - 16), (m + 20, y + 16)), 120, 240, fill=INK, width=3 * S)
    line((m + 4, y), (x2, y))
    text(m - 20, y - 28, "+", 14, True)


text(20, 16, "Line output with an LM386 amplifier (like a TZXduino)", 19, True)

SIG = 220
# ---- input: board pin + existing filter -----------------------------------
d.rounded_rectangle(P((30, 195), (120, 245)), radius=6 * S, outline=INK, width=LW)
text(75, 182, "IO connector", 12, False, GREY, "mm")
text(110, SIG, "IO17", 14, True, anchor="rm")
line((120, SIG), (140, SIG))
zigzag_h(140, 215, SIG)
text(178, 196, "R1 1 kΩ", 13, True, anchor="mm")
A = 250
line((215, SIG), (A, SIG))
dot(A, SIG)
cap_v(A, SIG, 340)
text(A + 18, 282, "C1", 13, True)
text(A + 18, 298, "10 nF", 13)
gnd(A, 340)
line((A, SIG), (275, SIG))
cap_h_polar(275, 335, SIG)
text(305, 252, "C2 10 µF", 13, True, anchor="mm")
B = 380
line((335, SIG), (B, SIG))
text(160, 410, "the same filter as\nimages/line-out.png", 12, False, GREY, "mm")

# ---- volume pot -------------------------------------------------------------
zigzag_v(B, SIG, 380, body=70)
gnd(B, 380)
text(B - 14, 300, "VR1", 13, True, anchor="rm")
text(B - 14, 316, "10 kΩ pot", 13, anchor="rm")
text(B - 14, 332, "(volume)", 12, False, GREY, "rm")
WIPER_Y = 295
line((B + 12, WIPER_Y), (540, WIPER_Y))
d.polygon(P((B + 10, WIPER_Y), (B + 22, WIPER_Y - 6), (B + 22, WIPER_Y + 6)), fill=INK)   # wiper arrow

# ---- LM386 ------------------------------------------------------------------
L, R_, T, Bm = 540, 670, 245, 395
C = (T + Bm) / 2
d.polygon(P((L, T), (L, Bm), (R_, C)), outline=INK, width=LW)
text(L + 40, C, "LM386", 15, True, anchor="lm")
text(L + 40, C + 18, "U1", 12, False, GREY, "lm")
# + input pin 3, - input pin 2
text(L + 8, WIPER_Y, "+", 15, True, anchor="lm")
text(L - 4, WIPER_Y - 10, "3", 11, False, GREY, "rm")
M2 = 350
line((L, M2), (510, M2), (510, 380))
gnd(510, 380)
text(L + 8, M2, "−", 15, True, anchor="lm")
text(L - 4, M2 - 10, "2", 11, False, GREY, "rm")
slope = (C - T) / (R_ - L)
# supply pin 6 (top) to +5 V
x6 = 595
y6 = T + (x6 - L) * slope
line((x6, y6), (x6, 90))
text(x6 + 6, y6 - 14, "6", 11, False, GREY)
line((x6, 90), (470, 90))
text(462, 90, "+5 V", 15, True, RED, "rm")
text(462, 108, "VIN (UART connector)", 11, False, GREY, "rm")
dot(520, 90)
cap_v(520, 90, 150)
text(536, 112, "C6 100 nF", 12, True)
gnd(520, 150)
# ground pin 4 (bottom)
x4 = 595
y4 = Bm - (x4 - L) * slope
line((x4, y4), (x4, 430))
text(x4 + 6, y4 + 2, "4", 11, False, GREY)
gnd(x4, 430)
# bypass pin 7 (optional)
x7 = 635
y7 = Bm - (x7 - L) * slope
line((x7, y7), (x7, 405))
text(x7 + 6, y7 + 2, "7", 11, False, GREY)
cap_v(x7, 405, 455, polar=True)
text(x7 + 22, 430, "C5 10 µF", 12, True, anchor="lm")
text(x7 + 22, 446, "(optional)", 11, False, GREY, "lm")
gnd(x7, 455)
text(x6 + 10, 200, "pins 1 and 8:", 11, False, GREY)
text(x6 + 10, 214, "not connected (gain 20)", 11, False, GREY)

# ---- output: Zobel, coupling cap, jack -------------------------------------
OUT = C
text(R_ + 4, OUT - 12, "5", 11, False, GREY)
N = 760
line((R_, OUT), (N, OUT))
dot(N, OUT)
zigzag_v(N, OUT, 400, body=40)
text(N + 14, 368, "R3 10 Ω", 12, True, anchor="lm")
cap_v(N, 400, 460)
text(N + 14, 430, "C4 47 nF", 12, True, anchor="lm")
gnd(N, 460)
line((N, OUT), (800, OUT))
cap_h_polar(800, 870, OUT)
text(835, OUT + 34, "C3 100 µF", 13, True, anchor="mm")
JX = 910
line((870, OUT), (JX, OUT))
d.rounded_rectangle(P((JX, 280), (1020, 470)), radius=8 * S, outline=INK, width=LW)
text(965, 266, "3.5 mm plug", 12, False, GREY, "mm")
text(JX + 10, OUT, "TIP", 14, True, anchor="lm")
SL = 440
text(JX + 10, SL, "SLEEVE", 14, True, anchor="lm")
line((JX, SL), (890, SL), (890, 470))
gnd(890, 470)
text(965, 380, "to EAR /", 12, False, GREY, "mm")
text(965, 396, "tape in", 12, False, GREY, "mm")

# ---- notes ----------------------------------------------------------------------
notes = [
    "Check VIN with a meter first: it should be about 5 V (USB). The LM386 needs at least 4 V, so the 3.3 V pin won't do.",
    "Start with VR1 turned low and turn it up until the computer loads reliably, like the volume on a tape deck.",
    "C2, C3 and C5 are electrolytic: + side as drawn. R3 + C4 stop the LM386 oscillating; keep them close to pin 5.",
    "All ground symbols join to the board's GND and the jack sleeve.",
]
for i, n in enumerate(notes):
    text(20, 520 + i * 26, "•  " + n, 13, False, GREY)

img.save(sys.argv[1])
print("saved", sys.argv[1])
