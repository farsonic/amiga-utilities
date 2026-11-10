/*
 * fvload.c - FitView's pictures through datatypes.library (fvload.h), in a process of its own.
 *
 * The reading is WBFit's (src/wbfit/wbfit.c): picture.datatype 43+ is asked for the whole
 * picture with PDTM_READPIXELARRAY (here ARGB, so alpha comes too); if that gives nothing (OS
 * 3.0/3.1's picture.datatype 40, or a datatype that can't), the class's bitmap is read with
 * ReadPixelLine8() and its colour registers (PDTA_CRegs), HAM6/HAM8 and Extra Half-Brite by the
 * ModeID. When 32 bits a pixel don't fit in memory, a true-colour picture is read in bands
 * twice: a histogram, then Floyd-Steinberg to its median cut of 256 colours (src/lib/pxquant.c,
 * fvk_dither).
 *
 * Copyright (C) 2026 the WBFit author.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <graphics/gfx.h>
#include <graphics/modeid.h>
#include <graphics/view.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/pictureclass.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/datatypes.h>
#include <clib/alib_protos.h>
#include "fvload.h"
#include "fvk.h"
#include "pxquant.h"
#ifdef FITVIEW_NC
#include <nativecall/nativecall.h>
#endif

/* ================================================================== memory */
/* a block: [size][kind] then the memory; kind 1 nc_alloc, 2 AllocVec */
void *fv_alloc(ULONG n)
{
  ULONG *p;
  if (n > 0x7FFFFFF0UL) return 0;
#ifdef FITVIEW_NC
  if ((p = (ULONG *)nc_alloc(n + 8))) { p[0] = n + 8; p[1] = 1; return p + 2; }
#endif
  if (!(p = (ULONG *)AllocVec(n + 8, MEMF_ANY))) return 0;
  p[0] = n + 8; p[1] = 2;
  return p + 2;
}
void fv_free(void *q)
{
  ULONG *p;
  if (!q) return;
  p = (ULONG *)q - 2;
#ifdef FITVIEW_NC
  if (p[1] == 1) { nc_free(p, p[0]); return; }
#endif
  FreeVec(p);
}
void fv_pic_free(FvPic *p)
{
  fv_free(p->px); fv_free(p->pal);
  p->px = p->pal = 0; p->pxn = 0;
}

/* ================================================================== reading */
static void seterr(FvLoadReq *rq, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(rq->err, sizeof rq->err, fmt, ap);
  va_end(ap);
  rq->result = FVL_ERR;
}
static int enough(ULONG n) { return AvailMem(MEMF_ANY | MEMF_LARGEST) > n + (512UL << 10); }

/* V43: the whole picture as ARGB in one call; 1 if the datatype did it */
static int read_argb(Object *o, UBYTE *px, ULONG w, ULONG top, ULONG h)
{
  struct pdtBlitPixelArray m;
  ULONG k, n = w * h * 4;
  m.MethodID = PDTM_READPIXELARRAY;
  m.pbpa_PixelData = px;
  m.pbpa_PixelFormat = PBPAFMT_ARGB;
  m.pbpa_PixelArrayMod = w * 4;
  m.pbpa_Left = 0; m.pbpa_Top = top;
  m.pbpa_Width = w; m.pbpa_Height = h;
  memset(px, 0, n);
  if (DoMethodA(o, (Msg)&m)) return 1;
  for (k = 0; k < n; k++) if (px[k]) return 1;   /* (some return 0 when they did it) */
  return 0;
}

/* alpha: none when the datatype gave none (all 0 or all 255) or says there is none; else the
   colour premultiplied */
static void fix_alpha(FvPic *p, int mayalpha)
{
  ULONG i, n = p->w * p->h;
  UBYTE *q = p->px, amin = 255, amax = 0;
  for (i = 0; i < n; i++, q += 4) { if (q[0] < amin) amin = q[0]; if (q[0] > amax) amax = q[0]; }
  if (amax == 0 || amin == 255 || !mayalpha) {
    if (amin != 255) for (i = 0, q = p->px; i < n; i++, q += 4) q[0] = 255;
    p->alpha = 0;
    return;
  }
  p->alpha = 1;
  for (i = 0, q = p->px; i < n; i++, q += 4) {
    ULONG a = q[0], k;
    if (a == 255) continue;
    for (k = 1; k < 4; k++) { ULONG t = q[k] * a + 128; q[k] = (UBYTE)((t + (t >> 8)) >> 8); }
  }
}

/* the class's bitmap through its colour registers (any picture.datatype); HAM and EHB by ModeID */
static const char *read_bitmap(Object *o, FvPic *p)
{
  struct BitMap *bm = NULL, *tmpbm = NULL;
  ULONG *cregs = NULL, ncol = 0, modeid = (ULONG)INVALID_ID, depth, y, aw = (p->w + 15) & ~15UL, ham;
  struct RastPort rp, tmprp;
  UBYTE pal[768];
  int i, ehb;
  GetDTAttrs(o, PDTA_BitMap, (ULONG)&bm, PDTA_CRegs, (ULONG)&cregs, PDTA_NumColors, (ULONG)&ncol, PDTA_ModeID, (ULONG)&modeid, TAG_DONE);
  if (!bm) {
    struct gpLayout gl;                    /* (DoMethodA: DoDTMethod without a window crashes on AROS) */
    gl.MethodID = DTM_PROCLAYOUT; gl.gpl_GInfo = NULL; gl.gpl_Initial = 1;
    DoMethodA(o, (Msg)&gl);
    GetDTAttrs(o, PDTA_BitMap, (ULONG)&bm, PDTA_CRegs, (ULONG)&cregs, PDTA_NumColors, (ULONG)&ncol, TAG_DONE);
    if (!bm) GetDTAttrs(o, PDTA_DestBitMap, (ULONG)&bm, TAG_DONE);
  }
  if (!bm) return "the datatype gave no picture";
  depth = GetBitMapAttr(bm, BMA_DEPTH);
  if (depth < 1 || depth > 8) return "the picture is deeper than 8 bits: it needs picture.datatype 43 (OS 3.5+, or P96/CGX's)";
  if (ncol > 256) ncol = 256;
  memset(pal, 0, sizeof pal);
  for (i = 0; i < (int)ncol && cregs; i++) {
    pal[i * 3] = (UBYTE)(cregs[i * 3] >> 24); pal[i * 3 + 1] = (UBYTE)(cregs[i * 3 + 1] >> 24); pal[i * 3 + 2] = (UBYTE)(cregs[i * 3 + 2] >> 24);
  }
  ham = modeid != (ULONG)INVALID_ID && (modeid & HAM_KEY) ? (depth == 8 ? 6 : 4) : 0;
  ehb = modeid != (ULONG)INVALID_ID && (modeid & EXTRAHALFBRITE_KEY) && depth == 6;
  if (ehb && ncol <= 32) for (i = 32; i < 64; i++) { int k; for (k = 0; k < 3; k++) pal[i * 3 + k] = pal[(i - 32) * 3 + k] >> 1; }
  p->depth = depth;
  p->stride = aw;
  p->pxn = aw * p->h;
  if (p->h && p->pxn / p->h != aw) return "the picture is too big";
  if (!enough(p->pxn) || !(p->px = fv_alloc(p->pxn)) || !(p->pal = fv_alloc(1024)) || !(tmpbm = AllocBitMap(aw, 1, depth, 0, bm)))
    return "not enough memory for the picture";
  for (i = 0; i < 256; i++) { p->pal[i * 4] = 255; p->pal[i * 4 + 1] = pal[i * 3]; p->pal[i * 4 + 2] = pal[i * 3 + 1]; p->pal[i * 4 + 3] = pal[i * 3 + 2]; }
  InitRastPort(&rp);
  rp.BitMap = bm;
  tmprp = rp;
  tmprp.Layer = NULL;
  tmprp.BitMap = tmpbm;
  for (y = 0; y < p->h; y++) ReadPixelLine8(&rp, 0, y, p->w, p->px + y * aw, &tmprp);
  WaitBlit();
  FreeBitMap(tmpbm);
  p->via = 2;
  if (ham) {
    /* HAM can't stay indices: to ARGB (WBFit's wfk_lut) */
    ULONG n = p->w * p->h * 4, x;
    UBYTE *rgb;
    int mask = (1 << ham) - 1;
    if (!enough(n) || !(rgb = fv_alloc(n))) return "not enough memory for the HAM picture";
    for (y = 0; y < p->h; y++) {
      const UBYTE *s = p->px + y * aw;
      UBYTE *q = rgb + y * p->w * 4;
      int r = pal[0], g = pal[1], b = pal[2];
      for (x = 0; x < p->w; x++, q += 4) {
        int v = s[x], ctl = v >> ham, d = v & mask, dv = ham == 4 ? d * 17 : (d << 2) | (d >> 4);
        if (ctl == 0) { r = pal[d * 3]; g = pal[d * 3 + 1]; b = pal[d * 3 + 2]; }
        else if (ctl == 1) b = dv;
        else if (ctl == 2) r = dv;
        else g = dv;
        q[0] = 255; q[1] = (UBYTE)r; q[2] = (UBYTE)g; q[3] = (UBYTE)b;
      }
    }
    fv_free(p->px); fv_free(p->pal);
    p->px = rgb; p->pxn = n; p->pal = 0; p->stride = p->w * 4;
    p->depth = 24;
  }
  return NULL;
}

/* a true-colour picture that does not fit at 32 bits: 256 colours, read in bands twice.
   0 done, 1 not true colour (try the bitmap), or an error */
#define BAND 32
static const char *read_cut(Object *o, FvPic *p, int *nottc)
{
  ULONG y, n, i, w = p->w, bn = w * 4 * BAND, ne = (w + 2) * 12;
  wfk_u32 *hist = 0;
  UBYTE *band = 0, pal3[768], pm[256];
  UWORD *cache = 0;
  LONG *e0 = 0, *e1 = 0;
  const char *err = "not enough memory for the picture";
  int ncol;
  *nottc = 0;
  p->pxn = w * p->h;
  p->stride = w;
  if (!enough(p->pxn) || !(p->px = fv_alloc(p->pxn)) || !(p->pal = fv_alloc(1024)) || !(band = fv_alloc(bn)) ||
      !(hist = fv_alloc(32768 * 4)) || !(cache = fv_alloc(65536)) || !(e0 = fv_alloc(ne)) || !(e1 = fv_alloc(ne))) goto out;
  memset(hist, 0, 32768 * 4);
  for (y = 0; y < p->h; y += BAND) {
    n = p->h - y < BAND ? p->h - y : BAND;
    if (!read_argb(o, band, w, y, n) && y == 0) { *nottc = 1; err = 0; goto out; }
    for (i = 0; i < w * n; i++) {
      const UBYTE *q = band + i * 4;
      ULONG k = ((ULONG)(q[1] >> 3) << 10) | ((ULONG)(q[2] >> 3) << 5) | (q[3] >> 3);
      if (hist[k] != 0xFFFFFFFFUL) hist[k]++;
    }
  }
  ncol = wf_mediancut(hist, 256, pal3);
  for (i = 0; i < 256; i++) {
    p->pal[i * 4] = 255;
    p->pal[i * 4 + 1] = i < (ULONG)ncol ? pal3[i * 3] : 0; p->pal[i * 4 + 2] = i < (ULONG)ncol ? pal3[i * 3 + 1] : 0;
    p->pal[i * 4 + 3] = i < (ULONG)ncol ? pal3[i * 3 + 2] : 0;
    pm[i] = (UBYTE)i;
  }
  memset(cache, 0xFF, 65536); memset(e0, 0, ne); memset(e1, 0, ne);
  for (y = 0; y < p->h; y += BAND) {
    struct FvkDither d;
    n = p->h - y < BAND ? p->h - y : BAND;
    read_argb(o, band, w, y, n);
    memset(&d, 0, sizeof d);
    /* (BAND is even: each band's row 0 has the parity of its row in the picture) */
    d.range.from = 0; d.range.to = n;
    d.px = WFK_SETP(band); d.pstride = w * 4; d.w = w; d.out = WFK_SETP(p->px + y * w); d.ostride = w;
    d.pal = WFK_SETP(pal3); d.npal = (wfk_u32)ncol; d.cache = WFK_SETP(cache); d.penmap = WFK_SETP(pm); d.fs = 1;
    d.e0 = WFK_SETP(e0); d.e1 = WFK_SETP(e1);
    fvk_dither(&d);                        /* (the 68k code: not the main task's native calls) */
  }
  p->via = 3;
  p->depth = 24;
  err = 0;
out:
  fv_free(e1); fv_free(e0); fv_free(cache); fv_free(hist); fv_free(band);
  if (err || *nottc) fv_pic_free(p);
  return err;
}

static void dterror(FvLoadReq *rq, LONG e)
{
  STRPTR s = e >= DTERROR_UNKNOWN_DATATYPE ? GetDTString((ULONG)e) : NULL;
  const char *name = rq->srctype == DTST_FILE ? (const char *)FilePart((STRPTR)rq->path) : "the clipboard";
  if (s) {
    /* (the system's strings may hold a %s for the name) */
    char b[240];
    const char *q = strstr((const char *)s, "%s");
    if (q && strlen((const char *)s) + strlen(name) < sizeof b) {
      memcpy(b, s, q - (const char *)s); b[q - (const char *)s] = 0;
      strcat(b, name); strcat(b, q + 2);
      seterr(rq, "%s", b);
      return;
    }
    seterr(rq, "%s: %s", name, s);
    return;
  }
  if (e == ERROR_OBJECT_NOT_FOUND) seterr(rq, "%s: no such file", name);
  else if (e == ERROR_NO_FREE_STORE) seterr(rq, "%s: not enough memory", name);
  else if (e) {
    char f[100];
    Fault(e, (STRPTR)"", (STRPTR)f, sizeof f);
    seterr(rq, "%s: could not be read%s", name, f);
  } else seterr(rq, "%s: no datatype can read it (is the datatype for this format installed?)", name);
}

void fvl_do(FvLoadReq *rq)
{
  Object *o;
  struct BitMapHeader *bmh = NULL;
  struct DataType *dtn = NULL;
  ULONG pages = 0, page = 0, alphach = 0;
  const char *err = NULL;
  FvPic *p = &rq->pic;
  memset(p, 0, sizeof *p);
  rq->result = FVL_OK; rq->err[0] = 0;
  o = NewDTObject(rq->srctype == DTST_FILE ? (APTR)rq->path : (APTR)rq->unit,
                  DTA_SourceType, rq->srctype, DTA_GroupID, GID_PICTURE, PDTA_Remap, FALSE,
                  rq->v40 ? TAG_IGNORE : PDTA_DestMode, PMODE_V43,
                  rq->page ? PDFDTA_Page : TAG_IGNORE, rq->page,
                  rq->boxw ? PDFDTA_FitWidth : TAG_IGNORE, rq->boxw,
                  rq->boxh ? PDFDTA_FitHeight : TAG_IGNORE, rq->boxh, TAG_DONE);
  if (!o) {
    LONG e = IoErr();
    if (e == ERROR_OBJECT_WRONG_TYPE) { rq->result = FVL_NOTPIC; return; }
    dterror(rq, e);
    return;
  }
  GetDTAttrs(o, PDTA_BitMapHeader, (ULONG)&bmh, DTA_DataType, (ULONG)&dtn, TAG_DONE);
  GetDTAttrs(o, PDFDTA_PageCount, (ULONG)&pages, TAG_DONE);
  GetDTAttrs(o, PDFDTA_Page, (ULONG)&page, TAG_DONE);
  GetDTAttrs(o, PDTA_AlphaChannel, (ULONG)&alphach, TAG_DONE);
  if (dtn && dtn->dtn_Header && dtn->dtn_Header->dth_Name) {
    strncpy(p->dtname, (const char *)dtn->dtn_Header->dth_Name, sizeof p->dtname - 1);
    p->dtname[sizeof p->dtname - 1] = 0;
  }
  p->pages = pages; p->page = pages ? (page ? page : rq->page ? rq->page : 1) : 0;
  p->boxw = rq->boxw; p->boxh = rq->boxh;
  if (!bmh || bmh->bmh_Width < 1 || bmh->bmh_Height < 1) err = "the datatype gave no picture size";
  else if ((ULONG)bmh->bmh_Width * bmh->bmh_Height > FV_MAXPIXELS) err = "the picture is too big (over 40 million pixels)";
  else {
    p->w = bmh->bmh_Width; p->h = bmh->bmh_Height;
    p->xasp = bmh->bmh_XAspect; p->yasp = bmh->bmh_YAspect;
    p->depth = bmh->bmh_Depth;
    if (!rq->v40) {
      ULONG n = p->w * p->h * 4;
      if (enough(n) && (p->px = fv_alloc(n))) {
        if (read_argb(o, p->px, p->w, 0, p->h)) {
          p->pxn = n; p->stride = p->w * 4; p->via = 1;
          fix_alpha(p, alphach || bmh->bmh_Masking == mskHasAlpha || bmh->bmh_Masking == mskHasTransparentColor);
        } else {
          /* not true colour: the bitmap; if that fails too, the (all black) pixel array */
          UBYTE *keep = p->px;
          p->px = 0;
          if ((err = read_bitmap(o, p)) && !p->px) {
            err = NULL; p->px = keep; p->pxn = n; p->stride = p->w * 4; p->via = 1;
            fix_alpha(p, 0);
          } else fv_free(keep);
        }
      } else {
        int nottc;
        if ((err = read_cut(o, p, &nottc)) == NULL && nottc) err = read_bitmap(o, p);
      }
    } else err = read_bitmap(o, p);
  }
  DisposeDTObject(o);
  if (err) {
    fv_pic_free(p);
    seterr(rq, "%s: %s", rq->srctype == DTST_FILE ? (const char *)FilePart((STRPTR)rq->path) : "the clipboard", err);
  }
}

/* ================================================================== the process */
static struct MsgPort *lport;
static struct Task *parent;
static BYTE startsig = -1;
static int running;

static void loader_main(void)
{
  struct MsgPort *port = CreateMsgPort();
  lport = port;
  Signal(parent, 1UL << startsig);
  if (!port) return;
  for (;;) {
    FvLoadReq *rq;
    WaitPort(port);
    while ((rq = (FvLoadReq *)GetMsg(port))) {
      if (rq->quit) {
        Forbid();                          /* (gone before the program can be unloaded) */
        lport = NULL;
        DeleteMsgPort(port);
        rq->result = FVL_QUIT;
        ReplyMsg(&rq->msg);
        return;
      }
      fvl_do(rq);
      ReplyMsg(&rq->msg);
    }
  }
}

int fvl_start(void)
{
  struct Process *me = (struct Process *)FindTask(NULL), *pr;
  BPTR dir;
  if (running) return 1;
  parent = &me->pr_Task;
  if ((startsig = AllocSignal(-1)) < 0) return 0;
  SetSignal(0, 1UL << startsig);
  dir = DupLock(me->pr_CurrentDir);
  pr = CreateNewProcTags(NP_Entry, (ULONG)loader_main, NP_Name, (ULONG)"FitView loader", NP_StackSize, 98304,
                         NP_Priority, -1, NP_CurrentDir, (ULONG)dir, NP_WindowPtr, (ULONG)me->pr_WindowPtr, TAG_DONE);
  if (!pr) { UnLock(dir); FreeSignal(startsig); startsig = -1; return 0; }
  Wait(1UL << startsig);
  FreeSignal(startsig); startsig = -1;
  if (!lport) return 0;
  running = 1;
  return 1;
}
struct MsgPort *fvl_port(void) { return running ? lport : NULL; }
void fvl_stop(void)
{
  FvLoadReq q;
  struct MsgPort *rp;
  if (!running) return;
  if (!(rp = CreateMsgPort())) return;     /* (then it stays; it holds no memory of ours) */
  memset(&q, 0, sizeof q);
  q.msg.mn_ReplyPort = rp; q.msg.mn_Length = sizeof q; q.quit = 1;
  PutMsg(lport, &q.msg);
  WaitPort(rp);
  GetMsg(rp);
  DeleteMsgPort(rp);
  running = 0;
}
