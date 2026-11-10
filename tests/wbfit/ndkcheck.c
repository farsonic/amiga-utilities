/*
 * ndkcheck.c - wfprefs.h's hand-written layout of Sys/WBPattern.prefs against the NDK's
 * prefs/wbpattern.h (built -m32 with the NDK's headers). The 68k sizes of struct WBPatternPrefs
 * (24) and struct PrefHeader (6, packed to 2 on the 68k: 8 here) are also checked at compile
 * time in wbfit.c, by vbcc.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stddef.h>
#include <exec/types.h>
#include <prefs/prefhdr.h>
#include <prefs/wbpattern.h>
#include "../../src/wbfit/wfprefs.h"

static int fails;
static void eq(long a, long b, const char *what)
{
  if (a != b) { printf("FAIL %s: %ld, expected %ld\n", what, a, b); fails++; }
}
int main(void)
{
  eq(sizeof(struct WBPatternPrefs), WFP_HDR, "sizeof WBPatternPrefs");
  eq(offsetof(struct WBPatternPrefs, wbp_Reserved), 0, "wbp_Reserved");
  eq(offsetof(struct WBPatternPrefs, wbp_Which), 16, "wbp_Which");
  eq(offsetof(struct WBPatternPrefs, wbp_Flags), 18, "wbp_Flags");
  eq(offsetof(struct WBPatternPrefs, wbp_Revision), 20, "wbp_Revision");
  eq(offsetof(struct WBPatternPrefs, wbp_Depth), 21, "wbp_Depth");
  eq(offsetof(struct WBPatternPrefs, wbp_DataLength), 22, "wbp_DataLength");
  eq(WBP_ROOT, WFP_ROOT, "WBP_ROOT"); eq(WBP_DRAWER, WFP_DRAWER, "WBP_DRAWER"); eq(WBP_SCREEN, WFP_SCREEN, "WBP_SCREEN");
  eq(WBPF_PATTERN, WFP_PATTERN, "WBPF_PATTERN");
  eq(WBPF_DITHER_MASK | WBPF_PRECISION_MASK, WFP_KEEP, "kept flags");
  eq(offsetof(struct PrefHeader, ph_Version), 0, "ph_Version"); eq(offsetof(struct PrefHeader, ph_Type), 1, "ph_Type");
  eq(ID_PTRN, 0x5054524EL, "ID_PTRN"); eq(ID_PRHD, 0x50524844L, "ID_PRHD"); eq(ID_PREF, 0x50524546L, "ID_PREF");
  printf("ndkcheck: WBPattern.prefs layout %s\n", fails ? "WRONG" : "matches the NDK");
  return fails != 0;
}
