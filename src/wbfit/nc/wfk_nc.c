/*
 * wfk_nc.c - WBFit's kernels as a NativeCall module (NativeCall test build only: built with
 * NATIVECALL=1 and the NativeCall SDK, see src/wbfit/nc.mk; nothing of the SDK is in this
 * repository). The same source as the plain program's 68k code (../wfk.c, compiled here with
 * 4-byte pointer fields), plus NEON for the vertical resampling pass, 16 bytes at a time
 * (src/lib/pxneon.h, shared with FitView: the integer arithmetic of the C exactly). The 68k
 * side of the NativeCall build compiles this file too: there it is the plain C, the fallback.
 *
 * Copyright (C) 2026 the WBFit author.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef WFK_NC
#define WFK_NC
#endif
#define PX_KERNEL_FILE
#include "wfk.h"

#include "pxneon.h"                      /* (src/lib: vrow_neon, WFK_VROW) */

#include "wfk.c"

NC_MODULE(wbfit, WFK_LIST)
