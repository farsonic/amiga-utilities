#!/usr/bin/env python3
"""check.py - WBFit's host test results (tests/wbfit/wbfit_test.c's dumps) against references
written here from the description in wfcore.h / wfk.h, independently of the C:
  - the resampler (weights from exact fractions, two integer passes), the borders, the
    picture's edge colour, the old picture.datatype's colour lookup and HAM;
  - the quantiser (median cut) and the ditherer (nearest bucket colour, Floyd-Steinberg);
  - the PNG files decoded by Pillow (and every chunk CRC and the zlib stream checked here);
  - the ILBM files decoded by a reader here (BMHD, CMAP, ByteRun1 BODY, bitplanes);
  - the WBPattern.prefs files parsed with the NDK's structure layout.
  check.py DIR"""
import os, struct, sys, zlib
from fractions import Fraction
import numpy as np
from PIL import Image

D = sys.argv[1]
fails = 0
checks = 0


def check(ok, what):
    global fails, checks
    checks += 1
    if not ok:
        fails += 1
        print("FAIL", what)


def rd(name):
    return open(os.path.join(D, name), "rb").read()


def ctrunc(a, b):            # C99 division
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


# ------------------------------------------------------------------ the resampler
def cubic(t):                # Catmull-Rom, Q12 in and out, as wfcore.h says
    t2 = (t * t) >> 12
    t3 = (t2 * t) >> 12
    if t < 4096:
        return ctrunc(3 * t3 - 5 * t2 + 8192, 2)
    if t < 8192:
        return ctrunc(-t3 + 5 * t2 - 8 * t + 16384, 2)
    return 0


def weights(src, dst, v0, n):
    """[(first, [w...])] for scaled positions v0..v0+n-1"""
    fs = max(Fraction(src, dst), 1)
    out = []
    for o in range(v0, v0 + n):
        centre = Fraction(2 * o + 1, 2) * Fraction(src, dst)        # in source pixels
        raw = []
        for j in range(src):
            t = abs(Fraction(2 * j + 1, 2) - centre) / fs              # the kernel's argument
            if t >= 2:
                continue
            tq = int(t * 4096)                                         # Q12, floor
            raw.append((j, cubic(tq)))
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
        out.append((raw[0][0], q))
    return out


def q14(a):
    return np.clip((a + 8192) >> 14, 0, 255)


def resample(src, dw, dh, vx0, vy0, vw, vh):
    sh, sw, _ = src.shape
    wx = weights(sw, dw, vx0, vw)
    wy = weights(sh, dh, vy0, vh)
    s = src.astype(np.int64)
    mid = np.zeros((sh, vw, 3), np.int64)
    for x, (f, w) in enumerate(wx):
        acc = np.zeros((sh, 3), np.int64)
        for k, c in enumerate(w):
            acc += c * s[:, f + k, :]
        mid[:, x, :] = q14(np.where(acc <= 0, 0, acc))
    out = np.zeros((vh, vw, 3), np.int64)
    for y, (f, w) in enumerate(wy):
        acc = np.zeros((vw, 3), np.int64)
        for k, c in enumerate(w):
            acc += c * mid[f + k]
        out[y] = q14(np.where(acc <= 0, 0, acc))
    return out.astype(np.uint8)


def edge(src):
    h, w, _ = src.shape
    m = np.zeros((h, w), bool)
    m[0, :] = m[-1, :] = m[:, 0] = m[:, -1] = True
    px = src[m].astype(np.int64)
    n = len(px)
    c = [(int(px[:, k].sum()) + n // 2) // n for k in range(3)]
    return c[0] << 16 | c[1] << 8 | c[2]


def lut(idx, pal, w, h, aw, ham):
    out = np.zeros((h, w, 3), np.uint8)
    for y in range(h):
        r, g, b = pal[0], pal[1], pal[2]
        for x in range(w):
            v = idx[y * aw + x]
            if not ham:
                r, g, b = pal[v * 3:v * 3 + 3]
            else:
                ctl, d = v >> ham, v & ((1 << ham) - 1)
                dv = d * 17 if ham == 4 else (d << 2) | (d >> 4)
                if ctl == 0:
                    r, g, b = pal[d * 3:d * 3 + 3]
                elif ctl == 1:
                    b = dv
                elif ctl == 2:
                    r = dv
                else:
                    g = dv
            out[y, x] = (r, g, b)
    return out


# ------------------------------------------------------------------ the quantiser, the ditherer
def key(r, g, b):
    return (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)


def mediancut(img, ncol):
    hist = {}
    for p in img.reshape(-1, 3):
        k = key(int(p[0]), int(p[1]), int(p[2]))
        hist[k] = hist.get(k, 0) + 1
    cells = [((k >> 10) & 31, (k >> 5) & 31, k & 31, n) for k, n in hist.items()]

    def shrink(lo, hi):
        inside = [c for c in cells if all(lo[a] <= c[a] <= hi[a] for a in range(3))]
        n = sum(c[3] for c in inside)
        if not n:
            return lo, hi, 0
        return [min(c[a] for c in inside) for a in range(3)], [max(c[a] for c in inside) for a in range(3)], n
    boxes = [shrink([0, 0, 0], [31, 31, 31])]
    while len(boxes) < ncol:
        sel, bestn = -1, 0
        for i, (lo, hi, n) in enumerate(boxes):
            if any(hi[a] > lo[a] for a in range(3)) and n > bestn:
                sel, bestn = i, n
        if sel < 0:
            break
        lo, hi, n = boxes[sel]
        ax = max(range(3), key=lambda a: ((hi[a] - lo[a]) * (3, 4, 2)[a], -a))
        marg = [0] * 32
        for c in cells:
            if all(lo[a] <= c[a] <= hi[a] for a in range(3)):
                marg[c[ax]] += c[3]
        acc, cut = 0, lo[ax]
        while cut < hi[ax] - 1:
            acc += marg[cut]
            if acc * 2 >= n:
                break
            cut += 1
        hi1 = list(hi); hi1[ax] = cut
        lo2 = list(lo); lo2[ax] = cut + 1
        boxes[sel] = shrink(list(lo), hi1)
        boxes.append(shrink(lo2, list(hi)))
    pal = []
    for lo, hi, _ in boxes:
        s, n = [0, 0, 0], 0
        for c in cells:
            if all(lo[a] <= c[a] <= hi[a] for a in range(3)):
                n += c[3]
                for a in range(3):
                    s[a] += c[3] * (c[a] << 3 | 4)
        pal += [(s[a] + n // 2) // n if n else 0 for a in range(3)]
    return pal


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


# ------------------------------------------------------------------ the files
def png_chunks(b):
    check(b[:8] == b"\x89PNG\r\n\x1a\n", "png signature")
    p, out = 8, []
    while p < len(b):
        n, t = struct.unpack(">I4s", b[p:p + 8])
        data = b[p + 8:p + 8 + n]
        crc = struct.unpack(">I", b[p + 8 + n:p + 12 + n])[0]
        check(crc == zlib.crc32(t + data) & 0xFFFFFFFF, "png crc %s" % t)
        out.append((t, data))
        p += 12 + n
    return out


def read_ilbm(b):
    check(b[:4] == b"FORM" and b[8:12] == b"ILBM", "ilbm form")
    check(struct.unpack(">I", b[4:8])[0] == len(b) - 8, "ilbm form length")
    p, ch = 12, {}
    while p < len(b):
        t, n = b[p:p + 4], struct.unpack(">I", b[p + 4:p + 8])[0]
        ch[t] = b[p + 8:p + 8 + n]
        p += 8 + n + (n & 1)
    w, h, _, _, npl, mask, comp, _, _, xa, ya, pw, ph = struct.unpack(">HHhhBBBBHBBhh", ch[b"BMHD"])
    rb = ((w + 15) >> 4) * 2
    body, q = ch[b"BODY"], 0
    planes = np.zeros((h, npl, rb), np.uint8)
    for y in range(h):
        for pl in range(npl):
            row = bytearray()
            if comp == 1:
                while len(row) < rb:
                    c = body[q]; q += 1
                    if c < 128:
                        row += body[q:q + c + 1]; q += c + 1
                    elif c > 128:
                        row += bytes([body[q]]) * (257 - c); q += 1
                check(len(row) == rb, "byterun1 row length")
            else:
                row = body[q:q + rb]; q += rb
            planes[y, pl] = np.frombuffer(bytes(row[:rb]), np.uint8)
    bits = np.unpackbits(planes, axis=2)[:, :, :w]          # h, npl, w
    val = np.zeros((h, w), np.int64)
    for pl in range(npl):
        val |= bits[:, pl, :].astype(np.int64) << pl
    return dict(w=w, h=h, planes=npl, comp=comp, aspect=(xa, ya), cmap=ch.get(b"CMAP"), val=val)


# ------------------------------------------------------------------ the cases
for line in open(os.path.join(D, "cases.txt")):
    (i, kind, sw, sh, W, H, rx, ry, mode, fmt, depth, dith, usepal, bgauto, bgused, ham, ncol,
     dw, dh, px, py, vx0, vy0, vw, vh) = map(int, line.split())
    tag = "case %d" % i
    src = np.frombuffer(rd("c%02d.src" % i), np.uint8).reshape(sh, sw, 3)
    if ham:
        aw = (sw + 15) & ~15
        src = lut(rd("c%02d.idxin" % i), rd("c%02d.ipal" % i), sw, sh, aw, 0 if ham == 1 else ham)
    rgb = np.frombuffer(rd("c%02d.rgb" % i), np.uint8).reshape(H, W, 3)
    # the picture's place
    if mode in (0, 1):
        a, b = W * rx * sh, H * ry * sw
        if (a >= b) == (mode == 0):
            edw, edh = W, (2 * sh * W * rx + sw * ry) // (2 * sw * ry)
        else:
            edw, edh = (2 * sw * H * ry + sh * rx) // (2 * sh * rx), H
    elif mode == 2:
        edw, edh = W, H
    else:
        edw, edh = sw, sh
    check((dw, dh) == (edw, edh), tag + " size %dx%d, expected %dx%d" % (dw, dh, edw, edh))
    ox, oy = max(px, 0), max(py, 0)
    check(vx0 == max(-px, 0) and vy0 == max(-py, 0) and vw == min(dw - vx0, W - ox) and vh == min(dh - vy0, H - oy), tag + " place")
    if bgauto:
        check(bgused == edge(src), tag + " edge colour")
    ref = np.zeros((H, W, 3), np.uint8)
    ref[:, :] = (bgused >> 16 & 255, bgused >> 8 & 255, bgused & 255)
    if vw < W or vh < H or True:
        ref[oy:oy + vh, ox:ox + vw] = resample(src, dw, dh, vx0, vy0, vw, vh)
    if not (vw < W or vh < H):
        pass
    same = np.array_equal(ref, rgb)
    check(same, tag + " scaled pixels (reference resampler)")
    if not same:
        d = np.argwhere(ref != rgb)
        print("   first differences", d[:4].tolist(), ref[tuple(d[0][:2])].tolist(), rgb[tuple(d[0][:2])].tolist())
    if mode == 3 and dw <= W and dh <= H:
        check(np.array_equal(rgb[oy:oy + dh, ox:ox + dw], src), tag + " CENTER copies exactly")
    if fmt == 0:
        ch = png_chunks(rd("c%02d.png" % i))
        ihdr = struct.unpack(">IIBBBBB", ch[0][1])
        check(ch[0][0] == b"IHDR" and ihdr == (W, H, 8, 2, 0, 0, 0), tag + " IHDR")
        z = b"".join(d for t, d in ch if t == b"IDAT")
        raw = zlib.decompress(z)
        check(len(raw) == H * (1 + W * 3) and all(raw[y * (1 + W * 3)] <= 4 for y in range(H)), tag + " zlib stream and filters")
        im = Image.open(os.path.join(D, "c%02d.png" % i))
        im.load()
        check(im.size == (W, H) and im.mode == "RGB", tag + " Pillow size")
        check(np.array_equal(np.asarray(im), rgb), tag + " Pillow pixels")
    else:
        il = read_ilbm(rd("c%02d.ilbm" % i))
        check((il["w"], il["h"]) == (W, H), tag + " ILBM size")
        g = 1
        if fmt == 1:
            check(il["planes"] == 24 and il["cmap"] is None, tag + " ILBM24 planes")
            v = il["val"]
            dec = np.stack([v & 255, (v >> 8) & 255, (v >> 16) & 255], axis=2).astype(np.uint8)
            check(np.array_equal(dec, rgb), tag + " ILBM24 pixels")
        else:
            ind = np.frombuffer(rd("c%02d.ind" % i), np.uint8).reshape(H, W)
            pal = list(rd("c%02d.pal" % i))
            check(il["planes"] == depth and len(il["cmap"]) == 3 << depth, tag + " ILBM planes, CMAP")
            check(list(il["cmap"][:len(pal)]) == pal and np.array_equal(il["val"], ind), tag + " ILBM indices and palette")
            if usepal:
                wb = [170, 170, 170, 0, 0, 0, 255, 255, 255, 102, 136, 187, 238, 68, 68, 85, 204, 85, 0, 68, 221, 238, 153, 0]
                epal = wb[:3 * min(8, 1 << depth)]
            else:
                epal = mediancut(rgb, 1 << depth)
            check(pal == epal, tag + " palette (reference median cut)" if not usepal else tag + " palette (the screen's)")
            check(np.array_equal(dither(rgb, epal, dith), ind), tag + " indices (reference %s)" % ("Floyd-Steinberg" if dith else "nearest"))

# ------------------------------------------------------------------ the prefs
WBP = ">4LHHbbH"            # struct WBPatternPrefs: 24 bytes on the 68k
check(struct.calcsize(WBP) == 24, "WBPatternPrefs size")
for n in (90, 91):
    b = rd("c%02d.prefs" % n)
    check(b[:4] == b"FORM" and b[8:12] == b"PREF" and struct.unpack(">I", b[4:8])[0] == len(b) - 8, "prefs FORM %d" % n)
    p, seen = 12, []
    while p < len(b):
        t, ln = b[p:p + 4], struct.unpack(">I", b[p + 4:p + 8])[0]
        d = b[p + 8:p + 8 + ln]
        if t == b"PTRN":
            r0, r1, r2, r3, which, flags, rev, depth, dl = struct.unpack(WBP, d[:24])
            name = d[24:24 + dl]
            seen.append((which, flags, name))
            check(rev == 0 and 24 + dl == ln, "PTRN layout %d" % n)
        elif t == b"PRHD":
            check(d == b"\0" * 6, "PRHD %d" % n)
        seen and None
        p += 8 + ln + (ln & 1)
    root = [s for s in seen if s[0] == 0]
    check(len(root) == 1 and root[0][2] == b"SYS:Prefs/Presets/WBFit/backdrop-640x256.ilbm\0" and root[0][1] & 1 == 0, "prefs ROOT %d" % n)
    if n == 91:
        check([s[0] for s in seen] == [0, 1, 2] and seen[1][2] == b"Work:a.iff\0" and seen[2][1] == 0x0C00, "prefs kept DRAWER/SCREEN")

print("check.py: %d checks, %d failed" % (checks, fails))
sys.exit(1 if fails else 0)
