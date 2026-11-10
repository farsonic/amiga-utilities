/*
 * wfk.h - WBFit's kernels: every long loop over pixels or bytes.
 *
 *   wfk_hpass    resampling, horizontal: source rows [from, to) into the intermediate rows
 *   wfk_vpass    resampling, vertical: output rows [from, to) from the intermediate rows
 *   wfk_fill     rows [from, to) of the output filled with one colour (letterbox borders)
 *   wfk_lut      colour conversion: rows of palette indices (or HAM) to RGB (old picture.datatype)
 *   wfk_hist     rows of RGB counted into a 32768-bucket histogram (5 bits a gun; the quantiser)
 *   wfk_dither   rows of RGB to palette indices: nearest colour, or Floyd-Steinberg
 *   wfk_pngfilt  PNG rows filtered (the five filters, the smallest sum of |bytes| wins)
 *   wfk_deflate  zlib deflate (greedy LZ77, dynamic/fixed/stored blocks), resumable, state in a block
 *   wfk_crc      CRC-32 of bytes [from, to)
 *   wfk_ilbm     ILBM BODY rows: chunky (indices, or 24-bit RGB) to bitplanes, ByteRun1 packed
 *
 * One source (wfk.c) is the plain 68k program's code, the host tests' code and, with WFK_NC,
 * the NativeCall kernels (AArch64 on an Emu68 Pi; nc/). The parameter blocks are shared with
 * native code there: every field 4 bytes (pointers as NC_U32 read with WFK_P), layouts pinned
 * with NC_CHECK_SIZE/OFFSET. In the plain build pointers are pointers and nothing here needs
 * the NativeCall SDK.
 *
 * Natively a range kernel stops after about 0.3 ms of work (the ARM counter) at the end of a
 * unit (a row, or 4 KB for wfk_crc) and sets range.to to where it stopped; the caller calls again
 * from there. wfk_deflate returns WFK_MORE until it is done. On the 68k they run to the end.
 * All arithmetic is integer, so the 68k and native results are the same bit for bit.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef WFK_H
#define WFK_H

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

#define WFK_LIST(K) \
  K(wfk_hpass)   \
  K(wfk_vpass)   \
  K(wfk_fill)    \
  K(wfk_lut)     \
  K(wfk_hist)    \
  K(wfk_dither)  \
  K(wfk_pngfilt) \
  K(wfk_deflate) \
  K(wfk_crc)     \
  K(wfk_ilbm)

#define WFK__ENUM(n) WFKI_##n,
enum { WFK_LIST(WFK__ENUM) WFK_COUNT };

/* weights are Q14: the taps of one output pixel add up to exactly 1 << WFK_WBITS */
#define WFK_WBITS 14

/* ---- resampling. tab: wfk_u32 [n][2] (first source index, taps), wts: wfk_s16 [n][k].
   hpass: source rows [from, to) (absolute), each from src + y * sstride (RGB, 3 bytes a pixel),
   into out + (y - y0) * ostride, ow pixels. vpass: output rows [from, to), each from the
   intermediate rows (in, istride, rows relative to the intermediate's first) into
   out + r * ostride, ow pixels. ident: every output pixel is one source pixel of weight 1
   (no scaling on this axis): a plain copy. */
struct WfkH {
  WFK_RANGE;
  wfk_ptr src;
  wfk_u32 sstride;
  wfk_ptr out;
  wfk_u32 ostride, ow, y0;
  wfk_ptr tab, wts;
  wfk_u32 k, ident;
};
WFK_CHECK_SIZE(struct WfkH, 48)
WFK_CHECK_OFFSET(struct WfkH, ident, 44)
struct WfkV {
  WFK_RANGE;
  wfk_ptr in;
  wfk_u32 istride;
  wfk_ptr out;
  wfk_u32 ostride, ow;
  wfk_ptr tab, wts;
  wfk_u32 k, ident, pad;
};
WFK_CHECK_SIZE(struct WfkV, 48)
WFK_CHECK_OFFSET(struct WfkV, k, 36)

/* ---- fill: rows [from, to) of out (stride a row), pixels [x0, x1) set to rgb (0x00RRGGBB) */
struct WfkFill {
  WFK_RANGE;
  wfk_ptr out;
  wfk_u32 stride, x0, x1, rgb, pad;
};
WFK_CHECK_SIZE(struct WfkFill, 32)

/* ---- lut: rows [from, to) of in (w indices a row, stride istride) to RGB at out (w * 3 a row).
   pal: wfk_u8 [256][3] (768 bytes). ham: 0, or the HAM data bits: 4 (HAM6) or 6 (HAM8). */
struct WfkLut {
  WFK_RANGE;
  wfk_ptr in;
  wfk_u32 istride, w;
  wfk_ptr out, pal;
  wfk_u32 ham;
};
WFK_CHECK_SIZE(struct WfkLut, 32)

/* ---- hist: rows [from, to) of px (w RGB pixels a row, stride) counted into hist
   (wfk_u32 [32768], index r5 << 10 | g5 << 5 | b5); counts saturate at 0xFFFFFFFF */
struct WfkHist {
  WFK_RANGE;
  wfk_ptr px;
  wfk_u32 stride, w;
  wfk_ptr hist;
  wfk_u32 pad[2];
};
WFK_CHECK_SIZE(struct WfkHist, 32)

/* ---- dither: rows [from, to) of px (RGB) to indices at out (w a row). pal: wfk_u8 [256][3],
   npal colours; cache: wfk_u16 [32768], 0xFFFF where not known yet (cleared by the caller to
   0xFF bytes): the nearest colour of each 5-bit bucket's centre. fs: Floyd-Steinberg, the
   errors in e0 / e1 (wfk_s32 [(w + 2) * 3] each, cleared by the caller); rows must come in
   order from row 0. */
struct WfkDither {
  WFK_RANGE;
  wfk_ptr px;
  wfk_u32 stride, w;
  wfk_ptr out, pal;
  wfk_u32 npal;
  wfk_ptr cache;
  wfk_u32 fs;
  wfk_ptr e0, e1;
  wfk_u32 pad;
};
WFK_CHECK_SIZE(struct WfkDither, 56)
WFK_CHECK_OFFSET(struct WfkDither, e1, 48)

/* ---- PNG filtering: rows [from, to) of in (bpp 3 RGB or 1 palette, w pixels, stride) to
   out + y * (1 + w * bpp): the filter type byte, then the row. adaptive: the filter with the
   smallest sum of the bytes taken as signed (libpng's heuristic); else filter 0. */
struct WfkFilt {
  WFK_RANGE;
  wfk_ptr in;
  wfk_u32 stride, w, bpp;
  wfk_ptr out;
  wfk_u32 adaptive;
};
WFK_CHECK_SIZE(struct WfkFilt, 32)

/* ---- deflate: in[0, n) as a zlib stream into out (ocap bytes), all state in st
   (struct WfkDefState, cleared by the caller before the first call). Call until it returns
   WFK_DONE (olen: the stream's length) or WFK_FULL (out too small). */
enum { WFK_DONE = 0, WFK_MORE = 1, WFK_FULL = 2 };
#define WFK_WSIZE 32768
#define WFK_HSIZE 32768
#define WFK_BLOCKSYM 16384
struct WfkDefState {
  wfk_u32 head[WFK_HSIZE], prev[WFK_WSIZE];      /* hash chains: positions + 1, 0 none */
  wfk_u16 sl[WFK_BLOCKSYM], sd[WFK_BLOCKSYM];    /* the block's symbols: literal (sd 0) or length, distance */
  wfk_u32 lf[288], df[32];
  wfk_u32 phase, pos, bstart, bend, ns;
  wfk_u32 opos, bits, nbits, full;
  wfk_u32 a, b, apos;                            /* Adler-32 */
  wfk_u32 w[2 * 288], tf[288];
  wfk_s32 par[2 * 288], dep[2 * 288], srt[288];
  wfk_u16 lc[288], pad16;
  wfk_u8 lcode[260], dlo[256], dhi[256];
  wfk_u8 ll[288], fl[288], all[316], cls[316], cle[316];
};
struct WfkDefl {
  WFK_RANGE;                                     /* (not used) */
  wfk_ptr st, in;
  wfk_u32 n;
  wfk_ptr out;
  wfk_u32 ocap, olen;
  wfk_u32 chain, nice, good;
};
WFK_CHECK_SIZE(struct WfkDefl, 44)
WFK_CHECK_OFFSET(struct WfkDefl, good, 40)

/* ---- CRC-32 (PNG's): bytes [from, to) of p added to crc (start 0: the pre- and
   post-inversion are done here). tab: wfk_u32 [256], made by wfk_crc_table. */
struct WfkCrc {
  WFK_RANGE;
  wfk_ptr p, tab;
  wfk_u32 crc, pad;
};
WFK_CHECK_SIZE(struct WfkCrc, 24)

/* ---- ILBM BODY: rows [from, to) of in (w pixels a row, stride; depth 1-8: indices, depth 24:
   RGB) as depth bitplanes (24: R0..R7 G0..G7 B0..B7), each row of rb = ((w + 15) / 16) * 2
   bytes ByteRun1 packed (pack) or not, appended at out + opos (ocap bytes in all); full is set
   (and nothing more written) if it does not fit. plane: rb bytes of work. Rows in order. */
struct WfkIlbm {
  WFK_RANGE;
  wfk_ptr in;
  wfk_u32 stride, w, depth;
  wfk_ptr out;
  wfk_u32 ocap, opos, full, pack;
  wfk_ptr plane;
};
WFK_CHECK_SIZE(struct WfkIlbm, 48)
WFK_CHECK_OFFSET(struct WfkIlbm, plane, 44)

/* the kernels (in the NativeCall build NC_DECLARE in wfk_nc.h declares them) */
#ifndef WFK_NC
#define WFK__PROTO(n) wfk_u32 n(void *params);
WFK_LIST(WFK__PROTO)
#endif
void wfk_crc_table(wfk_u32 *tab);
/* the deflate output size that always suffices for n bytes of input */
#define WFK_DEFLATE_BOUND(n) ((n) + (n) / 4096 * 8 + 1024)

#ifdef WFK_TEST_BUDGET
extern unsigned long wfk_test_budget;    /* host tests: stop after this many clock checks (0 never) */
#endif

#endif
