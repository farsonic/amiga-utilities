/*
 * wfcore.h - WBFit's work, without AmigaOS: where the picture goes on the screen (with the
 * display's pixel aspect), the resampling weights, the quantiser, the PNG and ILBM encoders,
 * and the pipeline that runs the kernels (wfk.h) through a runner: the plain build calls them
 * as C, the NativeCall build natively with the 68k code as fallback (nc/wfnc.c), the host tests
 * as C with stops everywhere.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef WFCORE_H
#define WFCORE_H
#include "wfk.h"
#include "pxweights.h"
#include "pxquant.h"

typedef wfk_u8 wf_u8;
typedef wfk_u32 wf_u32;
typedef wfk_s32 wf_s32;

#define WF_MAXDIM 32767                  /* pictures and screens, either side */

enum { WF_FILL, WF_FIT, WF_STRETCH, WF_CENTER };
enum { WF_PNG, WF_ILBM24, WF_ILBM };     /* 24-bit PNG, 24-bit ILBM, palette ILBM */

/* errors (wf_errstr) */
enum { WF_OK, WF_ENOMEM, WF_EFAULT, WF_EWRITE, WF_EBIG, WF_EARG };
const char *wf_errstr(int e);

/* ---- the runner */
typedef struct WfRun WfRun;
struct WfRun {
  /* kernel id over [from, to): 0, or WF_EFAULT when a native call failed (the stage is then
     redone from its start; the runner uses the 68k code from then on) */
  int (*range)(WfRun *r, int id, void *p, wf_u32 from, wf_u32 to);
  /* one call (wfk_deflate): its result; *fault set on a native failure */
  wf_u32 (*call)(WfRun *r, int id, void *p, int *fault);
  void *(*alloc)(WfRun *r, wf_u32 n);    /* cleared memory the kernels can use; NULL if none */
  void (*free)(WfRun *r, void *p, wf_u32 n);
  wf_u32 (*now_us)(WfRun *r);            /* a clock (microseconds), or NULL */
  void *ctx;
};
/* the plain runner: the kernels as C (they run to the end unless WFK_TEST_BUDGET stops them) */
void wf_run_plain(WfRun *r, void *(*alloc)(WfRun *, wf_u32), void (*free)(WfRun *, void *, wf_u32), wf_u32 (*now_us)(WfRun *));

/* ---- geometry. rx:ry is the display's pixel shape (DisplayInfo Resolution: ticks a pixel is
   wide and high); the picture's own pixels are square. */
typedef struct {
  wf_u32 dw, dh;                         /* the picture's size on the screen (scaled) */
  wf_s32 px, py;                         /* where its top left goes (negative: cropped) */
  wf_u32 vx0, vy0, vw, vh;               /* the part of it on the screen (in scaled pixels) */
  wf_u32 ox, oy;                         /* where that part starts on the screen */
} WfPlace;
int wf_place(WfPlace *pl, wf_u32 sw, wf_u32 sh, wf_u32 W, wf_u32 H, wf_u32 rx, wf_u32 ry, int mode);

/* ---- the resampling weights (wf_taps, wf_weights) and the median cut (wf_mediancut):
   src/lib/pxweights.h, pxquant.h */
/* the picture's border, averaged (FIT's background when BG is not given) */
wf_u32 wf_edge(const wf_u8 *src, wf_u32 w, wf_u32 h, wf_u32 stride);

/* ---- the job */
enum { WF_ST_CONVERT, WF_ST_SCALE, WF_ST_DITHER, WF_ST_ENCODE, WF_NST };
typedef struct {
  /* the picture: RGB, or (old picture.datatype) indices with a palette (and HAM) */
  const wf_u8 *src;  wf_u32 sw, sh, sstride;
  const wf_u8 *idx;  const wf_u8 *ipal; wf_u32 ham;       /* ipal: 768 bytes; ham 0, 4 or 6 */
  /* the screen */
  wf_u32 W, H, rx, ry;
  int mode;
  wf_u32 bg;  int bgauto;                /* FIT/CENTER background (0x00RRGGBB), or the picture's edge */
  int format;                            /* WF_PNG / WF_ILBM24 / WF_ILBM */
  int depth;                             /* WF_ILBM: planes, 1..8 */
  const wf_u8 *pal; int npal;            /* WF_ILBM: the palette to use (the screen's), or NULL: median cut */
  int dither;                            /* Floyd-Steinberg */
  int pngadaptive;                       /* PNG filters (1) or none */
  /* results */
  WfPlace pl;
  wf_u32 bgused;
  int ncol;  wf_u8 opal[768];            /* WF_ILBM's palette */
  wf_u32 kx, ky;                         /* taps */
  wf_u32 us[WF_NST];                     /* time a stage took */
  wf_u32 sum[WF_NST];                    /* a CRC-32 of each stage's result (BENCH: native = 68k) */
  wf_u32 faults;                         /* stages redone after a native failure */
  wf_u32 filebytes;
  /* internal */
  wf_u8 *rgb, *out, *enc, *ind;
  wf_u32 encn, enccap, outcap, indcap;
  wf_u32 *crcs; wf_u32 ncrc;
} WfJob;

/* the whole job; write (if not NULL) gets the file's bytes in order: 0, or its non-zero error */
int wf_fit(WfRun *r, WfJob *j, int (*write)(void *ctx, const void *p, wf_u32 n), void *wctx);
void wf_job_free(WfRun *r, WfJob *j);
/* CRC-32 of a buffer (plain C; the tests and BENCH) */
wf_u32 wf_crc32(wf_u32 crc, const wf_u8 *p, wf_u32 n);

#endif
