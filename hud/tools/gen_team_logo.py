"""Builds hud/images/team/atram.svg, the CS2 team logo (mp_teamlogo_1/2 atram, set by hud.so), from
the website's logo (public/at-icon.svg in Auto-Tournament/website): no orange fill, the orange shapes
cut as holes, the mouth black, an orange outline of offset copies (CS2 draws no strokes), cropped,
view box from 0,0. Run: python hud/tools/gen_team_logo.py [path to at-icon.svg]"""
import math, os, re, sys
P = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "images", "team", "atram.svg")
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.expanduser("~"), "Desktop", "at-web-checkin", "public", "at-icon.svg")
t = open(SRC, encoding="utf-8").read()
src = []
for m in re.finditer(r"<path([^>]*)/?>", t):
    a = m.group(1)
    d = re.search(r'd="([^"]*)"', a).group(1)
    f = re.search(r'fill="([^"]*)"', a)
    src.append(((f.group(1) if f else "#000000").upper(), d))


def segs(d):
    """[(cmd, [(x,y),...])] for an absolute M/L/C/Z path."""
    out = []
    for cmd, args in re.findall(r"([MLCZ])([^MLCZ]*)", d):
        n = [float(v) for v in re.findall(r"-?\d*\.?\d+", args)]
        out.append((cmd, [(n[i], n[i + 1]) for i in range(0, len(n), 2)]))
    return out


def bbox(d):
    p = [q for _, ps in segs(d) for q in ps]
    return min(x for x, _ in p), min(y for _, y in p), max(x for x, _ in p), max(y for _, y in p)


def reverse(d):
    """The same closed shape drawn the other way round (one subpath, M then L/C, Z)."""
    s = segs(d)
    start = s[0][1][0]
    pieces, cur = [], start
    for cmd, ps in s[1:]:
        if cmd == "L":
            pieces.append(("L", cur, [], ps[0]))
            cur = ps[0]
        elif cmd == "C":
            pieces.append(("C", cur, [ps[0], ps[1]], ps[2]))
            cur = ps[2]
    if cur != start:
        pieces.append(("L", cur, [], start))
    out = [("M", [start])]
    for kind, a, ctrl, b in reversed(pieces):
        out.append(("L", [a]) if kind == "L" else ("C", [ctrl[1], ctrl[0], a]))
    out.append(("Z", []))
    return out


# The original's stacking order matters: the black ram, the white face on it, the black eye on the
# face (drawing all black first hid the eye under the face, 2026-11-24). Each orange shape is a hole
# in the shape below it whose box holds it (the horn); the mouth (light orange) is black.
order = [(f, segs(d), d) for f, d in src]
shapes = []  # (fill, segs)
for f, s, d in order:
    if f == "#FF6A3D":
        hx0, hy0, hx1, hy1 = bbox(d)
        holder = None
        for j in range(len(shapes) - 1, -1, -1):
            sd = " ".join(c + " ".join(f"{x},{y}" for x, y in ps) for c, ps in shapes[j][1])
            x0, y0, x1, y1 = bbox(sd)
            if shapes[j][0] == "#1D1D1F" and x0 <= hx0 and y0 <= hy0 and x1 >= hx1 and y1 >= hy1:
                holder = j
                break
        if holder is None:
            print("hole without a holder", bbox(d))
            continue
        shapes[holder] = (shapes[holder][0], shapes[holder][1] + reverse(d))
        continue
    shapes.append(("#1D1D1F" if f == "#FF8F66" else f, s))
silhouette = shapes[0]  # the first, biggest black shape: the outline follows it

R, STEPS, ORANGE = 5.0, 16, "#FF6A3D"  # a thin outline (Sivert: thinner)
allp = [q for _, s in shapes for _, ps in s for q in ps]
x0 = min(x for x, _ in allp) - R
y0 = min(y for _, y in allp) - R
x1 = max(x for x, _ in allp) + R
y1 = max(y for _, y in allp) + R
side = max(x1 - x0, y1 - y0)
bx, by = -x0 + (side - (x1 - x0)) / 2, -y0 + (side - (y1 - y0)) / 2


def fmt(v):
    s = f"{v:.2f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def draw(s, ox=0.0, oy=0.0):
    return " ".join(cmd + " ".join(f"{fmt(x + bx + ox)},{fmt(y + by + oy)}" for x, y in ps) for cmd, ps in s)


outline, front = [], []
for k in range(STEPS):
    a = 2 * math.pi * k / STEPS
    # Every shape: the right horn and the neck stick out of the big one (Sivert, 2026-11-24). The
    # copies sit behind everything, so inner shapes (the eye) get no ring.
    for f, s in shapes:
        outline.append(f'  <path fill="{ORANGE}" fill-rule="evenodd" d="{draw(s, R * math.cos(a), R * math.sin(a))}"/>')
for f, s in shapes:
    front.append(f'  <path fill="{f}" fill-rule="evenodd" d="{draw(s)}"/>')
svg = ('<?xml version="1.0" encoding="utf-8"?>\n'
       '<!-- The Auto Tournament ram (website public/at-icon.svg) as a CS2 team logo (mp_teamlogo_1/2 atram,\n'
       '     hud.so): no orange fill (the orange shapes in the ram are holes, cut out of the shape around\n'
       '     them and drawn the other way round), the mouth black, an orange outline made of offset copies\n'
       '     (CS2 draws no strokes), cropped, view box from 0,0, fills and cubic curves only. -->\n'
       f'<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 {fmt(side)} {fmt(side)}">\n'
       + "\n".join(outline + front) + "\n</svg>\n")
open(P, "w", encoding="utf-8", newline="\n").write(svg)
print("shapes", len(shapes), "| bytes", len(svg))
