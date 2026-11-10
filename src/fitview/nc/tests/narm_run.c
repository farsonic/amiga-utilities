/*
 * narm_run.c - FitView's NativeCall module (fitview.narm) under qemu-aarch64_be, Linux user mode,
 * no libc (make fvnctest NATIVECALL=1 NC_SDK=...). As WBFit's: the module is copied to memory
 * below 4 GB at an address that is not 4 KB aligned, and its kernels are called through its
 * entry table as the hypervisor calls them, each call again from where the last one stopped
 * (the kernels watch the ARM counter, as on the Pi). The reference is the same kernels built
 * here without NEON, at -O1 without the vectorizers (the C the 68k runs):
 *   1. every kernel on its own: random rows, widths, taps, weights (extremes included),
 *      palettes, alpha, checkerboards, pen maps, both ditherers;
 *   2. whole renders (fvrender.c: preview and refine, shrinking and enlarging, scrolled,
 *      32-bit and palette sources, alpha) at the sizes of real use, through the module and
 *      through the reference: the same bytes;
 *   3. the longest native call, by the counter (qemu's speed, not the Pi's).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "fvrender.h"

long sys3(long nr, long a, long b, long c);
extern const unsigned char narm_image[], narm_image_end[];

static void out(const char *s) { long n = 0; while (s[n]) n++; sys3(64, 1, (long)s, n); }
static void num(unsigned long v)
{
  char b[24];
  int i = 23;
  b[i] = 0;
  do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
  out(b + i);
}
static unsigned long seed = 4242;
static NC_U32 rnd(void) { seed = seed * 6364136223846793005UL + 1442695040888963407UL; return (NC_U32)(seed >> 33); }
static int checks, fails;
static void check(int ok, const char *what, unsigned long n)
{
  checks++;
  if (!ok) { fails++; if (fails < 30) { out("FAIL "); out(what); out(" "); num(n); out("\n"); } }
}

/* memory below 4 GB (the binary is static at 4 MB): an arena */
static unsigned char arena[300 << 20] __attribute__((aligned(16)));
static unsigned long used;
static void *get(unsigned long n)
{
  void *p = arena + used;
  used += (n + 15) & ~15UL;
  if (used > sizeof arena) { out("arena full\n"); sys3(93, 2, 0, 0); }
  memset(p, 0, n);
  return p;
}

static inline unsigned long long cnt(void) { unsigned long long v; __asm__ __volatile__("isb\n mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static unsigned long long freq(void) { unsigned long long f; __asm__ __volatile__("mrs %0, CNTFRQ_EL0" : "=r"(f)); return f ? f : 1; }

/* ------------------------------------------------------------------ the module */
typedef NC_U32 (*KFn)(void *);
static KFn mod[FVK_COUNT];
#define REF(n) n,
static const KFn ref[FVK_COUNT] = { FVK_LIST(REF) };
static unsigned char codearea[1 << 20] __attribute__((aligned(4096)));
static unsigned long calls, stops, kcalls[FVK_COUNT];
static unsigned long long longest;
static int load(void)
{
  unsigned long n = (unsigned long)(narm_image_end - narm_image), i;
  unsigned char *m = codearea + 0x10;
  const NC_U32 *h = (const NC_U32 *)narm_image;
  if (n + 0x10 > sizeof codearea || h[0] != 0x4E41524DUL || h[2] != FVK_COUNT) return 0;
  memcpy(m, narm_image, n);
  if (sys3(226, (long)codearea, (long)sizeof codearea, 7) != 0) return 0;   /* mprotect rwx */
  for (i = 0; i < FVK_COUNT; i++) mod[i] = (KFn)(void *)(m + ((const NC_U32 *)m)[3 + i]);
  return 1;
}
/* the runners: the module (as fvnc.c calls it) or the reference C */
static int r_range(FvRun *r, int id, void *p, fv_u32 from, fv_u32 to)
{
  const KFn *t = r->ctx ? mod : ref;
  struct NCRange *rg = (struct NCRange *)p;
  while (from < to) {
    unsigned long long t0 = cnt(), dt;
    rg->from = from; rg->to = to;
    t[id](p);
    dt = cnt() - t0;
    if (r->ctx) { calls++; kcalls[id]++; if (dt > longest) longest = dt; }
    if (rg->to <= from || rg->to > to) return FV_EFAULT;
    if (rg->to < to && r->ctx) stops++;
    from = rg->to;
  }
  return 0;
}
static void *r_alloc(FvRun *r, fv_u32 n) { (void)r; return get(n); }
static void r_free(FvRun *r, void *p, fv_u32 n) { (void)r; (void)p; (void)n; }
static void runner(FvRun *r, int native) { r->range = r_range; r->alloc = r_alloc; r->free = r_free; r->ctx = native ? (void *)1 : 0; }

/* the same parameters through both, outputs compared */
static void both(int id, void *p1, void *p2, unsigned long psz, fv_u32 from, fv_u32 to, const unsigned char *o1, const unsigned char *o2,
                 unsigned long on, const char *what, unsigned long t)
{
  FvRun rm, rr;
  (void)psz;
  runner(&rm, 1); runner(&rr, 0);
  check(r_range(&rm, id, p1, from, to) == 0, what, t);
  check(r_range(&rr, id, p2, from, to) == 0, what, t);
  check(!memcmp(o1, o2, on), what, t);
}

/* ------------------------------------------------------------------ 1. kernels on their own */
static void t_vpass(void)
{
  int t;
  for (t = 0; t < 400; t++) {
    unsigned long mark = used;
    NC_U32 nb = 1 + rnd() % (t < 300 ? 280 : 2800), rows = 1 + rnd() % 40, n = 1 + rnd() % rows, oh = 1 + rnd() % 6, k = n + (rnd() & 2), i, r;
    NC_U32 base = rnd() % 5;
    unsigned char *in = get(rows * nb + 16), *o1 = get(oh * nb), *o2 = get(oh * nb);
    NC_U32 *tab = get(oh * 8);
    wfk_s16 *w = get(oh * k * 2);
    struct FvkV *p1 = get(sizeof *p1), *p2 = get(sizeof *p2);
    for (i = 0; i < rows * nb; i++) in[i] = (unsigned char)(t % 5 == 0 ? (rnd() & 1) * 255 : rnd());
    for (r = 0; r < oh; r++) {
      tab[r * 2] = base + rnd() % (rows - n + 1); tab[r * 2 + 1] = n;
      for (i = 0; i < n; i++) w[r * k + i] = (wfk_s16)(t % 3 == 0 ? (int)(rnd() % 65536) - 32768 : (int)(rnd() % 20000) - 2000);
    }
    p1->in = WFK_SETP(in); p1->istride = nb; p1->nb = nb; p1->tab = WFK_SETP(tab); p1->wts = WFK_SETP(w); p1->k = k; p1->ostride = nb;
    p1->base = base; p1->ident = t % 50 == 7;
    *p2 = *p1;
    p1->out = WFK_SETP(o1); p2->out = WFK_SETP(o2);
    both(FVKI_fvk_vpass, p1, p2, sizeof *p1, 0, oh, o1, o2, oh * nb, "vpass", (unsigned long)t);
    used = mark;
  }
}

static void t_hpass(void)
{
  int t;
  for (t = 0; t < 300; t++) {
    unsigned long mark = used;
    int usepal = t & 1;
    NC_U32 sw = 1 + rnd() % 300, rows = 1 + rnd() % 5, ow = 1 + rnd() % 200, k = 2 + (rnd() % 40), n, i, x, bpp = usepal ? 1 : 4;
    unsigned char *src = get(rows * sw * bpp), *pal = get(1024), *o1 = get(rows * ow * 4), *o2 = get(rows * ow * 4);
    NC_U32 *tab = get(ow * 8);
    wfk_s16 *w = get(ow * k * 2);
    struct FvkH *p1 = get(sizeof *p1), *p2 = get(sizeof *p2);
    for (i = 0; i < rows * sw * bpp; i++) src[i] = (unsigned char)(t % 5 == 0 ? (rnd() & 1) * 255 : rnd());
    for (i = 0; i < 1024; i++) pal[i] = (unsigned char)rnd();
    for (x = 0; x < ow; x++) {
      n = 1 + rnd() % (k < sw ? k : sw);
      tab[x * 2] = rnd() % (sw - n + 1); tab[x * 2 + 1] = n;
      for (i = 0; i < n; i++) w[x * k + i] = (wfk_s16)(t % 3 == 0 ? (int)(rnd() % 65536) - 32768 : (int)(rnd() % 20000) - 2000);
    }
    if (t % 40 == 3) { tab[0] = 0; p1->ident = 1; if (ow > sw) ow = sw; }
    p1->src = WFK_SETP(src); p1->sstride = sw * bpp; p1->pal = usepal ? WFK_SETP(pal) : 0; p1->ostride = ow * 4; p1->ow = ow;
    p1->tab = WFK_SETP(tab); p1->wts = WFK_SETP(w); p1->k = k;
    *p2 = *p1;
    p1->out = WFK_SETP(o1); p2->out = WFK_SETP(o2);
    both(FVKI_fvk_hpass, p1, p2, sizeof *p1, 0, rows, o1, o2, rows * ow * 4, "hpass", (unsigned long)t);
    used = mark;
  }
}

static void t_small(void)
{
  int t;
  for (t = 0; t < 200; t++) {
    unsigned long mark = used;
    NC_U32 W = 1 + rnd() % 400, H = 1 + rnd() % 30, i, n = W * H * 4;
    unsigned char *a = get(n), *b = get(n), *pal = get(1024), *src = get(n);
    for (i = 0; i < n; i++) a[i] = b[i] = src[i] = (unsigned char)rnd();
    for (i = 0; i < 1024; i++) pal[i] = (unsigned char)rnd();
    switch (t % 4) {
    case 0: {                                  /* fill */
      struct FvkFill *p1 = get(sizeof *p1), *p2 = get(sizeof *p2);
      p1->out = WFK_SETP(a); p1->ostride = W * 4; p1->x0 = rnd() % W; p1->x1 = p1->x0 + rnd() % (W - p1->x0 + 1); p1->argb = rnd();
      *p2 = *p1; p2->out = WFK_SETP(b);
      both(FVKI_fvk_fill, p1, p2, sizeof *p1, 0, H, a, b, n, "fill", (unsigned long)t);
      break;
    }
    case 1: {                                  /* comp */
      struct FvkComp *p1 = get(sizeof *p1), *p2 = get(sizeof *p2);
      for (i = 0; i < W * H; i++) { a[i * 4] = b[i * 4] = (unsigned char)(t % 8 == 1 ? (i & 1) * 255 : rnd()); }
      p1->out = WFK_SETP(a); p1->ostride = W * 4; p1->x0 = 0; p1->x1 = W; p1->c0 = rnd(); p1->c1 = rnd(); p1->shift = rnd() % 6;
      p1->orgx = (NC_S32)(rnd() % 100) - 50; p1->orgy = (NC_S32)(rnd() % 100) - 50;
      *p2 = *p1; p2->out = WFK_SETP(b);
      both(FVKI_fvk_comp, p1, p2, sizeof *p1, 0, H, a, b, n, "comp", (unsigned long)t);
      break;
    }
    case 2: {                                  /* nn */
      struct FvkNN *p1 = get(sizeof *p1), *p2 = get(sizeof *p2);
      NC_U32 sw = 1 + rnd() % 300, sh = 1 + rnd() % 50, *xt = get(W * 4), *yt = get(H * 4);
      int usepal = (t / 4) & 1;
      for (i = 0; i < W; i++) xt[i] = (rnd() % sw) * (usepal ? 1 : 4);
      for (i = 0; i < H; i++) yt[i] = rnd() % sh;
      src = get(sw * sh * 4);
      for (i = 0; i < sw * sh * 4; i++) src[i] = (unsigned char)rnd();
      p1->src = WFK_SETP(src); p1->sstride = usepal ? sw : sw * 4; p1->pal = usepal ? WFK_SETP(pal) : 0; p1->out = WFK_SETP(a);
      p1->ostride = W * 4; p1->ow = W; p1->xt = WFK_SETP(xt); p1->yt = WFK_SETP(yt);
      *p2 = *p1; p2->out = WFK_SETP(b);
      both(FVKI_fvk_nn, p1, p2, sizeof *p1, 0, H, a, b, n, "nn", (unsigned long)t);
      break;
    }
    default: {                                 /* dither */
      struct FvkDither *p1 = get(sizeof *p1), *p2 = get(sizeof *p2);
      unsigned char *o1 = get(W * H), *o2 = get(W * H), *pm = get(256);
      wfk_u16 *c1 = get(65536), *c2 = get(65536);
      NC_S32 *e0 = get((W + 2) * 12), *e1 = get((W + 2) * 12), *f0 = get((W + 2) * 12), *f1 = get((W + 2) * 12);
      for (i = 0; i < 256; i++) pm[i] = (unsigned char)rnd();
      memset(c1, 0xFF, 65536); memset(c2, 0xFF, 65536);
      p1->px = WFK_SETP(src); p1->pstride = W * 4; p1->w = W; p1->ostride = W; p1->pal = WFK_SETP(pal); p1->npal = 1 + rnd() % 256;
      p1->penmap = WFK_SETP(pm); p1->fs = (t / 4) & 1;
      *p2 = *p1;
      p1->out = WFK_SETP(o1); p1->cache = WFK_SETP(c1); p1->e0 = WFK_SETP(e0); p1->e1 = WFK_SETP(e1);
      p2->out = WFK_SETP(o2); p2->cache = WFK_SETP(c2); p2->e0 = WFK_SETP(f0); p2->e1 = WFK_SETP(f1);
      both(FVKI_fvk_dither, p1, p2, sizeof *p1, 0, H, o1, o2, W * H, "dither", (unsigned long)t);
      break;
    }
    }
    used = mark;
  }
}

/* ------------------------------------------------------------------ 2. whole renders */
typedef struct { fv_u32 sw, sh; int kind, usepal, alpha; fv_u32 W, H; int mode; fv_u32 zoom; fv_u32 sx, sy; } Case;
static const Case cases[] = {
  { 640, 480, 0, 0, 0, 400, 300, FV_FIT, 0, 0, 0 },
  { 640, 480, 2, 1, 0, 400, 300, FV_FIT, 0, 0, 0 },
  { 300, 200, 1, 0, 1, 500, 500, FV_FIT, 0, 0, 0 },
  { 800, 600, 0, 0, 0, 300, 200, FV_ZOOM, 3000, 700, 500 },
  { 800, 600, 2, 0, 1, 300, 200, FV_ONE, 0, 123, 77 },
  { 2000, 300, 0, 1, 0, 640, 480, FV_WIDTH, 0, 0, 0 },
  { 300, 2000, 1, 0, 0, 640, 480, FV_WIDTH, 0, 0, 900 },
  { 1000, 1000, 2, 0, 0, 640, 480, FV_FILL, 0, 0, 50 },
  /* the sizes of real use: a photo in a big RTG window, a page */
  { 1024, 768, 0, 0, 0, 1880, 1000, FV_FIT, 0, 0, 0 },
  { 4000, 3000, 2, 0, 0, 1200, 900, FV_FIT, 0, 0, 0 },
  { 1240, 1754, 1, 0, 0, 1900, 1000, FV_WIDTH, 0, 0, 400 },
  { 1600, 1200, 0, 0, 1, 1280, 960, FV_FIT, 0, 0, 0 },
};

static fv_u8 *picture(int kind, int alpha, fv_u32 w, fv_u32 h)
{
  fv_u8 *p = get((unsigned long)w * h * 4);
  fv_u32 x, y;
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) {
      fv_u8 *q = p + ((unsigned long)y * w + x) * 4;
      unsigned a = alpha ? (x * 7 + y * 3) & 255 : 255;
      switch (kind) {
      case 0: q[1] = (fv_u8)(x * 255 / (w > 1 ? w - 1 : 1)); q[2] = (fv_u8)(y * 255 / (h > 1 ? h - 1 : 1)); q[3] = (fv_u8)((x + y) & 255); break;
      case 1: { int c = ((x / 3) ^ (y / 3)) & 1 ? 255 : 0; q[1] = q[2] = q[3] = (fv_u8)c; break; }
      default: q[1] = (fv_u8)rnd(); q[2] = (fv_u8)rnd(); q[3] = (fv_u8)rnd(); break;
      }
      q[0] = (fv_u8)a;
      if (alpha) { q[1] = (fv_u8)(q[1] * a / 255); q[2] = (fv_u8)(q[2] * a / 255); q[3] = (fv_u8)(q[3] * a / 255); }
    }
  return p;
}

static void t_renders(void)
{
  unsigned i;
  for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    const Case *c = &cases[i];
    unsigned long mark = used;
    fv_u8 *src = picture(c->kind, c->alpha, c->sw, c->sh), *idx = 0, *pal = 0;
    fv_u8 *o1 = get((unsigned long)c->W * c->H * 4), *o2 = get((unsigned long)c->W * c->H * 4);
    FvGeom g;
    FvScene s;
    int q;
    memset(&g, 0, sizeof g);
    g.sw = c->sw; g.sh = c->sh; g.W = c->W; g.H = c->H; g.mode = c->mode; g.zoom = c->zoom;
    fv_geom(&g);
    memset(&s, 0, sizeof s);
    if (c->usepal) {
      fv_u32 k;
      idx = get((unsigned long)c->sw * c->sh); pal = get(1024);
      for (k = 0; k < 1024; k++) pal[k] = (fv_u8)(k * 37 + 11);
      for (k = 0; k < 256; k++) pal[k * 4] = 255;
      for (k = 0; k < c->sw * c->sh; k++) idx[k] = (fv_u8)(src[k * 4 + 1] ^ src[k * 4 + 3]);
      s.src = idx; s.sstride = c->sw; s.pal = pal;
    } else { s.src = src; s.sstride = c->sw * 4; }
    s.sw = c->sw; s.sh = c->sh; s.alpha = c->alpha;
    s.W = c->W; s.H = c->H; s.ostride = c->W * 4; s.dw = g.dw; s.dh = g.dh;
    fv_place(&g, c->sx, c->sy, &s.px, &s.py);
    s.bg = 0x102030; s.chk0 = 0x999999; s.chk1 = 0x666666; s.chkshift = c->alpha ? 3 : 0;
    for (q = 0; q < 2; q++) {
      FvRun rm, rr;
      FvJob j;
      fv_u32 a, b;
      int e1 = 0, e2 = 0;
      runner(&rm, 1); runner(&rr, 0);
      s.out = o1;
      if (!(e1 = fv_job_begin(&rm, &j, &s, q))) while ((e1 = fv_job_step(&rm, &j, 200000, &a, &b)) > 0) ;
      s.out = o2;
      if (!(e2 = fv_job_begin(&rr, &j, &s, q))) while ((e2 = fv_job_step(&rr, &j, 200000, &a, &b)) > 0) ;
      check(e1 == 0 && e2 == 0, "render ran", i * 2 + (unsigned)q);
      check(!memcmp(o1, o2, (unsigned long)c->W * c->H * 4), q ? "render refine" : "render preview", i);
    }
    used = mark;
  }
}

int main(void)
{
  static const char *const names[FVK_COUNT] = { "hpass", "vpass", "nn", "fill", "comp", "dither" };
  int i;
  if (!load()) { out("can't load the module\n"); return 2; }
  out("fitview.narm: "); num((unsigned long)(narm_image_end - narm_image)); out(" bytes, "); num(FVK_COUNT); out(" kernels\n");
  t_vpass();
  t_hpass();
  t_small();
  t_renders();
  out("native calls per kernel:");
  for (i = 0; i < FVK_COUNT; i++) { out(" "); out(names[i]); out(" "); num(kcalls[i]); check(kcalls[i] > 0, "kernel never ran natively", (unsigned long)i); }
  out("\nlongest native call (qemu, not the Pi): "); num((unsigned long)(longest * 1000000ULL / freq())); out(" us");
  out("\nfvnctest: "); num((unsigned long)checks); out(" checks, "); num((unsigned long)fails); out(" failures; ");
  num(calls); out(" native calls, "); num(stops); out(" stopped by the clock and called again\n");
  return fails ? 1 : 0;
}
