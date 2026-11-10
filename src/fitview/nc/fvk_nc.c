/*
 * fvk_nc.c - FitView's kernels as a NativeCall module (NativeCall test build only: built with
 * NATIVECALL=1 and the NativeCall SDK, see src/fitview/nc.mk; nothing of the SDK is in this
 * repository). The same source as the plain program's 68k code (../fvk.c, compiled here with
 * 4-byte pointer fields), plus NEON for the vertical scaling pass (src/lib/pxneon.h, WBFit's).
 * The 68k side of the NativeCall build compiles this file too: there it is the plain C, the
 * fallback.
 *
 * Copyright (C) 2026 the WBFit author.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef WFK_NC
#define WFK_NC
#endif
#define PX_KERNEL_FILE
#include "fvk.h"

#include "pxneon.h"

#include "fvk.c"

NC_MODULE(fitview, FVK_LIST)
