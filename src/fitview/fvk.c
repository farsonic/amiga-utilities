/*
 * fvk.c - FitView's kernels (fvk.h). Plain C, integer arithmetic only, no library calls but
 * memcpy/memset, so it builds unchanged as the 68k program's code, on the host and as AArch64
 * NativeCall kernels (nc/fvk_nc.c includes this file, with NEON for the vertical pass).
 * The resampling passes and the ditherer are WBFit's (src/wbfit/wfk.c) for four-byte pixels.
 *
 * Copyright (C) 2026 the WBFit author.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PX_KERNEL_FILE
#define PX_KERNEL_FILE
#endif
#include "fvk.h"

#ifndef FV_SHARED_BUDGET
WFK_DEFINE_BUDGET                       /* (the host tests that also link WBFit's kernels share its) */
#endif

typedef wfk_u8 u8;
typedef wfk_u16 u16;
typedef wfk_s16 s16;
typedef wfk_u32 u32;
typedef wfk_s32 s32;

/* a pixel (4 bytes, 4-aligned) copied as one word */
#if defined(__GNUC__)
typedef wfk_u32 __attribute__((may_alias)) au32;
#else
typedef wfk_u32 au32;
#endif
#define CP4(d, s) (*(au32 *)(void *)(d) = *(const au32 *)(const void *)(s))
#if NC_NATIVE
/* (not memcpy: the SDK's is a byte loop behind a call; this one the loop vectorizer widens) */
static void cpy(unsigned char *d, const unsigned char *s, size_t n) { while (n--) *d++ = *s++; }
#define CPY(d, s, n) cpy((unsigned char *)(d), (const unsigned char *)(s), (n))
#else
#define CPY(d, s, n) memcpy((d), (s), (n))
#endif

/* ================================================================== high-quality scale */
WFK_KERNEL(fvk_hpass)
{
  struct FvkH *p = (struct FvkH *)params;
  const u32 *tab = WFK_P(const u32, p->tab);
  const s16 *wts = WFK_P(const s16, p->wts);
  const u8 *pal = p->pal ? WFK_P(const u8, p->pal) : 0;
  u32 y, x, ow = p->ow, k = p->k;
  Clk ck;
  clk_start(&ck);
  for (y = p->range.from; y < p->range.to; y++) {
    const u8 *s = WFK_P(const u8, p->src) + (size_t)y * p->sstride;
    u8 *o = WFK_P(u8, p->out) + (size_t)(y - p->y0) * p->ostride;
    CLK_STOP(&ck, p, y);
    if (p->ident) {
      if (!pal) { CPY(o, s + (size_t)tab[0] * 4, (size_t)ow * 4); continue; }
      s += tab[0];
      for (x = 0; x < ow; x++, o += 4) CP4(o, pal + (size_t)s[x] * 4);
      continue;
    }
    for (x = 0; x < ow; x++, o += 4) {
      const s16 *w = wts + (size_t)x * k;
      u32 n = tab[x * 2 + 1], j;
      s32 a = 0, r = 0, g = 0, b = 0;
      if (!pal) {
        const u8 *q = s + (size_t)tab[x * 2] * 4;
        for (j = 0; j < n; j++, q += 4) {
          s32 c = w[j];
          a += MUL16(c, q[0]); r += MUL16(c, q[1]); g += MUL16(c, q[2]); b += MUL16(c, q[3]);
        }
      } else {
        const u8 *q = s + tab[x * 2];
        for (j = 0; j < n; j++) {
          const u8 *e = pal + (size_t)q[j] * 4;
          s32 c = w[j];
          a += MUL16(c, e[0]); r += MUL16(c, e[1]); g += MUL16(c, e[2]); b += MUL16(c, e[3]);
        }
      }
      o[0] = q14(a); o[1] = q14(r); o[2] = q14(g); o[3] = q14(b);
    }
  }
  return 0;
}

#ifdef WFK_VROW
static u32 WFK_VROW(const u8 *in, u32 istride, u32 n, const s16 *w, u8 *o, u32 nb);
#endif

WFK_KERNEL(fvk_vpass)
{
  struct FvkV *p = (struct FvkV *)params;
  const u32 *tab = WFK_P(const u32, p->tab);
  const s16 *wts = WFK_P(const s16, p->wts);
  u32 r, x, nb = p->nb, k = p->k;
  Clk ck;
  clk_start(&ck);
  for (r = p->range.from; r < p->range.to; r++) {
    const u8 *in = WFK_P(const u8, p->in) + (size_t)(tab[r * 2] - p->base) * p->istride;
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
         by WBFit's nc/tests); the row's bulk is NEON (WFK_VROW) anyway */
#pragma GCC novector
#endif
      for (j = 0; j < n; j++, q += p->istride) v += MUL16(w[j], *q);
      o[x] = q14(v);
    }
  }
  return 0;
}

/* ================================================================== the fast preview */
WFK_KERNEL(fvk_nn)
{
  struct FvkNN *p = (struct FvkNN *)params;
  const u32 *xt = WFK_P(const u32, p->xt), *yt = WFK_P(const u32, p->yt);
  const u8 *pal = p->pal ? WFK_P(const u8, p->pal) : 0;
  u32 r, x, ow = p->ow;
  Clk ck;
  clk_start(&ck);
  for (r = p->range.from; r < p->range.to; r++) {
    const u8 *s = WFK_P(const u8, p->src) + (size_t)yt[r] * p->sstride;
    u8 *o = WFK_P(u8, p->out) + (size_t)r * p->ostride;
    CLK_STOP(&ck, p, r);
    if (pal) for (x = 0; x < ow; x++, o += 4) CP4(o, pal + (size_t)s[xt[x]] * 4);
    else for (x = 0; x < ow; x++, o += 4) CP4(o, s + xt[x]);
  }
  return 0;
}

/* ================================================================== background, alpha */
WFK_KERNEL(fvk_fill)
{
  struct FvkFill *p = (struct FvkFill *)params;
  u32 y, x;
  union { u32 v; u8 b[4]; } c;           /* (a word: aligned for the 68000) */
  Clk ck;
  c.b[0] = (u8)(p->argb >> 24); c.b[1] = (u8)(p->argb >> 16); c.b[2] = (u8)(p->argb >> 8); c.b[3] = (u8)p->argb;
  clk_start(&ck);
  for (y = p->range.from; y < p->range.to; y++) {
    u8 *o = WFK_P(u8, p->out) + (size_t)y * p->ostride + (size_t)p->x0 * 4;
    CLK_STOP(&ck, p, y);
    for (x = p->x0; x < p->x1; x++, o += 4) CP4(o, &c.v);
  }
  return 0;
}

/* (x / 255 rounded, for 0 <= x <= 65025) */
#define DIV255(x) ((((x) + 128) + (((x) + 128) >> 8)) >> 8)

WFK_KERNEL(fvk_comp)
{
  struct FvkComp *p = (struct FvkComp *)params;
  u32 y, x, sh = p->shift;
  int k;
  u8 bg[2][3];
  Clk ck;
  bg[0][0] = (u8)(p->c0 >> 16); bg[0][1] = (u8)(p->c0 >> 8); bg[0][2] = (u8)p->c0;
  bg[1][0] = (u8)(p->c1 >> 16); bg[1][1] = (u8)(p->c1 >> 8); bg[1][2] = (u8)p->c1;
  clk_start(&ck);
  for (y = p->range.from; y < p->range.to; y++) {
    u8 *o = WFK_P(u8, p->out) + (size_t)y * p->ostride + (size_t)p->x0 * 4;
    /* (offset by 2^30, a multiple of every square, so the shifts are of positive numbers) */
    u32 ry = sh ? ((u32)((s32)y - p->orgy + 0x40000000L) >> sh) & 1 : 0;
    CLK_STOP(&ck, p, y);
    for (x = p->x0; x < p->x1; x++, o += 4) {
      u32 a = o[0];
      if (a != 255) {
        u32 sq = sh ? (ry ^ (((u32)((s32)x - p->orgx + 0x40000000L) >> sh) & 1)) : 0;
        const u8 *c = bg[sq];
        s32 ia = (s32)(255 - a);
        for (k = 0; k < 3; k++) {
          s32 t = MUL16(ia, c[k]), v = o[1 + k] + DIV255(t);
          o[1 + k] = (u8)(v > 255 ? 255 : v);
        }
        o[0] = 255;
      }
    }
  }
  return 0;
}

/* ================================================================== pens */
WFK_KERNEL(fvk_dither)
{
  struct FvkDither *p = (struct FvkDither *)params;
  const u8 *pal = WFK_P(const u8, p->pal), *pm = WFK_P(const u8, p->penmap);
  u16 *cache = WFK_P(u16, p->cache);
  u32 y, x, k, w = p->w;
  Clk ck;
  clk_start(&ck);
  for (y = p->range.from; y < p->range.to; y++) {
    const u8 *s = WFK_P(const u8, p->px) + (size_t)y * p->pstride;
    u8 *o = WFK_P(u8, p->out) + (size_t)y * p->ostride;
    CLK_STOP(&ck, p, y);
    if (!p->fs) {
      for (x = 0; x < w; x++, s += 4) o[x] = pm[px_nearest(cache, pal, p->npal, PX_KEY(s[1], s[2], s[3]))];
      continue;
    }
    {
      s32 *cur = WFK_P(s32, y & 1 ? p->e1 : p->e0), *nxt = WFK_P(s32, y & 1 ? p->e0 : p->e1);
      for (k = 0; k < (w + 2) * 3; k++) nxt[k] = 0;
      for (x = 0; x < w; x++, s += 4) {
        int v[3];
        u32 idx;
        const u8 *pc;
        for (k = 0; k < 3; k++) v[k] = px_clampi((s32)s[1 + k] + px_e16(cur[(x + 1) * 3 + k]));
        idx = px_nearest(cache, pal, p->npal, PX_KEY(v[0], v[1], v[2]));
        pc = pal + idx * 3;
        o[x] = pm[idx];
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
