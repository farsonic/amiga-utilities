/*
 * fvrender.h - FitView's renderer, without AmigaOS: the window's picture (ARGB, the window's
 * size) made from the source picture for a placement, in bands of rows, a few at a time, so the
 * program never stops answering the window. Two qualities: the preview (nearest neighbour,
 * fast, while the window changes) and the refined picture (WBFit's bicubic scaler, averaging
 * every source pixel when it shrinks). The background around the picture, and alpha over a
 * checkerboard, are drawn into the same buffer: the window gets whole rows, so nothing
 * flickers. The kernels (fvk.h) run through a runner: the plain build calls them as C, the
 * NativeCall build natively with the 68k code as fallback (nc/fvnc.c).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef FVRENDER_H
#define FVRENDER_H
#include "fvk.h"
#include "fvview.h"

typedef wfk_u8 fv_u8;

enum { FV_OK, FV_ENOMEM, FV_EFAULT, FV_EBIG };

typedef struct FvRun FvRun;
struct FvRun {
  /* kernel id over [from, to): 0, or FV_EFAULT when a native call failed (the band is done
     again; the runner uses the 68k code from then on) */
  int (*range)(FvRun *r, int id, void *p, fv_u32 from, fv_u32 to);
  void *(*alloc)(FvRun *r, fv_u32 n);    /* memory the kernels can use (not cleared); NULL if none */
  void (*free)(FvRun *r, void *p, fv_u32 n);
  void *ctx;
};
/* the plain runner: the kernels as C (they run to the end unless WFK_TEST_BUDGET stops them) */
void fv_run_plain(FvRun *r, void *(*alloc)(FvRun *, fv_u32), void (*free)(FvRun *, void *, fv_u32));

typedef struct {
  /* the picture: 4 bytes a pixel (A R G B, colour premultiplied), or 1 with pal (256 x ARGB) */
  const fv_u8 *src; fv_u32 sw, sh, sstride;
  const fv_u8 *pal;
  int alpha;                             /* it has transparent pixels: composite them */
  /* the window's picture: ARGB, W x H */
  fv_u8 *out; fv_u32 W, H, ostride;
  /* the scaled picture's size and where its top left goes (negative: scrolled) */
  fv_u32 dw, dh; fv_s32 px, py;
  fv_u32 bg;                             /* around it (0x00RRGGBB) */
  fv_u32 chk0, chk1, chkshift;           /* under alpha: squares of 1 << chkshift of chk0, chk1 (0: chk0) */
} FvScene;

enum { FV_PREVIEW, FV_REFINE };
typedef struct {
  FvScene s;
  int q;
  fv_u32 ox, oy, vx0, vy0, vw, vh;       /* the part of the scaled picture shown, and where */
  fv_u32 y;                              /* the next window row */
  fv_u32 *xt, *yt, nxt, nyt;             /* preview: source column offsets, rows */
  fv_u32 *tx, *ty, ntx, nty;             /* refine: taps */
  wfk_s16 *wx, *wy; fv_u32 nwx, nwy, kx, ky;
  int idx, idy;
  fv_u8 *mid; fv_u32 midcap, midn, mbase, m0, m1;
  fv_u32 cost;                           /* work per output row (pixel taps) */
  int nofine;                            /* refine fell back to the preview (no memory, too small) */
  fv_u32 faults;                         /* bands done again after a native failure */
} FvJob;

/* starts a job: 0 or FV_E*; fv_job_end() frees it either way */
int fv_job_begin(FvRun *r, FvJob *j, const FvScene *s, int q);
/* the next band of rows, about budget pixel-taps of work: window rows [*from, *to) are finished.
   1 while rows remain, 0 when the picture is done, or -FV_E* */
int fv_job_step(FvRun *r, FvJob *j, fv_u32 budget, fv_u32 *from, fv_u32 *to);
void fv_job_end(FvRun *r, FvJob *j);
#endif
