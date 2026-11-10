#!/usr/bin/env python3
"""check.py - FitView's host test results (tests/fitview/fitview_test.c's dumps) against
references written here from the descriptions in fvview.h, fvrender.h and fvk.h, independently
of the C:
  - the geometry of every zoom mode from exact fractions (rounded half up), scroll ranges and
    placement;
  - every rendered window: the background, the nearest-neighbour preview, the refined picture
    (Catmull-Rom weights from exact fractions, widened by the factor when shrinking, Q14, two
    passes; four channels), and alpha over a checkerboard or one colour;
  - the ditherer to pens (nearest bucket colour, Floyd-Steinberg, the pen map);
  - the alpha composite on its own.
  (The weights, the ditherer and their helpers are WBFit's references, tests/wbfit/check.py.)
  check.py DIR"""
import os, struct, sys
from fractions import Fraction
import numpy as np

D = sys.argv[1]
fails = 0
checks = 0


def check(ok, what):
    global fails, checks
    checks += 1
    if not ok:
        fails += 1
        if fails < 30:
            print("FAIL", what)


def ctrunc(a, b):
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def cubic(t):
    t2 = (t * t) >> 12
    t3 = (t2 * t) >> 12
    if t < 4096:
        return ctrunc(3 * t3 - 5 * t2 + 8192, 2)
    if t < 8192:
        return ctrunc(-t3 + 5 * t2 - 8 * t + 16384, 2)
    return 0


WCACHE = {}


def weights(src, dst, v0, n):
    key = (src, dst)
    if key not in WCACHE:
        WCACHE[key] = {}
    out = []
    fs = max(Fraction(src, dst), 1)
    for o in range(v0, v0 + n):
        if o in WCACHE[key]:
            out.append(WCACHE[key][o])
            continue
        centre = Fraction(2 * o + 1, 2) * Fraction(src, dst)
        lo = max(0, int(centre - 2 * fs) - 1)
        hi = min(src, int(centre + 2 * fs) + 2)
        raw = []
        for j in range(lo, hi):
            t = abs(Fraction(2 * j + 1, 2) - centre) / fs
            if t >= 2:
                continue
            raw.append((j, cubic(int(t * 4096))))
        while len(raw) > 1 and raw[0][1] == 0:
            raw.pop(0)
        while len(raw) > 1 and raw[-1][1] == 0:
            raw.pop()
        s = sum(w for _, w in raw)
        q = []
        for _, w in raw:
            v = w * 16384
            q.append((v + s // 2) // s if v >= 0 else -((-v + s // 2) // s))
        m = q.index(max(q))
        q[m] += 16384 - sum(q)
        WCACHE[key][o] = (raw[0][0], q)
        out.append(WCACHE[key][o])
    return out


def q14(a):
    return np.clip((np.where(a <= 0, 0, a) + 8192) >> 14, 0, 255)


def resample(src, dw, dh, vx0, vy0, vw, vh):
    sh, sw, ch = src.shape
    wx = weights(sw, dw, vx0, vw)
    wy = weights(sh, dh, vy0, vh)
    s = src.astype(np.int64)
    mid = np.zeros((sh, vw, ch), np.int64)
    for x, (f, w) in enumerate(wx):
        acc = np.zeros((sh, ch), np.int64)
        for k, c in enumerate(w):
            acc += c * s[:, f + k, :]
        mid[:, x, :] = q14(acc)
    out = np.zeros((vh, vw, ch), np.int64)
    for y, (f, w) in enumerate(wy):
        acc = np.zeros((vw, ch), np.int64)
        for k, c in enumerate(w):
            acc += c * mid[f + k]
        out[y] = q14(acc)
    return out.astype(np.uint8)


def nearest(src, dw, dh, vx0, vy0, vw, vh):
    sh, sw, _ = src.shape
    xs = [min(sw - 1, (2 * (vx0 + i) + 1) * sw // (2 * dw)) for i in range(vw)]
    ys = [min(sh - 1, (2 * (vy0 + i) + 1) * sh // (2 * dh)) for i in range(vh)]
    return src[np.ix_(ys, xs)]


def comp_px(px, x, y, c0, c1, shift, orgx, orgy):
    a = int(px[0])
    if a == 255:
        return list(px)
    sq = ((((x - orgx) >> shift) ^ ((y - orgy) >> shift)) & 1) if shift else 0
    bg = c1 if sq else c0
    bc = [(bg >> 16) & 255, (bg >> 8) & 255, bg & 255]
    return [255] + [min(255, int(px[1 + k]) + (2 * (255 - a) * bc[k] + 255) // 510) for k in range(3)]


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def rnd(a, b):               # a / b rounded half up
    return int((Fraction(a, b) + Fraction(1, 2)) // 1)


# ------------------------------------------------------------------ geometry
FIT, WIDTH, ONE, ZOOM, FILL = range(5)
b = open(os.path.join(D, "geom.bin"), "rb").read()
n = len(b) // (18 * 4)
for i in range(n):
    v = struct.unpack(">18I", b[i * 72:(i + 1) * 72])
    sw, sh, an, ad, W, H, mode, zoom, snap, dw, dh, maxx, maxy, h100, sx, sy, px, py = v
    clamp = lambda q: max(1, min(32767, q))
    h = clamp(rnd(sh * an, ad))
    if mode in (FIT, FILL):
        scale = min(Fraction(W, sw), Fraction(H, h)) if mode == FIT else max(Fraction(W, sw), Fraction(H, h))
        if scale == Fraction(W, sw):
            ew, eh = W, rnd(h * W, sw)
        else:
            ew, eh = rnd(sw * H, h), H
    elif mode == WIDTH:
        ew, eh = W, rnd(h * W, sw)
    elif mode == ZOOM:
        ew, eh = rnd(sw * zoom, 1000), rnd(h * zoom, 1000)
    else:
        ew, eh = sw, h
    if snap and abs(ew - sw) <= 1 and abs(eh - h) <= 1:
        ew, eh = sw, h
    ew, eh = clamp(ew), clamp(eh)
    check((dw, dh, h100) == (ew, eh, h), "geom %d: %s -> %s" % (i, v[:9], (dw, dh, ew, eh)))
    check((maxx, maxy) == (max(0, ew - W), max(0, eh - H)), "geom scroll %d" % i)
    ex = (W - ew) // 2 if ew <= W else -min(sx, ew - W)
    ey = (H - eh) // 2 if eh <= H else -min(sy, eh - H)
    check((s32(px), s32(py)) == (ex, ey), "geom place %d" % i)
print("check.py: geometry, %d cases" % n)

# ------------------------------------------------------------------ rendered windows
b = open(os.path.join(D, "render.bin"), "rb").read()
p = 0
nc = 0
while p < len(b):
    sw, sh, W, H, dw, dh, px, py, q, alpha, bg, chk0, chk1, shift, nofine = struct.unpack(">15I", b[p:p + 60])
    px, py = s32(px), s32(py)
    p += 60
    src = np.frombuffer(b[p:p + sw * sh * 4], np.uint8).reshape(sh, sw, 4)
    p += sw * sh * 4
    got = np.frombuffer(b[p:p + W * H * 4], np.uint8).reshape(H, W, 4)
    p += W * H * 4
    exp = np.zeros((H, W, 4), np.uint8)
    exp[:, :] = (255, (bg >> 16) & 255, (bg >> 8) & 255, bg & 255)
    vx0, vy0 = max(0, -px), max(0, -py)
    ox, oy = max(0, px), max(0, py)
    vw, vh = max(0, min(dw - vx0, W - ox)), max(0, min(dh - vy0, H - oy))
    if vw and vh:
        part = (resample if q == 1 and not nofine else nearest)(src, dw, dh, vx0, vy0, vw, vh)
        if alpha:
            part = part.copy()
            for y in range(vh):
                for x in range(vw):
                    part[y, x] = comp_px(part[y, x], ox + x, oy + y, chk0 if shift else bg, chk1, shift, px, py)
        exp[oy:oy + vh, ox:ox + vw] = part
    ok = np.array_equal(exp, got)
    if not ok:
        d = np.argwhere(exp != got)[0]
        print("  case %d first difference at row %d col %d ch %d: %d (expected %d)" % (nc, d[0], d[1], d[2], got[tuple(d)], exp[tuple(d)]))
    check(ok, "render case %d (%dx%d -> %dx%d in %dx%d at %d,%d, %s%s)" % (nc, sw, sh, dw, dh, W, H, px, py, "refine" if q else "preview", ", alpha" if alpha else ""))
    nc += 1
print("check.py: %d rendered windows" % nc)


# ------------------------------------------------------------------ the ditherer
def key(r, g, b):
    return (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)


def dither(img, pal, fs):
    npal = len(pal) // 3
    cache = {}

    def near(k):
        if k not in cache:
            r, g, b = ((k >> 10) & 31) << 3 | 4, ((k >> 5) & 31) << 3 | 4, (k & 31) << 3 | 4
            best, bd = 0, None
            for i in range(npal):
                dd = (pal[i * 3] - r) ** 2 * 3 + (pal[i * 3 + 1] - g) ** 2 * 4 + (pal[i * 3 + 2] - b) ** 2 * 2
                if bd is None or dd < bd:
                    best, bd = i, dd
            cache[k] = best
        return cache[k]
    h, w, _ = img.shape
    out = np.zeros((h, w), np.uint8)
    e16 = lambda e: (e + 8) >> 4 if e >= 0 else -((-e + 8) >> 4)
    cur = [[0, 0, 0] for _ in range(w + 2)]
    for y in range(h):
        nxt = [[0, 0, 0] for _ in range(w + 2)]
        for x in range(w):
            c = [int(v) for v in img[y, x]]
            if fs:
                c = [min(255, max(0, c[k] + e16(cur[x + 1][k]))) for k in range(3)]
            i = near(key(*c))
            out[y, x] = i
            if fs:
                for k in range(3):
                    e = c[k] - pal[i * 3 + k]
                    cur[x + 2][k] += e * 7
                    nxt[x][k] += e * 3
                    nxt[x + 1][k] += e * 5
                    nxt[x + 2][k] += e
        cur = nxt
    return out


b = open(os.path.join(D, "dither.bin"), "rb").read()
p = 0
nd = 0
while p < len(b):
    w, h, stride, ostride, npal, fs = struct.unpack(">6I", b[p:p + 24])
    p += 24
    px = np.frombuffer(b[p:p + stride * h], np.uint8).reshape(h, stride)[:, :w * 4].reshape(h, w, 4)
    p += stride * h
    pal = list(b[p:p + 768])[:npal * 3]
    p += 768
    pm = b[p:p + 256]
    p += 256
    got = np.frombuffer(b[p:p + ostride * h], np.uint8).reshape(h, ostride)[:, :w]
    p += ostride * h
    idx = dither(px[:, :, 1:4], pal, fs)
    exp = np.vectorize(lambda i: pm[i])(idx).astype(np.uint8)
    check(np.array_equal(exp, got), "dither %d (%s)" % (nd, "Floyd-Steinberg" if fs else "nearest"))
    nd += 1
print("check.py: %d dithers" % nd)

# ------------------------------------------------------------------ the composite
b = open(os.path.join(D, "comp.bin"), "rb").read()
p = 0
nk = 0
while p < len(b):
    W, H, c0, c1, shift, orgx, orgy = struct.unpack(">7I", b[p:p + 28])
    orgx, orgy = s32(orgx), s32(orgy)
    p += 28
    src = np.frombuffer(b[p:p + W * H * 4], np.uint8).reshape(H, W, 4)
    p += W * H * 4
    got = np.frombuffer(b[p:p + W * H * 4], np.uint8).reshape(H, W, 4)
    p += W * H * 4
    exp = src.copy()
    for y in range(1, H):
        for x in range(2, W - 1):
            exp[y, x] = comp_px(src[y, x], x, y, c0, c1, shift, orgx, orgy)
    check(np.array_equal(exp, got), "composite %d" % nk)
    nk += 1
print("check.py: %d composites" % nk)
print("check.py: %d checks, %d failed" % (checks, fails))
sys.exit(1 if fails else 0)
