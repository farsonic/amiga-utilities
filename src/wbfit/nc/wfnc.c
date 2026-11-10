/*
 * wfnc.c - WBFit's NativeCall runner (wfnc.h). NativeCall test build only.
 *
 * The kernel set (wbfit.narm, embedded) is opened on first use. Every buffer comes from
 * nc_alloc() while it can (behind each block its size and where it came from); a block native
 * code can't see turns native calls off for the rest of the run. The parameter block is copied
 * into a block of nc_alloc() memory for each call (the 68k's stack may not be visible to native
 * code) and back after it. Each native call stops after about 0.3 ms (the kernels watch the ARM
 * counter) and is called again from where it stopped. If a call fails (nc_status not
 * NC_ST_OK, or the range it says it did is wrong) the runner reports WF_EFAULT: wfcore.c redoes
 * that stage from its start, and from then on the runner uses the 68k code.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <nativecall/nativecall.h>
#include "wfk.h"
#include "wfnc.h"

extern struct Library *NativeCallBase;        /* (ncsupport.c) */

static struct NCKernels *kset;
static int opened, broken, want, invisible;
static char state[100] = "68k (not started)";
static void *pblk;
static ULONG ncalls, nfaults, n68k;
static const ULONG psize[WFK_COUNT] = {
  sizeof(struct WfkH), sizeof(struct WfkV), sizeof(struct WfkFill), sizeof(struct WfkLut), sizeof(struct WfkHist),
  sizeof(struct WfkDither), sizeof(struct WfkFilt), sizeof(struct WfkDefl), sizeof(struct WfkCrc), sizeof(struct WfkIlbm)
};
static const char *const kname[WFK_COUNT] = { "hpass", "vpass", "fill", "lut", "hist", "dither", "pngfilt", "deflate", "crc", "ilbm" };

const char *wfnc_state(void) { return state; }

static void put(const char *a, const char *b)
{
  strcpy(state, a);
  if (b && strlen(state) + strlen(b) < sizeof state - 2) { strcat(state, b); strcat(state, ")"); }
}

static int open_set(void)
{
  ULONG st;
  if (opened) return kset && nc_is_native(kset) && pblk;
  opened = 1;
  if (!(kset = nc_open(&wbfit_kernels))) { put("68k (", "out of memory"); return 0; }
  st = nc_status(kset);
  if (nc_is_native(kset)) {
    ULONG i;
    struct NCKernelStats s;
    if (!(pblk = nc_alloc(256)) || !nc_addr_ok(pblk, 256)) { put("68k (", "no memory native code can see"); return 0; }
    for (i = 0; i < WFK_COUNT; i++) nc_stats(kset, i, &s, TRUE);
    put("native (wbfit.narm, 0.3 ms a call)", 0);
    return 1;
  }
  put("68k (", !NativeCallBase ? "no nativecall.library" : st == NC_ST_NONE ? "no NativeCall armstub or not a 68040/060" :
               st == NC_ST_NOCPU ? "not a 68040/68060" : st == NC_ST_NOMEM ? "no Fast RAM above 16 MB" :
               st == NC_ST_MISMATCH ? "module mismatch" : st == NC_ST_NOMODULE ? "no module" : "no native calls");
  return 0;
}
static int native_now(void) { return want && !broken && !invisible && open_set(); }
int wfnc_native(void) { return !broken && !invisible && open_set(); }

/* a block: [size][kind] then the memory; kind 1 nc_alloc, 2 AllocVec */
static void *a_alloc(WfRun *r, wf_u32 n)
{
  ULONG *p;
  (void)r;
  if (n > 0x7FFFFFF0UL) return 0;
  open_set();
  if ((p = (ULONG *)nc_alloc(n + 8))) { p[0] = n + 8; p[1] = 1; }
  else if ((p = (ULONG *)AllocVec(n + 8, MEMF_ANY | MEMF_CLEAR))) { p[0] = n + 8; p[1] = 2; }
  else return 0;
  if (kset && nc_is_native(kset) && !nc_addr_ok(p, n + 8) && !invisible) { invisible = 1; put("68k (", "a buffer native code can't see"); }
  return p + 2;
}
static void a_free(WfRun *r, void *q, wf_u32 n)
{
  ULONG *p = (ULONG *)q - 2;
  (void)r; (void)n;
  if (p[1] == 1) nc_free(p, p[0]);
  else FreeVec(p);
}
static wf_u32 a_now(WfRun *r) { (void)r; return nc_now_us(); }

static void fault(void)
{
  nfaults++;
  broken = 1;
  put("68k (", "a kernel failed natively: redone with the 68k code");
}

static int r_range(WfRun *r, int id, void *p, wf_u32 from, wf_u32 to)
{
  WFK_RANGE_T *rg;
  (void)r;
  if (!native_now()) {
    rg = (WFK_RANGE_T *)p;
    rg->from = from; rg->to = to;
    wbfit_kernels.fallback[id](p);             /* the 68k code, to the end */
    n68k++;
    return rg->to == to ? 0 : WF_EFAULT;
  }
  memcpy(pblk, p, psize[id]);
  rg = (WFK_RANGE_T *)pblk;
  while (from < to) {
    rg->from = from; rg->to = to;
    ncalls++;
    nc_call(kset, (ULONG)id, pblk);
    if (nc_status(kset) != NC_ST_OK || rg->to <= from || rg->to > to) { fault(); return WF_EFAULT; }
    from = rg->to;
  }
  memcpy(p, pblk, psize[id]);
  return 0;
}
static wf_u32 r_call(WfRun *r, int id, void *p, int *flt)
{
  wf_u32 v;
  (void)r;
  *flt = 0;
  if (!native_now()) { n68k++; return wbfit_kernels.fallback[id](p); }
  memcpy(pblk, p, psize[id]);
  ncalls++;
  v = nc_call(kset, (ULONG)id, pblk);
  if (nc_status(kset) != NC_ST_OK) { fault(); *flt = 1; return 0; }
  memcpy(p, pblk, psize[id]);
  return v;
}

void wfnc_runner(WfRun *r, int native)
{
  r->range = r_range; r->call = r_call; r->alloc = a_alloc; r->free = a_free; r->now_us = a_now; r->ctx = 0;
  want = native;
}

void wfnc_report(void (*say)(const char *fmt, ...))
{
  int i, over = 0;
  say("  kernels: %s; native calls %lu, failed %lu, 68k runs %lu\n", state, (unsigned long)ncalls, (unsigned long)nfaults, (unsigned long)n68k);
  if (!kset || !nc_is_native(kset)) return;
  say("  kernel     calls   native ms   avg us   max us\n");
  for (i = 0; i < WFK_COUNT; i++) {
    struct NCKernelStats s;
    if (!nc_stats(kset, (ULONG)i, &s, TRUE) || !s.calls) continue;
    say("  %-8s %7lu %8lu.%01lu %8lu %8lu%s\n", kname[i], (unsigned long)s.calls, (unsigned long)(s.total_us / 1000),
        (unsigned long)(s.total_us / 100 % 10), (unsigned long)(s.total_us / s.calls), (unsigned long)s.max_us, s.max_us > 1000 ? " !" : "");
    if (s.max_us > 1000) over = 1;
  }
  if (over) say("  ! a native call took over 1 ms\n");
}

void wfnc_close(void)
{
  if (pblk) nc_free(pblk, 256);
  pblk = 0;
  if (kset) nc_close(kset);
  kset = 0;
}
