/*
 * pxquant.h - median cut of a 5-bit-a-gun histogram (32768 buckets, index r5 << 10 | g5 << 5 |
 * b5) to at most ncol colours (pal: RGB triples); returns the number of colours. Boxes are
 * split along their longest side (weighted 3:4:2) at the median.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PXQUANT_H
#define PXQUANT_H
#include "pxbase.h"
int wf_mediancut(const wfk_u32 *hist, int ncol, wfk_u8 *pal);
#endif
