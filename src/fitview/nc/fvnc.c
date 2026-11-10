/*
 * fvnc.c - FitView's NativeCall runner (fvnc.h). NativeCall test build only. From WBFit's
 * wfnc.c: the kernel set (fitview.narm, embedded) is opened when the runner is made (before the
 * loader process allocates anything, so nc_alloc() and nc_free() always agree on where memory
 * came from). The kernels' memory comes from fv_alloc() (nc_alloc() while it can); a buffer
 * native code can't see turns native calls off for the rest of the run. The parameter block is
 * copied into nc_alloc() memory for each call and back. Each native call stops after about
 * 0.3 ms (the kernels watch the ARM counter) and is called again from where it stopped. If a
 * call fails (nc_status not NC_ST_OK, or the range it says it did is wrong) the runner reports
 * FV_EFAULT: the renderer does that band again, and from then on the runner uses the 68k code.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <nativecall/nativecall.h>
#include "fvk.h"
#include "fvnc.h"
#include "fvload.h"

extern struct Library *NativeCallBase;        /* (ncsupport.c) */

static struct NCKernels *kset;
static int opened, broken, want, invisible;
static char state[100] = "68k (not started)";
static void *pblk;
static ULONG ncalls, nfaults, n68k;
static const ULONG psize[FVK_COUNT] = {
  sizeof(struct FvkH), sizeof(struct FvkV), sizeof(struct FvkNN), sizeof(struct FvkFill), sizeof(struct FvkComp), sizeof(struct FvkDither)
};
static const char *const kname[FVK_COUNT] = { "hpass", "vpass", "nn", "fill", "comp", "dither" };

const char *fvnc_state(void) { return state; }

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
  if (!(kset = nc_open(&fitview_kernels))) { put("68k (", "out of memory"); return 0; }
  st = nc_status(kset);
  if (nc_is_native(kset)) {
    ULONG i;
    struct NCKernelStats s;
    if (!(pblk = nc_alloc(256)) || !nc_addr_ok(pblk, 256)) { put("68k (", "no memory native code can see"); return 0; }
    for (i = 0; i < FVK_COUNT; i++) nc_stats(kset, i, &s, TRUE);
    put("native (fitview.narm, 0.3 ms a call)", 0);
    return 1;
  }
  put("68k (", !NativeCallBase ? "no nativecall.library" : st == NC_ST_NONE ? "no NativeCall armstub or not a 68040/060" :
               st == NC_ST_NOCPU ? "not a 68040/68060" : st == NC_ST_NOMEM ? "no Fast RAM above 16 MB" :
               st == NC_ST_MISMATCH ? "module mismatch" : st == NC_ST_NOMODULE ? "no module" : "no native calls");
  return 0;
}
static int native_now(void) { return want && !broken && !invisible && open_set(); }
int fvnc_native(void) { return !broken && !invisible && open_set(); }

void fvnc_check(const void *p, unsigned long n)
{
  if (!p || invisible) return;
  if (open_set() && !nc_addr_ok(p, n)) { invisible = 1; put("68k (", "a buffer native code can't see"); }
}

static void *a_alloc(FvRun *r, fv_u32 n)
{
  void *p;
  (void)r;
  if ((p = fv_alloc(n))) fvnc_check(p, n);
  return p;
}
static void a_free(FvRun *r, void *p, fv_u32 n) { (void)r; (void)n; fv_free(p); }

static void fault(void)
{
  nfaults++;
  broken = 1;
  put("68k (", "a kernel failed natively: done again with the 68k code");
}

static int r_range(FvRun *r, int id, void *p, fv_u32 from, fv_u32 to)
{
  WFK_RANGE_T *rg;
  (void)r;
  if (!native_now()) {
    rg = (WFK_RANGE_T *)p;
    rg->from = from; rg->to = to;
    fitview_kernels.fallback[id](p);           /* the 68k code, to the end */
    n68k++;
    return rg->to == to ? 0 : FV_EFAULT;
  }
  memcpy(pblk, p, psize[id]);
  rg = (WFK_RANGE_T *)pblk;
  while (from < to) {                          /* (bounded: every call does at least one unit) */
    rg->from = from; rg->to = to;
    ncalls++;
    nc_call(kset, (ULONG)id, pblk);
    if (nc_status(kset) != NC_ST_OK || rg->to <= from || rg->to > to) { fault(); return FV_EFAULT; }
    from = rg->to;
  }
  memcpy(p, pblk, psize[id]);
  return 0;
}

void fvnc_runner(FvRun *r, int native)
{
  r->range = r_range; r->alloc = a_alloc; r->free = a_free; r->ctx = 0;
  want = native;
  open_set();
}

void fvnc_report(void (*say)(const char *fmt, ...))
{
  int i, over = 0;
  say("  kernels: %s; native calls %lu, failed %lu, 68k runs %lu\n", state, (unsigned long)ncalls, (unsigned long)nfaults, (unsigned long)n68k);
  if (!kset || !nc_is_native(kset)) return;
  say("  kernel     calls   native ms   avg us   max us\n");
  for (i = 0; i < FVK_COUNT; i++) {
    struct NCKernelStats s;
    if (!nc_stats(kset, (ULONG)i, &s, TRUE) || !s.calls) continue;
    say("  %-8s %7lu %8lu.%01lu %8lu %8lu%s\n", kname[i], (unsigned long)s.calls, (unsigned long)(s.total_us / 1000),
        (unsigned long)(s.total_us / 100 % 10), (unsigned long)(s.total_us / s.calls), (unsigned long)s.max_us, s.max_us > 1000 ? " !" : "");
    if (s.max_us > 1000) over = 1;
  }
  if (over) say("  ! a native call took over 1 ms\n");
}

void fvnc_close(void)
{
  if (pblk) nc_free(pblk, 256);
  pblk = 0;
  if (kset) nc_close(kset);
  kset = 0;
}
