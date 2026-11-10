/*
 * fvview.c - FitView's view geometry (fvview.h).
 * Copyright (C) 2026 the WBFit author.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "fvview.h"

typedef unsigned long long u64;

static fv_u32 rdiv(u64 a, u64 b) { return b ? (fv_u32)((a + b / 2) / b) : 0; }
static fv_u32 clampd(fv_u32 v) { return v < 1 ? 1 : v > FV_MAXDIM ? FV_MAXDIM : v; }
static fv_u32 dist(fv_u32 a, fv_u32 b) { return a > b ? a - b : b - a; }

int fv_geom(FvGeom *g)
{
  fv_u32 sw = g->sw, sh = g->sh, W = g->W, H = g->H, h;
  g->dw = g->dh = 1; g->maxx = g->maxy = 0; g->pm = 1000; g->h100 = 1;
  if (!sw || !sh || sw > FV_MAXDIM || sh > FV_MAXDIM) return -1;
  if (!g->anum || !g->aden) g->anum = g->aden = 1;
  h = clampd(rdiv((u64)sh * g->anum, g->aden));
  g->h100 = h;
  if (!W || !H) return -1;
  switch (g->mode) {
  case FV_FIT:
  case FV_FILL:
    /* the width decides when W / sw is the smaller (FIT) or the larger (FILL) factor */
    if (((u64)W * h <= (u64)H * sw) == (g->mode == FV_FIT)) { g->dw = W; g->dh = rdiv((u64)h * W, sw); }
    else { g->dh = H; g->dw = rdiv((u64)sw * H, h); }
    break;
  case FV_WIDTH: g->dw = W; g->dh = rdiv((u64)h * W, sw); break;
  case FV_ZOOM:
    if (!g->zoom) g->zoom = 1000;
    g->dw = rdiv((u64)sw * g->zoom, 1000); g->dh = rdiv((u64)h * g->zoom, 1000);
    break;
  default: g->dw = sw; g->dh = h; break;
  }
  if (g->snap && dist(g->dw, sw) <= 1 && dist(g->dh, h) <= 1) { g->dw = sw; g->dh = h; }
  g->dw = clampd(g->dw); g->dh = clampd(g->dh);
  g->maxx = g->dw > W ? g->dw - W : 0;
  g->maxy = g->dh > H ? g->dh - H : 0;
  g->pm = rdiv((u64)g->dw * 1000, sw);
  return 0;
}

void fv_place(const FvGeom *g, fv_u32 sx, fv_u32 sy, fv_s32 *px, fv_s32 *py)
{
  if (sx > g->maxx) sx = g->maxx;
  if (sy > g->maxy) sy = g->maxy;
  *px = g->dw <= g->W ? (fv_s32)((g->W - g->dw) / 2) : -(fv_s32)sx;
  *py = g->dh <= g->H ? (fv_s32)((g->H - g->dh) / 2) : -(fv_s32)sy;
}

/* one axis: the scaled position at the window's middle, moved to the new scale */
static fv_u32 re1(fv_u32 od, fv_u32 ow, fv_u32 omax, fv_u32 nd, fv_u32 nw, fv_u32 nmax, fv_u32 s)
{
  u64 c;
  fv_u32 half = nw / 2, v;
  if (s > omax) s = omax;
  c = od > ow ? (u64)s + ow / 2 : od / 2;          /* (in the old scaled picture) */
  v = rdiv(c * nd, od ? od : 1);
  v = v > half ? v - half : 0;
  return v > nmax ? nmax : v;
}
void fv_rescroll(const FvGeom *o, const FvGeom *n, fv_u32 *sx, fv_u32 *sy)
{
  *sx = re1(o->dw, o->W, o->maxx, n->dw, n->W, n->maxx, *sx);
  *sy = re1(o->dh, o->H, o->maxy, n->dh, n->H, n->maxy, *sy);
}

static const fv_u32 steps[] = { 25, 50, 100, 125, 167, 250, 333, 500, 667, 750, 1000, 1250, 1500, 2000, 3000,
                                4000, 6000, 8000, 12000, 16000 };
#define NSTEPS (sizeof steps / sizeof steps[0])
fv_u32 fv_zoom_step(fv_u32 cur, int dir)
{
  unsigned i;
  if (dir > 0) {
    for (i = 0; i < NSTEPS; i++) if (steps[i] > cur + cur / 50) return steps[i];   /* (2% more at least) */
    return steps[NSTEPS - 1];
  }
  for (i = NSTEPS; i-- > 0; ) if (steps[i] + steps[i] / 50 < cur) return steps[i];
  return steps[0];
}

void fv_aspect(fv_u32 px, fv_u32 py, fv_u32 rx, fv_u32 ry, fv_u32 *anum, fv_u32 *aden)
{
  u64 n, d;
  if (!px || !py) px = py = 1;
  if (!rx || !ry) rx = ry = 1;
  /* one source pixel is py / px as high as wide, a screen pixel ry / rx */
  n = (u64)py * rx; d = (u64)px * ry;
  if (n * 20 <= d * 21 && d * 20 <= n * 21) n = d = 1;
  {
    u64 a = n, b = d, t;
    while (b) { t = a % b; a = b; b = t; }
    n /= a; d /= a;
  }
  while (n > 0xFFFF || d > 0xFFFF) { n >>= 1; d >>= 1; }
  *anum = (fv_u32)(n ? n : 1); *aden = (fv_u32)(d ? d : 1);
}

void fv_prop(fv_u32 total, fv_u32 visible, fv_u32 top, fv_u32 *ptotal, fv_u32 *pvis, fv_u32 *ptop)
{
  fv_u32 sh = 0;
  if (visible > total) visible = total;
  if (top > total - visible) top = total - visible;
  while ((total >> sh) > 0x7FFF) sh++;               /* (propgclass takes 16-bit values) */
  *ptotal = total >> sh; *pvis = visible >> sh; *ptop = top >> sh;
  if (!*pvis) *pvis = 1;
  if (*pvis > *ptotal) *ptotal = *pvis;
}
fv_u32 fv_unprop(fv_u32 total, fv_u32 visible, fv_u32 ptop)
{
  fv_u32 sh = 0, v;
  while ((total >> sh) > 0x7FFF) sh++;
  if (visible > total) return 0;
  if (ptop + (visible >> sh) >= (total >> sh)) return total - visible;     /* (the end, exactly) */
  v = ptop << sh;
  return v > total - visible ? total - visible : v;
}
