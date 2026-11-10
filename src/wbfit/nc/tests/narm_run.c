/*
 * narm_run.c - WBFit's NativeCall module (wbfit.narm) under qemu-aarch64_be, Linux user mode,
 * no libc (make nctest NATIVECALL=1 NC_SDK=...). The module is copied to memory below 4 GB at
 * an address that is not 4 KB aligned, and its kernels are called through its entry table as
 * the hypervisor calls them, each call again from where the last one stopped (the kernels
 * watch the ARM counter, as on the Pi). The reference is the same kernels built here without
 * NEON, at -O1 without the vectorizers (the C the 68k runs):
 *   1. the vertical pass (the NEON one) on random rows, widths, taps and weights, extremes
 *      included; deflate on random, repetitive and incompressible data;
 *   2. whole jobs (wfcore.c's wf_fit, every kernel: resampling each way, borders, colour
 *      lookup and HAM, histogram, median cut, dithering, PNG filters, deflate, CRC, ILBM)
 *      through the module and through the reference: the same bytes at every stage and in the
 *      file.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "wfcore.h"

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
static unsigned long seed = 12345;
static NC_U32 rnd(void) { seed = seed * 6364136223846793005UL + 1442695040888963407UL; return (NC_U32)(seed >> 33); }
static int checks, fails;
static void check(int ok, const char *what, unsigned long n)
{
  checks++;
  if (!ok) { fails++; out("FAIL "); out(what); out(" "); num(n); out("\n"); }
}

/* memory below 4 GB (the binary is static at 4 MB): an arena */
static unsigned char arena[200 << 20] __attribute__((aligned(16)));
static unsigned long used;
static void *get(unsigned long n)
{
  void *p = arena + used;
  used += (n + 15) & ~15UL;
  if (used > sizeof arena) { out("arena full\n"); sys3(93, 2, 0, 0); }
  memset(p, 0, n);
  return p;
}

/* ------------------------------------------------------------------ the module */
typedef NC_U32 (*KFn)(void *);
static KFn mod[WFK_COUNT];
#define REF(n) n,
static const KFn ref[WFK_COUNT] = { WFK_LIST(REF) };
static unsigned char codearea[1 << 20] __attribute__((aligned(4096)));
static unsigned long calls, stops, kcalls[WFK_COUNT];
static int load(void)
{
  unsigned long n = (unsigned long)(narm_image_end - narm_image), i;
  unsigned char *m = codearea + 0x10;
  const NC_U32 *h = (const NC_U32 *)narm_image;
  if (n + 0x10 > sizeof codearea || h[0] != 0x4E41524DUL || h[2] != WFK_COUNT) return 0;
  memcpy(m, narm_image, n);
  if (sys3(226, (long)codearea, (long)sizeof codearea, 7) != 0) return 0;   /* mprotect rwx */
  for (i = 0; i < WFK_COUNT; i++) mod[i] = (KFn)(void *)(m + ((const NC_U32 *)m)[3 + i]);
  return 1;
}
/* the runners: the module (as wfnc.c calls it) or the reference C */
static int r_range(WfRun *r, int id, void *p, wf_u32 from, wf_u32 to)
{
  const KFn *t = r->ctx ? mod : ref;
  struct NCRange *rg = (struct NCRange *)p;
  while (from < to) {
    rg->from = from; rg->to = to;
    t[id](p);
    if (r->ctx) { calls++; kcalls[id]++; }
    if (rg->to <= from || rg->to > to) return WF_EFAULT;
    if (rg->to < to && r->ctx) stops++;
    from = rg->to;
  }
  return 0;
}
static wf_u32 r_call(WfRun *r, int id, void *p, int *f)
{
  const KFn *t = r->ctx ? mod : ref;
  wf_u32 v = t[id](p);
  *f = 0;
  if (r->ctx) { calls++; kcalls[id]++; if (v == WFK_MORE) stops++; }
  return v;
}
static void *r_alloc(WfRun *r, wf_u32 n) { (void)r; return get(n); }
static void r_free(WfRun *r, void *p, wf_u32 n) { (void)r; (void)p; (void)n; }
static void runner(WfRun *r, int native)
{
  r->range = r_range; r->call = r_call; r->alloc = r_alloc; r->free = r_free; r->now_us = 0; r->ctx = native ? (void *)1 : 0;
}

/* ------------------------------------------------------------------ 1. kernels on their own */
static void t_vpass(void)
{
  int t;
  for (t = 0; t < 400; t++) {
    unsigned long mark = used;
    NC_U32 ow = 1 + rnd() % (t < 300 ? 70 : 700), nb = ow * 3, rows = 1 + rnd() % 40, n = 1 + rnd() % rows, oh = 1 + rnd() % 6, k = n + (rnd() & 2), i, r;
    unsigned char *in = get(rows * nb + 16), *o1 = get(oh * nb), *o2 = get(oh * nb);
    NC_U32 *tab = get(oh * 8);
    wfk_s16 *w = get(oh * k * 2);
    struct WfkV *p1 = get(sizeof *p1), *p2 = get(sizeof *p2);
    WfRun rm, rr;
    for (i = 0; i < rows * nb; i++) in[i] = (unsigned char)(t % 5 == 0 ? (rnd() & 1) * 255 : rnd());
    for (r = 0; r < oh; r++) {
      tab[r * 2] = rnd() % (rows - n + 1); tab[r * 2 + 1] = n;
      for (i = 0; i < n; i++) w[r * k + i] = (wfk_s16)(t % 3 == 0 ? (int)(rnd() % 65536) - 32768 : (int)(rnd() % 20000) - 2000);
    }
    p1->in = WFK_SETP(in); p1->istride = nb; p1->ow = ow; p1->tab = WFK_SETP(tab); p1->wts = WFK_SETP(w); p1->k = k; p1->ostride = nb;
    *p2 = *p1;
    p1->out = WFK_SETP(o1); p2->out = WFK_SETP(o2);
    runner(&rm, 1); runner(&rr, 0);
    r_range(&rm, WFKI_wfk_vpass, p1, 0, oh);
    r_range(&rr, WFKI_wfk_vpass, p2, 0, oh);
    check(!memcmp(o1, o2, oh * nb), "vpass", (unsigned long)t);
    if (memcmp(o1, o2, oh * nb) && fails < 4) {
      for (i = 0; i < oh * nb && o1[i] == o2[i]; i++) ;
      out("  at "); num(i); out(" of "); num(oh * nb); out(" nb "); num(nb); out(" n "); num(n); out(": "); num(o1[i]); out(" vs "); num(o2[i]);
      {
        NC_U32 rr = i / nb, xx = i % nb, jj;
        long long v = 0;
        for (jj = 0; jj < n; jj++) v += (long long)w[rr * k + jj] * in[(tab[rr * 2] + jj) * nb + xx];
        out(" sum64 "); if (v < 0) { out("-"); v = -v; } num((unsigned long)v); out(" k "); num(k);
      }
      out("\n");
    }
    used = mark;
  }
}

static void t_hpass(void)
{
  int t;
  for (t = 0; t < 300; t++) {
    unsigned long mark = used;
    NC_U32 sw = 1 + rnd() % 300, rows = 1 + rnd() % 5, ow = 1 + rnd() % 200, k = 2 + (rnd() % 40), n, i, x;
    unsigned char *src = get(rows * sw * 3), *o1 = get(rows * ow * 3), *o2 = get(rows * ow * 3);
    NC_U32 *tab = get(ow * 8);
    wfk_s16 *w = get(ow * k * 2);
    struct WfkH *p1 = get(sizeof *p1), *p2 = get(sizeof *p2);
    WfRun rm, rr;
    for (i = 0; i < rows * sw * 3; i++) src[i] = (unsigned char)(t % 5 == 0 ? (rnd() & 1) * 255 : rnd());
    for (x = 0; x < ow; x++) {
      n = 1 + rnd() % (k < sw ? k : sw);
      tab[x * 2] = rnd() % (sw - n + 1); tab[x * 2 + 1] = n;
      for (i = 0; i < n; i++) w[x * k + i] = (wfk_s16)(t % 3 == 0 ? (int)(rnd() % 65536) - 32768 : (int)(rnd() % 20000) - 2000);
    }
    p1->src = WFK_SETP(src); p1->sstride = sw * 3; p1->ostride = ow * 3; p1->ow = ow; p1->tab = WFK_SETP(tab); p1->wts = WFK_SETP(w); p1->k = k;
    *p2 = *p1;
    p1->out = WFK_SETP(o1); p2->out = WFK_SETP(o2);
    runner(&rm, 1); runner(&rr, 0);
    r_range(&rm, WFKI_wfk_hpass, p1, 0, rows);
    r_range(&rr, WFKI_wfk_hpass, p2, 0, rows);
    check(!memcmp(o1, o2, rows * ow * 3), "hpass", (unsigned long)t);
    used = mark;
  }
}

static void t_deflate(void)
{
  int t;
  for (t = 0; t < 40; t++) {
    unsigned long mark = used;
    NC_U32 n = t < 30 ? 1 + rnd() % 20000 : 200000 + rnd() % 400000, i, cap = WFK_DEFLATE_BOUND(n);
    unsigned char *in = get(n), *o1 = get(cap), *o2 = get(cap);
    struct WfkDefState *s1 = get(sizeof *s1), *s2 = get(sizeof *s2);
    struct WfkDefl *p1 = get(sizeof *p1), *p2 = get(sizeof *p2);
    WfRun rm, rr;
    int f;
    NC_U32 v1, v2, g = 0;
    for (i = 0; i < n; i++)
      in[i] = (unsigned char)(t % 4 == 0 ? rnd() : t % 4 == 1 ? (i / 7) & 3 : t % 4 == 2 ? "abcabcabd"[i % 9] : (rnd() % 3 ? in[i ? i - 1 : 0] : rnd()));
    p1->in = WFK_SETP(in); p1->n = n; p1->ocap = cap; p1->chain = 16; p1->nice = 32; p1->good = 8;
    *p2 = *p1;
    p1->st = WFK_SETP(s1); p1->out = WFK_SETP(o1); p2->st = WFK_SETP(s2); p2->out = WFK_SETP(o2);
    runner(&rm, 1); runner(&rr, 0);
    do v1 = r_call(&rm, WFKI_wfk_deflate, p1, &f); while (v1 == WFK_MORE && ++g < 10000000);
    g = 0;
    do v2 = r_call(&rr, WFKI_wfk_deflate, p2, &f); while (v2 == WFK_MORE && ++g < 10000000);
    check(v1 == WFK_DONE && v2 == WFK_DONE && p1->olen == p2->olen && !memcmp(o1, o2, p1->olen), "deflate", (unsigned long)t);
    used = mark;
  }
}

/* ------------------------------------------------------------------ 2. whole jobs */
static wf_u8 *picture(int kind, wf_u32 w, wf_u32 h)
{
  wf_u8 *p = get((unsigned long)w * h * 3);
  wf_u32 x, y;
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) {
      wf_u8 *q = p + ((unsigned long)y * w + x) * 3;
      switch (kind) {
      case 0: q[0] = (wf_u8)(x * 255 / (w > 1 ? w - 1 : 1)); q[1] = (wf_u8)(y * 255 / (h > 1 ? h - 1 : 1)); q[2] = (wf_u8)((x + y) & 255); break;
      case 1: { int c = ((x / 3) ^ (y / 3)) & 1 ? 255 : 0; q[0] = q[1] = q[2] = (wf_u8)c; break; }
      case 2: q[0] = (wf_u8)rnd(); q[1] = (wf_u8)rnd(); q[2] = (wf_u8)rnd(); break;
      default: {
        long dx = (long)x - (long)w / 2, dy = (long)y - (long)h / 2;
        unsigned d = (unsigned)((dx * dx + dy * dy) / 7);
        q[0] = (wf_u8)(d & 255); q[1] = (wf_u8)(x < w / 3 ? 200 : 40); q[2] = (wf_u8)(y < h / 2 ? 30 : 220);
      }
      }
    }
  return p;
}
typedef struct { wf_u8 *p; unsigned long n, cap; } Mem;
static int mput(void *ctx, const void *p, wf_u32 n)
{
  Mem *m = (Mem *)ctx;
  if (m->n + n > m->cap) return WF_EWRITE;
  memcpy(m->p + m->n, p, n);
  m->n += n;
  return 0;
}
typedef struct { int kind; wf_u32 sw, sh, W, H, rx, ry; int mode, format, depth, dither, usepal, v40; } Case;
static const Case cases[] = {
  { 0, 320, 200, 160, 100, 1, 1, WF_FILL, WF_PNG, 0, 0, 0, 0 },
  { 1, 300, 240, 100, 80, 1, 1, WF_STRETCH, WF_PNG, 0, 0, 0, 0 },
  { 3, 64, 48, 200, 150, 1, 1, WF_FILL, WF_PNG, 0, 0, 0, 0 },
  { 0, 200, 100, 64, 64, 1, 1, WF_FIT, WF_PNG, 0, 0, 0, 0 },
  { 3, 120, 90, 160, 64, 22, 44, WF_FIT, WF_ILBM24, 0, 0, 0, 0 },
  { 2, 77, 33, 50, 61, 1, 1, WF_CENTER, WF_ILBM24, 0, 0, 0, 0 },
  { 0, 333, 222, 96, 60, 44, 22, WF_FILL, WF_ILBM, 3, 1, 1, 0 },
  { 3, 200, 150, 90, 70, 1, 1, WF_FIT, WF_ILBM, 8, 1, 0, 0 },
  { 0, 100, 100, 70, 70, 1, 1, WF_FILL, WF_ILBM, 4, 0, 0, 0 },
  { 0, 64, 40, 70, 50, 1, 1, WF_FILL, WF_PNG, 0, 0, 0, 1 },
  { 0, 64, 40, 70, 50, 1, 1, WF_STRETCH, WF_PNG, 0, 0, 0, 4 },
  { 0, 64, 40, 70, 50, 1, 1, WF_STRETCH, WF_PNG, 0, 0, 0, 6 },
  { 1, 1000, 30, 41, 7, 1, 1, WF_FILL, WF_PNG, 0, 0, 0, 0 },
  { 2, 400, 300, 640, 256, 22, 44, WF_FILL, WF_PNG, 0, 0, 0, 0 },
  /* the sizes of real use */
  { 3, 1024, 768, 1920, 1080, 1, 1, WF_FILL, WF_PNG, 0, 0, 0, 0 },
  { 0, 2000, 1500, 1280, 720, 1, 1, WF_FIT, WF_PNG, 0, 0, 0, 0 },
  { 2, 1600, 1200, 800, 600, 1, 1, WF_FILL, WF_ILBM24, 0, 0, 0, 0 },
  { 3, 1280, 960, 640, 512, 22, 22, WF_FILL, WF_ILBM, 8, 1, 0, 0 },
  { 0, 800, 600, 640, 256, 22, 44, WF_FILL, WF_ILBM, 3, 1, 1, 0 },
};
static const wf_u8 wbpal8[24] = { 170, 170, 170, 0, 0, 0, 255, 255, 255, 102, 136, 187, 238, 68, 68, 85, 204, 85, 0, 68, 221, 238, 153, 0 };

static void t_jobs(void)
{
  unsigned i;
  for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    const Case *c = &cases[i];
    unsigned long mark = used;
    WfJob j1, j2;
    WfRun rm, rr;
    Mem m1, m2;
    wf_u8 *src = picture(c->kind, c->sw, c->sh), *idx = 0, *ipal = get(768);
    int e1, e2, s;
    memset(&j1, 0, sizeof j1);
    if (c->v40) {
      wf_u32 k, aw = (c->sw + 15) & ~15U, x, y;
      idx = get((unsigned long)aw * c->sh);
      for (k = 0; k < 768; k++) ipal[k] = (wf_u8)(k * 37 + 11);
      for (y = 0; y < c->sh; y++)
        for (x = 0; x < c->sw; x++) idx[y * aw + x] = (wf_u8)(src[(y * c->sw + x) * 3] ^ src[(y * c->sw + x) * 3 + 2]) & (c->v40 == 4 ? 63 : 255);
      j1.idx = idx; j1.ipal = ipal; j1.ham = c->v40 == 1 ? 0 : (wf_u32)c->v40; j1.sstride = aw;
    } else { j1.src = src; j1.sstride = c->sw * 3; }
    j1.sw = c->sw; j1.sh = c->sh; j1.W = c->W; j1.H = c->H; j1.rx = c->rx; j1.ry = c->ry;
    j1.mode = c->mode; j1.format = c->format; j1.depth = c->depth; j1.dither = c->dither; j1.pngadaptive = 1; j1.bgauto = 1;
    if (c->usepal) { j1.pal = wbpal8; j1.npal = 1 << c->depth < 8 ? 1 << c->depth : 8; }
    j2 = j1;
    m1.cap = m2.cap = (unsigned long)c->W * c->H * 4 + 65536;
    m1.p = get(m1.cap); m2.p = get(m2.cap); m1.n = m2.n = 0;
    runner(&rm, 1); runner(&rr, 0);
    e1 = wf_fit(&rm, &j1, mput, &m1);
    e2 = wf_fit(&rr, &j2, mput, &m2);
    check(e1 == 0 && e2 == 0, "job ran", i);
    for (s = 0; s < WF_NST; s++) check(j1.sum[s] == j2.sum[s], "job stage", i * 10 + (unsigned)s);
    check(m1.n == m2.n && !memcmp(m1.p, m2.p, m1.n), "job file", i);
    used = mark;
  }
}

int main(void)
{
  static const char *const names[WFK_COUNT] = { "hpass", "vpass", "fill", "lut", "hist", "dither", "pngfilt", "deflate", "crc", "ilbm" };
  int i;
  if (!load()) { out("can't load the module\n"); return 2; }
  out("wbfit.narm: "); num((unsigned long)(narm_image_end - narm_image)); out(" bytes, "); num(WFK_COUNT); out(" kernels\n");
  t_vpass();
  t_hpass();
  t_deflate();
  t_jobs();
  out("native calls per kernel:");
  for (i = 0; i < WFK_COUNT; i++) { out(" "); out(names[i]); out(" "); num(kcalls[i]); check(kcalls[i] > 0, "kernel never ran natively", (unsigned long)i); }
  out("\nnctest: "); num((unsigned long)checks); out(" checks, "); num((unsigned long)fails); out(" failures; ");
  num(calls); out(" native calls, "); num(stops); out(" stopped by the clock and called again\n");
  return fails ? 1 : 0;
}
