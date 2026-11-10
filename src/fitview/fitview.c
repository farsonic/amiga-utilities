/*
 * FitView - a MultiView replacement for AmigaOS 3.x that fits pictures to the window.
 *
 *   FitView FILE/M,BASENAME/K,CLIPBOARD/S,CLIPUNIT/K/N,SCREEN/S,PUBSCREEN/K,REQUESTER/S,
 *           BOOKMARK/S,FONTNAME/K,FONTSIZE/K/N,BACKDROP/S,WINDOW/S,PORTNAME/K,FIT/K,BENCH/S,V40/S,PENS/S
 *
 * Pictures (GID_PICTURE) are read through datatypes in a process of their own (fvload.c) and
 * drawn by FitView itself: scaled to the window (fit, fit width, 1:1, zoom steps, fill),
 * re-fitted at once while the window changes (a nearest-neighbour preview, fvrender.c) and
 * refined with WBFit's bicubic scaler when it stops changing, a band of rows at a time from the
 * event loop, so the window always answers. RTG screens get the pixels through
 * cybergraphics.library's WritePixelArray(); screens of 8 bits or fewer a cube of pens
 * (ObtainBestPen) and Floyd-Steinberg, drawn with WriteChunkyPixels() (graphics 45+) or
 * WritePixelArray8(). Paged documents (pdf.datatype's PDFDTA_Page) are shown a page at a time,
 * each drawn sharp for the window's size. Everything else (text, AmigaGuide, sound,
 * animation, ...) is the datatype's own gadget in the window, as MultiView shows it.
 *
 * Copyright (C) 2026 the WBFit author.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>
#include <dos/exall.h>
#include <devices/timer.h>
#include <devices/printer.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>
#include <graphics/view.h>
#include <graphics/rpattr.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <intuition/gadgetclass.h>
#include <intuition/imageclass.h>
#include <intuition/icclass.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>
#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/pictureclass.h>
#include <datatypes/soundclass.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <rexx/storage.h>
#include <rexx/rxslib.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/datatypes.h>
#include <proto/gadtools.h>
#include <proto/asl.h>
#include <proto/icon.h>
#include <proto/wb.h>
#include <proto/utility.h>
#include <proto/diskfont.h>
#include <proto/timer.h>
#include <clib/alib_protos.h>
#include "fvload.h"
#include "fvrender.h"
#include "fvview.h"
#include "fvprefs.h"
#ifdef FITVIEW_NC
#include "fvnc.h"
#endif

struct Library *CyberGfxBase;
#include <inline/cybergraphics_protos.h>
#define RECTFMT_ARGB 2
#define CYBRMATTR_ISCYBERGFX 0x80000008UL

#define VERSTR "1.0"
static const char version[] = "$VER: FitView " VERSTR " (10.11.2026)"
#ifdef FITVIEW_NC
  " NativeCall test build"
#endif
  ;
size_t __stack = 65536;
extern struct WBStartup *_WBenchMsg;

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *DataTypesBase, *UtilityBase, *GadToolsBase, *AslBase, *IconBase, *WorkbenchBase, *DiskfontBase;
struct Device *TimerBase;

#define PREFS_ENV    "ENV:FitView.prefs"
#define PREFS_ENVARC "ENVARC:FitView.prefs"
#define REFINE_US    200000UL           /* the window has stopped changing after this */

/* ================================================================== options */
static struct {
  char **files; int nfiles;
  char basename[40], fontname[64], pubscreen[80], portname[40];
  LONG fontsize, clipunit;
  int clip, ownscreen, backdrop, requester, bench, v40, fitgiven, pens;
} opt;
static FvPrefs prefs, savedprefs;
static int fromwb;

/* ================================================================== messages */
static struct Window *win;
static void say(const char *fmt, ...)
{
  char b[600];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (!fromwb) { PutStr((STRPTR)b); return; }
}
static int noecho;                 /* (About and BENCH: a requester only) */
static void tell(const char *fmt, ...)
{
  char b[600];
  struct EasyStruct es;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (!fromwb && !noecho) { PutStr((STRPTR)"FitView: "); PutStr((STRPTR)b); PutStr((STRPTR)"\n"); }   /* (the Shell sees it too) */
  if (!IntuitionBase) return;
  es.es_StructSize = sizeof es; es.es_Flags = 0;
  es.es_Title = (UBYTE *)"FitView"; es.es_TextFormat = (UBYTE *)"%s"; es.es_GadgetFormat = (UBYTE *)"OK";
  EasyRequest(win, &es, NULL, (ULONG)b);
}

/* ================================================================== the runner */
static FvRun run;
static void *r_alloc(FvRun *r, fv_u32 n) { (void)r; return fv_alloc(n); }
static void r_free(FvRun *r, void *p, fv_u32 n) { (void)r; (void)n; fv_free(p); }
static void runner(int native)
{
#ifdef FITVIEW_NC
  fvnc_runner(&run, native);
#else
  (void)native;
  fv_run_plain(&run, r_alloc, r_free);
#endif
}
/* a buffer the kernels get from outside the runner (NativeCall: it must be visible natively) */
static void seen(const void *p, ULONG n)
{
#ifdef FITVIEW_NC
  fvnc_check(p, n);
#else
  (void)p; (void)n;
#endif
}

/* ================================================================== the clock */
static struct MsgPort *tport;
static struct timerequest *treq;
static int tbusy, topen;
static ULONG now_us(void)
{
  struct timeval tv;
  if (!topen) return 0;
  GetSysTime(&tv);
  return (ULONG)(tv.tv_secs * 1000000UL + tv.tv_micro);
}
static void timer_stop(void)
{
  if (!tbusy) return;
  AbortIO((struct IORequest *)treq);
  WaitIO((struct IORequest *)treq);
  SetSignal(0, 1UL << tport->mp_SigBit);
  tbusy = 0;
}
static void timer_start(ULONG us)
{
  if (!topen) return;
  timer_stop();
  treq->tr_node.io_Command = TR_ADDREQUEST;
  treq->tr_time.tv_secs = us / 1000000UL;
  treq->tr_time.tv_micro = us % 1000000UL;
  SendIO((struct IORequest *)treq);
  tbusy = 1;
}

/* ================================================================== the screen and window */
static struct Screen *scr;
static int ownscr, scrlocked;
static struct DrawInfo *dri;
static APTR vi;
static struct Menu *menus;
static struct MsgPort *appport;
static struct AppWindow *appwin;
static struct MsgPort *rexxport;
static struct TextAttr fta;
static struct TextFont *tfont;
static char title[300];
static ULONG scrrx = 1, scrry = 1;          /* the screen's pixel shape */

enum { GID_VPROP = 1, GID_HPROP, GID_UP, GID_DOWN, GID_LEFT, GID_RIGHT, GID_DTO };
static Object *vprop, *hprop, *bup, *bdown, *bleft, *bright, *iup, *idown, *ileft, *iright, *isize;
static struct Gadget *gfirst;

/* the window's inside */
static ULONG IW(void) { return win ? (ULONG)(win->Width - win->BorderLeft - win->BorderRight) : 0; }
static ULONG IH(void) { return win ? (ULONG)(win->Height - win->BorderTop - win->BorderBottom) : 0; }

/* ================================================================== what is shown */
static FvPic pic;                  /* the picture */
static int havepic;
static Object *dto;                /* or a datatype's gadget */
static ULONG dtgroup;
static char dtname[40];
static FvGeom geom;
static fv_u32 sx, sy;              /* scroll position (scaled pixels) */
static ULONG gtop, gvis, gtot, gtoph, gvish, gtoth;   /* the gadget's scroll values */
static int shown_any;              /* something has been shown: errors no longer end the program */
static int sized;                  /* the window was given a size (prefs, or fitted once) */

/* the files */
static char **flist;
static int nflist, fcur = -1, fscanned;

/* ================================================================== drawing: buffers, pens */
static UBYTE *obuf; static ULONG obw, obh, obn;        /* the window's picture, ARGB */
static UBYTE *cbuf; static ULONG cstride, cbn;         /* pens, a byte a pixel (screens of 8 bits or fewer) */
static int rtg;                                         /* WritePixelArray() of ARGB */
static LONG pens[256]; static int npens;
static UBYTE ppal[768], pmap[256];
static UWORD *dcache;
static LONG *de0, *de1; static ULONG den;
static struct BitMap *tmpbm; static struct RastPort tmprp; static int usewcp;
static ULONG drow;                                      /* rows dithered so far in this job */

static FvJob job;
static int jobon, jobq;
static int refine_due, refined, fault_note;
static ULONG budget = 30000;

/* the paged document */
#define NCACHE 2
static FvPic pcache[NCACHE];
static int pcache_ok[NCACHE];
static int paged;

/* the loader */
static struct MsgPort *lreply;
static FvLoadReq lrq;              /* the request in flight */
static int linflight;
static struct { int want; char path[512]; ULONG srctype, unit, page, boxw, boxh; int keep; } lnext;
static ULONG lgen, docgen;
static int promote;                /* the page being fetched for later is wanted now: show it */

/* ------------------------------------------------------------------ pens */
static void release_pens(void)
{
  int i;
  if (scr) for (i = 0; i < npens; i++) if (pens[i] >= 0) ReleasePen(scr->ViewPort.ColorMap, (ULONG)pens[i]);
  npens = 0;
}
static int obtain_pens(void)
{
  struct ColorMap *cm = scr->ViewPort.ColorMap;
  ULONG depth = GetBitMapAttr(scr->RastPort.BitMap, BMA_DEPTH), lv, r, g, b;
  int i = 0;
  lv = depth >= 8 ? 6 : depth >= 6 ? 4 : depth >= 5 ? 3 : 2;    /* 216, 64, 27 or 8 colours */
  for (r = 0; r < lv; r++)
    for (g = 0; g < lv; g++)
      for (b = 0; b < lv; b++) {
        ULONG cr = r * 255 / (lv - 1), cg = g * 255 / (lv - 1), cb = b * 255 / (lv - 1), rgb[3];
        LONG pen = ObtainBestPen(cm, cr * 0x01010101UL, cg * 0x01010101UL, cb * 0x01010101UL,
                                 OBP_Precision, PRECISION_IMAGE, OBP_FailIfBad, FALSE, TAG_DONE);
        pens[i] = pen;
        if (pen < 0) pen = 1;
        GetRGB32(cm, (ULONG)pen, 1, rgb);
        ppal[i * 3] = (UBYTE)(rgb[0] >> 24); ppal[i * 3 + 1] = (UBYTE)(rgb[1] >> 24); ppal[i * 3 + 2] = (UBYTE)(rgb[2] >> 24);
        pmap[i] = (UBYTE)pen;
        i++;
      }
  npens = i;
  if (dcache) memset(dcache, 0xFF, 65536);
  return npens;
}

static void free_buffers(void)
{
  fv_free(obuf); obuf = 0; obn = 0; obw = obh = 0;
  fv_free(cbuf); cbuf = 0; cbn = 0;
  fv_free(de0); fv_free(de1); de0 = de1 = 0; den = 0;
  if (tmpbm) { FreeBitMap(tmpbm); tmpbm = 0; }
}

/* the window's picture for the inside's size; the background where nothing is yet */
static int ensure_buffers(void)
{
  ULONG W = IW(), H = IH(), n, i;
  if (!W || !H) return 0;
  if (obuf && obw == W && obh == H) return 1;
  free_buffers();
  n = W * H * 4;
  if (!(obuf = fv_alloc(n))) return 0;
  obw = W; obh = H; obn = n;
  for (i = 0; i < n; i += 4) { obuf[i] = 255; obuf[i + 1] = (UBYTE)(prefs.bg >> 16); obuf[i + 2] = (UBYTE)(prefs.bg >> 8); obuf[i + 3] = (UBYTE)prefs.bg; }
  seen(obuf, n);
  if (!rtg) {
    cstride = (W + 15) & ~15UL;
    cbn = cstride * H;
    den = (W + 2) * 12;
    if (!(cbuf = fv_alloc(cbn)) || !(de0 = fv_alloc(den)) || !(de1 = fv_alloc(den))) { free_buffers(); return 0; }
    memset(cbuf, npens ? pmap[0] : 0, cbn);
    seen(cbuf, cbn); seen(de0, den); seen(de1, den);
    if (!usewcp) {
      ULONG depth = GetBitMapAttr(scr->RastPort.BitMap, BMA_DEPTH);
      if (!(tmpbm = AllocBitMap(cstride, 1, depth, 0, scr->RastPort.BitMap))) { free_buffers(); return 0; }
      InitRastPort(&tmprp);
      tmprp.BitMap = tmpbm;
    }
  }
  return 1;
}

/* window rows [a, b) from the buffers */
static void blit_rows(ULONG a, ULONG b)
{
  struct RastPort *rp = win->RPort;
  if (b <= a || !obuf) return;
  if (rtg) {
    WritePixelArray(obuf, 0, (UWORD)a, (UWORD)(obw * 4), rp, (UWORD)win->BorderLeft, (UWORD)(win->BorderTop + a), (UWORD)obw, (UWORD)(b - a), RECTFMT_ARGB);
    return;
  }
  if (usewcp) WriteChunkyPixels(rp, win->BorderLeft, win->BorderTop + a, win->BorderLeft + obw - 1, win->BorderTop + b - 1, cbuf + a * cstride, cstride);
  else WritePixelArray8(rp, win->BorderLeft, win->BorderTop + a, win->BorderLeft + obw - 1, win->BorderTop + b - 1, cbuf + a * cstride, &tmprp);
}

/* new rows of this job: to pens (screens of 8 bits or fewer), then to the window */
static void show_rows(ULONG a, ULONG b)
{
  if (!rtg && npens) {
    struct FvkDither d;
    int fs = jobq == FV_REFINE && prefs.dither;
    if (a != drow) return;                 /* (rows come in order; Floyd-Steinberg needs them so) */
    if (a == 0) { memset(de0, 0, den); memset(de1, 0, den); }
    memset(&d, 0, sizeof d);
    d.px = WFK_SETP(obuf); d.pstride = obw * 4; d.w = obw; d.out = WFK_SETP(cbuf); d.ostride = cstride;
    d.pal = WFK_SETP(ppal); d.npal = (wfk_u32)npens; d.cache = WFK_SETP(dcache); d.penmap = WFK_SETP(pmap);
    d.fs = (wfk_u32)fs; d.e0 = WFK_SETP(de0); d.e1 = WFK_SETP(de1);
    if (run.range(&run, FVKI_fvk_dither, &d, a, b) == FV_EFAULT) {
      /* a native call failed: everything so far again, with the 68k code */
      memset(de0, 0, den); memset(de1, 0, den);
      run.range(&run, FVKI_fvk_dither, &d, 0, b);
      a = 0;
    }
    drow = b;
  }
  blit_rows(a, b);
}

/* ================================================================== the title and scrollers */
static void set_title(void)
{
  const char *name = opt.clip ? "Clipboard" : fcur >= 0 && fcur < nflist ? (const char *)FilePart((STRPTR)flist[fcur]) : "";
  int n;
  if (havepic && !dto) {
    n = snprintf(title, sizeof title, "%s  %lux%lu  ", name, (unsigned long)pic.w, (unsigned long)pic.h);
    if (paged && (prefs.mode == FV_FIT || prefs.mode == FV_WIDTH || prefs.mode == FV_FILL))
      n += snprintf(title + n, sizeof title - n, "%s", prefs.mode == FV_FIT ? "fit" : prefs.mode == FV_WIDTH ? "width" : "fill");
    else n += snprintf(title + n, sizeof title - n, "%lu%%", (unsigned long)((geom.pm + 5) / 10));
    if (paged) n += snprintf(title + n, sizeof title - n, "  page %lu/%lu", (unsigned long)pic.page, (unsigned long)pic.pages);
    if (nflist > 1 && fscanned < 2) snprintf(title + n, sizeof title - n, "  [%d/%d]", fcur + 1, nflist);
  } else if (dto) snprintf(title, sizeof title, "%s  (%s)", name, dtname[0] ? dtname : "datatype");
  else snprintf(title, sizeof title, "FitView%s", linflight ? ": loading..." : "");
  if (win) SetWindowTitles(win, (UBYTE *)title, (UBYTE *)-1);
}

static void set_props(void)
{
  fv_u32 t, v, p;
  if (!win || !vprop) return;
  if (dto) {
    fv_prop(gtot, gvis, gtop, &t, &v, &p);
    SetGadgetAttrs((struct Gadget *)vprop, win, NULL, PGA_Total, t, PGA_Visible, v, PGA_Top, p, TAG_DONE);
    fv_prop(gtoth, gvish, gtoph, &t, &v, &p);
    SetGadgetAttrs((struct Gadget *)hprop, win, NULL, PGA_Total, t, PGA_Visible, v, PGA_Top, p, TAG_DONE);
    return;
  }
  fv_prop(havepic ? geom.dh : 1, havepic ? (geom.dh < IH() ? geom.dh : IH()) : 1, sy, &t, &v, &p);
  SetGadgetAttrs((struct Gadget *)vprop, win, NULL, PGA_Total, t, PGA_Visible, v, PGA_Top, p, TAG_DONE);
  fv_prop(havepic ? geom.dw : 1, havepic ? (geom.dw < IW() ? geom.dw : IW()) : 1, sx, &t, &v, &p);
  SetGadgetAttrs((struct Gadget *)hprop, win, NULL, PGA_Total, t, PGA_Visible, v, PGA_Top, p, TAG_DONE);
}

/* ================================================================== rendering */
static void stop_job(void)
{
  if (jobon) fv_job_end(&run, &job);
  jobon = 0;
}

static void scene(FvScene *s)
{
  fv_s32 px, py;
  memset(s, 0, sizeof *s);
  fv_place(&geom, sx, sy, &px, &py);
  s->src = pic.px; s->sw = pic.w; s->sh = pic.h; s->sstride = pic.stride; s->pal = pic.pal; s->alpha = pic.alpha;
  s->out = obuf; s->W = obw; s->H = obh; s->ostride = obw * 4;
  s->dw = geom.dw; s->dh = geom.dh; s->px = px; s->py = py;
  s->bg = prefs.bg; s->chk0 = 0x999999; s->chk1 = 0x666666; s->chkshift = prefs.checker ? 3 : 0;
}

static void start_job(int q)
{
  FvScene s;
  int e;
  stop_job();
  if (!havepic || dto || !ensure_buffers()) return;
  scene(&s);
  jobq = q;
  drow = 0;
  if ((e = fv_job_begin(&run, &job, &s, q))) { fv_job_end(&run, &job); return; }
  jobon = 1;
}

/* one band of the job; done: refine next, or nothing */
static void work(void)
{
  fv_u32 a, b;
  ULONG t0 = now_us(), dt;
  int e = fv_job_step(&run, &job, budget, &a, &b);
  if (b > a) show_rows(a, b);
  /* about 20 ms a band on this machine */
  dt = now_us() - t0;
  if (t0 && dt < 10000 && budget < 0x1000000UL) budget += budget / 2;
  else if (t0 && dt > 40000 && budget > 2000) budget -= budget / 3;
  if (e > 0) return;
  if (job.faults && !fault_note) { fault_note = 1; say("FitView: a native call failed; drawn with the 68k code\n"); }
  stop_job();
  if (jobq == FV_REFINE) refined = 1;
}

/* the 1:1 view (and every other where the scale is one) needs no refining */
static int needs_refine(void) { return !(geom.dw == pic.w && geom.dh == pic.h); }

static void load_page(ULONG page, int keep);
static void want_box(ULONG *bw, ULONG *bh);

/* the view changed: geometry, scrollers, title, a preview now, refining when it settles */
static void view_changed(int recentre, const FvGeom *old)
{
  if (!havepic || dto) { set_title(); return; }
  geom.sw = pic.w; geom.sh = pic.h; geom.W = IW(); geom.H = IH();
  geom.mode = prefs.mode; geom.zoom = prefs.zoom;
  fv_aspect(pic.xasp, pic.yasp, scrrx, scrry, &geom.anum, &geom.aden);
  geom.snap = paged && pic.boxw;
  fv_geom(&geom);
  if (recentre && old) fv_rescroll(old, &geom, &sx, &sy);
  if (sx > geom.maxx) sx = geom.maxx;
  if (sy > geom.maxy) sy = geom.maxy;
  set_props();
  set_title();
  refined = 0;
  start_job(FV_PREVIEW);
  refine_due = 0;
  timer_start(REFINE_US);
}

static void scroll_to(LONG nx, LONG ny)
{
  if (dto) {
    if (ny < 0) ny = 0;
    if (gtot > gvis && (ULONG)ny > gtot - gvis) ny = (LONG)(gtot - gvis);
    if (gtot <= gvis) ny = 0;
    if (nx < 0) nx = 0;
    if (gtoth > gvish && (ULONG)nx > gtoth - gvish) nx = (LONG)(gtoth - gvish);
    if (gtoth <= gvish) nx = 0;
    if ((ULONG)ny != gtop || (ULONG)nx != gtoph) {
      gtop = (ULONG)ny; gtoph = (ULONG)nx;
      SetDTAttrs(dto, win, NULL, DTA_TopVert, gtop, DTA_TopHoriz, gtoph, TAG_DONE);
      set_props();
    }
    return;
  }
  if (!havepic) return;
  if (nx < 0) nx = 0;
  if (ny < 0) ny = 0;
  if ((ULONG)nx > geom.maxx) nx = (LONG)geom.maxx;
  if ((ULONG)ny > geom.maxy) ny = (LONG)geom.maxy;
  if ((ULONG)nx == sx && (ULONG)ny == sy) return;
  sx = (ULONG)nx; sy = (ULONG)ny;
  set_props();
  refined = 0;
  start_job(needs_refine() ? FV_PREVIEW : FV_REFINE);
  timer_start(REFINE_US);
}

/* ================================================================== the paged document */
static void want_box(ULONG *bw, ULONG *bh)
{
  ULONG W = IW(), H = IH();
  *bw = *bh = 0;
  switch (prefs.mode) {
  case FV_FIT: *bw = W; *bh = H; break;
  case FV_WIDTH: *bw = W; *bh = 0; break;
  case FV_FILL:
    if (havepic) {
      FvGeom g = geom;
      g.snap = 0; g.mode = FV_FILL; g.W = W; g.H = H;
      fv_geom(&g);
      *bw = g.dw; *bh = g.dh;
    } else { *bw = W; *bh = H; }
    break;
  default: break;                            /* (1:1 and zoom: the datatype's own size, scaled) */
  }
}

static void cache_drop(int i) { if (pcache_ok[i]) fv_pic_free(&pcache[i]); pcache_ok[i] = 0; }
static void cache_clear(void) { int i; for (i = 0; i < NCACHE; i++) cache_drop(i); }
static int cache_find(ULONG page, ULONG bw, ULONG bh)
{
  int i;
  for (i = 0; i < NCACHE; i++) if (pcache_ok[i] && pcache[i].page == page && pcache[i].boxw == bw && pcache[i].boxh == bh) return i;
  return -1;
}
/* keep p (a page not shown now): in a free slot, else instead of the one farthest away */
static void cache_put(FvPic *p)
{
  int i, far = 0;
  ULONG d, fd = 0;
  for (i = 0; i < NCACHE; i++) if (!pcache_ok[i]) { far = i; fd = 0xFFFFFFFFUL; break; }
  if (fd != 0xFFFFFFFFUL)
    for (i = 0; i < NCACHE; i++) {
      d = pcache[i].page > pic.page ? pcache[i].page - pic.page : pic.page - pcache[i].page;
      if (d >= fd) { fd = d; far = i; }
    }
  cache_drop(far);
  pcache[far] = *p; pcache_ok[far] = 1;
  memset(p, 0, sizeof *p);
}

/* ================================================================== the loader */
static void loader_send(void)
{
  struct MsgPort *lp = fvl_port();
  if (!lnext.want || linflight) return;
  memset(&lrq, 0, sizeof lrq);
  lrq.msg.mn_ReplyPort = lreply; lrq.msg.mn_Length = sizeof lrq;
  strcpy(lrq.path, lnext.path);
  lrq.srctype = lnext.srctype; lrq.unit = lnext.unit; lrq.page = lnext.page; lrq.boxw = lnext.boxw; lrq.boxh = lnext.boxh;
  lrq.keep = lnext.keep; lrq.v40 = opt.v40;
  lrq.gen = lnext.keep ? docgen : ++lgen;
  lnext.want = 0;
  if (!lp) { fvl_do(&lrq); ReplyMsg(&lrq.msg); }    /* (no process: here, waiting) */
  else PutMsg(lp, &lrq.msg);
  linflight = 1;
  if (!lrq.keep && win) SetWindowPointer(win, WA_BusyPointer, TRUE, WA_PointerDelay, TRUE, TAG_DONE);
}

static void request(const char *path, ULONG srctype, ULONG unit, ULONG page, ULONG bw, ULONG bh, int keep)
{
  if (keep && lnext.want && !lnext.keep) return;       /* (a page to show goes first) */
  lnext.want = 1;
  strncpy(lnext.path, path ? path : "", sizeof lnext.path - 1);
  lnext.path[sizeof lnext.path - 1] = 0;
  lnext.srctype = srctype; lnext.unit = unit; lnext.page = page; lnext.boxw = bw; lnext.boxh = bh; lnext.keep = keep;
  if (!keep) { lgen++; promote = 0; }                   /* (what is in flight now is stale) */
  loader_send();
}

static void load_page(ULONG page, int keep)
{
  ULONG bw, bh;
  int i;
  want_box(&bw, &bh);
  if (!keep && (i = cache_find(page, bw, bh)) >= 0) {
    /* from the cache at once; the page shown goes in its place */
    FvPic t = pcache[i];
    pcache_ok[i] = 0;
    if (havepic && pic.boxw == bw && pic.boxh == bh) cache_put(&pic);
    else fv_pic_free(&pic);
    pic = t;
    seen(pic.px, pic.pxn); if (pic.pal) seen(pic.pal, 1024);
    sx = sy = 0;
    view_changed(0, 0);
    return;
  }
  if (keep && cache_find(page, bw, bh) >= 0) return;
  if (!keep && linflight && lrq.keep && lrq.gen == docgen && lrq.page == page && lrq.boxw == bw && lrq.boxh == bh) {
    promote = 1;                           /* (on its way already) */
    lnext.want = 0;
    if (win) SetWindowPointer(win, WA_BusyPointer, TRUE, WA_PointerDelay, TRUE, TAG_DONE);
    return;
  }
  request(opt.clip ? 0 : flist[fcur], opt.clip ? DTST_CLIPBOARD : DTST_FILE, (ULONG)opt.clipunit, page, bw, bh, keep);
}

static void open_object(void);

static void loaded(void)
{
  FvLoadReq *rq = &lrq;
  int stale;
  linflight = 0;
  if (win && !lnext.want) SetWindowPointer(win, TAG_DONE);
  if (rq->keep && promote && rq->gen == docgen && rq->result == FVL_OK) { rq->keep = 0; rq->gen = lgen; }
  promote = 0;
  if (rq->keep) {
    /* a neighbouring page, for later */
    if (rq->result == FVL_OK && rq->gen == docgen && havepic && paged) {
      ULONG bw, bh;
      want_box(&bw, &bh);
      if (rq->pic.boxw == bw && rq->pic.boxh == bh && cache_find(rq->pic.page, bw, bh) < 0) cache_put(&rq->pic);
    }
    fv_pic_free(&rq->pic);
    loader_send();
    return;
  }
  stale = rq->gen != lgen;
  if (stale) { fv_pic_free(&rq->pic); loader_send(); return; }
  if (rq->result == FVL_NOTPIC) { open_object(); loader_send(); return; }
  if (rq->result != FVL_OK) {
    tell("%s", rq->err);
    if (!shown_any) {
      /* (nothing could be shown: as MultiView, give up; with more files, the next one) */
      Signal(FindTask(NULL), SIGBREAKF_CTRL_C);
    }
    set_title();
    loader_send();
    return;
  }
  /* the new picture (the old one goes, or to the cache when it is a page of the same document) */
  if (havepic && paged && rq->pic.pages && pic.page != rq->pic.page && pic.boxw == rq->pic.boxw && pic.boxh == rq->pic.boxh) cache_put(&pic);
  else fv_pic_free(&pic);
  {
    int samepage = havepic && paged && rq->pic.pages && pic.page == rq->pic.page;
    pic = rq->pic;
    memset(&rq->pic, 0, sizeof rq->pic);
    havepic = 1;
    paged = pic.pages > 0;
    shown_any = 1;
    seen(pic.px, pic.pxn); if (pic.pal) seen(pic.pal, 1024);
    if (!samepage) sx = sy = 0;
  }
  if (!paged && strstr(pic.dtname, "PDF") && !opt.fitgiven && prefs.mode == FV_FIT) {
    /* an older pdf.datatype without pages: its pages stacked into one tall picture */
    prefs.mode = FV_WIDTH;
  }
  if (!sized && !paged && !ownscr && !opt.backdrop && win) {
    /* the first picture: the window to its size, at most nine tenths of the screen */
    ULONG fw = win->BorderLeft + win->BorderRight, fh = win->BorderTop + win->BorderBottom;
    ULONG mw = scr->Width * 9 / 10 - fw, mh = (scr->Height - scr->BarHeight - 1) * 9 / 10 - fh, w = pic.w, h;
    fv_u32 an, ad;
    fv_aspect(pic.xasp, pic.yasp, scrrx, scrry, &an, &ad);
    h = pic.h * an / ad;
    if (h < 1) h = 1;
    if (w > mw) { h = h * mw / w; w = mw; }
    if (h > mh) { w = w * mh / h; h = mh; }
    if (w < 120) w = 120;
    if (h < 60) h = 60;
    sized = 1;
    if (w + fw != (ULONG)win->Width || h + fh != (ULONG)win->Height) {
      LONG l = win->LeftEdge, t = win->TopEdge;
      if (l + (LONG)(w + fw) > scr->Width) l = scr->Width - (LONG)(w + fw);
      if (t + (LONG)(h + fh) > scr->Height) t = scr->Height - (LONG)(h + fh);
      if (l < 0) l = 0;
      if (t < 0) t = 0;
      ChangeWindowBox(win, l, t, w + fw, h + fh);
    }
  }
  sized = 1;
  view_changed(0, 0);
  loader_send();
}

/* ================================================================== datatype gadgets */
static void remove_object(void)
{
  if (!dto) return;
  RemoveDTObject(win, dto);
  DisposeDTObject(dto);
  dto = 0;
  gtop = gvis = gtot = gtoph = gvish = gtoth = 0;
}

static void drop_picture(void)
{
  stop_job();
  fv_pic_free(&pic);
  havepic = 0; paged = 0;
  cache_clear();
}

/* the file (or clipboard) as the datatype's own gadget, as MultiView shows it */
static void open_object(void)
{
  ULONG st = opt.clip ? DTST_CLIPBOARD : DTST_FILE;
  APTR name = opt.clip ? (APTR)opt.clipunit : (APTR)flist[fcur];
  struct DataType *dtn = NULL;
  drop_picture();
  remove_object();
  dto = NewDTObject(name, DTA_SourceType, st,
                    opt.basename[0] ? DTA_BaseName : TAG_IGNORE, (ULONG)opt.basename,
                    GA_Left, win->BorderLeft, GA_Top, win->BorderTop,
                    GA_RelWidth, -(LONG)(win->BorderLeft + win->BorderRight), GA_RelHeight, -(LONG)(win->BorderTop + win->BorderBottom),
                    GA_ID, GID_DTO, ICA_TARGET, ICTARGET_IDCMP,
                    tfont ? DTA_TextAttr : TAG_IGNORE, (ULONG)&fta, TAG_DONE);
  if (!dto) {
    LONG e = IoErr();
    STRPTR s = e >= DTERROR_UNKNOWN_DATATYPE ? GetDTString((ULONG)e) : NULL;
    char f[100];
    if (!s) { Fault(e, (STRPTR)"", (STRPTR)f, sizeof f); s = (STRPTR)f; }
    tell("%s: %s", opt.clip ? "Clipboard" : (const char *)FilePart((STRPTR)name), s);
    if (!shown_any) Signal(FindTask(NULL), SIGBREAKF_CTRL_C);
    set_title();
    return;
  }
  dtname[0] = 0;
  GetDTAttrs(dto, DTA_DataType, (ULONG)&dtn, TAG_DONE);
  if (dtn && dtn->dtn_Header) {
    if (dtn->dtn_Header->dth_Name) { strncpy(dtname, (const char *)dtn->dtn_Header->dth_Name, sizeof dtname - 1); dtname[sizeof dtname - 1] = 0; }
    dtgroup = dtn->dtn_Header->dth_GroupID;
  }
  /* the window's inside: clear (the gadget draws its own) */
  SetAPen(win->RPort, dri ? dri->dri_Pens[BACKGROUNDPEN] : 0);
  RectFill(win->RPort, win->BorderLeft, win->BorderTop, win->Width - win->BorderRight - 1, win->Height - win->BorderBottom - 1);
  AddDTObject(win, NULL, dto, -1);
  RefreshDTObjects(dto, win, NULL, NULL);
  shown_any = 1;
  set_props();
  set_title();
}

/* ================================================================== files */
static char *dupstr(const char *s)
{
  char *d = AllocVec(strlen(s) + 1, MEMF_ANY);
  if (d) strcpy(d, s);
  return d;
}
static void free_list(void)
{
  int i;
  for (i = 0; i < nflist; i++) FreeVec(flist[i]);
  FreeVec(flist);
  flist = 0; nflist = 0;
}
static int add_file(const char *s)
{
  char **n;
  if (nflist % 32 == 0) {
    if (!(n = AllocVec((nflist + 32) * sizeof(char *), MEMF_ANY | MEMF_CLEAR))) return 0;
    if (flist) { memcpy(n, flist, nflist * sizeof(char *)); FreeVec(flist); }
    flist = n;
  }
  if (!(flist[nflist] = dupstr(s))) return 0;
  nflist++;
  return 1;
}
/* a name to a full one (so the loader process and next/previous file need no current dir) */
static void full_path(BPTR dir, const char *name, char *out, ULONG cap)
{
  BPTR l = dir ? 0 : Lock((STRPTR)name, ACCESS_READ);
  out[0] = 0;
  if (dir) {
    if (NameFromLock(dir, (STRPTR)out, cap)) AddPart((STRPTR)out, (STRPTR)name, cap);
  } else if (l) {
    NameFromLock(l, (STRPTR)out, cap);
    UnLock(l);
  }
  if (!out[0]) { strncpy(out, name, cap - 1); out[cap - 1] = 0; }
}

/* one file given: next/previous walk its drawer (sorted, no icons) */
static int nocase_cmp(const char *a, const char *b) { return Stricmp((STRPTR)a, (STRPTR)b); }
static void scan_drawer(void)
{
  char dirname[512], cur[512];
  BPTR l;
  struct FileInfoBlock *fib;
  int i, j;
  if (fscanned || nflist != 1) return;
  fscanned = 1;
  strcpy(cur, flist[0]);
  strcpy(dirname, cur);
  *PathPart((STRPTR)dirname) = 0;
  if (!(l = Lock((STRPTR)dirname, ACCESS_READ))) return;
  if ((fib = AllocDosObject(DOS_FIB, NULL))) {
    if (Examine(l, fib) && fib->fib_DirEntryType > 0) {
      free_list();
      while (ExNext(l, fib) && nflist < 4000) {
        int n = strlen((const char *)fib->fib_FileName);
        char full[600];
        if (fib->fib_DirEntryType > 0) continue;
        if (n >= 5 && !Stricmp((STRPTR)fib->fib_FileName + n - 5, (STRPTR)".info")) continue;
        strcpy(full, dirname);
        AddPart((STRPTR)full, fib->fib_FileName, sizeof full);
        add_file(full);
      }
      /* (an insertion sort: a few thousand names at most) */
      for (i = 1; i < nflist; i++) {
        char *t = flist[i];
        for (j = i; j > 0 && nocase_cmp(flist[j - 1], t) > 0; j--) flist[j] = flist[j - 1];
        flist[j] = t;
      }
      fcur = 0;
      for (i = 0; i < nflist; i++) if (!Stricmp((STRPTR)flist[i], (STRPTR)cur)) { fcur = i; break; }
      if (!nflist) { add_file(cur); fcur = 0; }
      fscanned = 2;
    }
    FreeDosObject(DOS_FIB, fib);
  }
  UnLock(l);
}

/* the file fcur (or the clipboard): a picture to the loader, anything else as a gadget */
static void load_current(void)
{
  ULONG group = GID_PICTURE, bw, bh;
  if (!opt.clip) {
    BPTR l;
    struct DataType *dtn;
    if (fcur < 0 || fcur >= nflist) return;
    if (!(l = Lock((STRPTR)flist[fcur], ACCESS_READ))) {
      tell("%s: no such file", FilePart((STRPTR)flist[fcur]));
      if (!shown_any) Signal(FindTask(NULL), SIGBREAKF_CTRL_C);
      return;
    }
    if ((dtn = ObtainDataType(DTST_FILE, (APTR)l, TAG_DONE))) {
      group = dtn->dtn_Header->dth_GroupID;
      ReleaseDataType(dtn);
    } else group = 0;
    UnLock(l);
    if (opt.basename[0]) group = 0;           /* (a datatype chosen by name: its gadget) */
  }
  docgen++;
  cache_clear();
  if (group != GID_PICTURE) { open_object(); return; }
  remove_object();
  paged = 0;
  /* a paged document is asked for page 1 drawn for the window; other datatypes ignore that */
  want_box(&bw, &bh);
  request(opt.clip ? 0 : flist[fcur], opt.clip ? DTST_CLIPBOARD : DTST_FILE, (ULONG)opt.clipunit, 1, bw, bh, 0);
  set_title();
}

static void next_file(int d)
{
  if (opt.clip) return;
  scan_drawer();
  if (nflist < 2) return;
  fcur = (fcur + d + nflist) % nflist;
  load_current();
}

/* ================================================================== object operations (save, print, copy) */
/* the object to save, print or copy: the gadget's, or the picture made again */
static Object *op_object(int *temp)
{
  *temp = 0;
  if (dto) return dto;
  if (!havepic) return NULL;
  *temp = 1;
  return NewDTObject(opt.clip ? (APTR)opt.clipunit : (APTR)flist[fcur], DTA_SourceType, opt.clip ? DTST_CLIPBOARD : DTST_FILE,
                     DTA_GroupID, GID_PICTURE, paged ? PDFDTA_Page : TAG_IGNORE, pic.page,
                     paged && pic.boxw ? PDFDTA_FitWidth : TAG_IGNORE, pic.boxw,
                     paged && pic.boxh ? PDFDTA_FitHeight : TAG_IGNORE, pic.boxh, TAG_DONE);
}
static void busy(int on) { if (win) { if (on) SetWindowPointer(win, WA_BusyPointer, TRUE, TAG_DONE); else SetWindowPointer(win, TAG_DONE); } }

static void do_copy(void)
{
  int temp;
  Object *o;
  busy(1);
  if ((o = op_object(&temp))) {
    if (!DoDTMethod(o, temp ? NULL : win, NULL, DTM_COPY, NULL)) tell("Copying to the clipboard failed (the datatype may not support it)");
    if (temp) DisposeDTObject(o);
  }
  busy(0);
}

static int asl_file(char *out, ULONG cap, int save, const char *hail)
{
  struct FileRequester *fr;
  char drawer[512];
  int ok = 0;
  if (!AslBase) return 0;
  drawer[0] = 0;
  if (fcur >= 0 && fcur < nflist && !opt.clip) { strcpy(drawer, flist[fcur]); *PathPart((STRPTR)drawer) = 0; }
  fr = AllocAslRequestTags(ASL_FileRequest, ASLFR_Window, (ULONG)win, ASLFR_TitleText, (ULONG)hail, ASLFR_InitialDrawer, (ULONG)drawer,
                           ASLFR_DoSaveMode, save, ASLFR_RejectIcons, TRUE, ASLFR_SleepWindow, TRUE, TAG_DONE);
  if (!fr) return 0;
  if (AslRequest(fr, NULL) && fr->fr_File[0]) {
    strncpy(out, (const char *)fr->fr_Drawer, cap - 1); out[cap - 1] = 0;
    AddPart((STRPTR)out, fr->fr_File, cap);
    ok = 1;
  }
  FreeAslRequest(fr);
  return ok;
}

static void do_save(void)
{
  char name[600];
  int temp;
  Object *o;
  BPTR fh;
  if (!asl_file(name, sizeof name, 1, "Save as IFF")) return;
  busy(1);
  if ((o = op_object(&temp))) {
    if ((fh = Open((STRPTR)name, MODE_NEWFILE))) {
      ULONG ok = DoDTMethod(o, temp ? NULL : win, NULL, DTM_WRITE, NULL, fh, DTWM_IFF, NULL);
      Close(fh);
      if (!ok) { DeleteFile((STRPTR)name); tell("This datatype can't save as IFF"); }
    } else tell("Can't write %s", name);
    if (temp) DisposeDTObject(o);
  }
  busy(0);
}

static void do_print(void)
{
  struct MsgPort *pp;
  union printerIO *pio;
  int temp;
  Object *o;
  if (!(pp = CreateMsgPort())) return;
  if (!(pio = (union printerIO *)CreateIORequest(pp, sizeof(union printerIO)))) { DeleteMsgPort(pp); return; }
  busy(1);
  if (OpenDevice((STRPTR)"printer.device", 0, (struct IORequest *)pio, 0)) tell("Can't open printer.device");
  else {
    if ((o = op_object(&temp))) {
      if (DoDTMethod(o, temp ? NULL : win, NULL, DTM_PRINT, NULL, pio, NULL)) tell("Printing failed");
      if (temp) DisposeDTObject(o);
    }
    CloseDevice((struct IORequest *)pio);
  }
  busy(0);
  DeleteIORequest((struct IORequest *)pio);
  DeleteMsgPort(pp);
}

static void do_open(void)
{
  char name[600];
  if (!asl_file(name, sizeof name, 0, "Open")) return;
  opt.clip = 0;
  free_list();
  fscanned = 0;
  if (add_file(name)) { fcur = 0; load_current(); }
}

/* ================================================================== benchmark */
static void bench(void)
{
  FvScene s;
  FvJob j;
  ULONG t[2][2], crc[2][2];
  int nat, q;
  char b[600];
  int n;
  if (!havepic || dto || !ensure_buffers()) { tell("BENCH needs a picture in the window"); return; }
  stop_job();
  scene(&s);
  for (nat = 1; nat >= 0; nat--)
    for (q = 0; q < 2; q++) {
      fv_u32 a, bb, i, c = 0;
      ULONG t0;
      runner(nat);
      t0 = now_us();
      if (!fv_job_begin(&run, &j, &s, q)) while (fv_job_step(&run, &j, 0x7FFFFFFF, &a, &bb) > 0) ;
      fv_job_end(&run, &j);
      t[nat][q] = now_us() - t0;
      for (i = 0; i < obn; i += 4) c = c * 31 + ((ULONG)obuf[i + 1] << 16 | (ULONG)obuf[i + 2] << 8 | obuf[i + 3]);
      crc[nat][q] = c;
    }
  runner(1);
  n = snprintf(b, sizeof b, "FitView BENCH: %lux%lu -> %lux%lu (window %lux%lu)\n", (unsigned long)pic.w, (unsigned long)pic.h,
               (unsigned long)geom.dw, (unsigned long)geom.dh, (unsigned long)obw, (unsigned long)obh);
#ifdef FITVIEW_NC
  n += snprintf(b + n, sizeof b - n, "  kernels: %s\n", fvnc_state());
  n += snprintf(b + n, sizeof b - n, "  preview: native %lu.%lu ms, 68k %lu.%lu ms%s\n", t[1][0] / 1000, t[1][0] / 100 % 10, t[0][0] / 1000, t[0][0] / 100 % 10,
                crc[1][0] == crc[0][0] ? ", same pixels" : ", DIFFERENT pixels");
  n += snprintf(b + n, sizeof b - n, "  refine:  native %lu.%lu ms, 68k %lu.%lu ms%s", t[1][1] / 1000, t[1][1] / 100 % 10, t[0][1] / 1000, t[0][1] / 100 % 10,
                crc[1][1] == crc[0][1] ? ", same pixels" : ", DIFFERENT pixels");
#else
  (void)crc;
  n += snprintf(b + n, sizeof b - n, "  preview %lu.%lu ms, refine %lu.%lu ms (68k code: the public build has no native kernels)", t[0][0] / 1000, t[0][0] / 100 % 10,
                t[0][1] / 1000, t[0][1] / 100 % 10);
#endif
  if (fromwb) { noecho = 1; tell("%s", b); noecho = 0; }
  else {
    say("%s\n", b);
#ifdef FITVIEW_NC
    fvnc_report(say);
#endif
  }
  blit_rows(0, 0);
  view_changed(0, 0);
}

/* ================================================================== menus */
enum { M_OPEN = 1, M_SAVE, M_PRINT, M_ABOUT, M_QUIT, M_COPY, M_FIT, M_WIDTH, M_ONE, M_FILL, M_ZIN, M_ZOUT,
       M_NPAGE, M_PPAGE, M_FPAGE, M_LPAGE, M_NFILE, M_PFILE, M_PLAY, M_SAVEPREFS, M_DITHER, M_CHECKER, M_BENCH };
static struct NewMenu nm[] = {
  { NM_TITLE, (STRPTR)"Project", 0, 0, 0, 0 },
  { NM_ITEM, (STRPTR)"Open...", (STRPTR)"O", 0, 0, (APTR)M_OPEN },
  { NM_ITEM, (STRPTR)"Save As IFF...", (STRPTR)"S", 0, 0, (APTR)M_SAVE },
  { NM_ITEM, (STRPTR)"Print", (STRPTR)"P", 0, 0, (APTR)M_PRINT },
  { NM_ITEM, NM_BARLABEL, 0, 0, 0, 0 },
  { NM_ITEM, (STRPTR)"About...", (STRPTR)"?", 0, 0, (APTR)M_ABOUT },
  { NM_ITEM, (STRPTR)"Quit", (STRPTR)"Q", 0, 0, (APTR)M_QUIT },
  { NM_TITLE, (STRPTR)"Edit", 0, 0, 0, 0 },
  { NM_ITEM, (STRPTR)"Copy", (STRPTR)"C", 0, 0, (APTR)M_COPY },
  { NM_TITLE, (STRPTR)"View", 0, 0, 0, 0 },
  { NM_ITEM, (STRPTR)"Fit to window", (STRPTR)"0", CHECKIT, ~1L, (APTR)M_FIT },
  { NM_ITEM, (STRPTR)"Fit width", (STRPTR)"W", CHECKIT, ~2L, (APTR)M_WIDTH },
  { NM_ITEM, (STRPTR)"1:1", (STRPTR)"1", CHECKIT, ~4L, (APTR)M_ONE },
  { NM_ITEM, (STRPTR)"Fill window", (STRPTR)"L", CHECKIT, ~8L, (APTR)M_FILL },
  { NM_ITEM, (STRPTR)"Zoom in", (STRPTR)"+", 0, 0, (APTR)M_ZIN },
  { NM_ITEM, (STRPTR)"Zoom out", (STRPTR)"-", 0, 0, (APTR)M_ZOUT },
  { NM_ITEM, NM_BARLABEL, 0, 0, 0, 0 },
  { NM_ITEM, (STRPTR)"Next page", (STRPTR)">", 0, 0, (APTR)M_NPAGE },
  { NM_ITEM, (STRPTR)"Previous page", (STRPTR)"<", 0, 0, (APTR)M_PPAGE },
  { NM_ITEM, (STRPTR)"First page", 0, 0, 0, (APTR)M_FPAGE },
  { NM_ITEM, (STRPTR)"Last page", 0, 0, 0, (APTR)M_LPAGE },
  { NM_ITEM, NM_BARLABEL, 0, 0, 0, 0 },
  { NM_ITEM, (STRPTR)"Next file", (STRPTR)"N", 0, 0, (APTR)M_NFILE },
  { NM_ITEM, (STRPTR)"Previous file", (STRPTR)"B", 0, 0, (APTR)M_PFILE },
  { NM_ITEM, (STRPTR)"Play", (STRPTR)"Y", 0, 0, (APTR)M_PLAY },
  { NM_TITLE, (STRPTR)"Settings", 0, 0, 0, 0 },
  { NM_ITEM, (STRPTR)"Dither on few colours", 0, CHECKIT | MENUTOGGLE, 0, (APTR)M_DITHER },
  { NM_ITEM, (STRPTR)"Checkerboard under alpha", 0, CHECKIT | MENUTOGGLE, 0, (APTR)M_CHECKER },
  { NM_ITEM, (STRPTR)"Benchmark", 0, 0, 0, (APTR)M_BENCH },
  { NM_ITEM, NM_BARLABEL, 0, 0, 0, 0 },
  { NM_ITEM, (STRPTR)"Save as default", 0, 0, 0, (APTR)M_SAVEPREFS },
  { NM_END, 0, 0, 0, 0, 0 }
};
static struct MenuItem *find_item(ULONG id)
{
  struct Menu *m;
  struct MenuItem *it;
  for (m = menus; m; m = m->NextMenu)
    for (it = m->FirstItem; it; it = it->NextItem)
      if ((ULONG)GTMENUITEM_USERDATA(it) == id) return it;
  return NULL;
}
static void set_check(ULONG id, int on)
{
  struct MenuItem *it = find_item(id);
  if (!it) return;
  if (on) it->Flags |= CHECKED; else it->Flags &= ~CHECKED;
}
static void update_menus(void)
{
  if (!win || !menus) return;
  ClearMenuStrip(win);
  set_check(M_FIT, prefs.mode == FV_FIT); set_check(M_WIDTH, prefs.mode == FV_WIDTH);
  set_check(M_ONE, prefs.mode == FV_ONE); set_check(M_FILL, prefs.mode == FV_FILL);
  set_check(M_DITHER, prefs.dither); set_check(M_CHECKER, prefs.checker);
  ResetMenuStrip(win, menus);
}

/* ================================================================== prefs */
static void read_prefs_file(const char *name, FvPrefs *p)
{
  BPTR f;
  char buf[1024];
  LONG n;
  if (!(f = Open((STRPTR)name, MODE_OLDFILE))) return;
  n = Read(f, buf, sizeof buf - 1);
  Close(f);
  if (n > 0) fvp_parse(p, buf, (unsigned long)n);
}
static int write_prefs_file(const char *name, const FvPrefs *p)
{
  BPTR f;
  char buf[400];
  unsigned long n = fvp_format(p, buf, sizeof buf);
  if (!(f = Open((STRPTR)name, MODE_NEWFILE))) return 0;
  Write(f, buf, (LONG)n);
  Close(f);
  return 1;
}
static void save_prefs(int window)
{
  FvPrefs p = prefs;
  if (window && win && !ownscr && !opt.backdrop) {
    p.winset = 1; p.wx = win->LeftEdge; p.wy = win->TopEdge; p.ww = win->Width; p.wh = win->Height;
  } else if (!window) { p.winset = savedprefs.winset; p.wx = savedprefs.wx; p.wy = savedprefs.wy; p.ww = savedprefs.ww; p.wh = savedprefs.wh; }
  write_prefs_file(PREFS_ENV, &p);
  if (!write_prefs_file(PREFS_ENVARC, &p) && window) tell("Can't write %s", PREFS_ENVARC);
  savedprefs = p;
}

/* ================================================================== commands */
static void set_mode(int mode)
{
  FvGeom old = geom;
  ULONG bw, bh;
  if (dto) return;
  if (mode == FV_ZOOM && prefs.mode != FV_ZOOM) prefs.zoom = geom.pm ? geom.pm : 1000;
  prefs.mode = mode;
  update_menus();
  view_changed(1, &old);
  if (paged && havepic) {
    want_box(&bw, &bh);
    if (bw != pic.boxw || bh != pic.boxh) load_page(pic.page, 0);
  }
}
static void zoom(int dir)
{
  FvGeom old = geom;
  if (dto || !havepic) return;
  prefs.zoom = fv_zoom_step(geom.pm, dir);
  prefs.mode = FV_ZOOM;
  update_menus();
  view_changed(1, &old);
}
static void goto_page(LONG p)
{
  if (!paged || !havepic) return;
  if (p < 1) p = 1;
  if ((ULONG)p > pic.pages) p = (LONG)pic.pages;
  if ((ULONG)p == pic.page && !lnext.want) return;
  load_page((ULONG)p, 0);
}
/* Space and Backspace, Page Down/Up: a page of a document; else down a screenful, then the next file */
static void page_key(int d)
{
  if (dto) { scroll_to((LONG)gtoph, (LONG)gtop + d * (LONG)(gvis > 1 ? gvis - 1 : 1)); return; }
  if (paged) { goto_page((LONG)pic.page + d); return; }
  if (havepic && geom.maxy && ((d > 0 && sy < geom.maxy) || (d < 0 && sy > 0))) { scroll_to((LONG)sx, (LONG)sy + d * (LONG)(IH() * 9 / 10)); return; }
  next_file(d);
}
static void trigger(ULONG what)
{
  if (dto) DoDTMethod(dto, win, NULL, DTM_TRIGGER, NULL, what, NULL);
}
static int running = 1;

static void about(void)
{
  noecho = 1;
  tell("FitView " VERSTR "\n\nA MultiView replacement that fits pictures to the window.\n"
       "Pictures: %s\nPages: Space/Backspace, Home/End\nZoom: 0 fit, W width, 1 1:1, F fill, +/-\n"
       "Copyright (C) 2026 the WBFit author. GPL-2.0-only."
#ifdef FITVIEW_NC
       "\n\nNativeCall test build: %s"
#endif
       , rtg ? "drawn in true colour (cybergraphics)" : "dithered to the screen's pens"
#ifdef FITVIEW_NC
       , fvnc_state()
#endif
       );
  noecho = 0;
}

static void command(ULONG id)
{
  switch (id) {
  case M_OPEN: do_open(); break;
  case M_SAVE: do_save(); break;
  case M_PRINT: do_print(); break;
  case M_ABOUT: about(); break;
  case M_QUIT: running = 0; break;
  case M_COPY: do_copy(); break;
  case M_FIT: set_mode(FV_FIT); break;
  case M_WIDTH: set_mode(FV_WIDTH); break;
  case M_ONE: set_mode(FV_ONE); break;
  case M_FILL: set_mode(FV_FILL); break;
  case M_ZIN: zoom(1); break;
  case M_ZOUT: zoom(-1); break;
  case M_NPAGE: goto_page((LONG)pic.page + 1); break;
  case M_PPAGE: goto_page((LONG)pic.page - 1); break;
  case M_FPAGE: goto_page(1); break;
  case M_LPAGE: goto_page((LONG)pic.pages); break;
  case M_NFILE: next_file(1); break;
  case M_PFILE: next_file(-1); break;
  case M_PLAY: trigger(STM_PLAY); break;
  case M_DITHER: prefs.dither = !prefs.dither; view_changed(0, 0); break;
  case M_CHECKER: prefs.checker = !prefs.checker; view_changed(0, 0); break;
  case M_BENCH: bench(); break;
  case M_SAVEPREFS: save_prefs(1); break;
  }
}

/* ================================================================== ARexx (optional, no results) */
static void rexx_msg(struct RexxMsg *m)
{
  char cmd[300], *arg;
  const char *s = (const char *)m->rm_Args[0];
  LONG rc = 0;
  int i;
  for (i = 0; s && s[i] && i < (int)sizeof cmd - 1; i++) cmd[i] = s[i];
  cmd[i] = 0;
  for (arg = cmd; *arg && *arg != ' '; arg++) ;
  if (*arg) *arg++ = 0;
  while (*arg == ' ') arg++;
  if (!Stricmp((STRPTR)cmd, (STRPTR)"QUIT")) running = 0;
  else if (!Stricmp((STRPTR)cmd, (STRPTR)"OPEN") && *arg) {
    char full[600];
    if (*arg == '"') { arg++; if (arg[0] && arg[strlen(arg) - 1] == '"') arg[strlen(arg) - 1] = 0; }
    full_path(0, arg, full, sizeof full);
    opt.clip = 0; free_list(); fscanned = 0;
    if (add_file(full)) { fcur = 0; load_current(); }
  }
  else if (!Stricmp((STRPTR)cmd, (STRPTR)"NEXTPAGE")) goto_page((LONG)pic.page + 1);
  else if (!Stricmp((STRPTR)cmd, (STRPTR)"PREVPAGE")) goto_page((LONG)pic.page - 1);
  else if (!Stricmp((STRPTR)cmd, (STRPTR)"FIRSTPAGE")) goto_page(1);
  else if (!Stricmp((STRPTR)cmd, (STRPTR)"LASTPAGE")) goto_page((LONG)pic.pages);
  else if (!Stricmp((STRPTR)cmd, (STRPTR)"NEXTFILE")) next_file(1);
  else if (!Stricmp((STRPTR)cmd, (STRPTR)"PREVFILE")) next_file(-1);
  else if (!Stricmp((STRPTR)cmd, (STRPTR)"FIT")) set_mode(FV_FIT);
  else if (!Stricmp((STRPTR)cmd, (STRPTR)"FITWIDTH")) set_mode(FV_WIDTH);
  else if (!Stricmp((STRPTR)cmd, (STRPTR)"ONETOONE")) set_mode(FV_ONE);
  else if (!Stricmp((STRPTR)cmd, (STRPTR)"FILL")) set_mode(FV_FILL);
  else if (!Stricmp((STRPTR)cmd, (STRPTR)"ZOOMIN")) zoom(1);
  else if (!Stricmp((STRPTR)cmd, (STRPTR)"ZOOMOUT")) zoom(-1);
  else if (!Stricmp((STRPTR)cmd, (STRPTR)"WINDOWTOFRONT")) WindowToFront(win);
  else if (!Stricmp((STRPTR)cmd, (STRPTR)"SCREENTOFRONT")) ScreenToFront(scr);
  else if (!Stricmp((STRPTR)cmd, (STRPTR)"ACTIVATE")) ActivateWindow(win);
  else if (!Stricmp((STRPTR)cmd, (STRPTR)"CHANGEWINDOW")) {
    /* CHANGEWINDOW left top width height (the tests resize the window this way) */
    LONG v[4], k;
    const char *q = arg;
    for (k = 0; k < 4; k++) { v[k] = 0; while (*q == ' ') q++; while (*q >= '0' && *q <= '9') v[k] = v[k] * 10 + (*q++ - '0'); }
    ChangeWindowBox(win, v[0], v[1], v[2], v[3]);
  }
  else if (!Stricmp((STRPTR)cmd, (STRPTR)"BENCH")) bench();
  else rc = 10;
  m->rm_Result1 = rc; m->rm_Result2 = 0;
}

/* ================================================================== the window */
static Object *sysimg(ULONG which, ULONG size)
{
  return NewObject(NULL, (UBYTE *)SYSICLASS, SYSIA_DrawInfo, (ULONG)dri, SYSIA_Which, which, SYSIA_Size, size, TAG_DONE);
}
static ULONG iwidth(Object *o) { ULONG v = 0; if (o) GetAttr(IA_Width, o, &v); return v; }
static ULONG iheight(Object *o) { ULONG v = 0; if (o) GetAttr(IA_Height, o, &v); return v; }

/* proportional scrollers and arrows in the right and bottom borders */
static int make_scrollers(void)
{
  ULONG size = (scr->Flags & SCREENHIRES) ? SYSISIZE_MEDRES : SYSISIZE_LOWRES;
  ULONG sw, sh, uh, dh, lw, rw, top = scr->WBorTop + scr->Font->ta_YSize + 1;
  struct Gadget *prev = NULL;
  if (!(isize = sysimg(SIZEIMAGE, size)) || !(iup = sysimg(UPIMAGE, size)) || !(idown = sysimg(DOWNIMAGE, size)) ||
      !(ileft = sysimg(LEFTIMAGE, size)) || !(iright = sysimg(RIGHTIMAGE, size))) return 0;
  sw = iwidth(isize); sh = iheight(isize);
  uh = iheight(iup); dh = iheight(idown); lw = iwidth(ileft); rw = iwidth(iright);
  vprop = NewObject(NULL, (UBYTE *)PROPGCLASS, GA_RelRight, -(LONG)sw + 5, GA_Top, top + 1, GA_Width, sw - 8,
                    GA_RelHeight, -(LONG)(top + sh + uh + dh + 2), GA_RightBorder, TRUE, GA_ID, GID_VPROP,
                    PGA_Freedom, FREEVERT, PGA_NewLook, TRUE, PGA_Borderless, (dri->dri_Flags & DRIF_NEWLOOK) ? TRUE : FALSE,
                    PGA_Total, 1, PGA_Visible, 1, PGA_Top, 0, ICA_TARGET, ICTARGET_IDCMP, TAG_DONE);
  gfirst = prev = (struct Gadget *)vprop;
  bup = NewObject(NULL, (UBYTE *)BUTTONGCLASS, GA_Image, (ULONG)iup, GA_RelRight, 1 - (LONG)iwidth(iup), GA_RelBottom, 1 - (LONG)(sh + uh + dh),
                  GA_RightBorder, TRUE, GA_ID, GID_UP, GA_Previous, (ULONG)prev, ICA_TARGET, ICTARGET_IDCMP, TAG_DONE);
  prev = (struct Gadget *)bup;
  bdown = NewObject(NULL, (UBYTE *)BUTTONGCLASS, GA_Image, (ULONG)idown, GA_RelRight, 1 - (LONG)iwidth(idown), GA_RelBottom, 1 - (LONG)(sh + dh),
                    GA_RightBorder, TRUE, GA_ID, GID_DOWN, GA_Previous, (ULONG)prev, ICA_TARGET, ICTARGET_IDCMP, TAG_DONE);
  prev = (struct Gadget *)bdown;
  hprop = NewObject(NULL, (UBYTE *)PROPGCLASS, GA_Left, scr->WBorLeft, GA_RelBottom, -(LONG)sh + 3,
                    GA_RelWidth, -(LONG)(scr->WBorLeft + sw + lw + rw + 1), GA_Height, sh - 4, GA_BottomBorder, TRUE, GA_ID, GID_HPROP,
                    PGA_Freedom, FREEHORIZ, PGA_NewLook, TRUE, PGA_Borderless, (dri->dri_Flags & DRIF_NEWLOOK) ? TRUE : FALSE,
                    PGA_Total, 1, PGA_Visible, 1, PGA_Top, 0, GA_Previous, (ULONG)prev, ICA_TARGET, ICTARGET_IDCMP, TAG_DONE);
  prev = (struct Gadget *)hprop;
  bleft = NewObject(NULL, (UBYTE *)BUTTONGCLASS, GA_Image, (ULONG)ileft, GA_RelRight, 1 - (LONG)(sw + rw + lw), GA_RelBottom, 1 - (LONG)iheight(ileft),
                    GA_BottomBorder, TRUE, GA_ID, GID_LEFT, GA_Previous, (ULONG)prev, ICA_TARGET, ICTARGET_IDCMP, TAG_DONE);
  prev = (struct Gadget *)bleft;
  bright = NewObject(NULL, (UBYTE *)BUTTONGCLASS, GA_Image, (ULONG)iright, GA_RelRight, 1 - (LONG)(sw + rw), GA_RelBottom, 1 - (LONG)iheight(iright),
                     GA_BottomBorder, TRUE, GA_ID, GID_RIGHT, GA_Previous, (ULONG)prev, ICA_TARGET, ICTARGET_IDCMP, TAG_DONE);
  return vprop && bup && bdown && hprop && bleft && bright;
}
static void free_scrollers(void)
{
  if (bright) DisposeObject(bright);
  if (bleft) DisposeObject(bleft);
  if (hprop) DisposeObject(hprop);
  if (bdown) DisposeObject(bdown);
  if (bup) DisposeObject(bup);
  if (vprop) DisposeObject(vprop);
  if (iright) DisposeObject(iright);
  if (ileft) DisposeObject(ileft);
  if (idown) DisposeObject(idown);
  if (iup) DisposeObject(iup);
  if (isize) DisposeObject(isize);
  bright = bleft = hprop = bdown = bup = vprop = iright = ileft = idown = iup = isize = 0;
}

static int open_screen(void)
{
  if (opt.ownscreen) {
    struct Screen *wb = LockPubScreen(NULL);
    ULONG mode = wb ? GetVPModeID(&wb->ViewPort) : (ULONG)INVALID_ID, depth = wb ? GetBitMapAttr(wb->RastPort.BitMap, BMA_DEPTH) : 8;
    if (wb) {
      scr = OpenScreenTags(NULL, SA_LikeWorkbench, TRUE, SA_DisplayID, mode, SA_Depth, depth, SA_Title, (ULONG)"FitView",
                           SA_ShowTitle, FALSE, SA_Quiet, TRUE, SA_Interleaved, TRUE, TAG_DONE);
      UnlockPubScreen(NULL, wb);
    }
    if (scr) { ownscr = 1; return 1; }
  }
  if (opt.pubscreen[0] && (scr = LockPubScreen((STRPTR)opt.pubscreen))) { scrlocked = 1; return 1; }
  if ((scr = LockPubScreen(NULL))) { scrlocked = 1; return 1; }
  return 0;
}

static int open_window(void)
{
  ULONG depth, modeid;
  int borderless = ownscr || opt.backdrop;
  LONG l, t, w, h;
  struct DisplayInfo dinfo;
  if (!(dri = GetScreenDrawInfo(scr))) return 0;
  vi = GetVisualInfo(scr, TAG_DONE);
  depth = GetBitMapAttr(scr->RastPort.BitMap, BMA_DEPTH);
  rtg = !opt.pens && CyberGfxBase && depth > 8 && GetCyberMapAttr(scr->RastPort.BitMap, CYBRMATTR_ISCYBERGFX);
  usewcp = GfxBase->LibNode.lib_Version >= 45;
  modeid = GetVPModeID(&scr->ViewPort);
  if (modeid != (ULONG)INVALID_ID && GetDisplayInfoData(NULL, (UBYTE *)&dinfo, sizeof dinfo, DTAG_DISP, modeid)) {
    scrrx = dinfo.Resolution.x; scrry = dinfo.Resolution.y;
  }
  if (!rtg) {
    if (!(dcache = fv_alloc(65536))) return 0;
    seen(dcache, 65536);
    obtain_pens();
  }
  if (!borderless && !make_scrollers()) return 0;
  if (prefs.winset && !borderless) { l = prefs.wx; t = prefs.wy; w = prefs.ww; h = prefs.wh; sized = 1; }
  else { w = scr->Width * 2 / 3; h = scr->Height * 2 / 3; l = (scr->Width - w) / 2; t = (scr->Height - h) / 2; }
  if (w > scr->Width) w = scr->Width;
  if (h > scr->Height) h = scr->Height;
  if (l + w > scr->Width) l = scr->Width - w;
  if (t + h > scr->Height) t = scr->Height - h;
  if (borderless) { l = 0; t = ownscr ? 0 : scr->BarHeight + 1; w = scr->Width; h = scr->Height - t; }
  win = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)scr, WA_Left, l, WA_Top, t, WA_Width, w, WA_Height, h,
                       WA_Title, borderless ? 0 : (ULONG)"FitView", WA_ScreenTitle, (ULONG)(version + 6),
                       WA_DragBar, !borderless, WA_DepthGadget, !borderless, WA_CloseGadget, !borderless, WA_SizeGadget, !borderless,
                       WA_SizeBRight, !borderless, WA_SizeBBottom, !borderless, WA_Borderless, borderless, WA_Backdrop, borderless,
                       WA_Gadgets, borderless ? 0 : (ULONG)gfirst, WA_SimpleRefresh, TRUE, WA_Activate, TRUE, WA_ReportMouse, TRUE,
                       WA_NewLookMenus, TRUE, WA_MinWidth, 120, WA_MinHeight, 80, WA_MaxWidth, ~0UL, WA_MaxHeight, ~0UL,
                       WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_NEWSIZE | IDCMP_CHANGEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_MENUPICK |
                                 IDCMP_RAWKEY | IDCMP_VANILLAKEY | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_IDCMPUPDATE,
                       TAG_DONE);
  if (!win) return 0;
  if (vi && (menus = CreateMenus(nm, GTMN_FrontPen, 0, TAG_DONE)) && LayoutMenus(menus, vi, GTMN_NewLookMenus, TRUE, TAG_DONE)) {
    SetMenuStrip(win, menus);
    update_menus();
  }
  if (WorkbenchBase && (appport = CreateMsgPort())) appwin = AddAppWindow(1, 0, win, appport, NULL);
  ensure_buffers();
  set_title();
  return 1;
}

static void close_window(void)
{
  stop_job();
  remove_object();
  if (appwin) RemoveAppWindow(appwin);
  appwin = 0;
  if (appport) {
    struct Message *m;
    while ((m = GetMsg(appport))) ReplyMsg(m);
    DeleteMsgPort(appport);
  }
  appport = 0;
  if (win) {
    if (menus) ClearMenuStrip(win);
    CloseWindow(win);
  }
  win = 0;
  if (menus) FreeMenus(menus);
  menus = 0;
  free_scrollers();
  free_buffers();
  release_pens();
  fv_free(dcache); dcache = 0;
  if (vi) FreeVisualInfo(vi);
  vi = 0;
  if (dri) FreeScreenDrawInfo(scr, dri);
  dri = 0;
  if (scr) { if (ownscr) CloseScreen(scr); else if (scrlocked) UnlockPubScreen(NULL, scr); }
  scr = 0;
}

/* ================================================================== events */
static int dragging;
static WORD dragx, dragy;
static fv_u32 dragsx, dragsy;

static void resized(void)
{
  FvGeom old = geom;
  ULONG bw, bh;
  sized = 1;                     /* (a size given before the first picture is kept) */
  if (dto) return;               /* (the gadget lays itself out: GA_RelWidth/Height) */
  if (!ensure_buffers()) { stop_job(); return; }
  view_changed(1, &old);
  if (paged && havepic && prefs.mode != FV_ONE && prefs.mode != FV_ZOOM) {
    want_box(&bw, &bh);
    (void)bw; (void)bh;          /* (the page is drawn again for the new size when the window settles) */
  }
}

static void idcmp_update(struct TagItem *tags)
{
  struct TagItem *ti, *tstate = tags;
  ULONG id = GetTagData(GA_ID, 0, tags);
  if (id == GID_DTO) {
    int sync = 0, props = 0;
    while ((ti = NextTagItem(&tstate))) {
      switch (ti->ti_Tag) {
      case DTA_Busy: if (ti->ti_Data) SetWindowPointer(win, WA_BusyPointer, TRUE, TAG_DONE); else SetWindowPointer(win, TAG_DONE); break;
      case DTA_Sync: sync = 1; break;
      case DTA_TopVert: gtop = ti->ti_Data; props = 1; break;
      case DTA_VisibleVert: gvis = ti->ti_Data; props = 1; break;
      case DTA_TotalVert: gtot = ti->ti_Data; props = 1; break;
      case DTA_TopHoriz: gtoph = ti->ti_Data; props = 1; break;
      case DTA_VisibleHoriz: gvish = ti->ti_Data; props = 1; break;
      case DTA_TotalHoriz: gtoth = ti->ti_Data; props = 1; break;
      }
    }
    if (sync && dto) RefreshDTObjects(dto, win, NULL, NULL);
    if (props) set_props();
    return;
  }
  if (id == GID_VPROP || id == GID_HPROP) {
    ULONG top = GetTagData(PGA_Top, 0, tags);
    if (dto) {
      if (id == GID_VPROP) scroll_to((LONG)gtoph, (LONG)fv_unprop(gtot, gvis, top));
      else scroll_to((LONG)fv_unprop(gtoth, gvish, top), (LONG)gtop);
    } else if (havepic) {
      if (id == GID_VPROP) scroll_to((LONG)sx, (LONG)fv_unprop(geom.dh, geom.dh < IH() ? geom.dh : IH(), top));
      else scroll_to((LONG)fv_unprop(geom.dw, geom.dw < IW() ? geom.dw : IW(), top), (LONG)sy);
    }
    return;
  }
  {
    LONG step = dto ? 1 : 24, x = dto ? (LONG)gtoph : (LONG)sx, y = dto ? (LONG)gtop : (LONG)sy;
    if (id == GID_UP) scroll_to(x, y - step);
    else if (id == GID_DOWN) scroll_to(x, y + step);
    else if (id == GID_LEFT) scroll_to(x - step, y);
    else if (id == GID_RIGHT) scroll_to(x + step, y);
  }
}

static void rawkey(UWORD code, UWORD qual)
{
  int shift = (qual & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) != 0;
  LONG x = dto ? (LONG)gtoph : (LONG)sx, y = dto ? (LONG)gtop : (LONG)sy;
  LONG pv = dto ? (LONG)(gvis > 1 ? gvis - 1 : 1) : (LONG)(IH() * 9 / 10), ph = dto ? (LONG)(gvish > 1 ? gvish - 1 : 1) : (LONG)(IW() * 9 / 10);
  LONG step = dto ? 1 : 32;
  switch (code) {
  case 0x4C: if (shift) scroll_to(x, y - pv); else scroll_to(x, y - step); break;          /* up */
  case 0x4D: if (shift) scroll_to(x, y + pv); else scroll_to(x, y + step); break;          /* down */
  case 0x4F: if (shift) scroll_to(x - ph, y); else scroll_to(x - step, y); break;          /* left */
  case 0x4E: if (shift) scroll_to(x + ph, y); else scroll_to(x + step, y); break;          /* right */
  case 0x48: page_key(-1); break;                                                          /* Page Up */
  case 0x49: page_key(1); break;                                                           /* Page Down */
  case 0x70: if (paged) goto_page(1); else scroll_to(x, 0); break;                         /* Home */
  case 0x71: if (paged) goto_page((LONG)pic.pages); else scroll_to(x, 0x7FFFFFF); break;   /* End */
  case 0x7A: scroll_to(x, y - step * 2); break;                                            /* wheel */
  case 0x7B: scroll_to(x, y + step * 2); break;
  case 0x5F: about(); break;                                                               /* Help */
  }
}

static void vanilla(UWORD c)
{
  switch (c) {
  case ' ': page_key(1); break;
  case 8: page_key(-1); break;
  case '+': case '=': zoom(1); break;
  case '-': case '_': zoom(-1); break;
  case '0': set_mode(FV_FIT); break;
  case '1': set_mode(FV_ONE); break;
  case 'w': case 'W': set_mode(FV_WIDTH); break;
  case 'f': case 'F': set_mode(FV_FILL); break;
  case 'n': case 'N': next_file(1); break;
  case 'b': case 'B': next_file(-1); break;
  case 'p': case 'P': if (dto) trigger(STM_PLAY); break;
  case 27: case 'q': case 'Q': running = 0; break;
  }
}

static void app_message(struct AppMessage *am)
{
  LONG i;
  char full[600];
  if (am->am_NumArgs < 1) return;
  opt.clip = 0;
  free_list();
  fscanned = 0;
  for (i = 0; i < am->am_NumArgs; i++) {
    full_path(am->am_ArgList[i].wa_Lock, (const char *)am->am_ArgList[i].wa_Name, full, sizeof full);
    if (am->am_ArgList[i].wa_Name && am->am_ArgList[i].wa_Name[0]) add_file(full);
  }
  if (nflist > 1) fscanned = 3;            /* (several dropped: those are the list) */
  if (nflist) { fcur = 0; load_current(); WindowToFront(win); }
}

static void handle_window(void)
{
  struct IntuiMessage *im;
  while (win && (im = GT_GetIMsg(win->UserPort))) {
    ULONG cls = im->Class;
    UWORD code = im->Code, qual = im->Qualifier;
    WORD mx = im->MouseX, my = im->MouseY;
    if (cls == IDCMP_IDCMPUPDATE) idcmp_update((struct TagItem *)im->IAddress);   /* (the tags go with the reply) */
    GT_ReplyIMsg(im);
    switch (cls) {
    case IDCMP_CLOSEWINDOW: running = 0; break;
    case IDCMP_NEWSIZE: resized(); break;
    case IDCMP_CHANGEWINDOW: if (obw != IW() || obh != IH()) resized(); break;
    case IDCMP_REFRESHWINDOW:
      BeginRefresh(win);
      if (!dto && havepic) blit_rows(0, obh);
      EndRefresh(win, TRUE);
      break;
    case IDCMP_MENUPICK:
      while (code != MENUNULL && running && win) {
        struct MenuItem *it = ItemAddress(menus, code);
        if (!it) break;
        command((ULONG)GTMENUITEM_USERDATA(it));
        code = it->NextSelect;
      }
      break;
    case IDCMP_RAWKEY: if (!(code & IECODE_UP_PREFIX)) rawkey(code, qual); break;
    case IDCMP_VANILLAKEY: vanilla(code); break;
    case IDCMP_MOUSEBUTTONS:
      if (code == SELECTDOWN && !dto && havepic && mx >= win->BorderLeft && my >= win->BorderTop &&
          mx < win->Width - win->BorderRight && my < win->Height - win->BorderBottom) {
        dragging = 1; dragx = mx; dragy = my; dragsx = sx; dragsy = sy;
      } else if (code == SELECTUP) dragging = 0;
      break;
    case IDCMP_MOUSEMOVE:
      if (dragging) scroll_to((LONG)dragsx - (mx - dragx), (LONG)dragsy - (my - dragy));
      break;
    }
  }
}

/* when nothing else is to be done: refine the view, draw the page again for the window, fetch the next page */
static void idle(void)
{
  ULONG bw, bh;
  if (!havepic || dto || jobon) return;
  if (refine_due && !refined) {
    refine_due = 0;
    if (needs_refine()) { start_job(FV_REFINE); return; }
    refined = 1;
  }
  if (!refined || linflight || lnext.want) return;
  if (paged) {
    want_box(&bw, &bh);
    if (bw != pic.boxw || bh != pic.boxh) { load_page(pic.page, 0); return; }
    if (pic.page < pic.pages && cache_find(pic.page + 1, bw, bh) < 0) { load_page(pic.page + 1, 1); return; }
    if (pic.page > 1 && cache_find(pic.page - 1, bw, bh) < 0) { load_page(pic.page - 1, 1); return; }
  }
  if (opt.bench) { opt.bench = 0; bench(); }
}

static void event_loop(void)
{
  ULONG wsig, lsig, tsig, asig, rsig;
  while (running && win) {
    ULONG sigs, mask;
    wsig = 1UL << win->UserPort->mp_SigBit;
    lsig = 1UL << lreply->mp_SigBit;
    tsig = topen ? 1UL << tport->mp_SigBit : 0;
    asig = appport ? 1UL << appport->mp_SigBit : 0;
    rsig = rexxport ? 1UL << rexxport->mp_SigBit : 0;
    mask = wsig | lsig | tsig | asig | rsig | SIGBREAKF_CTRL_C;
    idle();
    if (jobon) {
      sigs = SetSignal(0, 0) & mask;
      if (!sigs) { work(); continue; }
      sigs = SetSignal(0, mask) & mask;
    } else sigs = Wait(mask);
    if (sigs & SIGBREAKF_CTRL_C) running = 0;
    if (sigs & tsig) {
      if (tbusy && CheckIO((struct IORequest *)treq)) { WaitIO((struct IORequest *)treq); tbusy = 0; refine_due = 1; }
    }
    if (sigs & lsig) { struct Message *m; while ((m = GetMsg(lreply))) loaded(); }
    if (sigs & asig) { struct AppMessage *am; while ((am = (struct AppMessage *)GetMsg(appport))) { app_message(am); ReplyMsg((struct Message *)am); } }
    if (sigs & rsig) {
      struct RexxMsg *m;
      while ((m = (struct RexxMsg *)GetMsg(rexxport))) { rexx_msg(m); ReplyMsg((struct Message *)m); }
    }
    handle_window();
  }
}

/* ================================================================== arguments */
static int set_fit(const char *s)
{
  if (!fvp_mode(s, &prefs.mode)) { tell("FIT=%s: use FIT, WIDTH, 1:1 or FILL", s); return 0; }
  opt.fitgiven = 1;
  return 1;
}

/* a FILE argument: a name, or a pattern (#?.jpg) for every file it matches, sorted */
static void add_pattern(const char *name)
{
  char tok[300], full[600];
  struct AnchorPath *ap;
  int first = nflist, i, j;
  if (ParsePatternNoCase((STRPTR)name, (STRPTR)tok, sizeof tok) != 1 ||
      !(ap = AllocVec(sizeof(struct AnchorPath) + 512, MEMF_ANY | MEMF_CLEAR))) {
    full_path(0, name, full, sizeof full);
    add_file(full);
    return;
  }
  ap->ap_Strlen = 512;
  if (!MatchFirst((STRPTR)name, ap))
    do {
      if (ap->ap_Info.fib_DirEntryType > 0) continue;
      full_path(0, (const char *)ap->ap_Buf, full, sizeof full);
      if (nflist < 4000) add_file(full);
    } while (!MatchNext(ap));
  MatchEnd(ap);
  FreeVec(ap);
  for (i = first + 1; i < nflist; i++) {
    char *t = flist[i];
    for (j = i; j > first && Stricmp((STRPTR)flist[j - 1], (STRPTR)t) > 0; j--) flist[j] = flist[j - 1];
    flist[j] = t;
  }
}

static const char *tt(struct DiskObject *d, const char *n) { return d ? (const char *)FindToolType((STRPTR *)d->do_ToolTypes, (STRPTR)n) : NULL; }

static int get_args(int argc)
{
  char full[600];
  read_prefs_file(PREFS_ENVARC, &prefs);
  read_prefs_file(PREFS_ENV, &prefs);
  if (argc) {
    static const char tmpl[] = "FILE/M,BASENAME/K,CLIPBOARD/S,CLIPUNIT/K/N,SCREEN/S,PUBSCREEN/K,REQUESTER/S,BOOKMARK/S,"
                               "FONTNAME/K,FONTSIZE/K/N,BACKDROP/S,WINDOW/S,PORTNAME/K,FIT/K,BENCH/S,V40/S,PENS/S";
    LONG a[17];
    struct RDArgs *rd;
    memset(a, 0, sizeof a);
    if (!(rd = ReadArgs((STRPTR)tmpl, a, NULL))) { PrintFault(IoErr(), (STRPTR)"FitView"); return 0; }
    if (a[0]) { char **f = (char **)a[0]; for (; *f; f++) add_pattern(*f); }
    if (a[1]) strncpy(opt.basename, (char *)a[1], sizeof opt.basename - 1);
    opt.clip = a[2] != 0;
    if (a[3]) opt.clipunit = *(LONG *)a[3];
    opt.ownscreen = a[4] != 0;
    if (a[5]) strncpy(opt.pubscreen, (char *)a[5], sizeof opt.pubscreen - 1);
    opt.requester = a[6] != 0;
    if (a[8]) strncpy(opt.fontname, (char *)a[8], sizeof opt.fontname - 1);
    if (a[9]) opt.fontsize = *(LONG *)a[9];
    opt.backdrop = a[10] != 0;
    if (a[12]) strncpy(opt.portname, (char *)a[12], sizeof opt.portname - 1);
    if (a[13] && !set_fit((char *)a[13])) { FreeArgs(rd); return 0; }
    opt.bench = a[14] != 0;
    opt.v40 = a[15] != 0;
    opt.pens = a[16] != 0;
    FreeArgs(rd);
  } else {
    /* Workbench: the tool's tool types; the files are the other icons */
    struct WBStartup *wbs = _WBenchMsg;
    struct DiskObject *d = NULL;
    LONG i;
    const char *s;
    if (IconBase && wbs && wbs->sm_NumArgs > 0) {
      BPTR old = CurrentDir(wbs->sm_ArgList[0].wa_Lock);
      d = GetDiskObject(wbs->sm_ArgList[0].wa_Name);
      CurrentDir(old);
    }
    if (d) {
      if ((s = tt(d, "FIT"))) set_fit(s);
      if ((s = tt(d, "BG"))) fvp_set(&prefs, "BG", s);
      if ((s = tt(d, "ZOOM"))) fvp_set(&prefs, "ZOOM", s);
      if ((s = tt(d, "CHECKER"))) fvp_set(&prefs, "CHECKER", s);
      if ((s = tt(d, "DITHER"))) fvp_set(&prefs, "DITHER", s);
      if ((s = tt(d, "PUBSCREEN"))) strncpy(opt.pubscreen, s, sizeof opt.pubscreen - 1);
      if ((s = tt(d, "FONTNAME"))) strncpy(opt.fontname, s, sizeof opt.fontname - 1);
      if ((s = tt(d, "FONTSIZE"))) StrToLong((STRPTR)s, &opt.fontsize);
      if ((s = tt(d, "PORTNAME"))) strncpy(opt.portname, s, sizeof opt.portname - 1);
      if (tt(d, "SCREEN")) opt.ownscreen = 1;
      if (tt(d, "BACKDROP")) opt.backdrop = 1;
      if (tt(d, "BENCH")) opt.bench = 1;
      if (tt(d, "PENS")) opt.pens = 1;
      FreeDiskObject(d);
    }
    for (i = 1; wbs && i < wbs->sm_NumArgs; i++) {
      if (!wbs->sm_ArgList[i].wa_Name || !wbs->sm_ArgList[i].wa_Name[0]) continue;   /* (a drawer) */
      full_path(wbs->sm_ArgList[i].wa_Lock, (const char *)wbs->sm_ArgList[i].wa_Name, full, sizeof full);
      add_file(full);
    }
  }
  if (nflist > 1) fscanned = 3;
  return 1;
}

/* ================================================================== main */
int main(int argc, char **argv)
{
  int rc = RETURN_FAIL;
  (void)argv; (void)version;
  fromwb = argc == 0;
  memset(&opt, 0, sizeof opt);
  fvp_default(&prefs);
  IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 40);
  GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 40);
  DataTypesBase = OpenLibrary((STRPTR)"datatypes.library", 40);
  UtilityBase = OpenLibrary((STRPTR)"utility.library", 39);
  GadToolsBase = OpenLibrary((STRPTR)"gadtools.library", 39);
  if (!IntuitionBase || !GfxBase || !DataTypesBase || !UtilityBase || !GadToolsBase) {
    tell("needs OS 3.1 or newer (intuition, graphics and datatypes.library 40)");
    goto out;
  }
  AslBase = OpenLibrary((STRPTR)"asl.library", 39);
  IconBase = OpenLibrary((STRPTR)"icon.library", 39);
  WorkbenchBase = OpenLibrary((STRPTR)"workbench.library", 39);
  CyberGfxBase = OpenLibrary((STRPTR)"cybergraphics.library", 40);
  if ((tport = CreateMsgPort()) && (treq = (struct timerequest *)CreateIORequest(tport, sizeof *treq)) &&
      !OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)treq, 0)) { TimerBase = treq->tr_node.io_Device; topen = 1; }
  if (!(lreply = CreateMsgPort())) goto out;
  if (!get_args(argc)) goto out;
  savedprefs = prefs;
  if (opt.fontname[0] && (DiskfontBase = OpenLibrary((STRPTR)"diskfont.library", 39))) {
    char fn[80];
    strncpy(fn, opt.fontname, sizeof fn - 6); fn[sizeof fn - 6] = 0;
    if (!strstr(fn, ".font")) strcat(fn, ".font");
    fta.ta_Name = (STRPTR)fn; fta.ta_YSize = (UWORD)(opt.fontsize ? opt.fontsize : 8); fta.ta_Style = 0; fta.ta_Flags = 0;
    if ((tfont = OpenDiskFont(&fta))) { fta.ta_Name = (STRPTR)tfont->tf_Message.mn_Node.ln_Name; fta.ta_YSize = tfont->tf_YSize; }
  }
  runner(1);
  if (!fvl_start()) say("FitView: no loader process; pictures load in the window's task\n");
  if (!open_screen()) { tell("can't open or find the screen"); goto out; }
  if (!open_window()) { tell("can't open the window (not enough memory?)"); goto out; }
  {
    /* the ARexx port (FITVIEW.1, or PORTNAME): not when one of that name is there already */
    char *pn = dupstr(opt.portname[0] ? opt.portname : "FITVIEW.1");
    Forbid();
    if (pn && !FindPort((STRPTR)pn) && (rexxport = CreateMsgPort())) { rexxport->mp_Node.ln_Name = pn; rexxport->mp_Node.ln_Pri = 0; AddPort(rexxport); }
    Permit();
    if (!rexxport && pn) FreeVec(pn);
  }
  if ((!nflist && !opt.clip) || opt.requester) {
    char name[600];
    if (asl_file(name, sizeof name, 0, "FitView: open")) { free_list(); add_file(name); }
    else if (!nflist) { rc = RETURN_OK; goto out; }
  }
  fcur = 0;
  load_current();
  event_loop();
  rc = RETURN_OK;
  if (prefs.mode != savedprefs.mode || prefs.zoom != savedprefs.zoom || prefs.dither != savedprefs.dither || prefs.checker != savedprefs.checker)
    save_prefs(0);                         /* (the last zoom mode is remembered) */
out:
  if (linflight) { WaitPort(lreply); GetMsg(lreply); fv_pic_free(&lrq.pic); linflight = 0; }
  fvl_stop();
  timer_stop();
  close_window();
  drop_picture();
  if (rexxport) {
    struct Message *m;
    char *n = rexxport->mp_Node.ln_Name;
    RemPort(rexxport);
    while ((m = GetMsg(rexxport))) { ((struct RexxMsg *)m)->rm_Result1 = 20; ReplyMsg(m); }
    DeleteMsgPort(rexxport);
    FreeVec(n);
  }
  free_list();
#ifdef FITVIEW_NC
  fvnc_close();
#endif
  if (tfont) CloseFont(tfont);
  if (lreply) DeleteMsgPort(lreply);
  if (topen) CloseDevice((struct IORequest *)treq);
  if (treq) DeleteIORequest((struct IORequest *)treq);
  if (tport) DeleteMsgPort(tport);
  if (DiskfontBase) CloseLibrary(DiskfontBase);
  if (CyberGfxBase) CloseLibrary(CyberGfxBase);
  if (WorkbenchBase) CloseLibrary(WorkbenchBase);
  if (IconBase) CloseLibrary(IconBase);
  if (AslBase) CloseLibrary(AslBase);
  if (GadToolsBase) CloseLibrary(GadToolsBase);
  if (UtilityBase) CloseLibrary(UtilityBase);
  if (DataTypesBase) CloseLibrary(DataTypesBase);
  if (GfxBase) CloseLibrary((struct Library *)GfxBase);
  if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
  return rc;
}
