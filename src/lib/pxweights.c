/*
 * pxweights.c - the resampling weights (pxweights.h): bicubic (Catmull-Rom), widened by the
 * factor when shrinking, integer only (from WBFit's wfcore.c; WBFit and FitView use it).
 * Copyright (C) 2026 the WBFit author.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stddef.h>
#include "pxweights.h"

typedef long long s64;
typedef wfk_u32 wf_u32;
typedef wfk_s32 wf_s32;

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

