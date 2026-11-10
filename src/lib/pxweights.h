/*
 * pxweights.h - resampling weights (bicubic, Catmull-Rom; widened by the factor when shrinking,
 * so it averages every source pixel: no aliasing), integer only. For scaled positions
 * [v0, v0 + n) of a src -> dst scale: tab[i] = { first source index, taps }, wts[i * k ...] Q14
 * weights adding up to 1 << 14 exactly. k: wf_taps(). wf_weights returns 1 if this is the
 * identity (each output one source pixel), 0 otherwise, -1 if k is too small or a filter would
 * need more than 1024 taps (more than a 250-fold reduction).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PXWEIGHTS_H
#define PXWEIGHTS_H
#include "pxbase.h"
wfk_u32 wf_taps(wfk_u32 src, wfk_u32 dst);
int wf_weights(wfk_u32 src, wfk_u32 dst, wfk_u32 v0, wfk_u32 n, wfk_u32 *tab, wfk_s16 *wts, wfk_u32 k);
#endif
