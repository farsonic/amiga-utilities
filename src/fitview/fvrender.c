/*
 * fvrender.c - FitView's renderer (fvrender.h).
 * Copyright (C) 2026 the WBFit author.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>
#include "fvrender.h"
#include "pxweights.h"

typedef unsigned long long u64;

#define CHUNK 32768UL                    /* taps a kernel row at most (about 0.1 ms natively) */

/* ================================================================== the plain runner */
static wfk_u32 (*const kfn[FVK_COUNT])(void *) = {
#define FV__FN(n) n,
  FVK_LIST(FV__FN)
};
static int plain_range(FvRun *r, int id, void *p, fv_u32 from, fv_u32 to)
{
  WFK_RANGE_T *rg = (WFK_RANGE_T *)p;
  (void)r;
  while (from < to) {                     /* (on the 68k one call; the host tests stop often) */
    rg->from = from; rg->to = to;
    kfn[id](p);
    if (rg->to <= from || rg->to > to) return FV_EFAULT;
    from = rg->to;
  }
  return 0;
}
void fv_run_plain(FvRun *r, void *(*alloc)(FvRun *, fv_u32), void (*fr)(FvRun *, void *, fv_u32))
{
  r->range = plain_range; r->alloc = alloc; r->free = fr; r->ctx = 0;
}

/* ================================================================== helpers */
static void *get(FvRun *r, fv_u32 n) { return r->alloc(r, n ? n : 4); }
static void put(FvRun *r, void *p, fv_u32 n) { if (p) r->free(r, p, n ? n : 4); }
/* (a * b, or 0xFFFFFFFF past 1 GB) */
static fv_u32 mul(fv_u32 a, fv_u32 b) { u64 v = (u64)a * b; return v > 0x3FFFFFF0UL ? 0xFFFFFFFFUL : (fv_u32)v; }
#define BIG(n) ((n) == 0xFFFFFFFFUL)

/* a kernel over [from, to), once more (now the 68k code) if a native call failed */
static int run(FvRun *r, FvJob *j, int id, void *p, fv_u32 from, fv_u32 to)
{
  int e;
  if (from >= to) return 0;
  if ((e = r->range(r, id, p, from, to)) == FV_EFAULT) { j->faults++; e = r->range(r, id, p, from, to); }
  return e;
}

static int fill(FvRun *r, FvJob *j, fv_u32 y0, fv_u32 y1, fv_u32 x0, fv_u32 x1)
{
  struct FvkFill f;
  if (x0 >= x1) return 0;
  memset(&f, 0, sizeof f);
  f.out = WFK_SETP(j->s.out); f.ostride = j->s.ostride; f.x0 = x0; f.x1 = x1; f.argb = 0xFF000000UL | (j->s.bg & 0xFFFFFF);
  return run(r, j, FVKI_fvk_fill, &f, y0, y1);
}

/* the nearest source position of scaled positions [v0, v0 + n) of a src -> dst scale (the
   source pixel whose centre is nearest the scaled pixel's centre), times mult */
static void nearest_tab(fv_u32 *t, fv_u32 src, fv_u32 dst, fv_u32 v0, fv_u32 n, fv_u32 mult)
{
  fv_u32 i;
  for (i = 0; i < n; i++) {
    u64 c = (u64)(2 * (u64)(v0 + i) + 1) * src / (2 * (u64)dst);
    t[i] = (fv_u32)(c < src ? c : src - 1) * mult;
  }
}

/* ================================================================== the job */
static int setup_preview(FvRun *r, FvJob *j)
{
  j->nxt = mul(j->vw, 4); j->nyt = mul(j->vh, 4);
  if (BIG(j->nxt) || BIG(j->nyt)) return FV_EBIG;
  if (!(j->xt = get(r, j->nxt)) || !(j->yt = get(r, j->nyt))) return FV_ENOMEM;
  nearest_tab(j->xt, j->s.sw, j->s.dw, j->vx0, j->vw, j->s.pal ? 1 : 4);
  nearest_tab(j->yt, j->s.sh, j->s.dh, j->vy0, j->vh, 1);
  j->cost = j->vw ? j->vw : 1;
  return 0;
}

static void free_refine(FvRun *r, FvJob *j)
{
  put(r, j->mid, j->midn); put(r, j->wy, j->nwy); put(r, j->wx, j->nwx); put(r, j->ty, j->nty); put(r, j->tx, j->ntx);
  j->mid = 0; j->wy = j->wx = 0; j->ty = j->tx = 0;
}

static int setup_refine(FvRun *r, FvJob *j)
{
  fv_u32 rowb, want, perout;
  j->kx = wf_taps(j->s.sw, j->s.dw); j->ky = wf_taps(j->s.sh, j->s.dh);
  j->ntx = mul(j->vw, 8); j->nty = mul(j->vh, 8);
  j->nwx = mul(mul(j->vw, j->kx), 2); j->nwy = mul(mul(j->vh, j->ky), 2);
  if (BIG(j->ntx) || BIG(j->nty) || BIG(j->nwx) || BIG(j->nwy)) return FV_EBIG;
  if (!(j->tx = get(r, j->ntx)) || !(j->ty = get(r, j->nty)) || !(j->wx = get(r, j->nwx)) || !(j->wy = get(r, j->nwy)))
    return FV_ENOMEM;
  j->idx = wf_weights(j->s.sw, j->s.dw, j->vx0, j->vw, j->tx, j->wx, j->kx);
  j->idy = wf_weights(j->s.sh, j->s.dh, j->vy0, j->vh, j->ty, j->wy, j->ky);
  if (j->idx < 0 || j->idy < 0) return FV_EBIG;
  /* the intermediate rows: at least the taps of one output row, else about 256 KB worth */
  rowb = mul(j->vw, 4);
  if (BIG(rowb)) return FV_EBIG;
  want = (256UL << 10) / (rowb ? rowb : 1);
  if (want < 2 * j->ky + 4) want = 2 * j->ky + 4;
  if (want > j->s.sh) want = j->s.sh;
  if (want < j->ky) want = j->ky;
  j->midcap = want;
  j->midn = mul(want, rowb);
  if (BIG(j->midn)) return FV_EBIG;
  if (!(j->mid = get(r, j->midn))) return FV_ENOMEM;
  j->mbase = j->m0 = j->m1 = 0;
  perout = j->s.dh < j->s.sh ? (j->s.sh + j->s.dh - 1) / j->s.dh : 1;      /* source rows per output row */
  j->cost = mul(j->vw, j->ky + j->kx * perout);
  if (BIG(j->cost) || !j->cost) j->cost = 0x7FFFFFFFUL;
  return 0;
}

int fv_job_begin(FvRun *r, FvJob *j, const FvScene *s, int q)
{
  const FvScene *c = s;
  fv_s32 px = c->px, py = c->py;
  int e;
  memset(j, 0, sizeof *j);
  j->s = *s; j->q = q;
  if (!c->out || !c->W || !c->H || !c->src || !c->sw || !c->sh || !c->dw || !c->dh) return FV_EBIG;
  if (c->sw > FV_MAXDIM || c->sh > FV_MAXDIM || c->dw > FV_MAXDIM || c->dh > FV_MAXDIM) return FV_EBIG;
  /* the part of the scaled picture in the window */
  j->vx0 = px < 0 ? (fv_u32)-px : 0;
  j->vy0 = py < 0 ? (fv_u32)-py : 0;
  j->ox = px > 0 ? (fv_u32)px : 0;
  j->oy = py > 0 ? (fv_u32)py : 0;
  j->vw = j->vx0 >= c->dw || j->ox >= c->W ? 0 : (c->dw - j->vx0 < c->W - j->ox ? c->dw - j->vx0 : c->W - j->ox);
  j->vh = j->vy0 >= c->dh || j->oy >= c->H ? 0 : (c->dh - j->vy0 < c->H - j->oy ? c->dh - j->vy0 : c->H - j->oy);
  if (!j->vw || !j->vh) { j->vw = j->vh = 0; j->oy = c->H; return 0; }   /* (only background) */
  if (q == FV_REFINE) {
    if (!(e = setup_refine(r, j))) return 0;
    free_refine(r, j);                   /* no memory, or more than a 250-fold reduction: the preview */
    if (e == FV_EBIG || e == FV_ENOMEM) j->nofine = 1;
  }
  return setup_preview(r, j);
}

void fv_job_end(FvRun *r, FvJob *j)
{
  free_refine(r, j);
  put(r, j->yt, j->nyt); put(r, j->xt, j->nxt);
  j->yt = j->xt = 0;
}

/* refine: picture rows [pr, pr + n) */
static int refine_rows(FvRun *r, FvJob *j, fv_u32 pr, fv_u32 n)
{
  fv_u32 a = j->ty[pr * 2], e = 0, i, rowb = j->vw * 4;
  int err;
  for (i = pr; i < pr + n; i++) if (j->ty[i * 2] + j->ty[i * 2 + 1] > e) e = j->ty[i * 2] + j->ty[i * 2 + 1];
  /* the intermediate rows [m0, m1) live at mid + (row - mbase) * rowb: drop what is no longer
     needed, move the rest down when the new ones would not fit, then make the missing ones */
  if (a >= j->m1) j->mbase = j->m0 = j->m1 = a;
  else if (a > j->m0) j->m0 = a;
  if (e - j->mbase > j->midcap) {
    if (j->m1 > j->m0) memmove(j->mid, j->mid + (size_t)(j->m0 - j->mbase) * rowb, (size_t)(j->m1 - j->m0) * rowb);
    j->mbase = j->m0;
  }
  /* (in columns of at most about CHUNK taps a row, so one row of a kernel stays short: a
     native call stops only between rows; the columns are independent, the bytes the same) */
  if (e > j->m1) {
    struct FvkH h;
    fv_u32 c0, cw = CHUNK / j->kx;
    if (cw < 1) cw = 1;
    for (c0 = 0; c0 < j->vw; c0 += cw) {
      memset(&h, 0, sizeof h);
      h.src = WFK_SETP(j->s.src); h.sstride = j->s.sstride; h.pal = j->s.pal ? WFK_SETP(j->s.pal) : 0;
      h.out = WFK_SETP(j->mid + (size_t)c0 * 4); h.ostride = rowb; h.ow = j->vw - c0 < cw ? j->vw - c0 : cw; h.y0 = j->mbase;
      h.tab = WFK_SETP(j->tx + (size_t)c0 * 2); h.wts = WFK_SETP(j->wx + (size_t)c0 * j->kx); h.k = j->kx; h.ident = (wfk_u32)j->idx;
      if ((err = run(r, j, FVKI_fvk_hpass, &h, j->m1, e))) return err;
    }
    j->m1 = e;
  }
  {
    struct FvkV v;
    fv_u32 b0, cb = (CHUNK * 4 / j->ky) & ~15UL;
    if (cb < 16) cb = 16;
    for (b0 = 0; b0 < rowb; b0 += cb) {
      memset(&v, 0, sizeof v);
      v.in = WFK_SETP(j->mid + b0); v.istride = rowb; v.base = j->mbase;
      v.out = WFK_SETP(j->s.out + (size_t)(j->oy + pr) * j->s.ostride + (size_t)j->ox * 4 + b0); v.ostride = j->s.ostride;
      v.nb = rowb - b0 < cb ? rowb - b0 : cb; v.tab = WFK_SETP(j->ty + pr * 2); v.wts = WFK_SETP(j->wy + (size_t)pr * j->ky); v.k = j->ky;
      v.ident = (wfk_u32)j->idy;
      if ((err = run(r, j, FVKI_fvk_vpass, &v, 0, n))) return err;
    }
    return 0;
  }
}

static int preview_rows(FvRun *r, FvJob *j, fv_u32 pr, fv_u32 n)
{
  struct FvkNN p;
  memset(&p, 0, sizeof p);
  p.src = WFK_SETP(j->s.src); p.sstride = j->s.sstride; p.pal = j->s.pal ? WFK_SETP(j->s.pal) : 0;
  p.out = WFK_SETP(j->s.out + (size_t)j->oy * j->s.ostride + (size_t)j->ox * 4); p.ostride = j->s.ostride;
  p.ow = j->vw; p.xt = WFK_SETP(j->xt); p.yt = WFK_SETP(j->yt);
  return run(r, j, FVKI_fvk_nn, &p, pr, pr + n);
}

int fv_job_step(FvRun *r, FvJob *j, fv_u32 budget, fv_u32 *from, fv_u32 *to)
{
  const FvScene *s = &j->s;
  fv_u32 y = j->y, n, end;
  int e;
  *from = *to = y;
  if (y >= s->H) return 0;
  if (y < j->oy || y >= j->oy + j->vh) {
    /* background rows: above or below the picture */
    end = y < j->oy ? j->oy : s->H;
    n = budget / s->W;
    if (n < 1) n = 1;
    if (y + n < end) end = y + n;
    if ((e = fill(r, j, y, end, 0, s->W))) return -e;
  } else {
    fv_u32 pr = y - j->oy;
    n = budget / j->cost;
    if (n < 1) n = 1;
    if (n > j->vh - pr) n = j->vh - pr;
    if (j->mid) {
      /* no more rows than the intermediate rows can hold the taps of */
      while (n > 1 && j->ty[(pr + n - 1) * 2] + j->ty[(pr + n - 1) * 2 + 1] - j->ty[pr * 2] > j->midcap) n--;
    }
    end = y + n;
    if ((e = fill(r, j, y, end, 0, j->ox)) || (e = fill(r, j, y, end, j->ox + j->vw, s->W))) return -e;
    if ((e = j->mid ? refine_rows(r, j, pr, n) : preview_rows(r, j, pr, n))) return -e;
    if (s->alpha) {
      struct FvkComp c;
      memset(&c, 0, sizeof c);
      c.out = WFK_SETP(s->out); c.ostride = s->ostride; c.x0 = j->ox; c.x1 = j->ox + j->vw;
      c.c0 = s->chkshift ? s->chk0 : s->bg; c.c1 = s->chk1; c.shift = s->chkshift; c.orgx = s->px; c.orgy = s->py;
      if ((e = run(r, j, FVKI_fvk_comp, &c, y, end))) return -e;
    }
  }
  j->y = end;
  *from = y; *to = end;
  return end < s->H ? 1 : 0;
}
