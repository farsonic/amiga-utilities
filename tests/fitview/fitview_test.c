/*
 * fitview_test.c - FitView's host tests (make test-fitview; gcc with ASan and UBSan).
 *   1. geometry: every zoom mode, scroll ranges, placement, keeping the middle on a zoom or a
 *      resize, the zoom steps, the scroller values; the cases are also dumped for check.py;
 *   2. the renderer: preview and refined pictures of generated sources (gradients, fine checks,
 *      noise, alpha), shrinking and enlarging, centred and scrolled, 32-bit and palette
 *      sources; each rendered with many band sizes and with the kernels stopped after 1, 2, 3,
 *      7 and 50 clock checks: the same bytes every time; dumped for check.py, which recomputes
 *      them from exact fractions;
 *   3. the scaler against WBFit's own kernels (src/wbfit/wfk.c) with the same weights: the
 *      colour channels byte for byte the same;
 *   4. the ditherer to pens (nearest and Floyd-Steinberg) and the alpha composite, dumped.
 * DIR receives the dumps.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fvrender.h"
#include "fvview.h"
#include "pxweights.h"
#include "wfk.h"

static int checks, fails;
static void check(int ok, const char *what, int n)
{
  checks++;
  if (!ok) { fails++; if (fails < 40) printf("FAIL %s %d\n", what, n); }
}
static unsigned long seed = 1;
static unsigned rnd(void) { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return (unsigned)(seed >> 33); }
static const char *dir;
static FILE *dump(const char *name)
{
  char p[600];
  FILE *f;
  snprintf(p, sizeof p, "%s/%s", dir, name);
  if (!(f = fopen(p, "wb"))) { perror(p); exit(2); }
  return f;
}
static void w32(FILE *f, unsigned v) { unsigned char b[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v }; fwrite(b, 1, 4, f); }

/* allocations counted: every job must give back what it took */
static long live;
static void *a_alloc(FvRun *r, fv_u32 n) { unsigned char *p; (void)r; if (!(p = malloc(n + 16))) return 0; live++; memset(p + 16, 0xA5, n); return p + 16; }
static void a_free(FvRun *r, void *p, fv_u32 n) { (void)r; (void)n; live--; free((unsigned char *)p - 16); }
static long fail_at = -1;         /* (that allocation fails: refine falls back to the preview) */
static void *a_alloc_f(FvRun *r, fv_u32 n) { if (fail_at >= 0 && fail_at-- == 0) return 0; return a_alloc(r, n); }

/* ================================================================== 1. geometry */
static FILE *gf;
static void geom_case(fv_u32 sw, fv_u32 sh, fv_u32 an, fv_u32 ad, fv_u32 W, fv_u32 H, int mode, fv_u32 zoom, int snap)
{
  FvGeom g;
  memset(&g, 0, sizeof g);
  g.sw = sw; g.sh = sh; g.anum = an; g.aden = ad; g.W = W; g.H = H; g.mode = mode; g.zoom = zoom; g.snap = snap;
  check(fv_geom(&g) == 0, "geom ok", (int)sw);
  check(g.dw >= 1 && g.dh >= 1 && g.dw <= FV_MAXDIM && g.dh <= FV_MAXDIM, "geom range", (int)sw);
  check(g.maxx == (g.dw > W ? g.dw - W : 0) && g.maxy == (g.dh > H ? g.dh - H : 0), "geom scroll", (int)sw);
  if (snap && g.dw == sw && g.dh == g.h100) mode = -1;      /* (snapped to 100%) */
  if (mode == FV_FIT) check(g.dw <= W && g.dh <= H && (g.dw == W || g.dh == H), "fit inside", (int)sw);
  if (mode == FV_FILL) check(g.dw >= W && g.dh >= H && (g.dw == W || g.dh == H), "fill covers", (int)sw);
  if (mode == FV_WIDTH) check(g.dw == W, "width", (int)sw);
  w32(gf, sw); w32(gf, sh); w32(gf, an); w32(gf, ad); w32(gf, W); w32(gf, H); w32(gf, (unsigned)g.mode); w32(gf, zoom); w32(gf, (unsigned)snap);
  w32(gf, g.dw); w32(gf, g.dh); w32(gf, g.maxx); w32(gf, g.maxy); w32(gf, g.h100);
  {
    fv_s32 px, py;
    fv_u32 sx = rnd() % (g.maxx + 5), sy = rnd() % (g.maxy + 5);
    fv_place(&g, sx, sy, &px, &py);
    w32(gf, sx); w32(gf, sy); w32(gf, (unsigned)px); w32(gf, (unsigned)py);
    if (g.dw <= W) check(px >= 0 && (fv_u32)px + g.dw <= W, "place inside x", (int)sw);
    else check(px <= 0 && (fv_u32)(-px) <= g.maxx, "place scrolled x", (int)sw);
  }
}

static void t_geom(void)
{
  int i, m;
  gf = dump("geom.bin");
  for (m = 0; m < FV_NMODES; m++) {
    geom_case(640, 480, 1, 1, 400, 300, m, 1500, 0);
    geom_case(640, 480, 1, 1, 401, 299, m, 333, 0);
    geom_case(100, 1000, 1, 1, 640, 480, m, 4000, 0);
    geom_case(4000, 30, 1, 1, 640, 480, m, 25, 0);
    geom_case(320, 256, 1, 2, 640, 256, m, 1000, 0);         /* (PAL hires: pixels twice as tall) */
    geom_case(32767, 32767, 1, 1, 1, 1, m, 16000, 0);
    geom_case(1, 1, 1, 1, 32767, 32767, m, 16000, 0);
    geom_case(595, 842, 1, 1, 596, 842, m, 1000, 1);         /* (snap: a page drawn for the window) */
  }
  for (i = 0; i < 3000; i++)
    geom_case(1 + rnd() % 5000, 1 + rnd() % 5000, 1 + rnd() % 3, 1 + rnd() % 3, 1 + rnd() % 2000, 1 + rnd() % 1500,
              (int)(rnd() % FV_NMODES), 25 + rnd() % 16000, (int)(rnd() & 1));
  fclose(gf);
  {
    FvGeom g;
    memset(&g, 0, sizeof g);
    check(fv_geom(&g) < 0, "geom empty", 0);
    g.sw = 40000; g.sh = 10; g.W = g.H = 10;
    check(fv_geom(&g) < 0, "geom too big", 0);
  }
  /* zoom steps: strictly up and down, and back */
  {
    fv_u32 z = 25, last = 0;
    for (i = 0; i < 30; i++) { last = z; z = fv_zoom_step(z, 1); check(z > last || z == 16000, "zoom up", i); }
    check(z == 16000, "zoom top", 0);
    for (i = 0; i < 30; i++) { last = z; z = fv_zoom_step(z, -1); check(z < last || z == 25, "zoom down", i); }
    check(z == 25, "zoom bottom", 0);
    check(fv_zoom_step(1000, 1) == 1250 && fv_zoom_step(1000, -1) == 750 && fv_zoom_step(431, 1) == 500 && fv_zoom_step(431, -1) == 333, "zoom from", 0);
  }
  /* the middle stays the middle */
  {
    FvGeom o, n;
    fv_u32 sx, sy;
    memset(&o, 0, sizeof o);
    o.sw = 1000; o.sh = 800; o.W = 300; o.H = 200; o.mode = FV_ONE; fv_geom(&o);
    n = o; n.mode = FV_ZOOM; n.zoom = 2000; fv_geom(&n);
    sx = 350; sy = 300;                                   /* middle at (500, 400) */
    fv_rescroll(&o, &n, &sx, &sy);
    check(sx == 1000 - 150 && sy == 800 - 100, "rescroll zoom", (int)sx);
    n = o; n.W = 500; n.H = 100; fv_geom(&n);
    sx = 350; sy = 300;
    fv_rescroll(&o, &n, &sx, &sy);
    check(sx == 250 && sy == 350, "rescroll resize", (int)sx);
    n = o; n.mode = FV_FIT; fv_geom(&n);
    fv_rescroll(&o, &n, &sx, &sy);
    check(sx == 0 && sy == 0, "rescroll fit", 0);
  }
  /* scroller values */
  for (i = 0; i < 2000; i++) {
    fv_u32 tot = 1 + rnd() % 40000, vis = 1 + rnd() % tot, top = rnd() % (tot - vis + 1), pt, pv, pp, back;
    fv_prop(tot, vis, top, &pt, &pv, &pp);
    check(pt <= 0x7FFF && pv <= pt && pp + pv <= pt + 1, "prop range", i);
    back = fv_unprop(tot, vis, pp);
    check(back <= tot - vis && back + (tot >> 14) + 2 >= top && back <= top + 1, "prop back", i);
    check(fv_unprop(tot, vis, pt - pv) == tot - vis, "prop end", i);
  }
  {
    fv_u32 an, ad;
    fv_aspect(0, 0, 22, 44, &an, &ad); check(an * 2 == ad, "aspect hires", (int)an);
    fv_aspect(10, 11, 44, 44, &an, &ad); check(an == 11 && ad == 10, "aspect lores ilbm", (int)an);
    fv_aspect(1, 1, 22, 23, &an, &ad); check(an == 1 && ad == 1, "aspect near square", (int)an);
  }
}

/* ================================================================== 2. the renderer */
typedef struct { fv_u32 sw, sh; unsigned char *argb, *idx, *pal; int alpha; } Src;

static void make_src(Src *s, fv_u32 sw, fv_u32 sh, int kind, int alpha)
{
  fv_u32 x, y, i;
  s->sw = sw; s->sh = sh; s->alpha = alpha;
  s->argb = malloc((size_t)sw * sh * 4);
  s->idx = malloc((size_t)sw * sh);
  s->pal = malloc(1024);
  for (i = 0; i < 256; i++) {
    unsigned a = alpha ? (i * 37) & 255 : 255, r = (i * 53) & 255, g = (i * 91 + 7) & 255, b = (i * 13 + 100) & 255;
    if (alpha) { r = r * a / 255; g = g * a / 255; b = b * a / 255; }       /* (premultiplied) */
    s->pal[i * 4] = (unsigned char)a; s->pal[i * 4 + 1] = (unsigned char)r; s->pal[i * 4 + 2] = (unsigned char)g; s->pal[i * 4 + 3] = (unsigned char)b;
  }
  for (y = 0; y < sh; y++)
    for (x = 0; x < sw; x++) {
      unsigned v;
      switch (kind) {
      case 0: v = (x * 255 / (sw > 1 ? sw - 1 : 1)) ^ ((y * 255 / (sh > 1 ? sh - 1 : 1)) << 1); break;   /* gradients */
      case 1: v = ((x ^ y) & 1) ? 255 : 0; break;                                                    /* finest checks */
      default: v = rnd() & 255; break;                                                               /* noise */
      }
      s->idx[(size_t)y * sw + x] = (unsigned char)v;
      memcpy(s->argb + ((size_t)y * sw + x) * 4, s->pal + (v & 255) * 4, 4);
    }
}
static void free_src(Src *s) { free(s->argb); free(s->idx); free(s->pal); }

static int render(const FvScene *sc, int q, fv_u32 budget, int *nofine)
{
  FvRun r;
  FvJob j;
  fv_u32 a, b, next = 0;
  int e, guard = 0;
  fv_run_plain(&r, a_alloc_f, a_free);
  if ((e = fv_job_begin(&r, &j, sc, q))) { fv_job_end(&r, &j); return -1000 - e; }
  if (nofine) *nofine = j.nofine;
  do {
    e = fv_job_step(&r, &j, budget, &a, &b);
    if (e < 0) break;
    check(a == next && b > a, "rows in order", (int)a);
    next = b;
  } while (e > 0 && ++guard < 1000000);
  check(next == sc->H || e < 0, "all rows", (int)next);
  fv_job_end(&r, &j);
  return e;
}

static FILE *rf;
static int ncase;
static void scene_case(const Src *s, int usepal, fv_u32 W, fv_u32 H, fv_u32 dw, fv_u32 dh, fv_s32 px, fv_s32 py, int q, fv_u32 chkshift)
{
  FvScene sc;
  size_t n = (size_t)W * H * 4;
  unsigned char *ref = malloc(n), *o = malloc(n);
  static const fv_u32 budgets[] = { 1, 7, 100, 5000, 0x7FFFFFFF };
  static const unsigned long stops[] = { 1, 2, 3, 7, 50 };
  unsigned k;
  int nofine = 0, e;
  memset(&sc, 0, sizeof sc);
  sc.src = usepal ? s->idx : s->argb; sc.sw = s->sw; sc.sh = s->sh; sc.sstride = usepal ? s->sw : s->sw * 4;
  sc.pal = usepal ? s->pal : 0; sc.alpha = s->alpha;
  sc.W = W; sc.H = H; sc.ostride = W * 4; sc.dw = dw; sc.dh = dh; sc.px = px; sc.py = py;
  sc.bg = 0x203040; sc.chk0 = 0x999999; sc.chk1 = 0x666666; sc.chkshift = chkshift;
  memset(ref, 0x5A, n);
  sc.out = ref;
  e = render(&sc, q, 3000, &nofine);
  check(e == 0, "render", ncase);
  for (k = 0; k < sizeof budgets / sizeof budgets[0]; k++) {
    memset(o, k * 40 + 1, n); sc.out = o;
    render(&sc, q, budgets[k], 0);
    check(!memcmp(o, ref, n), "band size", ncase * 10 + (int)k);
  }
  for (k = 0; k < sizeof stops / sizeof stops[0]; k++) {
    memset(o, k * 30 + 2, n); sc.out = o;
    wfk_test_budget = stops[k];
    render(&sc, q, 2000, 0);
    wfk_test_budget = 0;
    check(!memcmp(o, ref, n), "stopped and called again", ncase * 10 + (int)k);
  }
  /* dumped: the scene, the source (as ARGB), the result */
  w32(rf, s->sw); w32(rf, s->sh); w32(rf, W); w32(rf, H); w32(rf, dw); w32(rf, dh); w32(rf, (unsigned)px); w32(rf, (unsigned)py);
  w32(rf, (unsigned)q); w32(rf, (unsigned)s->alpha); w32(rf, sc.bg); w32(rf, sc.chk0); w32(rf, sc.chk1); w32(rf, chkshift); w32(rf, (unsigned)nofine);
  fwrite(s->argb, 1, (size_t)s->sw * s->sh * 4, rf);
  fwrite(ref, 1, n, rf);
  ncase++;
  free(ref); free(o);
}

static void t_render(void)
{
  Src s;
  rf = dump("render.bin");
  /* shrinking, enlarging, both, one axis each; centred, scrolled; 32-bit and palette */
  make_src(&s, 61, 47, 0, 0);
  scene_case(&s, 0, 50, 40, 25, 19, 12, 10, FV_REFINE, 0);
  scene_case(&s, 0, 50, 40, 25, 19, 12, 10, FV_PREVIEW, 0);
  scene_case(&s, 1, 50, 40, 25, 19, 12, 10, FV_REFINE, 0);
  scene_case(&s, 0, 40, 30, 150, 110, -37, -21, FV_REFINE, 0);
  scene_case(&s, 0, 40, 30, 150, 110, -37, -21, FV_PREVIEW, 0);
  scene_case(&s, 1, 40, 30, 150, 110, -110, -80, FV_REFINE, 0);
  scene_case(&s, 0, 70, 30, 61, 90, 4, -13, FV_REFINE, 0);          /* (1:1 across, enlarged down) */
  scene_case(&s, 0, 61, 47, 61, 47, 0, 0, FV_REFINE, 0);            /* (1:1: copies) */
  scene_case(&s, 1, 61, 47, 61, 47, 0, 0, FV_PREVIEW, 0);
  free_src(&s);
  make_src(&s, 97, 33, 1, 0);                                       /* finest checks: no moire when shrunk */
  scene_case(&s, 0, 64, 64, 31, 11, 16, 26, FV_REFINE, 0);
  scene_case(&s, 0, 64, 64, 13, 60, 25, 2, FV_REFINE, 0);
  free_src(&s);
  make_src(&s, 220, 23, 2, 0);                                      /* noise, a big reduction across */
  scene_case(&s, 0, 30, 30, 9, 30, 10, 0, FV_REFINE, 0);
  scene_case(&s, 1, 30, 30, 9, 30, 10, 0, FV_REFINE, 0);
  free_src(&s);
  make_src(&s, 37, 29, 2, 1);                                       /* alpha: checkerboard, one colour */
  scene_case(&s, 0, 60, 50, 52, 41, 4, 4, FV_REFINE, 3);
  scene_case(&s, 0, 60, 50, 52, 41, 4, 4, FV_PREVIEW, 2);
  scene_case(&s, 1, 30, 20, 74, 58, -9, -30, FV_REFINE, 0);
  scene_case(&s, 0, 30, 20, 74, 58, -40, -31, FV_REFINE, 4);
  free_src(&s);
  make_src(&s, 9000, 3, 2, 0);                                      /* rows in column chunks (many taps) */
  scene_case(&s, 0, 320, 4, 300, 2, 10, 1, FV_REFINE, 0);
  free_src(&s);
  make_src(&s, 300, 9000, 0, 0);
  scene_case(&s, 0, 292, 302, 290, 300, 1, 1, FV_REFINE, 0);
  free_src(&s);
  make_src(&s, 400, 300, 0, 0);                                     /* intermediate rows moved down */
  scene_case(&s, 0, 80, 70, 40, 7, 20, 31, FV_REFINE, 0);
  scene_case(&s, 0, 80, 70, 80, 60, 0, 5, FV_REFINE, 0);
  free_src(&s);
  fclose(rf);
  /* refine without memory: the preview; and a picture scrolled out of the window: background */
  {
    FvScene sc;
    FvRun r;
    FvJob j;
    unsigned char *o = malloc(20 * 10 * 4), *p = malloc(20 * 10 * 4);
    int nofine = 0;
    make_src(&s, 50, 50, 0, 0);
    memset(&sc, 0, sizeof sc);
    sc.src = s.argb; sc.sw = 50; sc.sh = 50; sc.sstride = 200; sc.W = 20; sc.H = 10; sc.ostride = 80; sc.dw = 17; sc.dh = 9; sc.px = 1; sc.py = 0;
    sc.out = p; render(&sc, FV_PREVIEW, 100, 0);
    fail_at = 2;
    sc.out = o; check(render(&sc, FV_REFINE, 100, &nofine) == 0 && nofine == 1, "refine no memory", nofine);
    fail_at = -1;
    check(!memcmp(o, p, 800), "refine no memory = preview", 0);
    fv_run_plain(&r, a_alloc, a_free);
    check(fv_job_begin(&r, &j, &sc, FV_REFINE) == 0, "begin", 0);
    fv_job_end(&r, &j);
    sc.dw = 50000;
    check(fv_job_begin(&r, &j, &sc, FV_REFINE) == FV_EBIG, "too big", 0);
    fv_job_end(&r, &j);
    free(o); free(p); free_src(&s);
  }
  check(live == 0, "every allocation given back", (int)live);
}

/* ================================================================== 3. against WBFit's kernels */
static void t_wbfit(void)
{
  int t;
  for (t = 0; t < 40; t++) {
    fv_u32 sw = 2 + rnd() % 300, sh = 2 + rnd() % 200, dw = 1 + rnd() % 400, dh = 1 + rnd() % 300;
    fv_u32 vx0 = rnd() % dw, vy0 = rnd() % dh, vw = 1 + rnd() % (dw - vx0), vh = 1 + rnd() % (dh - vy0);
    fv_u32 kx = wf_taps(sw, dw), ky = wf_taps(sh, dh), x, y, i, y0, y1;
    unsigned char *argb = malloc((size_t)sw * sh * 4), *rgb = malloc((size_t)sw * sh * 3);
    unsigned char *out4 = malloc((size_t)vw * vh * 4), *out3 = malloc((size_t)vw * vh * 3);
    unsigned char *mid = malloc((size_t)sh * vw * 3);
    fv_u32 *tx = malloc(vw * 8), *ty = malloc(vh * 8);
    wfk_s16 *wx = malloc((size_t)vw * kx * 2), *wy = malloc((size_t)vh * ky * 2);
    FvScene sc;
    int idx, idy;
    for (i = 0; i < sw * sh; i++) {
      unsigned v = t & 1 ? rnd() : (i * 7) ^ (i >> 3);
      argb[i * 4] = 255; argb[i * 4 + 1] = rgb[i * 3] = (unsigned char)v; argb[i * 4 + 2] = rgb[i * 3 + 1] = (unsigned char)(v >> 8);
      argb[i * 4 + 3] = rgb[i * 3 + 2] = (unsigned char)(v >> 16);
    }
    /* FitView: the scaled picture's part (vx0, vy0, vw, vh) is the window */
    memset(&sc, 0, sizeof sc);
    sc.src = argb; sc.sw = sw; sc.sh = sh; sc.sstride = sw * 4; sc.out = out4; sc.W = vw; sc.H = vh; sc.ostride = vw * 4;
    sc.dw = dw; sc.dh = dh; sc.px = -(fv_s32)vx0; sc.py = -(fv_s32)vy0;
    check(render(&sc, FV_REFINE, 999, 0) == 0, "wbfit render", t);
    /* WBFit: wfk_hpass over the rows needed, wfk_vpass */
    idx = wf_weights(sw, dw, vx0, vw, tx, wx, kx);
    idy = wf_weights(sh, dh, vy0, vh, ty, wy, ky);
    y0 = ty[0]; y1 = ty[0] + ty[1];
    for (y = 0; y < vh; y++) { if (ty[y * 2] < y0) y0 = ty[y * 2]; if (ty[y * 2] + ty[y * 2 + 1] > y1) y1 = ty[y * 2] + ty[y * 2 + 1]; }
    for (y = 0; y < vh; y++) ty[y * 2] -= y0;
    {
      struct WfkH h;
      struct WfkV v;
      memset(&h, 0, sizeof h); memset(&v, 0, sizeof v);
      h.range.from = y0; h.range.to = y1; h.src = rgb; h.sstride = sw * 3; h.out = mid; h.ostride = vw * 3; h.ow = vw; h.y0 = y0;
      h.tab = tx; h.wts = wx; h.k = kx; h.ident = (wfk_u32)idx;
      wfk_hpass(&h);
      v.range.from = 0; v.range.to = vh; v.in = mid; v.istride = vw * 3; v.out = out3; v.ostride = vw * 3; v.ow = vw;
      v.tab = ty; v.wts = wy; v.k = ky; v.ident = (wfk_u32)idy;
      wfk_vpass(&v);
    }
    for (i = 0, x = 0; i < vw * vh; i++)
      x |= out4[i * 4 + 1] != out3[i * 3] || out4[i * 4 + 2] != out3[i * 3 + 1] || out4[i * 4 + 3] != out3[i * 3 + 2] || out4[i * 4] != 255;
    check(!x, "same as WBFit's kernels", t);
    free(argb); free(rgb); free(out4); free(out3); free(mid); free(tx); free(ty); free(wx); free(wy);
  }
}

/* ================================================================== 4. pens, alpha */
static void t_dither(void)
{
  FILE *f = dump("dither.bin");
  int t;
  for (t = 0; t < 6; t++) {
    fv_u32 w = 3 + rnd() % 70, h = 2 + rnd() % 40, npal = t == 5 ? 1 : 2 + rnd() % 60, i, stride = w * 4 + 8, ostride = w + 3;
    unsigned char *px = malloc((size_t)stride * h), pal[768], pm[256], *out = malloc((size_t)ostride * h), *o2 = malloc((size_t)ostride * h);
    wfk_u16 *cache = malloc(65536);
    wfk_s32 *e0 = malloc((w + 2) * 12), *e1 = malloc((w + 2) * 12);
    struct FvkDither d;
    int fs = t & 1;
    unsigned long st;
    for (i = 0; i < (fv_u32)stride * h; i++) px[i] = (unsigned char)(t < 2 ? i * 3 : rnd());
    for (i = 0; i < 768; i++) pal[i] = (unsigned char)rnd();
    for (i = 0; i < 256; i++) pm[i] = (unsigned char)(255 - i * 3);
    for (st = 0; st < 3; st++) {
      static const unsigned long stops[] = { 0, 1, 5 };
      unsigned char *o = st ? o2 : out;
      fv_u32 from = 0;
      memset(cache, 0xFF, 65536); memset(e0, 0, (w + 2) * 12); memset(e1, 0, (w + 2) * 12);
      memset(&d, 0, sizeof d);
      d.px = px; d.pstride = stride; d.w = w; d.out = o; d.ostride = ostride; d.pal = pal; d.npal = npal; d.cache = cache;
      d.penmap = pm; d.fs = (wfk_u32)fs; d.e0 = e0; d.e1 = e1;
      wfk_test_budget = stops[st];
      while (from < h) { d.range.from = from; d.range.to = h; fvk_dither(&d); check(d.range.to > from, "dither progress", t); from = d.range.to; }
      wfk_test_budget = 0;
      if (st) check(!memcmp(o2, out, (size_t)ostride * h), "dither stopped", t);
    }
    w32(f, w); w32(f, h); w32(f, stride); w32(f, ostride); w32(f, npal); w32(f, (unsigned)fs);
    fwrite(px, 1, (size_t)stride * h, f); fwrite(pal, 1, 768, f); fwrite(pm, 1, 256, f); fwrite(out, 1, (size_t)ostride * h, f);
    free(px); free(out); free(o2); free(cache); free(e0); free(e1);
  }
  fclose(f);
}

static void t_comp(void)
{
  FILE *f = dump("comp.bin");
  int t;
  for (t = 0; t < 5; t++) {
    fv_u32 W = 5 + rnd() % 60, H = 3 + rnd() % 40, i;
    unsigned char *o = malloc((size_t)W * H * 4), *in = malloc((size_t)W * H * 4);
    struct FvkComp c;
    for (i = 0; i < W * H; i++) {
      unsigned a = t == 0 ? (i & 1) * 255 : rnd() & 255;
      in[i * 4] = (unsigned char)a;
      in[i * 4 + 1] = (unsigned char)(t == 4 ? 255 : rnd() % (a + 1)); in[i * 4 + 2] = (unsigned char)(rnd() % (a + 1)); in[i * 4 + 3] = (unsigned char)(rnd() % (a + 1));
    }
    memcpy(o, in, (size_t)W * H * 4);
    memset(&c, 0, sizeof c);
    c.range.from = 1; c.range.to = H; c.out = o; c.ostride = W * 4; c.x0 = 2; c.x1 = W - 1;
    c.c0 = rnd() & 0xFFFFFF; c.c1 = rnd() & 0xFFFFFF; c.shift = (wfk_u32)(t % 4); c.orgx = (fv_s32)(rnd() % 50) - 25; c.orgy = (fv_s32)(rnd() % 50) - 25;
    fvk_comp(&c);
    w32(f, W); w32(f, H); w32(f, c.c0); w32(f, c.c1); w32(f, c.shift); w32(f, (unsigned)c.orgx); w32(f, (unsigned)c.orgy);
    fwrite(in, 1, (size_t)W * H * 4, f); fwrite(o, 1, (size_t)W * H * 4, f);
    free(o); free(in);
  }
  fclose(f);
}

int main(int argc, char **argv)
{
  if (argc < 2) { fprintf(stderr, "usage: fitview_test DIR\n"); return 2; }
  dir = argv[1];
  t_geom();
  t_render();
  t_wbfit();
  t_dither();
  t_comp();
  printf("fitview_test: %d checks, %d failed\n", checks, fails);
  return fails != 0;
}
