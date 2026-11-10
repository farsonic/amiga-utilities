/*
 * wfcore.c - WBFit's work without AmigaOS (wfcore.h).
 * Copyright (C) 2026 the WBFit author.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>
#include "wfcore.h"

typedef unsigned long long u64;
typedef long long s64;

const char *wf_errstr(int e)
{
  switch (e) {
  case WF_OK: return "OK";
  case WF_ENOMEM: return "Not enough memory";
  case WF_EFAULT: return "A native call failed";
  case WF_EWRITE: return "Writing the file failed";
  case WF_EBIG: return "The picture or screen is too big";
  }
  return "Bad arguments";
}

/* ================================================================== the plain runner */
static wfk_u32 (*const kfn[WFK_COUNT])(void *) = {
#define WF__FN(n) n,
  WFK_LIST(WF__FN)
};
static int plain_range(WfRun *r, int id, void *p, wf_u32 from, wf_u32 to)
{
  WFK_RANGE_T *rg = (WFK_RANGE_T *)p;
  (void)r;
  while (from < to) {                     /* (on the 68k one call; the host tests stop often) */
    rg->from = from; rg->to = to;
    kfn[id](p);
    if (rg->to <= from || rg->to > to) return WF_EFAULT;
    from = rg->to;
  }
  return 0;
}
static wf_u32 plain_call(WfRun *r, int id, void *p, int *fault) { (void)r; *fault = 0; return kfn[id](p); }
void wf_run_plain(WfRun *r, void *(*alloc)(WfRun *, wf_u32), void (*fr)(WfRun *, void *, wf_u32), wf_u32 (*now_us)(WfRun *))
{
  r->range = plain_range; r->call = plain_call; r->alloc = alloc; r->free = fr; r->now_us = now_us;
}

wf_u32 wf_crc32(wf_u32 crc, const wf_u8 *p, wf_u32 n)
{
  static wf_u32 tab[256];
  wf_u32 c;
  if (!tab[1]) wfk_crc_table(tab);
  c = crc ^ 0xFFFFFFFFUL;
  while (n--) c = tab[(c ^ *p++) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFUL;
}

/* ================================================================== geometry */
static wf_u32 rdiv(u64 a, u64 b) { return (wf_u32)((a + b / 2) / b); }

int wf_place(WfPlace *pl, wf_u32 sw, wf_u32 sh, wf_u32 W, wf_u32 H, wf_u32 rx, wf_u32 ry, int mode)
{
  u64 a, b;
  memset(pl, 0, sizeof *pl);
  if (!sw || !sh || !W || !H || sw > WF_MAXDIM || sh > WF_MAXDIM || W > WF_MAXDIM || H > WF_MAXDIM) return WF_EBIG;
  if (!rx || !ry) rx = ry = 1;
  /* the screen's shape against the picture's, in physical units: W rx : H ry against sw : sh */
  a = (u64)W * rx * sh;
  b = (u64)H * ry * sw;
  switch (mode) {
  case WF_FILL:
  case WF_FIT:
    if ((a >= b) == (mode == WF_FILL)) {   /* the width decides */
      pl->dw = W;
      pl->dh = rdiv((u64)sh * W * rx, (u64)sw * ry);
    } else {
      pl->dh = H;
      pl->dw = rdiv((u64)sw * H * ry, (u64)sh * rx);
    }
    break;
  case WF_STRETCH: pl->dw = W; pl->dh = H; break;
  default: pl->dw = sw; pl->dh = sh; break;
  }
  if (!pl->dw) pl->dw = 1;
  if (!pl->dh) pl->dh = 1;
  if (pl->dw > WF_MAXDIM || pl->dh > WF_MAXDIM) return WF_EBIG;
  pl->px = pl->dw >= W ? -(wf_s32)((pl->dw - W) / 2) : (wf_s32)((W - pl->dw) / 2);
  pl->py = pl->dh >= H ? -(wf_s32)((pl->dh - H) / 2) : (wf_s32)((H - pl->dh) / 2);
  pl->vx0 = pl->px < 0 ? (wf_u32)-pl->px : 0;
  pl->vy0 = pl->py < 0 ? (wf_u32)-pl->py : 0;
  pl->ox = pl->px > 0 ? (wf_u32)pl->px : 0;
  pl->oy = pl->py > 0 ? (wf_u32)pl->py : 0;
  pl->vw = pl->dw - pl->vx0 < W - pl->ox ? pl->dw - pl->vx0 : W - pl->ox;
  pl->vh = pl->dh - pl->vy0 < H - pl->oy ? pl->dh - pl->vy0 : H - pl->oy;
  return WF_OK;
}

/* ================================================================== weights */
wf_u32 wf_taps(wf_u32 src, wf_u32 dst)
{
  wf_u32 k = src > dst ? (4 * src + dst - 1) / dst + 2 : 6;
  return (k + 1) & ~1U;
}

/* Catmull-Rom (a = -1/2) at |t| (Q12) in Q12 */
static wf_s32 cubic(wf_s32 t)
{
  wf_s32 t2 = (t * t) >> 12, t3 = (t2 * t) >> 12;
  if (t < 4096) return (3 * t3 - 5 * t2 + 8192) / 2;
  if (t < 8192) return (-t3 + 5 * t2 - 8 * t + 16384) / 2;
  return 0;
}

int wf_weights(wf_u32 src, wf_u32 dst, wf_u32 v0, wf_u32 n, wf_u32 *tab, wfk_s16 *wts, wf_u32 k)
{
  /* in units of 1 / (2 dst) source pixels: output i's centre is at (2i + 1) src, source pixel
     j's at (2j + 1) dst; t = their distance / den, den = 2 max(src, dst) (the kernel is
     widened by src / dst when shrinking) */
  s64 den = 2 * (s64)(src > dst ? src : dst);
  wf_u32 i, r = (wf_u32)(den / dst) + 2;
  int ident = 1;
  static wf_s32 raw[1024];             /* (not on the stack: the Amiga's is small) */
  for (i = 0; i < n; i++) {
    wf_u32 o = v0 + i;
    s64 c2 = (s64)(2 * (s64)o + 1) * src;
    s64 jc = c2 / (2 * (s64)dst), jlo = jc - r, jhi = jc + r, j;
    wf_s32 sum = 0, tot = 0, bestw = -0x7FFFFFFF;
    wf_u32 first, cnt, m, best = 0;
    wfk_s16 *w = wts + (size_t)i * k;
    if (jlo < 0) jlo = 0;
    if (jhi > (s64)src - 1) jhi = (s64)src - 1;
    if (jhi - jlo + 1 > 1024) return -1;
    for (j = jlo; j <= jhi; j++) {
      s64 d = (2 * j + 1) * (s64)dst - c2;
      s64 t;
      if (d < 0) d = -d;
      t = d * 4096 / den;
      raw[j - jlo] = t < 8192 ? cubic((wf_s32)t) : 0;
      sum += raw[j - jlo];
    }
    /* the taps without the zero ones at either end */
    first = 0; cnt = (wf_u32)(jhi - jlo + 1);
    while (cnt > 1 && !raw[first]) { first++; cnt--; }
    while (cnt > 1 && !raw[first + cnt - 1]) cnt--;
    if (cnt > k) return -1;
    if (sum <= 0) {                         /* (cannot happen; nearest pixel) */
      tab[i * 2] = (wf_u32)jc < src ? (wf_u32)jc : src - 1; tab[i * 2 + 1] = 1; w[0] = 1 << WFK_WBITS;
      for (m = 1; m < k; m++) w[m] = 0;
      ident = 0;
      continue;
    }
    for (m = 0; m < cnt; m++) {
      s64 v = (s64)raw[first + m] * (1 << WFK_WBITS);
      wf_s32 q = (wf_s32)(v >= 0 ? (v + sum / 2) / sum : -((-v + sum / 2) / sum));
      w[m] = (wfk_s16)q;
      tot += q;
      if (q > bestw) { bestw = q; best = m; }
    }
    w[best] = (wfk_s16)(w[best] + ((1 << WFK_WBITS) - tot));
    for (m = cnt; m < k; m++) w[m] = 0;
    tab[i * 2] = (wf_u32)jlo + first;
    tab[i * 2 + 1] = cnt;
    if (cnt != 1 || tab[i * 2] != (i ? tab[0] + i : tab[0])) ident = 0;
  }
  return ident;
}

/* ================================================================== quantiser */
typedef struct { int lo[3], hi[3]; u64 n; } Box;
#define HI(r, g, b) (((r) << 10) | ((g) << 5) | (b))

static void box_shrink(const wf_u32 *h, Box *bx)
{
  int r, g, b, lo[3] = { 31, 31, 31 }, hi[3] = { 0, 0, 0 };
  u64 n = 0;
  for (r = bx->lo[0]; r <= bx->hi[0]; r++)
    for (g = bx->lo[1]; g <= bx->hi[1]; g++)
      for (b = bx->lo[2]; b <= bx->hi[2]; b++) {
        wf_u32 c = h[HI(r, g, b)];
        if (!c) continue;
        n += c;
        if (r < lo[0]) lo[0] = r;
        if (r > hi[0]) hi[0] = r;
        if (g < lo[1]) lo[1] = g;
        if (g > hi[1]) hi[1] = g;
        if (b < lo[2]) lo[2] = b;
        if (b > hi[2]) hi[2] = b;
      }
  bx->n = n;
  if (n) { memcpy(bx->lo, lo, sizeof lo); memcpy(bx->hi, hi, sizeof hi); }
}

int wf_mediancut(const wf_u32 *h, int ncol, wf_u8 *pal)
{
  static Box bx[256];
  int nb = 1, i, k;
  if (ncol < 1) ncol = 1;
  if (ncol > 256) ncol = 256;
  for (k = 0; k < 3; k++) { bx[0].lo[k] = 0; bx[0].hi[k] = 31; }
  box_shrink(h, &bx[0]);
  if (!bx[0].n) { pal[0] = pal[1] = pal[2] = 0; return 1; }
  while (nb < ncol) {
    /* the most populated box that can be split, along its longest side (weighted 3:4:2) */
    int sel = -1, ax = 0, r, g, b, cut;
    u64 bestn = 0, marg[32], acc;
    for (i = 0; i < nb; i++) {
      int s = 0;
      for (k = 0; k < 3; k++) s |= bx[i].hi[k] > bx[i].lo[k];
      if (s && bx[i].n > bestn) { bestn = bx[i].n; sel = i; }
    }
    if (sel < 0) break;
    {
      int best = -1;
      static const int wt[3] = { 3, 4, 2 };
      for (k = 0; k < 3; k++) {
        int len = (bx[sel].hi[k] - bx[sel].lo[k]) * wt[k];
        if (len > best) { best = len; ax = k; }
      }
    }
    for (i = 0; i < 32; i++) marg[i] = 0;
    for (r = bx[sel].lo[0]; r <= bx[sel].hi[0]; r++)
      for (g = bx[sel].lo[1]; g <= bx[sel].hi[1]; g++)
        for (b = bx[sel].lo[2]; b <= bx[sel].hi[2]; b++) {
          int v = ax == 0 ? r : ax == 1 ? g : b;
          marg[v] += h[HI(r, g, b)];
        }
    acc = 0;
    for (cut = bx[sel].lo[ax]; cut < bx[sel].hi[ax] - 1; cut++) {
      acc += marg[cut];
      if (acc * 2 >= bx[sel].n) break;
    }
    bx[nb] = bx[sel];
    bx[sel].hi[ax] = cut;
    bx[nb].lo[ax] = cut + 1;
    box_shrink(h, &bx[sel]);
    box_shrink(h, &bx[nb]);
    nb++;
  }
  for (i = 0; i < nb; i++) {
    u64 s[3] = { 0, 0, 0 }, n = 0;
    int r, g, b;
    for (r = bx[i].lo[0]; r <= bx[i].hi[0]; r++)
      for (g = bx[i].lo[1]; g <= bx[i].hi[1]; g++)
        for (b = bx[i].lo[2]; b <= bx[i].hi[2]; b++) {
          wf_u32 c = h[HI(r, g, b)];
          n += c;
          s[0] += (u64)c * (u64)(r << 3 | 4); s[1] += (u64)c * (u64)(g << 3 | 4); s[2] += (u64)c * (u64)(b << 3 | 4);
        }
    for (k = 0; k < 3; k++) pal[i * 3 + k] = n ? (wf_u8)((s[k] + n / 2) / n) : 0;
  }
  return nb;
}

wf_u32 wf_edge(const wf_u8 *src, wf_u32 w, wf_u32 h, wf_u32 stride)
{
  u64 s[3] = { 0, 0, 0 }, n = 0;
  wf_u32 x, y, k;
  for (y = 0; y < h; y++) {
    const wf_u8 *row = src + (size_t)y * stride;
    for (x = 0; x < w; x++) {
      if (y && y + 1 < h && x && x + 1 < w) { x = w - 2; continue; }   /* (the border only) */
      for (k = 0; k < 3; k++) s[k] += row[x * 3 + k];
      n++;
    }
  }
  if (!n) return 0;
  return (wf_u32)((s[0] + n / 2) / n) << 16 | (wf_u32)((s[1] + n / 2) / n) << 8 | (wf_u32)((s[2] + n / 2) / n);
}

/* ================================================================== the pipeline */
static wf_u32 now(WfRun *r) { return r->now_us ? r->now_us(r) : 0; }
static void *get(WfRun *r, wf_u32 n) { return r->alloc(r, n ? n : 4); }
static void put(WfRun *r, void *p, wf_u32 n) { if (p) r->free(r, p, n ? n : 4); }
/* (n * m, or 0xFFFFFFFF when it overflows 2 GB) */
static wf_u32 mul(wf_u32 a, wf_u32 b) { u64 v = (u64)a * b; return v > 0x7FFFFFF0UL ? 0xFFFFFFFFUL : (wf_u32)v; }

/* the old picture.datatype's indices to RGB (into j->rgb2, which becomes the source) */
static int st_convert(WfRun *r, WfJob *j, wf_u8 *rgb)
{
  struct WfkLut p;
  wf_u8 *pal = get(r, 768);              /* (the caller's palette may be memory native code can't see) */
  int e;
  if (!pal) return WF_ENOMEM;
  memcpy(pal, j->ipal, 768);
  memset(&p, 0, sizeof p);
  p.in = WFK_SETP(j->idx); p.istride = j->sstride; p.w = j->sw;
  p.out = WFK_SETP(rgb); p.pal = WFK_SETP(pal); p.ham = j->ham;
  e = r->range(r, WFKI_wfk_lut, &p, 0, j->sh);
  put(r, pal, 768);
  return e;
}

static int st_scale(WfRun *r, WfJob *j, const wf_u8 *src, wf_u32 sstride)
{
  WfPlace *pl = &j->pl;
  wf_u32 W = j->W, H = j->H, kx = j->kx, ky = j->ky, y0 = 0, y1 = 0, i, ib = 0;
  wf_u32 *tx = 0, *ty = 0, ntx = mul(pl->vw, 8), nty = mul(pl->vh, 8), nwx = mul(mul(pl->vw, kx), 2), nwy = mul(mul(pl->vh, ky), 2);
  wfk_s16 *wx = 0, *wy = 0;
  wf_u8 *mid = 0;
  int idx, idy, e = WF_ENOMEM;
  if (ntx == 0xFFFFFFFFUL || nty == 0xFFFFFFFFUL || nwx == 0xFFFFFFFFUL || nwy == 0xFFFFFFFFUL) return WF_EBIG;
  /* the borders: the background (FIT, CENTER with a small picture) */
  if (pl->vw < W || pl->vh < H) {
    struct WfkFill f;
    memset(&f, 0, sizeof f);
    f.out = WFK_SETP(j->rgb); f.stride = W * 3; f.x0 = 0; f.x1 = W; f.rgb = j->bgused;
    if ((e = r->range(r, WFKI_wfk_fill, &f, 0, H))) return e;
  }
  e = WF_ENOMEM;
  if (!(tx = get(r, ntx)) || !(ty = get(r, nty)) || !(wx = get(r, nwx)) || !(wy = get(r, nwy))) goto done;
  idx = wf_weights(j->sw, pl->dw, pl->vx0, pl->vw, tx, wx, kx);
  idy = wf_weights(j->sh, pl->dh, pl->vy0, pl->vh, ty, wy, ky);
  e = WF_EBIG;
  if (idx < 0 || idy < 0) goto done;
  /* the source rows the shown rows need */
  y0 = ty[0]; y1 = ty[0] + ty[1];
  for (i = 0; i < pl->vh; i++) {
    if (ty[i * 2] < y0) y0 = ty[i * 2];
    if (ty[i * 2] + ty[i * 2 + 1] > y1) y1 = ty[i * 2] + ty[i * 2 + 1];
  }
  for (i = 0; i < pl->vh; i++) ty[i * 2] -= y0;
  ib = mul(mul(y1 - y0, pl->vw), 3);
  if (ib == 0xFFFFFFFFUL) goto done;
  e = WF_ENOMEM;
  if (!(mid = get(r, ib))) goto done;
  {
    struct WfkH h;
    memset(&h, 0, sizeof h);
    h.src = WFK_SETP(src); h.sstride = sstride; h.out = WFK_SETP(mid); h.ostride = pl->vw * 3; h.ow = pl->vw; h.y0 = y0;
    h.tab = WFK_SETP(tx); h.wts = WFK_SETP(wx); h.k = kx; h.ident = (wf_u32)idx;
    if ((e = r->range(r, WFKI_wfk_hpass, &h, y0, y1))) goto done;
  }
  {
    struct WfkV v;
    memset(&v, 0, sizeof v);
    v.in = WFK_SETP(mid); v.istride = pl->vw * 3; v.out = WFK_SETP(j->rgb + ((size_t)pl->oy * W + pl->ox) * 3); v.ostride = W * 3;
    v.ow = pl->vw; v.tab = WFK_SETP(ty); v.wts = WFK_SETP(wy); v.k = ky; v.ident = (wf_u32)idy;
    e = r->range(r, WFKI_wfk_vpass, &v, 0, pl->vh);
  }
done:
  put(r, mid, ib); put(r, wy, nwy); put(r, wx, nwx); put(r, ty, nty); put(r, tx, ntx);
  return e;
}

static int st_dither(WfRun *r, WfJob *j)
{
  wf_u32 W = j->W, H = j->H, ne = (W + 2) * 3 * 4;
  wf_u8 *pal = 0;
  wf_u32 *hist = 0;
  wfk_u16 *cache = 0;
  wfk_s32 *e0 = 0, *e1 = 0;
  int e = WF_ENOMEM, ncol = 1 << j->depth;
  if (!(pal = get(r, 768)) || !(cache = get(r, 65536)) || !(e0 = get(r, ne)) || !(e1 = get(r, ne))) goto done;
  if (j->pal && j->npal > 0) {
    j->ncol = j->npal > ncol ? ncol : j->npal;
    memcpy(j->opal, j->pal, (size_t)j->ncol * 3);
  } else {
    struct WfkHist hp;
    if (!(hist = get(r, 32768 * 4))) goto done;
    memset(&hp, 0, sizeof hp);
    hp.px = WFK_SETP(j->rgb); hp.stride = W * 3; hp.w = W; hp.hist = WFK_SETP(hist);
    if ((e = r->range(r, WFKI_wfk_hist, &hp, 0, H))) goto done;
    j->ncol = wf_mediancut(hist, ncol, j->opal);
  }
  memcpy(pal, j->opal, (size_t)j->ncol * 3);
  memset(cache, 0xFF, 65536);
  {
    struct WfkDither d;
    memset(&d, 0, sizeof d);
    d.px = WFK_SETP(j->rgb); d.stride = W * 3; d.w = W; d.out = WFK_SETP(j->ind); d.pal = WFK_SETP(pal);
    d.npal = (wf_u32)j->ncol; d.cache = WFK_SETP(cache); d.fs = (wf_u32)j->dither; d.e0 = WFK_SETP(e0); d.e1 = WFK_SETP(e1);
    e = r->range(r, WFKI_wfk_dither, &d, 0, H);
  }
done:
  put(r, hist, 32768 * 4); put(r, e1, ne); put(r, e0, ne); put(r, cache, 65536); put(r, pal, 768);
  return e;
}

/* ---- the encoders: the file in j->out (headers), j->enc (the big part) */
static wf_u8 *be32(wf_u8 *p, wf_u32 v) { p[0] = (wf_u8)(v >> 24); p[1] = (wf_u8)(v >> 16); p[2] = (wf_u8)(v >> 8); p[3] = (wf_u8)v; return p + 4; }
static wf_u8 *be16(wf_u8 *p, wf_u32 v) { p[0] = (wf_u8)(v >> 8); p[1] = (wf_u8)v; return p + 2; }
#define IDATMAX 32768

static int st_png(WfRun *r, WfJob *j)
{
  wf_u32 W = j->W, H = j->H, bpp = 3, rb = W * bpp, fn = mul(rb + 1, H), cap, i, ntab = 256 * 4;
  wf_u8 *filt = 0;
  struct WfkDefState *st = 0;
  wf_u32 *tab = 0;
  int e = WF_ENOMEM, fault = 0;
  if (fn == 0xFFFFFFFFUL) return WF_EBIG;
  cap = WFK_DEFLATE_BOUND(fn);
  put(r, j->crcs, j->ncrc * 4); j->crcs = 0; j->ncrc = 0;     /* (a second try after a native failure) */
  if (!(filt = get(r, fn)) || !(st = get(r, sizeof *st)) || !(tab = get(r, ntab))) goto done;
  if (!j->enc || j->enccap < cap) {
    put(r, j->enc, j->enccap);
    j->enccap = 0;
    if (!(j->enc = get(r, cap))) goto done;
    j->enccap = cap;
  }
  {
    struct WfkFilt f;
    memset(&f, 0, sizeof f);
    f.in = WFK_SETP(j->rgb); f.stride = rb; f.w = W; f.bpp = bpp; f.out = WFK_SETP(filt); f.adaptive = (wf_u32)j->pngadaptive;
    if ((e = r->range(r, WFKI_wfk_pngfilt, &f, 0, H))) goto done;
  }
  {
    struct WfkDefl d;
    wf_u32 res, guard = 0;
    memset(&d, 0, sizeof d);
    d.st = WFK_SETP(st); d.in = WFK_SETP(filt); d.n = fn; d.out = WFK_SETP(j->enc); d.ocap = cap;
    d.chain = 16; d.nice = 32; d.good = 8;
    do {
      res = r->call(r, WFKI_wfk_deflate, &d, &fault);
      if (fault) { e = WF_EFAULT; goto done; }
    } while (res == WFK_MORE && ++guard < 0x7FFFFFFFUL);
    e = WF_EBIG;
    if (res != WFK_DONE) goto done;
    j->encn = d.olen;
  }
  /* CRCs of the IDAT chunks: "IDAT" then the data */
  wfk_crc_table(tab);
  j->ncrc = (j->encn + IDATMAX - 1) / IDATMAX;
  e = WF_ENOMEM;
  if (!(j->crcs = get(r, j->ncrc * 4))) goto done;
  for (i = 0; i < j->ncrc; i++) {
    struct WfkCrc c;
    wf_u32 a = i * IDATMAX, b = a + IDATMAX < j->encn ? a + IDATMAX : j->encn;
    memset(&c, 0, sizeof c);
    c.p = WFK_SETP(j->enc); c.tab = WFK_SETP(tab); c.crc = wf_crc32(0, (const wf_u8 *)"IDAT", 4);
    if ((e = r->range(r, WFKI_wfk_crc, &c, a, b))) goto done;
    j->crcs[i] = c.crc;
  }
  e = WF_OK;
done:
  put(r, tab, ntab); put(r, st, sizeof *st); put(r, filt, fn);
  return e;
}

static int st_ilbm(WfRun *r, WfJob *j)
{
  wf_u32 W = j->W, H = j->H, depth = j->format == WF_ILBM24 ? 24 : (wf_u32)j->depth, rb = ((W + 15) >> 4) << 1;
  wf_u32 cap = mul(mul(H, depth), rb + (rb + 127) / 128);
  wf_u8 *plane = 0;
  int e = WF_ENOMEM;
  if (cap == 0xFFFFFFFFUL) return WF_EBIG;
  if (!(plane = get(r, rb))) goto done;
  if (!j->enc || j->enccap < cap) {
    put(r, j->enc, j->enccap);
    j->enccap = 0;
    if (!(j->enc = get(r, cap))) goto done;
    j->enccap = cap;
  }
  {
    struct WfkIlbm p;
    memset(&p, 0, sizeof p);
    p.in = WFK_SETP(depth == 24 ? j->rgb : j->ind); p.stride = depth == 24 ? W * 3 : W; p.w = W; p.depth = depth;
    p.out = WFK_SETP(j->enc); p.ocap = cap; p.pack = 1; p.plane = WFK_SETP(plane);
    if ((e = r->range(r, WFKI_wfk_ilbm, &p, 0, H))) goto done;
    e = WF_EBIG;
    if (p.full) goto done;
    j->encn = p.opos;
  }
  e = WF_OK;
done:
  put(r, plane, rb);
  return e;
}

/* the file's bytes, in pieces */
static int emit(WfJob *j, int (*write)(void *, const void *, wf_u32), void *wctx)
{
  static wf_u8 hdr[1024];
  wf_u8 *p = hdr;
  wf_u32 W = j->W, H = j->H;
  int e;
  j->filebytes = 0;
#define OUT(ptr, n) do { wf_u32 n_ = (n); if (write && (e = write(wctx, (ptr), n_)) != 0) return e; j->filebytes += n_; } while (0)
  if (j->format == WF_PNG) {
    static const wf_u8 sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    static const wf_u8 iend[12] = { 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82 };
    wf_u32 i;
    memcpy(p, sig, 8); p += 8;
    p = be32(p, 13); memcpy(p, "IHDR", 4); p += 4;
    p = be32(p, W); p = be32(p, H); *p++ = 8; *p++ = 2; *p++ = 0; *p++ = 0; *p++ = 0;
    p = be32(p, wf_crc32(0, hdr + 12, 17));
    OUT(hdr, (wf_u32)(p - hdr));
    for (i = 0; i < j->ncrc; i++) {
      wf_u32 a = i * IDATMAX, n = a + IDATMAX < j->encn ? IDATMAX : j->encn - a;
      wf_u8 c[8];
      be32(c, n); memcpy(c + 4, "IDAT", 4);
      OUT(c, 8);
      OUT(j->enc + a, n);
      be32(c, j->crcs[i]);
      OUT(c, 4);
    }
    OUT(iend, 12);
  } else {
    wf_u32 depth = j->format == WF_ILBM24 ? 24 : (wf_u32)j->depth, ncm = depth == 24 ? 0 : 3U << depth, rx = j->rx, ry = j->ry;
    wf_u32 cmapc = ncm ? 8 + ((ncm + 1) & ~1U) : 0, body = 8 + ((j->encn + 1) & ~1U), form = 4 + 28 + cmapc + body;
    if (!rx || !ry) rx = ry = 1;
    while (rx > 255 || ry > 255) { rx = (rx + 1) / 2; ry = (ry + 1) / 2; }
    memcpy(p, "FORM", 4); p = be32(p + 4, form); memcpy(p, "ILBM", 4); p += 4;
    memcpy(p, "BMHD", 4); p = be32(p + 4, 20);
    p = be16(p, W); p = be16(p, H); p = be16(p, 0); p = be16(p, 0);
    *p++ = (wf_u8)depth; *p++ = 0; *p++ = 1; *p++ = 0;     /* planes, no mask, ByteRun1 */
    p = be16(p, 0); *p++ = (wf_u8)rx; *p++ = (wf_u8)ry;    /* transparent colour, pixel aspect */
    p = be16(p, W); p = be16(p, H);
    if (ncm) {
      memcpy(p, "CMAP", 4); p = be32(p + 4, ncm);
      memset(p, 0, (ncm + 1) & ~1U);
      memcpy(p, j->opal, (size_t)j->ncol * 3);
      p += (ncm + 1) & ~1U;
    }
    memcpy(p, "BODY", 4); p = be32(p + 4, j->encn);
    OUT(hdr, (wf_u32)(p - hdr));
    OUT(j->enc, j->encn);
    if (j->encn & 1) OUT("", 1);
  }
#undef OUT
  return 0;
}

void wf_job_free(WfRun *r, WfJob *j)
{
  put(r, j->crcs, j->ncrc * 4); j->crcs = 0; j->ncrc = 0;
  put(r, j->enc, j->enccap); j->enc = 0; j->enccap = 0;
  put(r, j->ind, j->indcap); j->ind = 0; j->indcap = 0;
  put(r, j->rgb, j->outcap); j->rgb = 0; j->outcap = 0;
}

/* run a stage; after a native failure the runner is on the 68k code: once more from the start */
#define STAGE(s, call) do { \
    wf_u32 t0_ = now(r); \
    e = (call); \
    if (e == WF_EFAULT) { j->faults++; e = (call); } \
    j->us[s] = now(r) - t0_; \
    if (e) goto fail; \
  } while (0)

int wf_fit(WfRun *r, WfJob *j, int (*write)(void *, const void *, wf_u32), void *wctx)
{
  wf_u8 *conv = 0;
  wf_u32 convn = 0, rgbn;
  const wf_u8 *src = j->src;
  wf_u32 sstride = j->sstride;
  int e, k;
  for (k = 0; k < WF_NST; k++) { j->us[k] = 0; j->sum[k] = 0; }
  j->faults = 0; j->ncol = 0; j->encn = 0; j->filebytes = 0;
  if (j->format == WF_ILBM && (j->depth < 1 || j->depth > 8)) return WF_EARG;
  if ((e = wf_place(&j->pl, j->sw, j->sh, j->W, j->H, j->rx, j->ry, j->mode))) return e;
  j->kx = wf_taps(j->sw, j->pl.dw);
  j->ky = wf_taps(j->sh, j->pl.dh);
  if (j->kx > 1024 || j->ky > 1024) return WF_EBIG;
  rgbn = mul(mul(j->W, j->H), 3);
  if (rgbn == 0xFFFFFFFFUL) return WF_EBIG;
  if (!j->rgb || j->outcap < rgbn) {
    put(r, j->rgb, j->outcap); j->outcap = 0;
    if (!(j->rgb = get(r, rgbn))) return WF_ENOMEM;
    j->outcap = rgbn;
  }
  /* the old picture.datatype's indices: to RGB first */
  if (!src) {
    convn = mul(mul(j->sw, j->sh), 3);
    if (convn == 0xFFFFFFFFUL) return WF_EBIG;
    if (!(conv = get(r, convn))) return WF_ENOMEM;
    STAGE(WF_ST_CONVERT, st_convert(r, j, conv));
    j->sum[WF_ST_CONVERT] = wf_crc32(0, conv, convn);
    src = conv; sstride = j->sw * 3;
  }
  j->bgused = j->bg;
  if (j->bgauto) j->bgused = wf_edge(src, j->sw, j->sh, sstride);
  STAGE(WF_ST_SCALE, st_scale(r, j, src, sstride));
  j->sum[WF_ST_SCALE] = wf_crc32(0, j->rgb, rgbn);
  put(r, conv, convn); conv = 0;
  if (j->format == WF_ILBM) {
    wf_u32 n = j->W * j->H;
    if (!j->ind || j->indcap < n) {
      put(r, j->ind, j->indcap); j->indcap = 0;
      e = WF_ENOMEM;
      if (!(j->ind = get(r, n))) goto fail;
      j->indcap = n;
    }
    STAGE(WF_ST_DITHER, st_dither(r, j));
    j->sum[WF_ST_DITHER] = wf_crc32(wf_crc32(0, j->ind, n), j->opal, (wf_u32)j->ncol * 3);
  }
  put(r, j->crcs, j->ncrc * 4); j->crcs = 0; j->ncrc = 0;
  STAGE(WF_ST_ENCODE, j->format == WF_PNG ? st_png(r, j) : st_ilbm(r, j));
  j->sum[WF_ST_ENCODE] = wf_crc32(0, j->enc, j->encn);
  return emit(j, write, wctx);
fail:
  put(r, conv, convn);
  return e;
}
