/*
 * wfk.c - WBFit's kernels (wfk.h). Plain C and integer arithmetic only, no library calls but
 * memcpy/memset, so it builds unchanged as the 68k program's code, on the host and as AArch64
 * NativeCall kernels (nc/wfk_nc.c includes this file, with NEON for the vertical pass).
 *
 * The deflate encoder is Folio's (core/filters.c: hash chains, the smallest of dynamic, fixed
 * and stored blocks), made resumable: all its state is in struct WfkDefState and it stops only
 * between two symbols, so where it stops never changes the stream.
 *
 * Copyright (C) 2026 the WBFit author.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define PX_KERNEL_FILE
#include "wfk.h"

#if NC_NATIVE
/* (not memcpy: the SDK's is a byte loop behind a call; this one the loop vectorizer widens) */
static void cpy(unsigned char *d, const unsigned char *s, size_t n) { while (n--) *d++ = *s++; }
#define CPY(d, s, n) cpy((unsigned char *)(d), (const unsigned char *)(s), (n))
#else
#define CPY(d, s, n) memcpy((d), (s), (n))
#endif

/* the clock (CLK_STOP), q14(), px_nearest(): src/lib/pxbase.h */
WFK_DEFINE_BUDGET

typedef wfk_u8 u8;
typedef wfk_u16 u16;
typedef wfk_s16 s16;
typedef wfk_u32 u32;
typedef wfk_s32 s32;

/* ================================================================== resampling */
WFK_KERNEL(wfk_hpass)
{
  struct WfkH *p = (struct WfkH *)params;
  const u32 *tab = WFK_P(const u32, p->tab);
  const s16 *wts = WFK_P(const s16, p->wts);
  u32 y, x, ow = p->ow, k = p->k;
  Clk ck;
  clk_start(&ck);
  for (y = p->range.from; y < p->range.to; y++) {
    const u8 *s = WFK_P(const u8, p->src) + (size_t)y * p->sstride;
    u8 *o = WFK_P(u8, p->out) + (size_t)(y - p->y0) * p->ostride;
    CLK_STOP(&ck, p, y);
    if (p->ident) {
      CPY(o, s + (size_t)tab[0] * 3, (size_t)ow * 3);
      continue;
    }
    for (x = 0; x < ow; x++, o += 3) {
      const u8 *q = s + (size_t)tab[x * 2] * 3;
      const s16 *w = wts + (size_t)x * k;
      u32 n = tab[x * 2 + 1], j;
      s32 r = 0, g = 0, b = 0;
      for (j = 0; j < n; j++, q += 3) {
        s32 c = w[j];
        r += c * q[0]; g += c * q[1]; b += c * q[2];
      }
      o[0] = q14(r); o[1] = q14(g); o[2] = q14(b);
    }
  }
  return 0;
}

#ifdef WFK_VROW
static u32 WFK_VROW(const u8 *in, u32 istride, u32 n, const s16 *w, u8 *o, u32 nb);
#endif

WFK_KERNEL(wfk_vpass)
{
  struct WfkV *p = (struct WfkV *)params;
  const u32 *tab = WFK_P(const u32, p->tab);
  const s16 *wts = WFK_P(const s16, p->wts);
  u32 r, x, nb = p->ow * 3, k = p->k;
  Clk ck;
  clk_start(&ck);
  for (r = p->range.from; r < p->range.to; r++) {
    const u8 *in = WFK_P(const u8, p->in) + (size_t)tab[r * 2] * p->istride;
    const s16 *w = wts + (size_t)r * k;
    u8 *o = WFK_P(u8, p->out) + (size_t)r * p->ostride;
    u32 n = tab[r * 2 + 1], j;
    CLK_STOP(&ck, p, r);
    if (p->ident) { CPY(o, in, nb); continue; }
    x = 0;
#ifdef WFK_VROW
    x = WFK_VROW(in, p->istride, n, w, o, nb);
#endif
    for (; x < nb; x++) {
      const u8 *q = in + x;
      s32 v = 0;
#if NC_NATIVE
      /* GCC 14.2 -O3 for big-endian AArch64 vectorizes this strided widening sum wrongly (found
         by nc/tests: large weights gave 0); the row's bulk is NEON (WFK_VROW) anyway */
#pragma GCC novector
#endif
      for (j = 0; j < n; j++, q += p->istride) v += w[j] * *q;
      o[x] = q14(v);
    }
  }
  return 0;
}

/* ================================================================== fill, colour lookup */
WFK_KERNEL(wfk_fill)
{
  struct WfkFill *p = (struct WfkFill *)params;
  u32 y, x;
  u8 c0 = (u8)(p->rgb >> 16), c1 = (u8)(p->rgb >> 8), c2 = (u8)p->rgb;
  Clk ck;
  clk_start(&ck);
  for (y = p->range.from; y < p->range.to; y++) {
    u8 *o = WFK_P(u8, p->out) + (size_t)y * p->stride + (size_t)p->x0 * 3;
    CLK_STOP(&ck, p, y);
    for (x = p->x0; x < p->x1; x++, o += 3) { o[0] = c0; o[1] = c1; o[2] = c2; }
  }
  return 0;
}

WFK_KERNEL(wfk_lut)
{
  struct WfkLut *p = (struct WfkLut *)params;
  const u8 *pal = WFK_P(const u8, p->pal);
  u32 y, x, w = p->w;
  int ham = (int)p->ham, mask = ham ? (1 << ham) - 1 : 0;
  Clk ck;
  clk_start(&ck);
  for (y = p->range.from; y < p->range.to; y++) {
    const u8 *s = WFK_P(const u8, p->in) + (size_t)y * p->istride;
    u8 *o = WFK_P(u8, p->out) + (size_t)y * w * 3;
    int r = pal[0], g = pal[1], b = pal[2];
    CLK_STOP(&ck, p, y);
    if (!ham) {
      for (x = 0; x < w; x++, o += 3) { const u8 *c = pal + s[x] * 3; o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; }
      continue;
    }
    for (x = 0; x < w; x++, o += 3) {
      int v = s[x], ctl = v >> ham, d = v & mask, dv = ham == 4 ? d * 17 : (d << 2) | (d >> 4);
      if (ctl == 0) { r = pal[d * 3]; g = pal[d * 3 + 1]; b = pal[d * 3 + 2]; }
      else if (ctl == 1) b = dv;
      else if (ctl == 2) r = dv;
      else g = dv;
      o[0] = (u8)r; o[1] = (u8)g; o[2] = (u8)b;
    }
  }
  return 0;
}

/* ================================================================== quantising */
WFK_KERNEL(wfk_hist)
{
  struct WfkHist *p = (struct WfkHist *)params;
  u32 *h = WFK_P(u32, p->hist), y, x;
  Clk ck;
  clk_start(&ck);
  for (y = p->range.from; y < p->range.to; y++) {
    const u8 *s = WFK_P(const u8, p->px) + (size_t)y * p->stride;
    CLK_STOP(&ck, p, y);
    for (x = 0; x < p->w; x++, s += 3) {
      u32 key = ((u32)(s[0] >> 3) << 10) | ((u32)(s[1] >> 3) << 5) | (u32)(s[2] >> 3);
      if (h[key] != 0xFFFFFFFFUL) h[key]++;
    }
  }
  return 0;
}

static u32 nearest(const struct WfkDither *p, u32 key) { return px_nearest(WFK_P(u16, p->cache), WFK_P(const u8, p->pal), p->npal, key); }
#define KEY PX_KEY
#define clampi px_clampi
#define e16 px_e16

WFK_KERNEL(wfk_dither)
{
  struct WfkDither *p = (struct WfkDither *)params;
  const u8 *pal = WFK_P(const u8, p->pal);
  u32 y, x, k, w = p->w;
  Clk ck;
  clk_start(&ck);
  for (y = p->range.from; y < p->range.to; y++) {
    const u8 *s = WFK_P(const u8, p->px) + (size_t)y * p->stride;
    u8 *o = WFK_P(u8, p->out) + (size_t)y * w;
    CLK_STOP(&ck, p, y);
    if (!p->fs) {
      for (x = 0; x < w; x++, s += 3) o[x] = (u8)nearest(p, KEY(s[0], s[1], s[2]));
      continue;
    }
    {
      s32 *cur = WFK_P(s32, y & 1 ? p->e1 : p->e0), *nxt = WFK_P(s32, y & 1 ? p->e0 : p->e1);
      for (k = 0; k < (w + 2) * 3; k++) nxt[k] = 0;
      for (x = 0; x < w; x++, s += 3) {
        int v[3];
        u32 idx;
        const u8 *pc;
        for (k = 0; k < 3; k++) v[k] = clampi((s32)s[k] + e16(cur[(x + 1) * 3 + k]));
        idx = nearest(p, KEY(v[0], v[1], v[2]));
        pc = pal + idx * 3;
        o[x] = (u8)idx;
        for (k = 0; k < 3; k++) {
          s32 e = v[k] - pc[k];
          cur[(x + 2) * 3 + k] += e * 7;
          nxt[x * 3 + k] += e * 3;
          nxt[(x + 1) * 3 + k] += e * 5;
          nxt[(x + 2) * 3 + k] += e;
        }
      }
    }
  }
  return 0;
}

/* ================================================================== PNG filtering */
static int paeth(int a, int b, int c)
{
  int pp = a + b - c, pa = pp > a ? pp - a : a - pp, pb = pp > b ? pp - b : b - pp, pc = pp > c ? pp - c : c - pp;
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}
static u8 filt(int f, const u8 *cur, const u8 *prev, u32 x, u32 bpp)
{
  int a = x >= bpp ? cur[x - bpp] : 0, b = prev ? prev[x] : 0, c = prev && x >= bpp ? prev[x - bpp] : 0;
  switch (f) {
  case 1: return (u8)(cur[x] - a);
  case 2: return (u8)(cur[x] - b);
  case 3: return (u8)(cur[x] - ((a + b) >> 1));
  case 4: return (u8)(cur[x] - paeth(a, b, c));
  }
  return cur[x];
}
WFK_KERNEL(wfk_pngfilt)
{
  struct WfkFilt *p = (struct WfkFilt *)params;
  u32 y, x, bpp = p->bpp, rb = p->w * bpp;
  Clk ck;
  clk_start(&ck);
  for (y = p->range.from; y < p->range.to; y++) {
    const u8 *cur = WFK_P(const u8, p->in) + (size_t)y * p->stride, *prev = y ? cur - p->stride : 0;
    u8 *o = WFK_P(u8, p->out) + (size_t)y * (rb + 1);
    int f, best = 0;
    CLK_STOP(&ck, p, y);
    if (p->adaptive) {
      u32 bs = 0xFFFFFFFFUL;
      for (f = 0; f < 5; f++) {
        u32 s = 0;
        for (x = 0; x < rb && s < bs; x++) { u8 v = filt(f, cur, prev, x, bpp); s += v < 128 ? v : 256 - v; }
        if (s < bs) { bs = s; best = f; }
      }
    }
    o[0] = (u8)best;
    if (!best) CPY(o + 1, cur, rb);
    else for (x = 0; x < rb; x++) o[1 + x] = filt(best, cur, prev, x, bpp);
  }
  return 0;
}

/* ================================================================== deflate (from Folio) */
static const u16 len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const u8 len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const u16 dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                   4097, 6145, 8193, 12289, 16385, 24577 };
static const u8 dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const u8 clen_order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
#define WMASK (WFK_WSIZE - 1)

typedef struct { struct WfkDefState *s; struct WfkDefl *p; const u8 *in; u8 *out; } Def;

static u32 rev_bits(u32 v, int n) { u32 r = 0; while (n-- > 0) { r = (r << 1) | (v & 1); v >>= 1; } return r; }
static void def_byte(Def *d, int c)
{
  struct WfkDefState *s = d->s;
  if (s->opos < d->p->ocap) d->out[s->opos++] = (u8)c;
  else s->full = 1;
}
static void def_put(Def *d, u32 v, int k)
{
  struct WfkDefState *s = d->s;
  s->bits |= v << s->nbits;
  s->nbits += (u32)k;
  while (s->nbits >= 8) { def_byte(d, (int)(s->bits & 0xFF)); s->bits >>= 8; s->nbits -= 8; }
}
static void def_align(Def *d)
{
  if (d->s->nbits > 0) def_byte(d, (int)(d->s->bits & 0xFF));
  d->s->bits = 0; d->s->nbits = 0;
}
/* Huffman code lengths for n symbols of frequencies freq, none longer than limit */
static void def_lengths(struct WfkDefState *d, const u32 *freq, int n, int limit, u8 *len)
{
  int m, i, j, k, a, b, maxd, tries;
  s32 *srt = d->srt;
  u32 *f = d->tf, *w = d->w;
  for (i = 0; i < n; i++) f[i] = freq[i];
  for (tries = 0; tries < 40; tries++) {
    for (i = 0; i < n; i++) len[i] = 0;
    for (m = 0, i = 0; i < n; i++) if (f[i]) srt[m++] = i;
    if (m == 0) return;
    if (m == 1) { len[srt[0]] = 1; return; }
    for (k = m / 2; k > 0; k /= 2)            /* Shell sort by frequency (n <= 288) */
      for (i = k; i < m; i++) {
        s32 sv = srt[i];
        for (j = i; j >= k && f[srt[j - k]] > f[sv]; j -= k) srt[j] = srt[j - k];
        srt[j] = sv;
      }
    for (i = 0; i < m; i++) w[i] = f[srt[i]];
    for (i = 0, j = m, k = m; k < 2 * m - 1; k++) {
      a = (i < m && (j >= k || w[i] <= w[j])) ? i++ : j++;
      b = (i < m && (j >= k || w[i] <= w[j])) ? i++ : j++;
      w[k] = w[a] + w[b];
      d->par[a] = d->par[b] = k;
    }
    d->dep[2 * m - 2] = 0;
    for (maxd = 0, k = 2 * m - 3; k >= 0; k--) {
      d->dep[k] = d->dep[d->par[k]] + 1;
      if (k < m && d->dep[k] > maxd) maxd = d->dep[k];
    }
    if (maxd <= limit) {
      for (i = 0; i < m; i++) len[srt[i]] = (u8)d->dep[i];
      return;
    }
    for (i = 0; i < n; i++) if (f[i]) f[i] = (f[i] >> 1) | 1;   /* flatten and try again */
  }
}
static void def_codes(const u8 *len, int n, u16 *code)
{
  int count[16], next[16], i, c = 0;
  for (i = 0; i < 16; i++) count[i] = 0;
  for (i = 0; i < n; i++) count[len[i]]++;
  count[0] = 0;
  for (i = 1; i < 16; i++) { c = (c + count[i - 1]) << 1; next[i] = c; }
  for (i = 0; i < n; i++) code[i] = len[i] ? (u16)rev_bits((u32)next[len[i]]++, len[i]) : 0;
}
static int def_dcode(const struct WfkDefState *s, u32 dist) { return dist <= 256 ? s->dlo[dist - 1] : s->dhi[(dist - 1) >> 7]; }
static void def_symbols(Def *d, const u16 *lc, const u8 *ll, const u16 *dc, const u8 *dl)
{
  struct WfkDefState *s = d->s;
  u32 i;
  for (i = 0; i < s->ns; i++) {
    u32 v = s->sl[i], dist = s->sd[i];
    if (!dist) { def_put(d, lc[v], ll[v]); continue; }
    {
      int c = s->lcode[v], e = def_dcode(s, dist);
      def_put(d, lc[257 + c], ll[257 + c]);
      if (len_extra[c]) def_put(d, v - len_base[c], len_extra[c]);
      def_put(d, dc[e], dl[e]);
      if (dist_extra[e]) def_put(d, dist - dist_base[e], dist_extra[e]);
    }
  }
  def_put(d, lc[256], ll[256]);
}
/* the collected block as the smallest of dynamic, fixed and stored */
static void def_flush(Def *d, int final)
{
  struct WfkDefState *s = d->s;
  u8 *ll = s->ll, *fl = s->fl, *all = s->all, *cls = s->cls, *cle = s->cle, dl[32], fd[32], cl[19];
  u16 *lc = s->lc, dc[32], cc[19];
  u32 cf[19], dyn, fix, sto, raw = s->bend - s->bstart, extra = 0;
  int i, hlit, hdist, hclen, ncl = 0, used = 0;
  s->lf[256]++;
  for (i = 0; i < 30; i++) used += s->df[i] != 0;
  def_lengths(s, s->lf, 286, 15, ll);
  def_lengths(s, s->df, 30, 15, dl);
  if (!used) dl[0] = 1;
  for (hlit = 286; hlit > 257 && !ll[hlit - 1]; hlit--) ;
  for (hdist = 30; hdist > 1 && !dl[hdist - 1]; hdist--) ;
  for (i = 0; i < hlit; i++) all[i] = ll[i];
  for (i = 0; i < hdist; i++) all[hlit + i] = dl[i];
  for (i = 0; i < 19; i++) cf[i] = 0;
  for (i = 0; i < hlit + hdist; ) {
    int v = all[i], run = 1;
    while (i + run < hlit + hdist && all[i + run] == v) run++;
    if (v == 0 && run >= 3) {
      int r = run > 138 ? 138 : run;
      if (r >= 11) { cls[ncl] = 18; cle[ncl++] = (u8)(r - 11); }
      else { cls[ncl] = 17; cle[ncl++] = (u8)(r - 3); }
      i += r;
    } else if (v != 0 && run >= 4) {
      cls[ncl] = (u8)v; cle[ncl++] = 0;
      i++; run--;
      while (run >= 3) {
        int r = run > 6 ? 6 : run;
        cls[ncl] = 16; cle[ncl++] = (u8)(r - 3);
        i += r; run -= r;
      }
    } else {
      cls[ncl] = (u8)v; cle[ncl++] = 0; i++;
    }
  }
  for (i = 0; i < ncl; i++) cf[cls[i]]++;
  def_lengths(s, cf, 19, 7, cl);
  for (used = 0, i = 0; i < 19; i++) used += cl[i] != 0;
  if (used == 1) cl[cl[0] ? 1 : 0] = 1;      /* zlib rejects an incomplete code-length code */
  for (hclen = 19; hclen > 4 && !cl[clen_order[hclen - 1]]; hclen--) ;
  for (i = 0; i < 29; i++) extra += s->lf[257 + i] * len_extra[i];
  for (i = 0; i < 30; i++) extra += s->df[i] * dist_extra[i];
  dyn = 3 + 14 + 3 * (u32)hclen + extra;
  for (i = 0; i < 19; i++) dyn += cf[i] * cl[i];
  dyn += cf[16] * 2 + cf[17] * 3 + cf[18] * 7;
  for (i = 0; i < 286; i++) dyn += s->lf[i] * ll[i];
  for (i = 0; i < 30; i++) dyn += s->df[i] * dl[i];
  for (i = 0; i < 288; i++) fl[i] = (u8)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
  for (i = 0; i < 32; i++) fd[i] = 5;
  fix = 3 + extra;
  for (i = 0; i < 286; i++) fix += s->lf[i] * fl[i];
  for (i = 0; i < 30; i++) fix += s->df[i] * 5;
  sto = raw * 8 + (raw / 65535 + 1) * 42;
  if (sto < dyn && sto < fix) {
    const u8 *q = d->in + s->bstart;
    do {
      u32 k = raw > 65535 ? 65535 : raw;
      raw -= k;
      def_put(d, final && !raw ? 1 : 0, 1); def_put(d, 0, 2);
      def_align(d);
      def_byte(d, (int)(k & 0xFF)); def_byte(d, (int)(k >> 8));
      def_byte(d, (int)(~k & 0xFF)); def_byte(d, (int)((~k >> 8) & 0xFF));
      if (s->opos + k <= d->p->ocap && s->opos + k >= s->opos) { CPY(d->out + s->opos, q, k); s->opos += k; }
      else s->full = 1;
      q += k;
    } while (raw);
  } else if (fix <= dyn) {
    def_put(d, final ? 1 : 0, 1); def_put(d, 1, 2);
    def_codes(fl, 288, lc); def_codes(fd, 32, dc);
    def_symbols(d, lc, fl, dc, fd);
  } else {
    def_put(d, final ? 1 : 0, 1); def_put(d, 2, 2);
    def_put(d, (u32)(hlit - 257), 5); def_put(d, (u32)(hdist - 1), 5); def_put(d, (u32)(hclen - 4), 4);
    for (i = 0; i < hclen; i++) def_put(d, cl[clen_order[i]], 3);
    def_codes(cl, 19, cc);
    for (i = 0; i < ncl; i++) {
      def_put(d, cc[cls[i]], cl[cls[i]]);
      if (cls[i] == 16) def_put(d, cle[i], 2);
      else if (cls[i] == 17) def_put(d, cle[i], 3);
      else if (cls[i] == 18) def_put(d, cle[i], 7);
    }
    def_codes(ll, 286, lc); def_codes(dl, 30, dc);
    def_symbols(d, lc, ll, dc, dl);
  }
  s->ns = 0;
  for (i = 0; i < 288; i++) s->lf[i] = 0;
  for (i = 0; i < 32; i++) s->df[i] = 0;
  s->bstart = s->bend;
}
static void def_lit(Def *d, int c)
{
  struct WfkDefState *s = d->s;
  s->sl[s->ns] = (u16)c; s->sd[s->ns++] = 0;
  s->lf[c]++; s->bend++;
  if (s->ns == WFK_BLOCKSYM) def_flush(d, 0);
}
static void def_match(Def *d, int len, u32 dist)
{
  struct WfkDefState *s = d->s;
  s->sl[s->ns] = (u16)len; s->sd[s->ns++] = (u16)dist;
  s->lf[257 + s->lcode[len]]++; s->df[def_dcode(s, dist)]++;
  s->bend += (u32)len;
  if (s->ns == WFK_BLOCKSYM) def_flush(d, 0);
}
static u32 def_insert(Def *d, u32 pos)
{
  struct WfkDefState *s = d->s;
  const u8 *q = d->in + pos;
  u32 h = (((u32)q[0] << 10) ^ ((u32)q[1] << 5) ^ q[2]) & (WFK_HSIZE - 1), old = s->head[h];
  s->prev[pos & WMASK] = old;
  s->head[h] = pos + 1;
  return old;
}
static int def_longest(Def *d, u32 pos, u32 cand, u32 *dist)
{
  const u8 *q0 = d->in + pos;
  u32 avail = d->p->n - pos, chain = d->p->chain;
  int maxlen = avail > 258 ? 258 : (int)avail, best = 2, nice = (int)d->p->nice < maxlen ? (int)d->p->nice : maxlen;
  while (cand && chain-- > 0) {
    u32 c = cand - 1;
    const u8 *q = d->in + c;
    if (pos - c >= WFK_WSIZE) break;
    if (q[best] == q0[best] && q[0] == q0[0] && q[1] == q0[1]) {
      int l = 2;
      while (l < maxlen && q[l] == q0[l]) l++;
      if (l > best) {
        best = l; *dist = pos - c;
        if (l >= nice) break;
      }
    }
    cand = d->s->prev[c & WMASK];
  }
  return best;
}

WFK_KERNEL(wfk_deflate)
{
  struct WfkDefl *p = (struct WfkDefl *)params;
  struct WfkDefState *s = WFK_P(struct WfkDefState, p->st);
  Def d;
  u32 n = p->n, cnt = 0;
  Clk ck;
  int i, c;
  d.s = s; d.p = p; d.in = WFK_P(const u8, p->in); d.out = WFK_P(u8, p->out);
  clk_start(&ck);
  if (s->phase == 0) {
    for (c = 0; c < 29; c++)
      for (i = 0; i < (1 << len_extra[c]) && len_base[c] + i <= 258; i++) s->lcode[len_base[c] + i] = (u8)c;
    for (c = 0; c < 30; c++)
      for (i = 0; i < (1 << dist_extra[c]); i++) {
        int dd = dist_base[c] + i;
        if (dd <= 256) s->dlo[dd - 1] = (u8)c; else s->dhi[(dd - 1) >> 7] = (u8)c;
      }
    def_byte(&d, 0x78); def_byte(&d, 0x5E);        /* zlib: deflate, 32 KB window, "fast" */
    s->a = 1; s->b = 0;
    s->phase = 1;
  }
  if (s->phase == 1) {
    while (s->pos < n) {
      u32 pos = s->pos, cand, dist = 0;
      int len = 2;
      if (!(++cnt & 255) && clk_over(&ck)) return s->full ? WFK_FULL : WFK_MORE;
      if (n - pos >= 3 && (cand = def_insert(&d, pos)) != 0) len = def_longest(&d, pos, cand, &dist);
      if (len >= 3) {
        u32 end = pos + (u32)len, k;
        def_match(&d, len, dist);
        if (len <= (int)p->good) for (k = pos + 1; k < end && k + 3 <= n; k++) def_insert(&d, k);
        s->pos = end;
      } else {
        def_lit(&d, d.in[pos]);
        s->pos = pos + 1;
      }
    }
    def_flush(&d, 1);
    def_align(&d);
    s->phase = 2;
  }
  if (s->phase == 2) {
    int first = 1;
    while (s->apos < n) {
      u32 e = s->apos + 5552 < n ? s->apos + 5552 : n, k, a = s->a, b = s->b;
      if (!first && clk_over(&ck)) return s->full ? WFK_FULL : WFK_MORE;   /* (never before some progress) */
      first = 0;
      for (k = s->apos; k < e; k++) { a += d.in[k]; b += a; }
      s->a = a % 65521; s->b = b % 65521; s->apos = e;
    }
    def_byte(&d, (int)(s->b >> 8)); def_byte(&d, (int)(s->b & 0xFF)); def_byte(&d, (int)(s->a >> 8)); def_byte(&d, (int)(s->a & 0xFF));
    s->phase = 3;
  }
  p->olen = s->opos;
  return s->full ? WFK_FULL : WFK_DONE;
}

/* ================================================================== CRC-32 */
void wfk_crc_table(wfk_u32 *tab)
{
  u32 k, c, i;
  for (k = 0; k < 256; k++) {
    c = k;
    for (i = 0; i < 8; i++) c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
    tab[k] = c;
  }
}
WFK_KERNEL(wfk_crc)
{
  struct WfkCrc *p = (struct WfkCrc *)params;
  const u32 *tab = WFK_P(const u32, p->tab);
  const u8 *q = WFK_P(const u8, p->p);
  u32 c = p->crc ^ 0xFFFFFFFFUL, i;
  Clk ck;
  clk_start(&ck);
  for (i = p->range.from; i < p->range.to; i++) {
    if (!(i & 4095) && i > p->range.from && clk_over(&ck)) { p->range.to = i; break; }
    c = tab[(c ^ q[i]) & 0xFF] ^ (c >> 8);
  }
  p->crc = c ^ 0xFFFFFFFFUL;
  return 0;
}

/* ================================================================== ILBM */
/* ByteRun1: runs of 3 to 128 equal bytes as -(n-1), n, literals up to 128 */
static u32 byterun1(const u8 *s, u32 n, u8 *o)
{
  u32 i = 0, op = 0;
  while (i < n) {
    u32 run = 1;
    while (i + run < n && run < 128 && s[i + run] == s[i]) run++;
    if (run >= 3) {
      o[op++] = (u8)(257 - run); o[op++] = s[i];
      i += run;
    } else {
      u32 j = i;
      while (j < n && j - i < 128) {
        if (j + 2 < n && s[j] == s[j + 1] && s[j] == s[j + 2]) break;
        j++;
      }
      o[op++] = (u8)(j - i - 1);
      CPY(o + op, s + i, j - i);
      op += j - i;
      i = j;
    }
  }
  return op;
}
WFK_KERNEL(wfk_ilbm)
{
  struct WfkIlbm *p = (struct WfkIlbm *)params;
  u32 y, x, pl, w = p->w, depth = p->depth, rb = ((w + 15) >> 4) << 1, worst = rb + (rb + 127) / 128;
  u8 *plane = WFK_P(u8, p->plane), *out = WFK_P(u8, p->out);
  Clk ck;
  clk_start(&ck);
  for (y = p->range.from; y < p->range.to; y++) {
    const u8 *s = WFK_P(const u8, p->in) + (size_t)y * p->stride;
    CLK_STOP(&ck, p, y);
    if (p->full) return 0;
    for (pl = 0; pl < depth; pl++) {
      u32 bpp = depth == 24 ? 3 : 1, ch = depth == 24 ? pl >> 3 : 0, bit = depth == 24 ? pl & 7 : pl;
      const u8 *q = s + ch;
      if (p->opos + worst > p->ocap || p->opos + worst < p->opos) { p->full = 1; return 0; }
      for (x = 0; x < rb; x++) plane[x] = 0;
      for (x = 0; x < w; x++, q += bpp) plane[x >> 3] |= (u8)(((*q >> bit) & 1) << (7 - (x & 7)));
      if (p->pack) p->opos += byterun1(plane, rb, out + p->opos);
      else { CPY(out + p->opos, plane, rb); p->opos += rb; }
    }
  }
  return 0;
}
