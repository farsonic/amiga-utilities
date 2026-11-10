/*
 * fvk.h - FitView's kernels: every loop over the pixels of the picture or the window.
 *
 *   fvk_hpass   high-quality scale, across: source rows [from, to) into intermediate rows
 *   fvk_vpass   high-quality scale, down: output rows [from, to) from the intermediate rows
 *   fvk_nn      the fast preview: nearest neighbour, output rows [from, to)
 *   fvk_fill    rows [from, to), pixels [x0, x1) of the window set to one colour
 *   fvk_comp    pictures with alpha: rows over a checkerboard (or one colour), in place
 *   fvk_dither  rows of the window to pens: nearest colour, or Floyd-Steinberg
 *
 * Pixels are 4 bytes, A R G B in memory order (PBPAFMT_ARGB, RECTFMT_ARGB), colour
 * premultiplied by alpha. A source may instead be 1 byte a pixel with a palette of 256 ARGB
 * entries (the palette version of a picture: old picture.datatype, or too little memory for
 * 32 bits). The scaler is WBFit's (src/wbfit/wfk.c), four channels instead of three: the same
 * weights (src/lib/pxweights.c), the same Q14 sums and rounding, so its colour channels are
 * byte for byte what WBFit's kernels give (tests/fitview checks it). The ditherer is WBFit's
 * with a pen map. One source (fvk.c) is the 68k program's code, the host tests' and, with
 * WFK_NC, the NativeCall kernels (nc/fvk_nc.c); conventions as src/lib/pxbase.h (4-byte
 * parameter fields, the 0.3 ms clock, bounded loops, integer arithmetic only).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef FVK_H
#define FVK_H
#include "pxbase.h"

#define FVK_LIST(K) \
  K(fvk_hpass)  \
  K(fvk_vpass)  \
  K(fvk_nn)     \
  K(fvk_fill)   \
  K(fvk_comp)   \
  K(fvk_dither)

#define FVK__ENUM(n) FVKI_##n,
enum { FVK_LIST(FVK__ENUM) FVK_COUNT };

/* ---- hpass: source rows [from, to) (absolute), each from src + y * sstride (4 bytes a pixel,
   or 1 with pal: wfk_u8 [256][4] ARGB), into out + (y - y0) * ostride, ow pixels (4 bytes).
   tab: wfk_u32 [ow][2] (first source column, taps), wts: wfk_s16 [ow][k] (Q14). ident: every
   output pixel is one source pixel (no scaling across): a copy. */
struct FvkH {
  WFK_RANGE;
  wfk_ptr src;
  wfk_u32 sstride;
  wfk_ptr pal;
  wfk_ptr out;
  wfk_u32 ostride, ow, y0;
  wfk_ptr tab, wts;
  wfk_u32 k, ident, pad;
};
WFK_CHECK_SIZE(struct FvkH, 56)
WFK_CHECK_OFFSET(struct FvkH, ident, 48)

/* ---- vpass: output rows [from, to), each nb bytes from the intermediate rows: row r's taps
   start at intermediate row tab[r * 2] - base (in + that * istride), tab[r * 2 + 1] of them,
   weights wts + r * k; into out + r * ostride. Every byte on its own (any channels). */
struct FvkV {
  WFK_RANGE;
  wfk_ptr in;
  wfk_u32 istride;
  wfk_ptr out;
  wfk_u32 ostride, nb;
  wfk_ptr tab, wts;
  wfk_u32 k, ident, base;
};
WFK_CHECK_SIZE(struct FvkV, 48)
WFK_CHECK_OFFSET(struct FvkV, base, 44)

/* ---- nn: output rows [from, to): row r is source row yt[r], pixel x of it source byte offset
   xt[x] (4 * column, or the column with pal); ow pixels into out + r * ostride */
struct FvkNN {
  WFK_RANGE;
  wfk_ptr src;
  wfk_u32 sstride;
  wfk_ptr pal;
  wfk_ptr out;
  wfk_u32 ostride, ow;
  wfk_ptr xt, yt;
};
WFK_CHECK_SIZE(struct FvkNN, 40)

/* ---- fill: rows [from, to) of out (ostride a row), pixels [x0, x1) set to argb (0xAARRGGBB) */
struct FvkFill {
  WFK_RANGE;
  wfk_ptr out;
  wfk_u32 ostride, x0, x1, argb, pad;
};
WFK_CHECK_SIZE(struct FvkFill, 32)

/* ---- comp: rows [from, to) of out, pixels [x0, x1): premultiplied A R G B over the background
   (c + (255 - a) * bg / 255, rounded, at most 255), alpha set to 255. The background: c0 alone
   (shift 0), or a checkerboard of c0 and c1 (0x00RRGGBB) in squares of 1 << shift pixels whose
   origin is window pixel (orgx, orgy) */
struct FvkComp {
  WFK_RANGE;
  wfk_ptr out;
  wfk_u32 ostride, x0, x1, c0, c1, shift;
  wfk_s32 orgx, orgy;
  wfk_u32 pad;
};
WFK_CHECK_SIZE(struct FvkComp, 48)

/* ---- dither: rows [from, to) of px (ARGB, pstride a row, w pixels) to pens at out + y * ostride.
   pal: wfk_u8 [256][3], npal colours (the pens' colours); penmap: wfk_u8 [256], the pen of each;
   cache: wfk_u16 [32768] (cleared to 0xFF bytes by the caller): the nearest colour of each 5-bit
   bucket. fs: Floyd-Steinberg with the errors in e0 / e1 (wfk_s32 [(w + 2) * 3] each, cleared
   by the caller); rows must then come in order from row 0. */
struct FvkDither {
  WFK_RANGE;
  wfk_ptr px;
  wfk_u32 pstride, w;
  wfk_ptr out;
  wfk_u32 ostride;
  wfk_ptr pal;
  wfk_u32 npal;
  wfk_ptr cache, penmap;
  wfk_u32 fs;
  wfk_ptr e0, e1;
  wfk_u32 pad;
};
WFK_CHECK_SIZE(struct FvkDither, 60)
WFK_CHECK_OFFSET(struct FvkDither, e1, 52)

#ifdef WFK_NC
NC_DECLARE(fitview, FVK_LIST)
#else
#define FVK__PROTO(n) wfk_u32 n(void *params);
FVK_LIST(FVK__PROTO)
#endif

#endif
