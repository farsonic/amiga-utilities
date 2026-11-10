/*
 * wbfit_test.c - WBFit's host tests (make test-wbfit; ASan/UBSan, WFK_TEST_BUDGET):
 *   - the weights: every output's taps add up to 1 << 14, the identity is found;
 *   - whole jobs (generated pictures only) run to the end and with the kernels stopped after
 *     1, 2, 3, 7, 50 clock checks (called again from where they stopped, as natively): the
 *     same bytes every time; their source, scaled pixels, indices and files are written for
 *     check.py (the references in Python, Pillow, zlib, our own ILBM reader);
 *   - WBPattern.prefs: the bytes for a new file and for an old one with all three places.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wfcore.h"
#include "wfprefs.h"

static int checks, fails;
static void check(int ok, const char *what, int n)
{
  checks++;
  if (!ok) { fails++; printf("FAIL %s %d\n", what, n); }
}
static unsigned long seed = 1;
static unsigned rnd(void) { seed = seed * 1103515245UL + 12345UL; return (unsigned)(seed >> 16) & 0x7FFF; }

static void *h_alloc(WfRun *r, wf_u32 n) { (void)r; return calloc(1, n); }
static void h_free(WfRun *r, void *p, wf_u32 n) { (void)r; (void)n; free(p); }

/* ---- generated pictures */
static wf_u8 *picture(int kind, wf_u32 w, wf_u32 h)
{
  wf_u8 *p = malloc((size_t)w * h * 3);
  wf_u32 x, y;
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) {
      wf_u8 *q = p + ((size_t)y * w + x) * 3;
      switch (kind) {
      case 0: q[0] = (wf_u8)(x * 255 / (w > 1 ? w - 1 : 1)); q[1] = (wf_u8)(y * 255 / (h > 1 ? h - 1 : 1)); q[2] = (wf_u8)((x + y) & 255); break;
      case 1: { int c = ((x / 3) ^ (y / 3)) & 1 ? 255 : 0; q[0] = q[1] = q[2] = (wf_u8)c; break; }   /* fine checks: aliasing */
      case 2: q[0] = (wf_u8)rnd(); q[1] = (wf_u8)rnd(); q[2] = (wf_u8)rnd(); break;
      default: {                                  /* rings and a few flat colours */
        long dx = (long)x - (long)w / 2, dy = (long)y - (long)h / 2;
        unsigned d = (unsigned)((dx * dx + dy * dy) / 7);
        q[0] = (wf_u8)(d & 255); q[1] = (wf_u8)(x < w / 3 ? 200 : 40); q[2] = (wf_u8)(y < h / 2 ? 30 : 220);
      }
      }
    }
  return p;
}

static void dump(const char *dir, const char *name, int i, const void *p, size_t n)
{
  char f[512];
  FILE *fp;
  snprintf(f, sizeof f, "%s/c%02d.%s", dir, i, name);
  fp = fopen(f, "wb");
  if (!fp) { printf("cannot write %s\n", f); exit(2); }
  if (n) fwrite(p, 1, n, fp);
  fclose(fp);
}
typedef struct { unsigned char *p; size_t n, cap; } Mem;
static int mput(void *ctx, const void *p, wf_u32 n)
{
  Mem *m = (Mem *)ctx;
  if (m->n + n > m->cap) { m->cap = (m->n + n) * 2; m->p = realloc(m->p, m->cap); }
  memcpy(m->p + m->n, p, n);
  m->n += n;
  return 0;
}

typedef struct {
  int kind; wf_u32 sw, sh, W, H, rx, ry; int mode, format, depth, dither, usepal, bgauto; wf_u32 bg; int v40ham;
} Case;
static const Case cases[] = {
  /* shrinking, enlarging, each mode, the aspect, the formats */
  { 0, 320, 200, 160, 100, 1, 1, WF_FILL, WF_PNG, 0, 0, 0, 0, 0, 0 },
  { 1, 300, 240, 100, 80, 1, 1, WF_STRETCH, WF_PNG, 0, 0, 0, 0, 0, 0 },
  { 3, 64, 48, 200, 150, 1, 1, WF_FILL, WF_PNG, 0, 0, 0, 0, 0, 0 },
  { 0, 200, 100, 64, 64, 1, 1, WF_FIT, WF_PNG, 0, 0, 0, 0, 0x123456, 0 },
  { 3, 120, 90, 160, 64, 22, 44, WF_FILL, WF_PNG, 0, 0, 0, 1, 0, 0 },      /* PAL hires: pixels twice as tall */
  { 3, 120, 90, 160, 64, 22, 44, WF_FIT, WF_ILBM24, 0, 0, 0, 1, 0, 0 },
  { 2, 77, 33, 50, 61, 1, 1, WF_CENTER, WF_ILBM24, 0, 0, 0, 0, 0xFF8000, 0 },
  { 2, 40, 30, 23, 17, 1, 1, WF_CENTER, WF_PNG, 0, 0, 0, 0, 0x00FF00, 0 },
  { 0, 333, 222, 96, 60, 44, 22, WF_FILL, WF_ILBM, 3, 1, 1, 0, 0, 0 },     /* 8 colours, the "screen's" palette, FS */
  { 3, 200, 150, 90, 70, 1, 1, WF_FIT, WF_ILBM, 8, 1, 0, 1, 0, 0 },       /* median cut 256, FS */
  { 0, 100, 100, 70, 70, 1, 1, WF_FILL, WF_ILBM, 4, 0, 0, 0, 0, 0 },      /* median cut 16, nearest */
  { 3, 100, 80, 64, 48, 1, 1, WF_FILL, WF_ILBM, 2, 0, 1, 0, 0, 0 },       /* 4 colours, nearest */
  { 0, 64, 40, 70, 50, 1, 1, WF_FILL, WF_PNG, 0, 0, 0, 0, 0, 1 },         /* old picture.datatype: indices */
  { 0, 64, 40, 70, 50, 1, 1, WF_STRETCH, WF_PNG, 0, 0, 0, 0, 0, 4 },      /* HAM6 */
  { 0, 64, 40, 70, 50, 1, 1, WF_STRETCH, WF_PNG, 0, 0, 0, 0, 0, 6 },      /* HAM8 */
  { 1, 1000, 30, 41, 7, 1, 1, WF_FILL, WF_PNG, 0, 0, 0, 0, 0, 0 },         /* a wide strip */
  { 2, 1, 1, 13, 9, 1, 1, WF_FILL, WF_PNG, 0, 0, 0, 0, 0, 0 },             /* one pixel */
  { 2, 400, 300, 640, 256, 22, 44, WF_FILL, WF_PNG, 0, 0, 0, 0, 0, 0 },    /* noise, a bigger deflate */
};
#define NCASES (int)(sizeof cases / sizeof cases[0])

static const wf_u8 wbpal8[24] = { 170, 170, 170, 0, 0, 0, 255, 255, 255, 102, 136, 187, 238, 68, 68, 85, 204, 85, 0, 68, 221, 238, 153, 0 };

static void t_weights(void)
{
  static const wf_u32 sz[][2] = { { 10, 3 }, { 3, 10 }, { 7, 7 }, { 1000, 41 }, { 41, 1000 }, { 1, 9 }, { 9, 1 }, { 640, 639 }, { 2, 3 }, { 5000, 20 } };
  unsigned i, o, m;
  for (i = 0; i < sizeof sz / sizeof sz[0]; i++) {
    wf_u32 s = sz[i][0], d = sz[i][1], k = wf_taps(s, d), *tab = malloc(d * 8);
    wfk_s16 *w = malloc((size_t)d * k * 2);
    int id = wf_weights(s, d, 0, d, tab, w, k);
    check(id >= 0, "weights", (int)i);
    check(id == (s == d), "identity", (int)i);
    for (o = 0; o < d && id >= 0; o++) {
      int sum = 0;
      for (m = 0; m < tab[o * 2 + 1]; m++) sum += w[o * k + m];
      check(sum == 1 << WFK_WBITS, "weight sum", (int)(i * 1000 + o));
      check(tab[o * 2] + tab[o * 2 + 1] <= s, "taps inside", (int)(i * 1000 + o));
    }
    free(tab); free(w);
  }
}

static void t_place(void)
{
  WfPlace p;
  /* a 4:3 picture on PAL hires (640x256, pixels 1:2): FILL keeps the picture's shape physically */
  wf_place(&p, 400, 300, 640, 256, 22, 44, WF_FILL);
  /* physically the screen is 640*22 x 256*44 = 14080 x 11264 (5:4); the picture 4:3 is wider:
     the height decides: dh 256, dw = 400 * 256 * 44 / (300 * 22) = 682.67 -> 683 */
  check(p.dh == 256 && p.dw == 683 && p.px == -21 && p.vw == 640 && p.vh == 256, "place-pal-fill", 1);
  wf_place(&p, 400, 300, 640, 256, 22, 44, WF_FIT);
  check(p.dw == 640 && p.dh == 240 && p.py == 8 && p.oy == 8 && p.vh == 240, "place-pal-fit", 2);
  wf_place(&p, 100, 50, 30, 40, 1, 1, WF_CENTER);
  check(p.dw == 100 && p.dh == 50 && p.vx0 == 35 && p.vy0 == 5 && p.vw == 30 && p.vh == 40, "place-center", 3);
  check(wf_place(&p, 0, 5, 10, 10, 1, 1, WF_FILL) != 0, "place-zero", 4);
}

static void t_jobs(const char *dir)
{
  FILE *man;
  char f[512];
  int i;
  static const unsigned long budgets[] = { 1, 2, 3, 7, 50 };
  snprintf(f, sizeof f, "%s/cases.txt", dir);
  man = fopen(f, "w");
  for (i = 0; i < NCASES; i++) {
    const Case *c = &cases[i];
    WfRun r;
    WfJob j;
    Mem m0 = { 0, 0, 0 };
    wf_u8 *src = picture(c->kind, c->sw, c->sh), *idx = 0, ipal[768];
    wf_u32 b;
    int e;
    wf_run_plain(&r, h_alloc, h_free, NULL);
    memset(&j, 0, sizeof j);
    if (c->v40ham) {                              /* indices: made from the picture (low bits), a palette */
      wf_u32 k, n = c->sw * c->sh, aw = (c->sw + 15) & ~15U, x, y;
      idx = calloc(1, (size_t)aw * c->sh);
      for (k = 0; k < 768; k++) ipal[k] = (wf_u8)(k * 37 + 11);
      for (y = 0; y < c->sh; y++)
        for (x = 0; x < c->sw; x++) idx[y * aw + x] = (wf_u8)(src[(y * c->sw + x) * 3] ^ src[(y * c->sw + x) * 3 + 2]);
      if (c->v40ham == 4) for (k = 0; k < aw * c->sh; k++) idx[k] &= 63;
      (void)n;
      j.idx = idx; j.ipal = ipal; j.ham = c->v40ham == 1 ? 0 : (wf_u32)c->v40ham; j.sstride = aw;
      dump(dir, "idxin", i, idx, (size_t)aw * c->sh);
      dump(dir, "ipal", i, ipal, 768);
    } else { j.src = src; j.sstride = c->sw * 3; }
    j.sw = c->sw; j.sh = c->sh; j.W = c->W; j.H = c->H; j.rx = c->rx; j.ry = c->ry;
    j.mode = c->mode; j.format = c->format; j.depth = c->depth; j.dither = c->dither; j.pngadaptive = 1;
    j.bg = c->bg; j.bgauto = c->bgauto;
    if (c->usepal) { j.pal = wbpal8; j.npal = 1 << c->depth < 8 ? 1 << c->depth : 8; }
    wfk_test_budget = 0;
    e = wf_fit(&r, &j, mput, &m0);
    check(e == 0, "fit", i);
    if (e) { printf("  error %s\n", wf_errstr(e)); continue; }
    check(m0.n == j.filebytes, "filebytes", i);
    dump(dir, "src", i, src, (size_t)c->sw * c->sh * 3);
    dump(dir, "rgb", i, j.rgb, (size_t)c->W * c->H * 3);
    if (c->format == WF_ILBM) { dump(dir, "ind", i, j.ind, (size_t)c->W * c->H); dump(dir, "pal", i, j.opal, (size_t)j.ncol * 3); }
    dump(dir, c->format == WF_PNG ? "png" : "ilbm", i, m0.p, m0.n);
    fprintf(man, "%d %d %u %u %u %u %u %u %d %d %d %d %d %d %u %d %d %u %u %d %d %u %u %u %u\n", i, c->kind, c->sw, c->sh, c->W, c->H, c->rx, c->ry,
            c->mode, c->format, c->depth, c->dither, c->usepal, c->bgauto, j.bgused, c->v40ham, j.ncol, j.pl.dw, j.pl.dh, j.pl.px, j.pl.py,
            j.pl.vx0, j.pl.vy0, j.pl.vw, j.pl.vh);
    /* again with the kernels stopping everywhere: the same file */
    for (b = 0; b < sizeof budgets / sizeof budgets[0]; b++) {
      WfJob k = j;
      Mem m1 = { 0, 0, 0 };
      int s;
      k.rgb = k.out = k.enc = k.ind = 0; k.encn = k.enccap = k.outcap = k.indcap = 0; k.crcs = 0; k.ncrc = 0;
      wfk_test_budget = budgets[b];
      e = wf_fit(&r, &k, mput, &m1);
      check(e == 0 && m1.n == m0.n && !memcmp(m1.p, m0.p, m0.n), "resumed", i * 10 + (int)b);
      for (s = 0; s < WF_NST; s++) check(k.sum[s] == j.sum[s], "stage sums", i * 10 + s);
      wf_job_free(&r, &k);
      free(m1.p);
    }
    wfk_test_budget = 0;
    wf_job_free(&r, &j);
    free(m0.p); free(src); free(idx);
  }
  fclose(man);
}

/* ---- prefs */
static void be16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static void be32(unsigned char *p, unsigned long v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }
static size_t ptrn(unsigned char *o, int which, unsigned flags, int depth, const char *data, unsigned dl)
{
  size_t len = 24 + dl;
  memcpy(o, "PTRN", 4); be32(o + 4, len);
  memset(o + 8, 0, 24);
  be16(o + 8 + 16, (unsigned)which); be16(o + 8 + 18, flags); o[8 + 20] = 0; o[8 + 21] = (unsigned char)depth; be16(o + 8 + 22, dl);
  memcpy(o + 8 + 24, data, dl);
  if (len & 1) o[8 + len++] = 0;
  return 8 + len;
}
static void t_prefs(const char *dir)
{
  static unsigned char out[4096], old[4096], exp[4096];
  const char *name = "SYS:Prefs/Presets/WBFit/backdrop-640x256.ilbm";
  unsigned long n;
  size_t p, q;
  int ok;
  unsigned fl;
  char nm[256];
  /* a new file */
  n = wfp_build(NULL, 0, name, out, sizeof out, &ok);
  p = 0;
  memcpy(exp, "FORM", 4); memcpy(exp + 8, "PREF", 4); p = 12;
  memcpy(exp + p, "PRHD", 4); be32(exp + p + 4, 6); memset(exp + p + 8, 0, 6); p += 14;
  p += ptrn(exp + p, 0, 0, 0, name, (unsigned)strlen(name) + 1);
  be32(exp + 4, p - 8);
  check(n == p && !memcmp(out, exp, p), "prefs-new", 0);
  check(wfp_get(out, n, WFP_ROOT, nm, sizeof nm, &fl) && !strcmp(nm, name) && fl == 0, "prefs-get", 0);
  dump(dir, "prefs", 90, out, n);
  /* an old one: PRHD, ROOT a pattern, DRAWER a picture (odd length), SCREEN a picture with
     dither flags, an unknown chunk: all but ROOT kept, in order */
  memcpy(old, "FORM", 4); memcpy(old + 8, "PREF", 4); q = 12;
  memcpy(old + q, "PRHD", 4); be32(old + q + 4, 6); memset(old + q + 8, 0, 6); old[q + 8] = 1; q += 14;
  {
    char pat[64];
    memset(pat, 0x5A, sizeof pat);
    q += ptrn(old + q, 0, 0x0301, 2, pat, 64);          /* ROOT: a pattern (its dither bits not kept) */
  }
  q += ptrn(old + q, 1, 0x0200, 0, "Work:a.iff", 11);   /* DRAWER: 35 bytes, padded */
  q += ptrn(old + q, 2, 0x0C00, 0, "Work:bb.png", 12);
  memcpy(old + q, "XTRA", 4); be32(old + q + 4, 3); memcpy(old + q + 8, "abc", 3); old[q + 11] = 0; q += 12;
  be32(old + 4, q - 8);
  n = wfp_build(old, q, name, out, sizeof out, &ok);
  check(ok, "prefs-old-valid", 1);
  p = 12;
  memcpy(exp + p, "PRHD", 4); be32(exp + p + 4, 6); memset(exp + p + 8, 0, 6); p += 14;
  p += ptrn(exp + p, 0, 0, 0, name, (unsigned)strlen(name) + 1);
  p += ptrn(exp + p, 1, 0x0200, 0, "Work:a.iff", 11);
  p += ptrn(exp + p, 2, 0x0C00, 0, "Work:bb.png", 12);
  memcpy(exp + p, "XTRA", 4); be32(exp + p + 4, 3); memcpy(exp + p + 8, "abc", 3); exp[p + 11] = 0; p += 12;
  be32(exp + 4, p - 8);
  check(n == p && !memcmp(out, exp, p), "prefs-old", 1);
  dump(dir, "prefs", 91, out, n);
  /* ROOT a picture with flags: its dither/precision flags kept, the rest cleared */
  q = 12 + 14;
  memcpy(old + q - 14, "PRHD", 4);
  q += ptrn(old + q, 0, 0x3F10, 0, "X:y", 4);
  be32(old + 4, q - 8);
  n = wfp_build(old, q, name, out, sizeof out, &ok);
  check(n && wfp_get(out, n, WFP_ROOT, nm, sizeof nm, &fl) && fl == 0x0F00 && !strcmp(nm, name), "prefs-flags", 2);
  /* rubbish: ignored, a new file */
  memcpy(old, "FORM\0\0\0\x40PREF", 12);
  n = wfp_build(old, 20, name, out, sizeof out, &ok);
  check(n && !ok, "prefs-bad", 3);
  check(wfp_build(NULL, 0, name, out, 40, &ok) == 0, "prefs-cap", 4);
}

int main(int argc, char **argv)
{
  const char *dir = argc > 1 ? argv[1] : ".";
  t_weights();
  t_place();
  t_jobs(dir);
  t_prefs(dir);
  printf("wbfit_test: %d checks, %d failed\n", checks, fails);
  return fails != 0;
}
