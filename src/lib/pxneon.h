/*
 * pxneon.h - the vertical resampling pass's row in NEON (NativeCall kernels only: WBFit's
 * wfk_vpass and FitView's fvk_vpass): 16 bytes at a time with the integer arithmetic of the C
 * exactly (products and sums in 32 bits, then max(0), +8192, >>14, saturated to a byte).
 * Include it once, in a kernel file, before the kernels (it defines WFK_VROW).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PXNEON_H
#define PXNEON_H
#if NC_NATIVE && !defined(WFK_NO_NEON)
#include <arm_neon.h>
static inline int32x4_t px_fin(int32x4_t a)
{
  return vshrq_n_s32(vaddq_s32(vmaxq_s32(a, vdupq_n_s32(0)), vdupq_n_s32(1 << (WFK_WBITS - 1))), WFK_WBITS);
}
/* bytes [0, x) of an output row, x a multiple of 16 (the rest is the C's) */
static wfk_u32 vrow_neon(const wfk_u8 *in, wfk_u32 istride, wfk_u32 n, const wfk_s16 *w, wfk_u8 *o, wfk_u32 nb)
{
  wfk_u32 x, j;
  for (x = 0; x + 16 <= nb; x += 16) {
    int32x4_t a0 = vdupq_n_s32(0), a1 = a0, a2 = a0, a3 = a0;
    const wfk_u8 *q = in + x;
    for (j = 0; j < n; j++, q += istride) {
      uint8x16_t v = vld1q_u8(q);
      int16x8_t lo = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(v))), hi = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(v)));
      int16_t c = w[j];
      a0 = vmlal_n_s16(a0, vget_low_s16(lo), c);
      a1 = vmlal_n_s16(a1, vget_high_s16(lo), c);
      a2 = vmlal_n_s16(a2, vget_low_s16(hi), c);
      a3 = vmlal_n_s16(a3, vget_high_s16(hi), c);
    }
    {
      uint16x8_t l = vcombine_u16(vqmovun_s32(px_fin(a0)), vqmovun_s32(px_fin(a1)));
      uint16x8_t h = vcombine_u16(vqmovun_s32(px_fin(a2)), vqmovun_s32(px_fin(a3)));
      vst1q_u8(o + x, vcombine_u8(vqmovn_u16(l), vqmovn_u16(h)));
    }
  }
  return x;
}
#define WFK_VROW vrow_neon
#endif
#endif
