#!/usr/bin/env python3
"""Render an animated emoji set, 128x128, for the xiaozhi ESP32-S3-BOX-3.

Round warm-yellow faces with eyes, brows and mouths, in one consistent style, one
seamless looping GIF per emotion name the firmware's emoji collection asks for.

    python3 gen_emoji.py --out <collection-dir> [--contact sheet.png]

Copy the output to `managed_components/78__xiaozhi-fonts/png/<collection>/` and point
DEFAULT_EMOJI_COLLECTION at it: scripts/build_default_assets.py then packs the set into
the assets partition the device flashes.
"""
import argparse
import math
import os

from PIL import Image, ImageChops, ImageDraw

SIZE = 128
SS = 8
W = SIZE * SS
CX, CY = 64.0, 65.0
R = 58.0

SKIN_TOP = (255, 214, 88)
SKIN_BOTTOM = (255, 180, 62)
SKIN_EDGE = (244, 160, 38)
EYE = (59, 43, 31)
EYE_SOFT = (92, 70, 52)
WHITE = (255, 255, 255, 255)
MOUTH = (96, 42, 30)
TONGUE = (255, 108, 108)
BLUSH = (255, 122, 118, 92)
TEAR = (108, 196, 255)
BROW = (140, 96, 44)
BROW_ANGRY = (176, 62, 44)
HEART = (255, 78, 112)
SPARK = (255, 240, 150)
GLASS = (48, 52, 64)
ACCENT = (255, 214, 90)

FPS = 10


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def ease(t):
    t = clamp(t, 0.0, 1.0)
    return 0.5 - 0.5 * math.cos(math.pi * t)


def pulse(t, phase=0.0):
    return math.sin((t + phase) * 2 * math.pi)


def blink(t, start=0.60, hold=0.075):
    if t < start or t > start + 3 * hold:
        return 1.0
    if t < start + hold:
        return 1.0 - 0.94 * ease((t - start) / hold)
    if t < start + 2 * hold:
        return 0.06
    return 0.06 + 0.94 * ease((t - start - 2 * hold) / hold)


# --------------------------------------------------------------------------- #
# face
# --------------------------------------------------------------------------- #

def face_base():
    """Gradient disc with a soft top highlight and a darker rim."""
    grad = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(grad)
    for i in range(W):
        u = i / W
        col = tuple(int(SKIN_TOP[c] + (SKIN_BOTTOM[c] - SKIN_TOP[c]) * u) for c in range(3))
        d.rectangle([0, i, W, i + 1], fill=col + (255,))

    disc = Image.new("L", (W, W), 0)
    ImageDraw.Draw(disc).ellipse([(CX - R) * SS, (CY - R) * SS, (CX + R) * SS, (CY + R) * SS],
                                 fill=255)
    out = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    out.paste(grad, (0, 0), disc)

    # A soft gloss and a slightly darker outer ring, both *inside* the mask: an arc
    # drawn on the rim itself quantises into a hard stepped band and is what made the
    # bottom edge look chipped.
    detail = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    dd = ImageDraw.Draw(detail)
    dd.ellipse([(CX - 40) * SS, (CY - 54) * SS, (CX + 18) * SS, (CY - 14) * SS],
               fill=(255, 255, 255, 34))
    inner = Image.new("L", (W, W), 0)
    ImageDraw.Draw(inner).ellipse([(CX - R + 3.2) * SS, (CY - R + 3.2) * SS,
                                   (CX + R - 3.2) * SS, (CY + R - 3.2) * SS], fill=255)
    ring = ImageChops.subtract(disc, inner)  # 3.2 px outer band
    ring_colour = Image.new("RGBA", (W, W), SKIN_EDGE + (70,))
    out.alpha_composite(Image.composite(detail, Image.new("RGBA", (W, W), (0, 0, 0, 0)), disc))
    out.alpha_composite(Image.composite(ring_colour, Image.new("RGBA", (W, W), (0, 0, 0, 0)), ring))
    return out


def draw_open_eye(d, centre, *, open_ratio=1.0, look=(0.0, 0.0), heart=False, star=False):
    cx, cy = centre
    rx, ry = 9.6 * SS, 11.4 * SS * clamp(open_ratio, 0.08, 1.3)
    if heart:
        draw_heart(d, (cx, cy), 10.5 * SS, HEART)
        return
    if star:
        draw_star(d, (cx, cy), 11.0 * SS, SPARK)
        return
    d.ellipse([cx - rx, cy - ry, cx + rx, cy + ry], fill=EYE)
    hr = 3.2 * SS
    hx = cx - rx * 0.34 + look[0]
    hy = cy - ry * 0.42 + look[1]
    d.ellipse([hx - hr, hy - hr, hx + hr, hy + hr], fill=WHITE)
    sr = 1.5 * SS
    d.ellipse([cx + rx * 0.34 - sr, cy + ry * 0.34 - sr,
               cx + rx * 0.34 + sr, cy + ry * 0.34 + sr], fill=(255, 255, 255, 210))


def draw_x_eye(d, centre, r, colour=EYE, thickness=3.4):
    cx, cy = centre
    d.line([(cx - r, cy - r), (cx + r, cy + r)], fill=colour, width=int(thickness * SS))
    d.line([(cx - r, cy + r), (cx + r, cy - r)], fill=colour, width=int(thickness * SS))


def draw_lid(img, centre, ry, lid, colour=SKIN_TOP):
    """Skin-coloured cap over the top of an open eye: smug / heavy eyelids."""
    cx, cy = centre
    cap_h = ry * 2 * lid
    ImageDraw.Draw(img).ellipse([cx - 10.5 * SS, cy - ry - 1, cx + 10.5 * SS, cy - ry + cap_h],
                                fill=colour)


def draw_closed_eye(d, centre, *, style="up", scale=1.0, thickness=3.6):
    cx, cy = centre
    w = 10.5 * SS * scale
    h = w * (0.62 if style == "up" else 0.55)
    if style == "flat":
        d.line([(cx - w, cy), (cx + w, cy)], fill=EYE, width=int(thickness * SS))
        return
    box = [cx - w, cy - h, cx + w, cy + h]
    d.arc(box, start=200 if style == "up" else 20, end=340 if style == "up" else 160,
          fill=EYE, width=int(thickness * SS))


def draw_brow(d, centre, *, lift=0.0, angle=0.0, side=1, colour=BROW, thickness=3.4):
    cx, cy = centre
    half = 10.5 * SS
    base = cy - 20.0 * SS + lift
    pts = []
    for i in range(13):
        u = i / 12.0
        x = cx - half + 2 * half * u
        # mirror so `u_inner` always runs outer -> inner, whichever eye this is
        u_inner = u if side < 0 else 1.0 - u
        arch = math.sin(u * math.pi) * 2.4 * SS
        slope = (u_inner - 0.4) * angle * 7.0 * SS
        pts.append((x, base - arch + slope))
    d.line(pts, fill=colour, width=int(thickness * SS), joint="curve")
    for p in (pts[0], pts[-1]):
        r = thickness * SS * 0.5
        d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=colour)


def draw_mouth(d, kind, *, size=1.0, open_amt=1.0, width_scale=1.0, y=88.0):
    """kind: smile small open_laugh frown cry flat smirk kiss tongue o wavy."""
    cx, cy = CX * SS, y * SS
    w = 13.0 * SS * width_scale
    dark = MOUTH
    if kind == "small":
        d.arc([cx - w * 0.55, cy - 5 * SS, cx + w * 0.55, cy + 5 * SS], start=15, end=165,
              fill=dark, width=int(2.6 * SS))
    elif kind == "smile":
        d.arc([cx - w, cy - 8 * SS * size, cx + w, cy + 10 * SS * size], start=15, end=165,
              fill=dark, width=int(3.0 * SS))
    elif kind == "smirk":
        d.arc([cx - w * 0.7, cy - 5 * SS, cx + w * 1.05, cy + 9 * SS], start=25, end=140,
              fill=dark, width=int(3.0 * SS))
    elif kind == "flat":
        d.line([(cx - w * 0.55, cy), (cx + w * 0.55, cy)], fill=dark, width=int(3.0 * SS))
    elif kind == "frown":
        d.arc([cx - w * 0.85, cy + 2 * SS, cx + w * 0.85, cy + 16 * SS], start=195, end=345,
              fill=dark, width=int(3.0 * SS))
    elif kind == "wavy":
        pts = []
        for i in range(15):
            u = i / 14.0
            x = cx - w * 0.8 + 1.6 * w * u
            y_off = math.sin(u * 3 * math.pi) * 2.6 * SS
            pts.append((x, cy + 2 * SS + y_off))
        d.line(pts, fill=dark, width=int(3.0 * SS), joint="curve")
    elif kind in ("open_laugh", "o", "cry", "kiss", "tongue"):
        if kind == "open_laugh":
            rx, ry = w * 0.95, 11.0 * SS * open_amt
        elif kind == "cry":
            rx, ry = w * 0.8, 9.0 * SS * open_amt
        elif kind == "o":
            rx, ry = 6.4 * SS, 8.2 * SS * open_amt
        elif kind == "kiss":
            rx, ry = 5.6 * SS, 4.6 * SS * open_amt
        else:
            rx, ry = w * 0.8, 8.0 * SS * open_amt
        box = [cx - rx, cy - ry * 0.55, cx + rx, cy + ry * 1.25]
        d.ellipse(box, fill=dark)
        if kind in ("open_laugh", "cry", "tongue"):
            # teeth strip + tongue, which is what makes a laugh read as a laugh
            d.rectangle([box[0] + 1, box[1] + 1, box[2] - 1, box[1] + 3.2 * SS],
                        fill=(255, 255, 255, 235))
            tr = rx * 0.55
            ty = box[3] - ry * 0.55
            d.ellipse([cx - tr, ty - tr * 0.55, cx + tr, ty + tr * 0.75], fill=TONGUE)
    if kind == "kiss":
        # puckered lips: a heart with the point up, dark outline for contrast
        r = 6.4 * SS * open_amt
        pts = []
        for i in range(24):
            ang = i / 24.0 * 2 * math.pi
            # heart curve, flipped vertically so the point faces up
            hx = 16 * math.sin(ang) ** 3
            hy = -(13 * math.cos(ang) - 5 * math.cos(2 * ang) - 2 * math.cos(3 * ang)
                   - math.cos(4 * ang))
            pts.append((cx + hx * r / 16.0, cy + 4 * SS - hy * r / 16.0))
        d.polygon(pts, fill=MOUTH)
        d.arc([cx - r * 0.45, cy - r * 0.15, cx + r * 0.45, cy + r * 0.55], start=200,
              end=340, fill=TONGUE, width=int(1.4 * SS))


def draw_heart(d, centre, r, colour):
    x, y = centre
    d.ellipse([x - r, y - r * 0.92, x, y], fill=colour)
    d.ellipse([x, y - r * 0.92, x + r, y], fill=colour)
    d.polygon([(x - r * 0.95, y - r * 0.2), (x + r * 0.95, y - r * 0.2), (x, y + r)],
              fill=colour)


def draw_star(d, centre, r, colour):
    x, y = centre
    pts = []
    for i in range(10):
        ang = -math.pi / 2 + i * math.pi / 5
        rr = r if i % 2 == 0 else r * 0.44
        pts.append((x + rr * math.cos(ang), y + rr * math.sin(ang)))
    d.polygon(pts, fill=colour)


def draw_sparkle(d, centre, r, colour=SPARK):
    x, y = centre
    d.polygon([(x, y - r), (x + r * 0.22, y - r * 0.22), (x + r, y),
               (x + r * 0.22, y + r * 0.22), (x, y + r), (x - r * 0.22, y + r * 0.22),
               (x - r, y), (x - r * 0.22, y - r * 0.22)], fill=colour)


def draw_tear(d, centre, r, colour=TEAR):
    x, y = centre
    d.ellipse([x - r * 0.72, y - r * 0.45, x + r * 0.72, y + r], fill=colour)
    d.polygon([(x - r * 0.72, y + r * 0.3), (x + r * 0.72, y + r * 0.3), (x, y - r * 1.6)],
              fill=colour)


def draw_sunglasses(d):
    y = CY * SS - 6 * SS
    for cx in (46 * SS, 82 * SS):
        d.rounded_rectangle([cx - 15 * SS, y - 10 * SS, cx + 15 * SS, y + 10 * SS],
                             radius=4 * SS, fill=GLASS)
        d.line([(cx - 12 * SS, y - 6 * SS), (cx + 6 * SS, y - 2 * SS)],
               fill=(120, 130, 150, 200), width=int(1.4 * SS))
    d.line([(46 * SS + 15 * SS, y), (82 * SS - 15 * SS, y)], fill=GLASS, width=int(3 * SS))


def draw_angry_mark(d, colour=BROW_ANGRY):
    x, y = 108 * SS, 19 * SS
    for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
        d.line([(x, y), (x + dx * 6 * SS, y + dy * 6 * SS)], fill=colour, width=int(3.0 * SS))


def draw_z(d, centre, size, colour=BROW):
    x, y = centre
    w = max(2, int(size * 0.24))
    d.line([(x, y), (x + size, y)], fill=colour, width=w)
    d.line([(x + size, y), (x, y + size)], fill=colour, width=w)
    d.line([(x, y + size), (x + size, y + size)], fill=colour, width=w)


# --------------------------------------------------------------------------- #
# poses
# --------------------------------------------------------------------------- #

def base(**kw):
    p = dict(open=1.0, look=(0.0, 0.0), left="open", right="open", heart_eyes=False,
             star_eyes=False, brow_l=0.0, brow_r=0.0, lift=0.0, brow_colour=BROW,
             mouth="smile", mouth_size=1.0, mouth_open=1.0, mouth_w=1.0, mouth_y=88.0,
             lid=0.0, tongue=None, blush=0, glasses=False, tears=(), hearts=(), sparks=(),
             dots=0, zzz=0,
             bang=0, angry=0, question=0.0, tilt=0.0, bob=0.0)
    p.update(kw)
    return p


def pose_neutral(t):
    return base(open=blink(t), look=(2.0 * pulse(t, 0.15), 0.6 * pulse(t, 0.5)),
                mouth="flat", mouth_w=0.75, lift=0.6 * pulse(t, 0.7),
                bob=0.7 * pulse(t, 0.3))


def pose_happy(t):
    lift = -1.6 - 1.2 * abs(pulse(t))
    return base(left="up", right="up", lift=lift, bob=-2.0 * abs(pulse(t)),
                mouth="smile", mouth_size=1.08,
                blush=70, sparks=((18, 30, 5.2 + 1.2 * pulse(t)), (110, 92, 4.2)))


def pose_laughing(t):
    lift = -2.4 - 1.8 * abs(pulse(t * 2))
    return base(left="up", right="up", lift=lift, bob=-3.6 * abs(pulse(t * 2)),
                mouth="open_laugh", mouth_open=0.85 + 0.3 * abs(pulse(t * 2)),
                tilt=2.2 * pulse(t * 2), blush=80,
                sparks=((16, 28, 6.0), (112, 30, 5.0), (104, 96, 4.4)))


def pose_funny(t):
    wink = (t % 1.0) < 0.55
    return base(left="open", right="up" if wink else "open",
                brow_l=0.2, brow_r=-1.0 if wink else 0.2, lift=-1.4,
                look=(4.0 * pulse(t, 0.1), -1.0), mouth="smirk",
                mouth_w=0.95, tilt=-3.2 if wink else 0.0,
                sparks=((112, 98, 4.4),), bob=-1.2 * abs(pulse(t)))


def pose_sad(t):
    tears = ()
    if t > 0.45:
        u = (t - 0.45) / 0.55
        tears = ((46 * 1.0, 74 + 24 * u, 4.6 * (1 - 0.45 * u)),)
    return base(open=0.66, look=(0.0, 3.4), brow_l=-1.35, brow_r=-1.35, lift=2.4,
                mouth="frown", mouth_w=0.8, tears=tears,
                bob=1.4 + 0.6 * pulse(t, 0.35))


def pose_angry(t):
    return base(open=0.52, look=(0.0, 1.6), brow_l=1.35, brow_r=1.35, lift=2.6,
                brow_colour=BROW_ANGRY, mouth="frown", mouth_w=1.1, angry=1,
                tilt=1.6 * pulse(t * 3), bob=0.7 * abs(pulse(t * 3)))


def pose_crying(t):
    tears = []
    for i in range(2):
        u = (t * 1.4 + i * 0.5) % 1.0
        r = 4.8 * (1 - 0.4 * u)
        tears.append((40, 78 + 26 * u, r))
        tears.append((88, 78 + 26 * u, r))
    return base(left="x", right="x", brow_l=-1.2, brow_r=-1.2, lift=2.4,
                mouth="cry", mouth_open=1.0 + 0.2 * abs(pulse(t, 0.25)),
                mouth_w=1.05, tears=tuple(tears), bob=1.8 + 0.8 * pulse(t, 0.2))


def pose_loving(t):
    beat = 1.0 + 0.10 * pulse(t * 2)
    hearts = ((20, 26 - 8 * abs(pulse(t, 0.1)), 6.4), (108, 26 - 8 * abs(pulse(t, 0.6)), 5.4))
    return base(heart_eyes=True, brow_l=-0.5, brow_r=-0.5, lift=-2.2, mouth="smile",
                mouth_size=beat, blush=95, hearts=hearts, bob=-1.2 * abs(pulse(t, 0.2)))


def pose_embarrassed(t):
    return base(open=0.88, look=(-6.0, 2.0), brow_l=-0.8, brow_r=-0.8, lift=0.6,
                mouth="wavy", mouth_w=0.75, blush=int(120 + 45 * abs(pulse(t, 0.3))),
                tears=((110, 54, 4.2),), bob=0.5 * pulse(t, 0.4))


def pose_surprised(t):
    return base(open=1.24, brow_l=-0.1, brow_r=-0.1, lift=-7.0 - 1.2 * abs(pulse(t, 0.3)),
                mouth="o", mouth_open=0.95 + 0.1 * abs(pulse(t, 0.3)),
                bob=-1.8 * abs(pulse(t, 0.3)))


def pose_shocked(t):
    return base(open=1.34, brow_l=-0.05, brow_r=-0.05, lift=-8.6, mouth="o",
                mouth_open=1.2, bang=1, tilt=2.2 * pulse(t * 4),
                bob=1.2 * abs(pulse(t * 4)))


def pose_thinking(t):
    drift = 0.5 + 0.5 * math.sin(t * 2 * math.pi)
    return base(open=0.94, lid=0.18, look=(6.0, -5.0), brow_l=-1.0, brow_r=0.4,
                lift=-0.4, mouth="flat", mouth_w=0.8, dots=1 + int(drift * 2.99),
                tilt=-1.6 * pulse(t, 0.2), bob=0.5 * pulse(t, 0.35))


def pose_winking(t):
    closed = (t % 1.0) < 0.55
    return base(left="open", right="up" if closed else "open",
                brow_l=0.1, brow_r=-0.85 if closed else 0.1, lift=-1.2,
                look=(2.4, -0.4), mouth="smirk", mouth_w=0.95,
                tilt=-2.0 if closed else 0.0)


def pose_cool(t):
    return base(open=1.0, glasses=True, brow_l=0.1, brow_r=0.1, lift=-1.2,
                mouth="smirk", mouth_w=0.9, bob=0.6 * pulse(t, 0.25))


def pose_relaxed(t):
    breathe = 1.0 + 1.4 * abs(pulse(t))
    return base(left="flat", right="flat", lift=-breathe, mouth="small",
                mouth_w=0.8, bob=-breathe * 0.6)


def pose_delicious(t):
    return base(left="up", right="up", lift=-1.8, mouth="tongue", mouth_w=1.05,
                blush=90, sparks=((14, 26, 5.6 + 1.2 * pulse(t)), (114, 30, 5.0)),
                hearts=((104, 96, 5.0),), bob=-1.4 * abs(pulse(t, 0.2)))


def pose_kissy(t):
    closed = (t % 1.0) < 0.5
    return base(left="open", right="up" if closed else "open",
                brow_l=-0.5, brow_r=-0.5, lift=-1.6, look=(-2.0, 0.0),
                mouth="kiss", mouth_open=0.9 + 0.2 * abs(pulse(t, 0.2)), blush=85,
                tilt=-1.4 if closed else 0.0,
                hearts=((64, 112 - 6 * abs(pulse(t, 0.2)), 5.2),))


def pose_confident(t):
    return base(open=1.0, lid=0.34, brow_l=-0.45, brow_r=-0.1,
                lift=-2.2 + 0.6 * pulse(t, 0.45), mouth="smirk", mouth_w=1.05,
                tilt=-1.4, bob=-0.8 + 0.4 * pulse(t, 0.45))


def pose_sleepy(t):
    lid = clamp(0.5 + 0.5 * math.sin(t * 2 * math.pi), 0.0, 1.0)
    closed = lid > 0.80
    return base(left="flat" if closed else "open", right="flat" if closed else "open",
                open=clamp(1.0 - lid * 0.55, 0.12, 1.0), look=(0.0, 1.4),
                brow_l=-0.3, brow_r=-0.3, lift=1.4, mouth="small", mouth_w=0.7,
                zzz=1, bob=1.0 * pulse(t, 0.18))


def pose_silly(t):
    return base(open=0.95, look=(-7.0 + 2.0 * pulse(t), 2.0), star_eyes=False,
                brow_l=-0.6, brow_r=0.6, lift=-2.2 * abs(pulse(t, 0.1)),
                mouth="tongue", mouth_w=1.0, tilt=3.2 * pulse(t, 0.25),
                bob=-1.0 * abs(pulse(t, 0.1)))


def pose_confused(t):
    return base(open=0.95, look=(5.0 * pulse(t, 0.15), -2.2), brow_l=1.0, brow_r=-1.0,
                lift=0.4, mouth="wavy", mouth_w=0.8, tilt=3.0 * pulse(t, 0.1),
                question=1.0)


POSES = {
    "neutral": pose_neutral,
    "happy": pose_happy,
    "laughing": pose_laughing,
    "funny": pose_funny,
    "sad": pose_sad,
    "angry": pose_angry,
    "crying": pose_crying,
    "loving": pose_loving,
    "embarrassed": pose_embarrassed,
    "surprised": pose_surprised,
    "shocked": pose_shocked,
    "thinking": pose_thinking,
    "winking": pose_winking,
    "cool": pose_cool,
    "relaxed": pose_relaxed,
    "delicious": pose_delicious,
    "kissy": pose_kissy,
    "confident": pose_confident,
    "sleepy": pose_sleepy,
    "silly": pose_silly,
    "confused": pose_confused,
}

EYE_L = (46.0, 58.0)
EYE_R = (82.0, 58.0)


def render(pose):
    bob = pose.get("bob", 0.0) * SS

    # --- face features, drawn on their own layer so the rim is never resampled ---
    layer = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)

    if pose.get("blush"):
        for cx in (28, 100):
            d.ellipse([(cx - 11) * SS, (74 + pose.get("bob", 0)) * SS,
                       (cx + 11) * SS, (90 + pose.get("bob", 0)) * SS],
                      fill=(255, 118, 112, pose["blush"]))

    if not pose.get("glasses"):
        for centre, angle, side in ((EYE_L, pose.get("brow_l", 0.0), -1),
                                    (EYE_R, pose.get("brow_r", 0.0), 1)):
            draw_brow(d, (centre[0] * SS, centre[1] * SS + bob),
                      lift=pose.get("lift", 0.0) * SS, angle=angle, side=side,
                      colour=pose.get("brow_colour", BROW))

    for centre, state in ((EYE_L, pose.get("left", "open")), (EYE_R, pose.get("right", "open"))):
        pos = (centre[0] * SS, centre[1] * SS + bob)
        if state == "open":
            look = pose.get("look", (0.0, 0.0))
            draw_open_eye(d, pos, open_ratio=pose.get("open", 1.0),
                          look=(look[0] * SS * 0.6, look[1] * SS * 0.6),
                          heart=pose.get("heart_eyes", False),
                          star=pose.get("star_eyes", False))
            if pose.get("lid"):
                draw_lid(layer, pos, 11.4 * SS * pose.get("open", 1.0), pose["lid"])
        elif state == "x":
            draw_x_eye(d, pos, 9.0 * SS)
        else:
            draw_closed_eye(d, pos, style=state)

    if pose.get("glasses"):
        draw_sunglasses(d)

    draw_mouth(d, pose.get("mouth", "smile"), size=pose.get("mouth_size", 1.0),
               open_amt=pose.get("mouth_open", 1.0),
               width_scale=pose.get("mouth_w", 1.0),
               y=pose.get("mouth_y", 88.0) + pose.get("bob", 0.0))

    if abs(pose.get("tilt", 0.0)) > 0.05:
        layer = layer.rotate(pose["tilt"], resample=Image.BICUBIC, center=(W / 2, W / 2))

    face = face_base()
    face.alpha_composite(layer)
    face = face.resize((SIZE, SIZE), Image.LANCZOS)
    # exact circular silhouette: computed per pixel at final size, so the outline is
    # symmetric all the way round instead of inheriting an arc drawn on the rim
    face.putalpha(circle_mask())

    # --- props: outside the disc, so they are composited after the mask ---
    props = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    dp = ImageDraw.Draw(props)
    for tear in pose.get("tears", ()):
        x, y, r = tear
        draw_tear(dp, (x * SS, y * SS + bob), r * SS)
    for heart in pose.get("hearts", ()):
        x, y, r = heart
        draw_heart(dp, (x * SS, y * SS + bob), r * SS, HEART)
    for spark in pose.get("sparks", ()):
        x, y, r = spark
        draw_sparkle(dp, (x * SS, y * SS + bob), r * SS)
    for i in range(pose.get("dots", 0)):
        r = (5.2 - i * 0.5) * SS
        cx = (110 + i * 8) * SS
        cy = (30 - i * 9) * SS + bob
        dp.ellipse([cx - r, cy - r, cx + r, cy + r], fill=ACCENT)
    if pose.get("zzz"):
        u = (pose["zzz"] * 3) % 1.0
        draw_z(dp, (100 * SS, (40 - 10 * u) * SS + bob), 10 * SS)
        draw_z(dp, (111 * SS, (28 - 10 * ((u + 0.45) % 1.0)) * SS + bob), 7 * SS)
    if pose.get("bang"):
        x, y = 110 * SS, 22 * SS + bob
        dp.line([(x, y), (x, y + 13 * SS)], fill=BROW_ANGRY, width=int(4.6 * SS))
        r = 2.8 * SS
        dp.ellipse([x - r, y + 21 * SS - r, x + r, y + 21 * SS + r], fill=BROW_ANGRY)
    if pose.get("angry"):
        draw_angry_mark(dp)
    if pose.get("question"):
        off = 2.0 * math.sin(pose["question"] * 2 * math.pi)
        qx, qy = (104 + off) * SS, 30 * SS + bob
        dp.arc([qx - 7 * SS, qy, qx + 7 * SS, qy + 12 * SS], start=200, end=20, fill=ACCENT,
               width=int(3.0 * SS))
        dp.line([(qx + 6.5 * SS, qy + 7 * SS), (qx + 1 * SS, qy + 14 * SS)], fill=ACCENT,
                width=int(3.0 * SS))
        r = 2.0 * SS
        dp.ellipse([qx + 0.5 * SS - r, qy + 20 * SS - r, qx + 0.5 * SS + r,
                    qy + 20 * SS + r], fill=ACCENT)

    props = props.resize((SIZE, SIZE), Image.LANCZOS)
    props.putalpha(props.getchannel("A").point(lambda v: 255 if v >= 128 else 0))
    face.alpha_composite(props)
    return face


def circle_mask(centre=(CX, CY), radius=R):
    """1-bit coverage mask of the face disc, computed analytically at final size."""
    mask = Image.new("L", (SIZE, SIZE), 0)
    px = mask.load()
    for y in range(SIZE):
        dy = y + 0.5 - centre[1]
        for x in range(SIZE):
            dx = x + 0.5 - centre[0]
            if dx * dx + dy * dy <= radius * radius:
                px[x, y] = 255
    return mask


def build_palette(frames):
    strip = Image.new("RGB", (SIZE * len(frames), SIZE))
    for i, f in enumerate(frames):
        strip.paste(f.convert("RGB"), (SIZE * i, 0))
    return strip.quantize(colors=255, method=Image.Quantize.MEDIANCUT)


def to_transparent_frame(frame, palette):
    rgb = frame.convert("RGB").quantize(palette=palette,
                                        dither=Image.Dither.FLOYDSTEINBERG)
    pal = palette.getpalette()[:255 * 3] + [0, 0, 0]
    rgb.putpalette(pal)
    alpha = frame.getchannel("A").point(lambda a: 255 if a < 128 else 0)
    rgb.paste(255, mask=alpha)
    return rgb


def save_gif(name, frames, out_dir, fps=FPS):
    palette = build_palette(frames)
    quantised = [to_transparent_frame(f, palette) for f in frames]
    path = os.path.join(out_dir, f"{name}.gif")
    quantised[0].save(path, save_all=True, append_images=quantised[1:], loop=0,
                      duration=int(1000 / fps), disposal=2, transparency=255,
                      optimize=False)
    return path


def contact_sheet(items, path, cols=5):
    rows = (len(items) + cols - 1) // cols
    pad = 6
    canvas = Image.new("RGB", (cols * (SIZE + pad) + pad, rows * (SIZE + pad + 14) + pad),
                       (6, 8, 12))
    d = ImageDraw.Draw(canvas)
    for idx, (name, frames) in enumerate(items):
        x = pad + (idx % cols) * (SIZE + pad)
        y = pad + (idx // cols) * (SIZE + pad + 14)
        frame = frames[len(frames) // 4]
        canvas.paste(frame, (x, y), frame)
        d.text((x + 2, y + SIZE + 1), name, fill=(215, 220, 226))
    canvas.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--frames", type=int, default=16)
    ap.add_argument("--contact", default=None)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    items = []
    for name, pose_fn in POSES.items():
        frames = [render(pose_fn(i / args.frames)) for i in range(args.frames)]
        path = save_gif(name, frames, args.out)
        items.append((name, frames))
        print(f"{name:12s} {len(frames)} frames {os.path.getsize(path) // 1024:4d} KB")
    if args.contact:
        contact_sheet(items, args.contact)
        print("contact sheet:", args.contact)


if __name__ == "__main__":
    main()
