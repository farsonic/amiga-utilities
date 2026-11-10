/*
 * pxbase.h - what the pixel kernels of our tools share (WBFit's wfk.c, FitView's fvk.c): the
 * types, the parameter-block conventions, the clock that makes a native call stop after about
 * 0.3 ms, the Q14 rounding of the resampler, a 16 x 16 bit multiply, and the palette search of
 * the ditherers. Header only (static functions): each kernel file includes it once.
 *
 * One source is the plain 68k program's code, the host tests' code and, with WFK_NC, the
 * NativeCall kernels (AArch64, big-endian). Parameter blocks are shared with native code there:
 * every field 4 bytes (pointers as NC_U32, read with WFK_P), layouts pinned with
 * WFK_CHECK_SIZE/OFFSET. In the plain build pointers are pointers and nothing here needs the
 * NativeCall SDK. (The names say wfk_: they were WBFit's first.)
 *
 * The clock: natively a range kernel stops after about WFK_BUDGET_US of work (the ARM counter)
 * at the end of a unit and sets range.to to where it stopped; the caller calls again from
 * there. With WFK_TEST_BUDGET (host tests) it stops after a count of checks, so the stops come
 * at many places; else (the 68k) never. Each kernel file that uses the test budget defines
 * wfk_test_budget once (WFK_DEFINE_BUDGET).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PXBASE_H
#define PXBASE_H

#ifdef WFK_NC
#include <nativecall/kernel.h>
typedef NC_U32 wfk_u32;
typedef NC_S32 wfk_s32;
typedef NC_U16 wfk_u16;
typedef NC_S16 wfk_s16;
typedef NC_U8 wfk_u8;
typedef NC_U32 wfk_ptr;
#define WFK_P(type, f)   NC_PTR(type, f)
#define WFK_SETP(x)      ((NC_U32)(NC_UPTR)(x))
#define WFK_KERNEL(name) NC_KERNEL(name)
#define WFK_RANGE        struct NCRange range
#define WFK_RANGE_T      struct NCRange
#define WFK_CHECK_SIZE(t, n)      NC_CHECK_SIZE(t, n)
#define WFK_CHECK_OFFSET(t, f, n) NC_CHECK_OFFSET(t, f, n)
#else
#include <stddef.h>
#include <string.h>
typedef unsigned int wfk_u32;            /* 32 bits on the host and on the 68k (vbcc) */
typedef int wfk_s32;
typedef unsigned short wfk_u16;
typedef short wfk_s16;
typedef unsigned char wfk_u8;
typedef void *wfk_ptr;
struct WfkRange { wfk_u32 from, to; };
#define WFK_P(type, f)   ((type *)(f))
#define WFK_SETP(x)      ((void *)(x))
#define WFK_KERNEL(name) wfk_u32 name(void *params)
#define WFK_RANGE        struct WfkRange range
#define WFK_RANGE_T      struct WfkRange
#define WFK_CHECK_SIZE(t, n)
#define WFK_CHECK_OFFSET(t, f, n)
#ifndef NC_NATIVE
#define NC_NATIVE 0
#endif
#endif

/* weights are Q14: the taps of one output pixel add up to exactly 1 << WFK_WBITS */
#define WFK_WBITS 14

#ifdef WFK_TEST_BUDGET
extern unsigned long wfk_test_budget;    /* host tests: stop after this many clock checks (0 never) */
#endif

/* ---------------------------------------------------------------- for kernel files only */
#ifdef PX_KERNEL_FILE

#if NC_NATIVE
#include <stdint.h>
#endif

#define WFK_BUDGET_US 300
#if NC_NATIVE
typedef struct { uint64_t end; } Clk;
static inline uint64_t cnt(void) { uint64_t v; __asm__ __volatile__("isb\n mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static void clk_start(Clk *c)
{
  uint64_t f;
  __asm__ __volatile__("mrs %0, CNTFRQ_EL0" : "=r"(f));
  if (!f || f > 1000000000ULL) f = 54000000ULL;     /* (a Pi 4's, if unset) */
  c->end = cnt() + f / 1000000ULL * WFK_BUDGET_US;
}
static int clk_over(Clk *c) { return cnt() >= c->end; }
#define WFK_DEFINE_BUDGET
#elif defined(WFK_TEST_BUDGET)
typedef struct { unsigned long n; } Clk;
static void clk_start(Clk *c) { c->n = 0; }
static int clk_over(Clk *c) { return wfk_test_budget && ++c->n >= wfk_test_budget; }
#define WFK_DEFINE_BUDGET unsigned long wfk_test_budget;
#else
typedef int Clk;
static void clk_start(Clk *c) { *c = 0; }
#define clk_over(c) 0
#define WFK_DEFINE_BUDGET
#endif
/* at the top of a loop over units: stop before unit u (never before the first) */
#define CLK_STOP(c, p, u) do { if ((u) > (p)->range.from && clk_over(c)) { (p)->range.to = (u); return 0; } } while (0)

/* a Q14 sum to a byte: clamp(floor((v + 8192) / 16384), 0, 255) */
static wfk_u8 q14(wfk_s32 v)
{
  if (v <= 0) return 0;
  v = (v + (1 << (WFK_WBITS - 1))) >> WFK_WBITS;
  return (wfk_u8)(v > 255 ? 255 : v);
}

/* a 16 x 16 -> 32 bit signed multiply: one muls.w on any 68k (vbcc would make a 32-bit one,
   three mulu.w on the 68000), plain C elsewhere. Both factors must fit in 16 bits. */
#if defined(__VBCC__) && defined(__M68K__)
wfk_s32 px_mul16(__reg("d0") wfk_s16 a, __reg("d1") wfk_s16 b) = "\tmuls.w\td1,d0";
#define MUL16(a, b) px_mul16((wfk_s16)(a), (wfk_s16)(b))
#else
#define MUL16(a, b) ((wfk_s32)(wfk_s16)(a) * (wfk_s32)(wfk_s16)(b))
#endif

/* the palette colour (pal: npal RGB triples) nearest a 5-bit bucket's centre (weighted 3:4:2),
   remembered in cache (32768 entries, 0xFFFF where not known yet) */
static wfk_u32 px_nearest(wfk_u16 *cache, const wfk_u8 *pal, wfk_u32 npal, wfk_u32 key)
{
  wfk_s32 r, g, b, bd = 0x7FFFFFFFL;
  wfk_u32 i, best = 0;
  if (cache[key] != 0xFFFF) return cache[key];
  r = (wfk_s32)(((key >> 10) & 31) << 3 | 4); g = (wfk_s32)(((key >> 5) & 31) << 3 | 4); b = (wfk_s32)((key & 31) << 3 | 4);
  for (i = 0; i < npal; i++) {
    const wfk_u8 *q = pal + i * 3;
    wfk_s32 dr = q[0] - r, dg = q[1] - g, db = q[2] - b, dd = dr * dr * 3 + dg * dg * 4 + db * db * 2;
    if (dd < bd) { bd = dd; best = i; }
  }
  cache[key] = (wfk_u16)best;
  return best;
}
#define PX_KEY(r, g, b) (((wfk_u32)((r) >> 3) << 10) | ((wfk_u32)((g) >> 3) << 5) | (wfk_u32)((b) >> 3))
static int px_clampi(wfk_s32 v) { return v < 0 ? 0 : v > 255 ? 255 : (int)v; }
/* an error sixteenth, rounded half away from zero (the same on every compiler) */
static wfk_s32 px_e16(wfk_s32 e) { return e >= 0 ? (e + 8) >> 4 : -((-e + 8) >> 4); }

#endif /* PX_KERNEL_FILE */
#endif
