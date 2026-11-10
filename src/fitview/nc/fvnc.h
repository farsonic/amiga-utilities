/*
 * fvnc.h - FitView's NativeCall runner (NativeCall test build only), WBFit's (src/wbfit/nc/wfnc.c)
 * for FitView's kernel set: opened once at the start, each kernel called natively (every call
 * stops after about 0.3 ms and is called again from where it stopped), the 68k code when native
 * calls are not possible or a buffer is not visible to native code; a failed native call makes
 * the runner say so (the band is done again with the 68k code) and native calls stay off.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef FVNC_H
#define FVNC_H
#include "fvrender.h"

void fvnc_runner(FvRun *r, int native);      /* native 0: always the 68k code (BENCH) */
void fvnc_check(const void *p, unsigned long n);   /* a buffer the kernels get: natively visible? */
int fvnc_native(void);
const char *fvnc_state(void);
void fvnc_report(void (*say)(const char *fmt, ...));
void fvnc_close(void);
#endif
