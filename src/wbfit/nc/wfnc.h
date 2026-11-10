/*
 * wfnc.h - WBFit's NativeCall runner (NativeCall test build only): the kernel set opened once,
 * memory native code can see, each kernel called natively (every call stops after about
 * 0.3 ms and is called again from where it stopped), the 68k code when native calls are not
 * possible; a failed native call makes the runner say so (the stage is redone with the 68k code)
 * and native calls stay off.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef WFNC_H
#define WFNC_H
#include "wfcore.h"

void wfnc_runner(WfRun *r, int native);      /* native 0: always the 68k code (BENCH) */
int wfnc_native(void);                       /* native calls are running */
const char *wfnc_state(void);                /* "native (...)" or "68k (why)" */
void wfnc_report(void (*say)(const char *fmt, ...));   /* BENCH: each kernel's native calls (the hypervisor's times) */
void wfnc_close(void);
#endif
