/*
 * WBFit - the Workbench backdrop from any picture, scaled to the Workbench screen's exact size
 * and pixel shape, so Workbench never tiles or crops it.
 *
 *   WBFit FILE,MODE/K,BG/K,OUT/K,FORMAT/K,PALETTE/K,DITHER/S,RESTORE/S,WATCH/S,QUIET/S,TIMES/S,BENCH/S,V40/S
 *
 * The picture is read through datatypes.library (PDTM_READPIXELARRAY with picture.datatype 43+,
 * else the class's bitmap and colour registers), fitted (wfcore.c), written as PNG, 24-bit ILBM
 * or palette ILBM, and named as the WBP_ROOT picture in ENVARC: and ENV: Sys/WBPattern.prefs;
 * IPrefs sees ENV: change and gives Workbench the new backdrop. OS 3.1 (V40) calls only in the
 * base path; the V43 picture.datatype is used when it is there.
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
#include <dos/notify.h>
#include <dos/rdargs.h>
#include <devices/timer.h>
#include <graphics/gfx.h>
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>
#include <graphics/view.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <intuition/gadgetclass.h>
#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/pictureclass.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <libraries/commodities.h>
#include <prefs/prefhdr.h>
#include <prefs/wbpattern.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/datatypes.h>
#include <proto/icon.h>
#include <proto/commodities.h>
#include <proto/timer.h>
#include <proto/utility.h>
#include <clib/alib_protos.h>
#include "wfcore.h"
#include "wfprefs.h"
#ifdef WBFIT_NC
#include "wfnc.h"
#endif

#define VERSTR "1.0"
static const char version[] = "$VER: WBFit " VERSTR " (10.11.2026)"
#ifdef WBFIT_NC
  " NativeCall test build"
#endif
  ;
size_t __stack = 65536;
extern struct WBStartup *_WBenchMsg;

/* the layouts wfprefs.c writes by hand, checked against the NDK's structures */
typedef char check_wbp[sizeof(struct WBPatternPrefs) == WFP_HDR ? 1 : -1];
typedef char check_wbp2[offsetof(struct WBPatternPrefs, wbp_DataLength) == 22 ? 1 : -1];
typedef char check_prhd[sizeof(struct PrefHeader) == 6 ? 1 : -1];

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *DataTypesBase, *IconBase, *CxBase, *UtilityBase;
struct Device *TimerBase;
static struct timerequest treq;
static int timer_open;

#define PREFS_ENV    "ENV:Sys/WBPattern.prefs"
#define PREFS_ENVARC "ENVARC:Sys/WBPattern.prefs"
#define PREFS_BACKUP "ENVARC:Sys/WBPattern.prefs.wbfit-backup"
#define DEFDIR       "SYS:Prefs/Presets/WBFit"

enum { FMT_AUTO = -1 };
enum { PAL_AUTO, PAL_WB, PAL_PICTURE };
typedef struct {
  char file[512], out[512];
  int mode, format, palette, dither, restore, watch, quiet, times, bench, bgset, v40;
  ULONG bg;
} Opt;

/* ================================================================== messages */
static int fromwb, quiet, failed;
static char msg[3000];
static ULONG msglen;
static void say(const char *fmt, ...)
{
  char b[600];
  va_list ap;
  ULONG n;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  n = strlen(b);
  if (!fromwb) { PutStr((STRPTR)b); return; }
  if (msglen + n < sizeof msg) { memcpy(msg + msglen, b, n + 1); msglen += n; }
}
#define INFO(...) do { if (!quiet) say(__VA_ARGS__); } while (0)
static int fail(const char *fmt, ...)
{
  char b[600];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  say("WBFit: %s\n", b);
  failed = 1;
  return 0;
}
static void show_wb(void)
{
  struct EasyStruct es;
  if (!fromwb || !msglen || !IntuitionBase) return;
  if (quiet && !failed) return;
  es.es_StructSize = sizeof es; es.es_Flags = 0;
  es.es_Title = (UBYTE *)"WBFit"; es.es_TextFormat = (UBYTE *)"%s"; es.es_GadgetFormat = (UBYTE *)"OK";
  if (msglen && msg[msglen - 1] == '\n') msg[--msglen] = 0;
  EasyRequest(NULL, &es, NULL, (ULONG)msg);
  msglen = 0; msg[0] = 0;
}

/* ================================================================== memory, clock, runner */
static void *am_alloc(WfRun *r, wf_u32 n) { (void)r; return AllocVec(n, MEMF_ANY | MEMF_CLEAR); }
static void am_free(WfRun *r, void *p, wf_u32 n) { (void)r; (void)n; FreeVec(p); }
static wf_u32 am_now(WfRun *r)
{
  struct timeval tv;
  (void)r;
  if (!timer_open) return 0;
  GetSysTime(&tv);
  return (wf_u32)(tv.tv_secs * 1000000UL + tv.tv_micro);
}
static WfRun run;
static void runner(int native)
{
#ifdef WBFIT_NC
  wfnc_runner(&run, native);
#else
  (void)native;
  wf_run_plain(&run, am_alloc, am_free, am_now);
#endif
}
static wf_u32 now(void) { return run.now_us ? run.now_us(&run) : am_now(0); }
static void ms(char *b, wf_u32 us) { sprintf(b, "%lu.%lu", (unsigned long)(us / 1000), (unsigned long)(us / 100 % 10)); }

/* ================================================================== files */
static int exists(const char *p)
{
  BPTR l = Lock((STRPTR)p, ACCESS_READ);
  if (l) UnLock(l);
  return l != 0;
}
static int isdir(const char *p)
{
  BPTR l = Lock((STRPTR)p, ACCESS_READ);
  struct FileInfoBlock *fib;
  int d = 0;
  if (!l) return 0;
  if ((fib = AllocDosObject(DOS_FIB, NULL))) {
    if (Examine(l, fib)) d = fib->fib_DirEntryType > 0;
    FreeDosObject(DOS_FIB, fib);
  }
  UnLock(l);
  return d;
}
/* a small file read whole; -1 if missing or bigger than cap */
static long read_file(const char *p, UBYTE *buf, ULONG cap)
{
  BPTR f = Open((STRPTR)p, MODE_OLDFILE);
  long n;
  if (!f) return -1;
  n = Read(f, buf, cap);
  if (n >= 0 && (ULONG)n == cap) { UBYTE c; if (Read(f, &c, 1) == 1) n = -1; }
  Close(f);
  return n;
}
/* a directory and its parents made where missing */
static int make_dir(const char *p)
{
  char b[512];
  ULONG i, n = strlen(p);
  if (n >= sizeof b) return 0;
  strcpy(b, p);
  if (n && b[n - 1] == '/') b[--n] = 0;
  for (i = 0; i <= n; i++) {
    if (b[i] == '/' || i == n) {
      char c = b[i];
      BPTR l;
      if (i && b[i - 1] != ':') {
        b[i] = 0;
        if (!exists(b) && (l = CreateDir((STRPTR)b))) UnLock(l);
        b[i] = c;
      }
    }
  }
  return isdir(b);
}

/* writing through a buffer, to name.tmp; done: renamed over name */
typedef struct { BPTR f; UBYTE buf[16384]; ULONG n; int err; } WFile;
static WFile wf;
static int wflush(WFile *w)
{
  if (w->n && !w->err && Write(w->f, w->buf, w->n) != (LONG)w->n) w->err = 1;
  w->n = 0;
  return !w->err;
}
static int wput(void *ctx, const void *p, wf_u32 n)
{
  WFile *w = (WFile *)ctx;
  if (w->err) return WF_EWRITE;
  if (n >= sizeof w->buf) {
    if (!wflush(w) || Write(w->f, (APTR)p, n) != (LONG)n) { w->err = 1; return WF_EWRITE; }
    return 0;
  }
  if (w->n + n > sizeof w->buf && !wflush(w)) return WF_EWRITE;
  memcpy(w->buf + w->n, p, n);
  w->n += n;
  return 0;
}
static int open_tmp(const char *name, char *tmp)
{
  sprintf(tmp, "%s.wbfit-tmp", name);
  memset(&wf, 0, sizeof wf);
  wf.f = Open((STRPTR)tmp, MODE_NEWFILE);
  return wf.f != 0;
}
/* close the temporary file; 1 if it is now name */
static int commit_tmp(const char *name, const char *tmp, int ok)
{
  ok = wflush(&wf) && ok;
  if (!Close(wf.f)) ok = 0;
  wf.f = 0;
  if (ok && exists(name) && !DeleteFile((STRPTR)name)) ok = 0;
  if (ok && !Rename((STRPTR)tmp, (STRPTR)name)) ok = 0;
  if (!ok) DeleteFile((STRPTR)tmp);
  return ok;
}
static int write_whole(const char *name, const UBYTE *p, ULONG n, int viatmp)
{
  char tmp[600];
  BPTR f;
  int ok;
  if (viatmp) {
    if (strlen(name) > 500 || !open_tmp(name, tmp)) return 0;
    ok = wput(&wf, p, n) == 0;
    return commit_tmp(name, tmp, ok);
  }
  if (!(f = Open((STRPTR)name, MODE_NEWFILE))) return 0;
  ok = Write(f, (APTR)p, n) == (LONG)n;
  if (!Close(f)) ok = 0;
  return ok;
}

/* ================================================================== the screen */
typedef struct {
  ULONG W, H, depth, modeid, rx, ry;
  int npal;
  UBYTE pal[768];
} Scr;
static ULONG rgb32[256 * 3 + 2];
static int get_screen(Scr *s)
{
  struct Screen *scr;
  struct DisplayInfo di;
  memset(s, 0, sizeof *s);
  if (!(scr = LockPubScreen((STRPTR)"Workbench"))) return 0;
  s->W = (ULONG)scr->Width; s->H = (ULONG)scr->Height;
  s->depth = GetBitMapAttr(scr->RastPort.BitMap, BMA_DEPTH);
  s->modeid = GetVPModeID(&scr->ViewPort);
  s->rx = s->ry = 1;
  if (s->modeid != (ULONG)INVALID_ID &&
      GetDisplayInfoData(NULL, (UBYTE *)&di, sizeof di, DTAG_DISP, s->modeid) >= offsetof(struct DisplayInfo, Resolution) + sizeof di.Resolution &&
      di.Resolution.x > 0 && di.Resolution.y > 0) {
    s->rx = (ULONG)di.Resolution.x; s->ry = (ULONG)di.Resolution.y;
    /* within 5% of square (RTG modes often say 22:23 and the like): square. The Amiga's own
       modes are far off it (PAL hires 22:44, NTSC hires 22:52, superhires 11:44) */
    if (s->rx * 20 >= s->ry * 19 && s->ry * 20 >= s->rx * 19) s->rx = s->ry = 1;
  }
  if (s->depth >= 1 && s->depth <= 8) {
    ULONG i, n = 1UL << s->depth;
    GetRGB32(scr->ViewPort.ColorMap, 0, n, rgb32);
    for (i = 0; i < n * 3; i++) s->pal[i] = (UBYTE)(rgb32[i] >> 24);
    s->npal = (int)n;
  }
  UnlockPubScreen(NULL, scr);
  return 1;
}
static int same_screen(const Scr *a, const Scr *b)
{
  return a->W == b->W && a->H == b->H && a->depth == b->depth && a->rx == b->rx && a->ry == b->ry;
}

/* ================================================================== the picture */
typedef struct {
  UBYTE *rgb, *idx;
  ULONG rgbn, idxn, w, h, stride, ham;
  UBYTE pal[768];
  int via;                                 /* 1 V43 pixel array, 2 bitmap */
} Pic;
static int pdt_version(void)
{
  struct Library *l = OpenLibrary((STRPTR)"datatypes/picture.datatype", 0);
  int v = 0;
  if (l) { v = l->lib_Version; CloseLibrary(l); }
  return v;
}
static void pic_free(Pic *p)
{
  if (p->rgb) run.free(&run, p->rgb, p->rgbn);
  if (p->idx) run.free(&run, p->idx, p->idxn);
  p->rgb = p->idx = 0;
}
/* V43: the whole picture as RGB in one call; 1 if the datatype did it */
static int read_v43(Object *o, Pic *p)
{
  struct pdtBlitPixelArray m;
  ULONG r, k;
  m.MethodID = PDTM_READPIXELARRAY;
  m.pbpa_PixelData = p->rgb;
  m.pbpa_PixelFormat = PBPAFMT_RGB;
  m.pbpa_PixelArrayMod = p->w * 3;
  m.pbpa_Left = 0; m.pbpa_Top = 0;
  m.pbpa_Width = p->w; m.pbpa_Height = p->h;
  r = DoMethodA(o, (Msg)&m);
  if (r) return 1;
  for (k = 0; k < p->rgbn; k++) if (p->rgb[k]) return 1;   /* (some return 0 when they did it) */
  return 0;
}
/* the class's bitmap through its colour registers (any picture.datatype); HAM and EHB by ModeID */
static const char *read_bitmap(Object *o, Pic *p)
{
  struct BitMap *bm = NULL, *tmpbm = NULL;
  ULONG *cregs = NULL, ncol = 0, modeid = (ULONG)INVALID_ID, depth, y, aw = (p->w + 15) & ~15UL;
  struct RastPort rp, tmprp;
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
  memset(p->pal, 0, sizeof p->pal);
  for (i = 0; i < (int)ncol && cregs; i++) {
    p->pal[i * 3] = (UBYTE)(cregs[i * 3] >> 24); p->pal[i * 3 + 1] = (UBYTE)(cregs[i * 3 + 1] >> 24); p->pal[i * 3 + 2] = (UBYTE)(cregs[i * 3 + 2] >> 24);
  }
  p->ham = modeid != (ULONG)INVALID_ID && (modeid & HAM_KEY) ? (depth == 8 ? 6 : 4) : 0;
  ehb = modeid != (ULONG)INVALID_ID && (modeid & EXTRAHALFBRITE_KEY) && depth == 6;
  if (ehb && ncol <= 32) for (i = 32; i < 64; i++) { int k; for (k = 0; k < 3; k++) p->pal[i * 3 + k] = p->pal[(i - 32) * 3 + k] >> 1; }
  p->stride = aw;
  p->idxn = aw * p->h;
  if (p->h && p->idxn / p->h != aw) return "the picture is too big";
  if (!(p->idx = run.alloc(&run, p->idxn)) || !(tmpbm = AllocBitMap(aw, 1, depth, 0, bm))) return "not enough memory for the picture";
  InitRastPort(&rp);
  rp.BitMap = bm;
  tmprp = rp;
  tmprp.Layer = NULL;
  tmprp.BitMap = tmpbm;
  for (y = 0; y < p->h; y++) ReadPixelLine8(&rp, 0, y, p->w, p->idx + y * aw, &tmprp);
  WaitBlit();
  FreeBitMap(tmpbm);
  p->via = 2;
  return NULL;
}
static int load_pic(const char *path, Pic *p, int v43)
{
  Object *o;
  struct BitMapHeader *bmh = NULL;
  const char *err = NULL;
  memset(p, 0, sizeof *p);
  o = NewDTObject((APTR)path, DTA_SourceType, DTST_FILE, DTA_GroupID, GID_PICTURE, PDTA_Remap, FALSE,
                  v43 ? PDTA_DestMode : TAG_IGNORE, PMODE_V43, TAG_DONE);
  if (!o) {
    LONG e = IoErr();
    STRPTR s = e >= DTERROR_UNKNOWN_DATATYPE ? GetDTString((ULONG)e) : NULL;
    if (s) {                               /* (the system's strings may hold a %s for the name) */
      char b[300];
      const char *q = strstr((const char *)s, "%s");
      if (q && strlen((const char *)s) + strlen(path) < sizeof b) {
        memcpy(b, s, q - (const char *)s); b[q - (const char *)s] = 0;
        strcat(b, path); strcat(b, q + 2);
        return fail("%s", b);
      }
      return fail("%s: %s", path, s);
    }
    if (e == ERROR_OBJECT_NOT_FOUND) return fail("%s: no such file", path);
    return fail("%s: no datatype can read it (is the datatype for this format installed?)", path);
  }
  GetDTAttrs(o, PDTA_BitMapHeader, (ULONG)&bmh, TAG_DONE);
  if (!bmh || bmh->bmh_Width < 1 || bmh->bmh_Height < 1) err = "the datatype gave no picture size";
  else if (bmh->bmh_Width > WF_MAXDIM || bmh->bmh_Height > WF_MAXDIM) err = "the picture is too big";
  else {
    p->w = bmh->bmh_Width; p->h = bmh->bmh_Height;
    if (v43) {
      p->rgbn = p->w * p->h * 3;
      if (p->rgbn / 3 / p->h != p->w || !(p->rgb = run.alloc(&run, p->rgbn))) err = "not enough memory for the picture";
      else if (read_v43(o, p)) { p->via = 1; p->stride = p->w * 3; }
      else {
        if ((err = read_bitmap(o, p)) && !p->idx) { err = NULL; p->via = 1; p->stride = p->w * 3; }  /* (all black) */
        if (p->via == 2) { run.free(&run, p->rgb, p->rgbn); p->rgb = 0; }
      }
    } else err = read_bitmap(o, p);
  }
  DisposeDTObject(o);
  if (err) { pic_free(p); return fail("%s: %s", path, err); }
  return 1;
}

/* ================================================================== arguments */
static int parse_mode(const char *s, int *m)
{
  static const char *const names[] = { "FILL", "FIT", "STRETCH", "CENTER", "CENTRE" };
  int i;
  for (i = 0; i < 5; i++) if (!Stricmp((STRPTR)s, (STRPTR)names[i])) { *m = i == 4 ? WF_CENTER : i; return 1; }
  return 0;
}
static int parse_format(const char *s, int *f)
{
  if (!Stricmp((STRPTR)s, (STRPTR)"AUTO")) *f = FMT_AUTO;
  else if (!Stricmp((STRPTR)s, (STRPTR)"PNG")) *f = WF_PNG;
  else if (!Stricmp((STRPTR)s, (STRPTR)"ILBM24")) *f = WF_ILBM24;
  else if (!Stricmp((STRPTR)s, (STRPTR)"ILBM")) *f = WF_ILBM;
  else return 0;
  return 1;
}
static int parse_palette(const char *s, int *p)
{
  if (!Stricmp((STRPTR)s, (STRPTR)"AUTO")) *p = PAL_AUTO;
  else if (!Stricmp((STRPTR)s, (STRPTR)"WB") || !Stricmp((STRPTR)s, (STRPTR)"WORKBENCH")) *p = PAL_WB;
  else if (!Stricmp((STRPTR)s, (STRPTR)"PICTURE")) *p = PAL_PICTURE;
  else return 0;
  return 1;
}
static int parse_bg(const char *s, ULONG *v)
{
  ULONG x = 0;
  int n = 0;
  if (*s == '#') s++;
  else if (s[0] == '$') s++;
  else if (s[0] == '0' && (s[1] | 32) == 'x') s += 2;
  for (; *s; s++, n++) {
    int c = *s, d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
    if (d < 0 || n >= 6) return 0;
    x = x << 4 | (ULONG)d;
  }
  if (n != 6) return 0;
  *v = x;
  return 1;
}
static int set_opts(Opt *o, const char *mode, const char *bg, const char *fmt, const char *pal)
{
  if (mode && !parse_mode(mode, &o->mode)) return fail("MODE must be FILL, FIT, STRETCH or CENTER");
  if (bg) {
    if (!parse_bg(bg, &o->bg)) return fail("BG must be RRGGBB (hexadecimal)");
    o->bgset = 1;
  }
  if (fmt && !parse_format(fmt, &o->format)) return fail("FORMAT must be AUTO, PNG, ILBM or ILBM24");
  if (pal && !parse_palette(pal, &o->palette)) return fail("PALETTE must be AUTO, WB or PICTURE");
  return 1;
}
static int full_name(BPTR dir, const char *name, char *out, ULONG cap)
{
  if (!NameFromLock(dir, (STRPTR)out, (LONG)cap)) return 0;
  return AddPart((STRPTR)out, (STRPTR)name, cap) != 0;
}
static const char *tt(struct DiskObject *d, const char *n) { return d ? (const char *)FindToolType((STRPTR *)d->do_ToolTypes, (STRPTR)n) : NULL; }
static int tt_yes(struct DiskObject *d, const char *n)
{
  const char *v = tt(d, n);
  return v && Stricmp((STRPTR)v, (STRPTR)"NO") && Stricmp((STRPTR)v, (STRPTR)"OFF") && Stricmp((STRPTR)v, (STRPTR)"FALSE");
}
static int get_opts(Opt *o)
{
  memset(o, 0, sizeof *o);
  o->mode = WF_FILL; o->format = FMT_AUTO; o->palette = PAL_AUTO;
  if (!fromwb) {
    enum { A_FILE, A_MODE, A_BG, A_OUT, A_FORMAT, A_PALETTE, A_DITHER, A_RESTORE, A_WATCH, A_QUIET, A_TIMES, A_BENCH, A_V40, A_N };
    LONG a[A_N];
    struct RDArgs *rd;
    int ok = 1;
    memset(a, 0, sizeof a);
    if (!(rd = ReadArgs((STRPTR)"FILE,MODE/K,BG/K,OUT/K,FORMAT/K,PALETTE/K,DITHER/S,RESTORE/S,WATCH/S,QUIET/S,TIMES/S,BENCH/S,V40/S", a, NULL))) {
      PrintFault(IoErr(), (STRPTR)"WBFit");
      failed = 1;
      return 0;
    }
    if (a[A_FILE]) strncpy(o->file, (char *)a[A_FILE], sizeof o->file - 1);
    if (a[A_OUT]) strncpy(o->out, (char *)a[A_OUT], sizeof o->out - 1);
    ok = set_opts(o, (char *)a[A_MODE], (char *)a[A_BG], (char *)a[A_FORMAT], (char *)a[A_PALETTE]);
    o->dither = a[A_DITHER] != 0; o->restore = a[A_RESTORE] != 0; o->watch = a[A_WATCH] != 0;
    o->quiet = a[A_QUIET] != 0; o->times = a[A_TIMES] != 0; o->bench = a[A_BENCH] != 0; o->v40 = a[A_V40] != 0;
    FreeArgs(rd);
    return ok;
  }
  {
    struct WBStartup *wb = _WBenchMsg;
    struct DiskObject *d = NULL;
    BPTR old;
    int ok;
    if (IconBase && wb->sm_NumArgs > 0) {
      old = CurrentDir(wb->sm_ArgList[0].wa_Lock);
      d = GetDiskObject((STRPTR)wb->sm_ArgList[0].wa_Name);
      CurrentDir(old);
    }
    if (wb->sm_NumArgs > 1) {           /* a picture's icon dropped on ours (or ours a project's tool) */
      if (!full_name(wb->sm_ArgList[1].wa_Lock, (const char *)wb->sm_ArgList[1].wa_Name, o->file, sizeof o->file)) o->file[0] = 0;
    } else if (tt(d, "FILE")) strncpy(o->file, tt(d, "FILE"), sizeof o->file - 1);
    if (tt(d, "OUT")) strncpy(o->out, tt(d, "OUT"), sizeof o->out - 1);
    ok = set_opts(o, tt(d, "MODE"), tt(d, "BG"), tt(d, "FORMAT"), tt(d, "PALETTE"));
    o->dither = tt_yes(d, "DITHER"); o->restore = tt_yes(d, "RESTORE"); o->watch = tt_yes(d, "WATCH");
    o->quiet = tt_yes(d, "QUIET"); o->times = tt_yes(d, "TIMES"); o->bench = tt_yes(d, "BENCH"); o->v40 = tt_yes(d, "V40");
    if (d) FreeDiskObject(d);
    return ok;
  }
}

/* ================================================================== the prefs */
static UBYTE oldp[65536], newp[65536 + 600];
static int set_prefs(const char *picname, int *backedup)
{
  long n = read_file(PREFS_ENV, oldp, sizeof oldp), bn;
  ULONG len;
  int oldok;
  *backedup = 0;
  if (n < 0) n = read_file(PREFS_ENVARC, oldp, sizeof oldp);
  len = wfp_build(n > 0 ? oldp : NULL, n > 0 ? (ULONG)n : 0, picname, newp, sizeof newp, &oldok);
  if (!len) return fail("the picture's name is too long for the prefs");
  if (n > 0 && !oldok) INFO("  (the old WBPattern.prefs was not valid: written new)\n");
  make_dir("ENVARC:Sys");
  make_dir("ENV:Sys");
  /* the backup, once: the ENVARC: prefs (else ENV:'s), or an empty file for "there were none" */
  if (!exists(PREFS_BACKUP)) {
    bn = read_file(PREFS_ENVARC, oldp, sizeof oldp);
    if (bn < 0) bn = read_file(PREFS_ENV, oldp, sizeof oldp);
    if (!write_whole(PREFS_BACKUP, oldp, bn > 0 ? (ULONG)bn : 0, 1)) return fail("cannot write %s", PREFS_BACKUP);
    *backedup = 1;
  }
  if (!write_whole(PREFS_ENVARC, newp, len, 1)) return fail("cannot write %s", PREFS_ENVARC);
  /* ENV: as the Prefs editors write it (one Write, then Close: what IPrefs is notified of) */
  if (!write_whole(PREFS_ENV, newp, len, 0)) {
    DeleteFile((STRPTR)PREFS_ENV);
    return fail("cannot write %s", PREFS_ENV);
  }
  return 1;
}
static int restore_prefs(void)
{
  long n = read_file(PREFS_BACKUP, oldp, sizeof oldp);
  if (n < 0) return fail("there is no backup to restore (%s)", PREFS_BACKUP);
  if (n == 0) {
    DeleteFile((STRPTR)PREFS_ENVARC);
    DeleteFile((STRPTR)PREFS_ENV);
    INFO("WBFit: there was no WBPattern.prefs before WBFit: removed it (the default pattern comes back after a reboot)\n");
  } else {
    if (!write_whole(PREFS_ENVARC, oldp, (ULONG)n, 1)) return fail("cannot write %s", PREFS_ENVARC);
    if (!write_whole(PREFS_ENV, oldp, (ULONG)n, 0)) return fail("cannot write %s", PREFS_ENV);
    INFO("WBFit: restored the WBPattern.prefs from before WBFit\n");
  }
  DeleteFile((STRPTR)PREFS_BACKUP);
  return 1;
}

/* ================================================================== one fit */
typedef struct { Scr s; int format; char path[512]; } Cache;
static Cache cache[8];
static int ncache;

static const char *const modename[] = { "FILL", "FIT", "STRETCH", "CENTER" };
static const char *const fmtname[] = { "PNG (24-bit)", "ILBM (24-bit)", "ILBM" };
static const char *const stname[WF_NST] = { "convert", "scale", "dither", "encode" };

/* truecolour: picture.datatype takes true-colour pixels (version 43+, or it gave us an RGB pixel
   array: AROS's says 41 but can) */
static int truecolour_seen;
static int choose(const Opt *o, const Scr *s, int truecolour, int *format, int *depth, int *usewb)
{
  int deep = s->depth > 8;
  *format = o->format;
  if (*format == FMT_AUTO) *format = truecolour && deep ? (exists("DEVS:DataTypes/PNG") ? WF_PNG : WF_ILBM24) : WF_ILBM;
  if (*format != WF_ILBM && !truecolour)
    INFO("  (this picture.datatype can't take a 24-bit picture: Workbench may not show it)\n");
  *depth = deep ? 8 : (int)s->depth;
  if (*depth < 1) *depth = 1;
  *usewb = o->palette == PAL_WB || (o->palette == PAL_AUTO && !deep);
  if (*usewb && !s->npal) { *usewb = 0; if (o->palette == PAL_WB) INFO("  (the screen has no palette: PALETTE=PICTURE)\n"); }
  return 1;
}

/* the output name: OUT (a file, or a drawer), else DEFDIR; in a drawer backdrop-WxH.ext, or
   backdrop-WxH-2.ext when the prefs name the first already (so they always change) */
static int out_name(const Opt *o, const Scr *s, int format, char *full, ULONG cap)
{
  char dir[512], f1[64], f2[64], cur[WFP_MAXNAME + 1];
  const char *ext = format == WF_PNG ? "png" : "ilbm";
  BPTR l;
  long n;
  ULONG len = strlen(o->out);
  if (len && o->out[len - 1] != ':' && o->out[len - 1] != '/' && !isdir(o->out)) {
    /* a file: as given, made absolute through its drawer */
    char *fp = (char *)FilePart((STRPTR)o->out);
    BPTR dl;
    strncpy(dir, o->out, sizeof dir - 1); dir[sizeof dir - 1] = 0;
    dir[fp - o->out] = 0;
    dl = Lock((STRPTR)dir, ACCESS_READ);
    if (!dl) return fail("OUT: cannot find the drawer of %s", o->out);
    n = full_name(dl, fp, full, cap);
    UnLock(dl);
    return n ? 1 : fail("OUT: name too long");
  }
  strncpy(dir, len ? o->out : DEFDIR, sizeof dir - 1); dir[sizeof dir - 1] = 0;
  if (!make_dir(dir)) return fail("cannot make the drawer %s", dir);
  sprintf(f1, "backdrop-%lux%lu.%s", (unsigned long)s->W, (unsigned long)s->H, ext);
  sprintf(f2, "backdrop-%lux%lu-2.%s", (unsigned long)s->W, (unsigned long)s->H, ext);
  if (!(l = Lock((STRPTR)dir, ACCESS_READ))) return fail("cannot find %s", dir);
  n = full_name(l, f1, full, cap);
  UnLock(l);
  if (!n) return fail("the output name is too long");
  cur[0] = 0;
  if ((n = read_file(PREFS_ENV, oldp, sizeof oldp)) > 0) wfp_get(oldp, (ULONG)n, WFP_ROOT, cur, sizeof cur, NULL);
  if (!Stricmp((STRPTR)cur, (STRPTR)full)) {
    char *p = (char *)FilePart((STRPTR)full);
    *p = 0;
    AddPart((STRPTR)full, (STRPTR)f2, cap);
  }
  return 1;
}

static int write_job(WfJob *j, const char *path, wf_u32 *us)
{
  char tmp[600];
  int e, ok;
  wf_u32 t0 = now();
  if (strlen(path) > 500 || !open_tmp(path, tmp)) return fail("cannot write %s", path);
  e = wf_fit(&run, j, wput, &wf);
  ok = e == WF_OK;
  if (!commit_tmp(path, tmp, ok) && ok) { *us = now() - t0; return fail("cannot write %s (disk full or in use?)", path); }
  *us = now() - t0;
  if (e) return fail("%s", e == WF_EWRITE ? "writing the file failed (disk full?)" : wf_errstr(e));
  return 1;
}

static void times_line(const WfJob *j, wf_u32 dec, wf_u32 wrt, wf_u32 tot)
{
  char b[5][16];
  wf_u32 enc = j->us[WF_ST_SCALE] + j->us[WF_ST_CONVERT] + j->us[WF_ST_DITHER] + j->us[WF_ST_ENCODE];
  ms(b[0], dec); ms(b[1], j->us[WF_ST_CONVERT] + j->us[WF_ST_SCALE]); ms(b[2], j->us[WF_ST_DITHER]); ms(b[3], j->us[WF_ST_ENCODE]);
  ms(b[4], wrt > enc ? wrt - enc : 0);
  INFO("  time: decode %s ms, scale %s ms, dither %s ms, encode %s ms, write %s ms", b[0], b[1], b[2], b[3], b[4]);
  ms(b[0], tot);
  INFO(", total %s ms\n", b[0]);
}

#ifdef WBFIT_NC
/* BENCH: the job again with the 68k code, a table of both */
static void bench(WfJob *j, wf_u32 dec, wf_u32 nat_total)
{
  WfJob k;
  wf_u32 t0, tot68, i, same = 1, nat = 0, m68 = 0;
  char a[16], b[16], c[16];
  int e;
  if (!wfnc_native()) { say("  BENCH: native calls are off (%s): only the 68k code ran\n", wfnc_state()); return; }
  k = *j;
  k.rgb = k.out = k.enc = k.ind = 0; k.encn = k.enccap = k.outcap = k.indcap = 0; k.crcs = 0; k.ncrc = 0;
  runner(0);
  t0 = now();
  e = wf_fit(&run, &k, NULL, NULL);
  tot68 = now() - t0;
  say("\n  BENCH            native ms       68k ms     gain   same\n");
  ms(a, dec);
  say("  decode (dt)  %12s %12s        -      -\n", a, a);
  for (i = 0; i < WF_NST; i++) {
    int used = (i != WF_ST_CONVERT || !j->src) && (i != WF_ST_DITHER || j->format == WF_ILBM);
    wf_u32 g;
    if (!used) continue;
    ms(a, j->us[i]); ms(b, k.us[i]);
    g = j->us[i] ? (k.us[i] * 10 + j->us[i] / 2) / j->us[i] : 0;
    sprintf(c, "%lu.%lux", (unsigned long)(g / 10), (unsigned long)(g % 10));
    say("  %-12s %12s %12s %8s %6s\n", stname[i], a, b, c, j->sum[i] == k.sum[i] ? "yes" : "NO");
    if (j->sum[i] != k.sum[i]) same = 0;
    nat += j->us[i]; m68 += k.us[i];
  }
  ms(a, nat); ms(b, m68);
  sprintf(c, "%lu.%lux", (unsigned long)(nat ? m68 * 10 / nat / 10 : 0), (unsigned long)(nat ? m68 * 10 / nat % 10 : 0));
  say("  %-12s %12s %12s %8s %6s\n", "our stages", a, b, c, same ? "yes" : "NO");
  ms(a, nat_total); ms(b, dec + tot68);
  say("  %-12s %12s %12s\n", "total", a, b);
  if (e) say("  (the 68k run failed: %s)\n", wf_errstr(e));
  wfnc_report(say);
  wf_job_free(&run, &k);
  runner(1);
}
#endif

/* the whole thing for the screen as it is now; 1 if done */
static int fit(const Opt *o, Scr *s, int setprefs)
{
  Pic p;
  WfJob j;
  int pdt = pdt_version(), format, depth, usewb, ok = 0, backedup = 0;
  wf_u32 t0 = now(), tdec, twrite = 0;
  char path[512], a[16];
  if (!get_screen(s)) return fail("the Workbench screen is not open");
  if (!load_pic(o->file, &p, !o->v40)) return 0;   /* (V40: the old way only, for testing) */
  tdec = now() - t0;
  truecolour_seen = pdt >= 43 || p.via == 1;
  choose(o, s, truecolour_seen, &format, &depth, &usewb);
  if (!out_name(o, s, format, path, sizeof path)) { pic_free(&p); return 0; }
  memset(&j, 0, sizeof j);
  if (p.rgb) { j.src = p.rgb; j.sstride = p.stride; }
  else { j.idx = p.idx; j.ipal = p.pal; j.ham = p.ham; j.sstride = p.stride; }
  j.sw = p.w; j.sh = p.h;
  j.W = s->W; j.H = s->H; j.rx = s->rx; j.ry = s->ry;
  j.mode = o->mode; j.bg = o->bg; j.bgauto = !o->bgset;
  j.format = format; j.depth = depth; j.dither = o->dither; j.pngadaptive = 1;
  if (format == WF_ILBM && usewb) { j.pal = s->pal; j.npal = s->npal; }
  if (write_job(&j, path, &twrite)) {
    ULONG g;
    INFO("WBFit: %s %lux%lu -> %lux%lu, %s", (char *)FilePart((STRPTR)o->file), (unsigned long)p.w, (unsigned long)p.h,
         (unsigned long)s->W, (unsigned long)s->H, modename[o->mode]);
    if (j.pl.dw != s->W || j.pl.dh != s->H)
      INFO(" (scaled to %lux%lu, %s %lu,%lu)", (unsigned long)j.pl.dw, (unsigned long)j.pl.dh,
           j.pl.px < 0 || j.pl.py < 0 ? "cropped at" : "placed at",
           (unsigned long)(j.pl.px < 0 ? -j.pl.px : j.pl.px), (unsigned long)(j.pl.py < 0 ? -j.pl.py : j.pl.py));
    INFO("\n");
    for (g = s->rx > s->ry ? s->ry : s->rx; g > 1 && (s->rx % g || s->ry % g); g--) ;
    INFO("  screen %lu bits, mode $%08lx, pixel aspect %lu:%lu%s; read via %s\n", (unsigned long)s->depth, (unsigned long)s->modeid,
         (unsigned long)(s->rx / (g ? g : 1)), (unsigned long)(s->ry / (g ? g : 1)), s->rx == s->ry ? " (square)" : " (corrected)",
         p.via == 1 ? "picture.datatype 43 pixel array" : "the datatype's bitmap");
    INFO("  wrote %s: %s", path, fmtname[format]);
    if (format == WF_ILBM) INFO(", %d colours (%s%s)", j.ncol, usewb ? "the Workbench palette" : "median cut", o->dither ? ", Floyd-Steinberg" : "");
    INFO(", %lu bytes\n", (unsigned long)j.filebytes);
    if (j.faults) INFO("  (a native call failed: %lu stage(s) redone with the 68k code)\n", (unsigned long)j.faults);
    ok = 1;
    if (setprefs) {
      ok = set_prefs(path, &backedup);
      if (ok) INFO("  backdrop set: %s and %s%s\n", PREFS_ENVARC, PREFS_ENV, backedup ? " (the old prefs saved in " PREFS_BACKUP ")" : "");
    }
    times_line(&j, tdec, twrite, now() - t0);
#ifdef WBFIT_NC
    if (o->bench) bench(&j, tdec, now() - t0);
    else if (o->times) INFO("  kernels: %s\n", wfnc_state());
#endif
    if (o->times && !o->bench) {
      int i;
      for (i = 0; i < WF_NST; i++) { ms(a, j.us[i]); INFO("    %-8s %10s ms  (CRC %08lx)\n", stname[i], a, (unsigned long)j.sum[i]); }
    }
    if (ok && ncache < 8) { cache[ncache].s = *s; cache[ncache].format = format; strcpy(cache[ncache].path, path); ncache++; }
  }
  wf_job_free(&run, &j);
  pic_free(&p);
  return ok;
}

/* ================================================================== WATCH */
static void watch(const Opt *o, Scr *last)
{
  struct MsgPort *port = CreateMsgPort();
  struct NotifyRequest nr[2];
  static const char *const files[2] = { "ENV:Sys/ScreenMode.prefs", "ENV:Sys/Overscan.prefs" };
  struct NewBroker nb;
  CxObj *broker = NULL;
  LONG sig = AllocSignal(-1), err = 0;
  int i, done = 0, enabled = 1, nn = 0;
  if (!port || sig < 0) { fail("WATCH: no signal"); goto out; }
  if ((CxBase = OpenLibrary((STRPTR)"commodities.library", 37))) {
    memset(&nb, 0, sizeof nb);
    nb.nb_Version = NB_VERSION; nb.nb_Name = (STRPTR)"WBFit"; nb.nb_Title = (STRPTR)"WBFit " VERSTR;
    nb.nb_Descr = (STRPTR)"Refits the backdrop when the Workbench screen changes";
    nb.nb_Unique = NBU_UNIQUE | NBU_NOTIFY; nb.nb_Port = port;
    if (!(broker = CxBroker(&nb, &err))) {
      if (err == CBERR_DUP) INFO("WBFit: told the WBFit already watching to stop\n");
      else fail("WATCH: cannot make the commodity");
      goto out;
    }
    ActivateCxObj(broker, TRUE);
  }
  for (i = 0; i < 2; i++) {
    memset(&nr[i], 0, sizeof nr[i]);
    nr[i].nr_Name = (STRPTR)files[i];
    nr[i].nr_Flags = NRF_SEND_SIGNAL;
    nr[i].nr_stuff.nr_Signal.nr_Task = FindTask(NULL);
    nr[i].nr_stuff.nr_Signal.nr_SignalNum = (UBYTE)sig;
    if (StartNotify(&nr[i])) nn = i + 1; else break;
  }
  if (!nn) { fail("WATCH: cannot watch %s", files[0]); goto out; }
  INFO("WBFit: watching the Workbench screen (Exchange or Ctrl-C stops it)\n");
  show_wb();
  quiet = 1;                                /* (no requesters from now on but for errors) */
  while (!done) {
    ULONG got = Wait((1UL << sig) | (1UL << port->mp_SigBit) | SIGBREAKF_CTRL_C);
    CxMsg *m;
    if (got & SIGBREAKF_CTRL_C) done = 1;
    while (CxBase && (m = (CxMsg *)GetMsg(port))) {
      ULONG type = CxMsgType(m), id = CxMsgID(m);
      ReplyMsg((struct Message *)m);
      if (type != CXM_COMMAND) continue;
      if (id == CXCMD_DISABLE) { ActivateCxObj(broker, FALSE); enabled = 0; }
      else if (id == CXCMD_ENABLE) { ActivateCxObj(broker, TRUE); enabled = 1; }
      else if (id == CXCMD_KILL || id == CXCMD_UNIQUE) done = 1;
    }
    if (!done && enabled && (got & (1UL << sig))) {
      /* the prefs changed: IPrefs reopens Workbench if the screen changed; wait for it (Delay: no CPU) */
      Scr s;
      int tries;
      for (tries = 0; tries < 30; tries++) {
        Delay(50);
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) break;
        if (get_screen(&s) && !same_screen(&s, last)) break;
      }
      if (tries < 30 && get_screen(&s) && !same_screen(&s, last)) {
        int k, hit = -1, format, depth, usewb;
        choose(o, &s, truecolour_seen, &format, &depth, &usewb);
        for (k = 0; k < ncache; k++) if (same_screen(&cache[k].s, &s) && cache[k].format == format && exists(cache[k].path)) hit = k;
        failed = 0;
        if (hit >= 0) { int b; set_prefs(cache[hit].path, &b); }
        else fit(o, &s, 1);
        *last = s;
        show_wb();
      }
    }
  }
  for (i = 0; i < nn; i++) EndNotify(&nr[i]);
out:
  if (broker) DeleteCxObjAll(broker);
  if (port) { CxMsg *m; while ((m = (CxMsg *)GetMsg(port))) ReplyMsg((struct Message *)m); DeleteMsgPort(port); }
  if (sig >= 0) FreeSignal(sig);
  if (CxBase) CloseLibrary(CxBase);
  CxBase = NULL;
}

/* ================================================================== main */
int main(int argc, char **argv)
{
  Opt o;
  Scr s;
  int rc = RETURN_FAIL;
  (void)argv; (void)version;
  fromwb = argc == 0;
  IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 39);
  GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 39);
  DataTypesBase = OpenLibrary((STRPTR)"datatypes.library", 39);
  IconBase = OpenLibrary((STRPTR)"icon.library", 36);
  UtilityBase = OpenLibrary((STRPTR)"utility.library", 37);
  if (!IntuitionBase || !GfxBase || !DataTypesBase || !UtilityBase) {
    fail("needs OS 3.0 or newer (intuition, graphics and datatypes.library 39)");
    goto out;
  }
  if (!OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)&treq, 0)) { TimerBase = treq.tr_node.io_Device; timer_open = 1; }
  runner(1);
  if (!get_opts(&o)) goto out;
  quiet = o.quiet;
  if (o.restore) { if (restore_prefs()) rc = RETURN_OK; goto out; }
  if (!o.file[0]) {
    fail(fromwb ? "drop a picture's icon on WBFit's icon (or give it a FILE tool type)" :
                  "give a picture: WBFit FILE [MODE=FILL|FIT|STRETCH|CENTER] [BG=RRGGBB] [OUT=file|drawer] "
                  "[FORMAT=AUTO|PNG|ILBM|ILBM24] [PALETTE=AUTO|WB|PICTURE] [DITHER] [RESTORE] [WATCH] [QUIET] [TIMES] [BENCH]");
    goto out;
  }
  if (!fit(&o, &s, 1)) goto out;
  rc = RETURN_OK;
  if (o.watch) watch(&o, &s);
out:
  show_wb();
#ifdef WBFIT_NC
  wfnc_close();
#endif
  if (timer_open) CloseDevice((struct IORequest *)&treq);
  if (IconBase) CloseLibrary(IconBase);
  if (UtilityBase) CloseLibrary(UtilityBase);
  if (DataTypesBase) CloseLibrary(DataTypesBase);
  if (GfxBase) CloseLibrary((struct Library *)GfxBase);
  if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
  return rc;
}
