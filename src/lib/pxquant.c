/*
 * pxquant.c - the median cut (pxquant.h), from WBFit's wfcore.c (WBFit and FitView use it).
 * Copyright (C) 2026 the WBFit author.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>
#include "pxquant.h"

typedef unsigned long long u64;
typedef wfk_u32 wf_u32;
typedef wfk_u8 wf_u8;

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

