"""Draw images/line-out.png (TouchDuino line-out / REM wiring).

Usage: python tools/draw_line_out.py images/line-out.png   (needs Pillow and Windows Arial fonts)
"""
import sys
from PIL import Image, ImageDraw, ImageFont

S = 2                                   # supersample: draw in logical px, scaled x2
W, H = 820, 594
img = Image.new("RGB", (W * S, H * S), "white")
d = ImageDraw.Draw(img)
INK = (20, 24, 32)
GREY = (110, 118, 130)
ACC = (30, 90, 200)
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


def text(x, y, s, size=15, bold=False, fill=INK, anchor="la"):
    d.text((x * S, y * S), s, font=F(size, bold), fill=fill, anchor=anchor)


def resistor_h(x1, x2, y):
    n, amp = 7, 8
    body = 56
    a = (x1 + x2 - body) / 2
    pts = [(x1, y), (a, y)]
    step = body / n
    for i in range(n):
        pts.append((a + step * (i + 0.5), y + (-amp if i % 2 == 0 else amp)))
    pts += [(a + body, y), (x2, y)]
    line(*pts)


def resistor_v(x, y1, y2):
    n, amp = 7, 8
    body = 56
    a = (y1 + y2 - body) / 2
    pts = [(x, y1), (x, a)]
    step = body / n
    for i in range(n):
        pts.append((x + (-amp if i % 2 == 0 else amp), a + step * (i + 0.5)))
    pts += [(x, a + body), (x, y2)]
    line(*pts)


def cap_v(x, y1, y2):
    m = (y1 + y2) / 2
    line((x, y1), (x, m - 5))
    line((x, m + 5), (x, y2))
    line((x - 16, m - 5), (x + 16, m - 5), w=3 * S)
    line((x - 16, m + 5), (x + 16, m + 5), w=3 * S)


def cap_h_polar(x1, x2, y):
    m = (x1 + x2) / 2
    line((x1, y), (m - 6, y))
    line((m - 6, y - 16), (m - 6, y + 16), w=3 * S)          # + plate (straight)
    d.arc(P((m + 2, y - 18), (m + 22, y + 18)), 120, 240, fill=INK, width=3 * S)  # - plate (curved)
    line((m + 4, y), (x2, y))
    text(m - 20, y - 30, "+", 16, True)


# ---------------------------------------------------------------- line out
text(20, 18, "Line output: GPIO 17 to the computer's EAR / tape input", 19, True)

# board connector
d.rounded_rectangle(P((40, 70), (170, 270)), radius=8 * S, outline=INK, width=LW)
text(105, 52, "JC3248W535 IO connector", 13, False, GREY, "mm")
for y, name in ((100, "IO17"), (150, "IO18"), (200, "3.3V"), (250, "GND")):
    text(160, y, name, 15, True, anchor="rm")
    line((170, y), (190, y))
text(200, 150, "to REM (below)", 12, False, GREY, "lm")
text(200, 200, "not used", 12, False, GREY, "lm")

GND_Y = 320
A, B, JX = 380, 560, 680
# signal path
line((190, 100), (230, 100))
resistor_h(230, 320, 100)
text(275, 74, "R1  1 kΩ", 15, True, anchor="mm")
line((320, 100), (A, 100))
dot(A, 100)
line((A, 100), (430, 100))
cap_h_polar(430, 490, 100)
text(462, 132, "C2  10 µF", 15, True, anchor="mm")
line((490, 100), (B, 100))
dot(B, 100)
line((B, 100), (JX, 100))

# shunt parts
cap_v(A, 100, GND_Y)
text(A + 24, 210, "C1  10 nF", 15, True, anchor="lm")
resistor_v(B, 100, GND_Y)
text(B + 18, 210, "R2  4.7 kΩ", 15, True, anchor="lm")

# ground rail
line((190, 250), (210, 250), (210, GND_Y))
line((210, GND_Y), (JX, GND_Y))
dot(A, GND_Y)
dot(B, GND_Y)

# 3.5 mm plug
d.rounded_rectangle(P((JX, 70), (800, GND_Y + 20)), radius=8 * S, outline=INK, width=LW)
text(740, 52, "3.5 mm plug", 13, False, GREY, "mm")
text(JX + 10, 100, "TIP", 15, True, anchor="lm")
text(JX + 10, GND_Y, "SLEEVE", 15, True, anchor="lm")
text(740, 200, "to EAR /", 13, False, GREY, "mm")
text(740, 218, "tape in", 13, False, GREY, "mm")

text(20, 370, "R1 + C1 round off the edges (~16 kHz).  C2 blocks the DC offset (+ toward the board).  "
              "R2 holds the output at 0 V.", 13, False, GREY)

# ---------------------------------------------------------------- REM
text(20, 412, "Motor control (optional): the computer's REM switch to GPIO 18", 19, True)
d.rounded_rectangle(P((40, 474), (170, 574)), radius=8 * S, outline=INK, width=LW)
for y, name in ((504, "IO18"), (549, "GND")):
    text(160, y, name, 15, True, anchor="rm")
    line((170, y), (330, y))
d.rounded_rectangle(P((330, 479), (504, 569)), radius=8 * S, outline=INK, width=LW)
text(400, 466, "2.5 mm REM plug", 13, False, GREY, "mm")
# relay contact symbol inside the plug box
line((340, 504), (370, 504))
line((340, 549), (370, 549))
line((370, 504), (370, 514))
line((370, 549), (370, 539))
line((370, 539), (392, 516))
dot(370, 514)
dot(370, 539)
text(400, 526, "switch in", 12, False, GREY, "lm")
text(400, 542, "computer", 12, False, GREY, "lm")
text(524, 504, "Switch closed = IO18 LOW = play.", 13, False, GREY, "lm")
text(524, 524, "IO18 has an internal pull-up.", 13, False, GREY, "lm")
text(524, 544, "Select Auto REM in Options.", 13, False, GREY, "lm")

out = sys.argv[1]
img.resize((W * S, H * S)).save(out)
print("saved", out)
