/*
 * wfprefs.h - Sys/WBPattern.prefs, read and written as bytes (no iffparse.library, so the host
 * tests can check the layout). The file is an IFF FORM PREF: a PRHD chunk (struct PrefHeader,
 * 6 bytes on the 68k: version, type, flags), then a PTRN chunk for each of the three places
 * (struct WBPatternPrefs, 24 bytes, then wbp_DataLength bytes: a pattern, or a picture file's
 * name when WBPF_PATTERN is not set). From the NDK's prefs/prefhdr.h and prefs/wbpattern.h:
 *
 *   PTRN data  0  ULONG wbp_Reserved[4]
 *             16  UWORD wbp_Which       WBP_ROOT 0, WBP_DRAWER 1, WBP_SCREEN 2
 *             18  UWORD wbp_Flags       WBPF_PATTERN 0x0001, WBPF_NOREMAP 0x0010, dither 0x0300,
 *                                       precision 0x0C00, V47 placement 0x3000, alignment 0xC000
 *             20  BYTE  wbp_Revision    0
 *             21  BYTE  wbp_Depth
 *             22  UWORD wbp_DataLength
 *             24  the data
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef WFPREFS_H
#define WFPREFS_H

#define WFP_ROOT    0
#define WFP_DRAWER  1
#define WFP_SCREEN  2
#define WFP_PATTERN 0x0001
#define WFP_KEEP    0x0F00      /* kept from the old WBP_ROOT: dither (0x0300) and precision (0x0C00) */
#define WFP_HDR     24          /* sizeof(struct WBPatternPrefs) */
#define WFP_MAXNAME 255

/* The new file: the old one's chunks kept in their order (the drawer and screen PTRN chunks,
   anything else), PRHD first, the WBP_ROOT chunk replaced by the picture name (or added).
   old may be NULL. Returns the length, or 0 if it does not fit in cap or the name is too long.
   *oldok: 1 if old was a valid FORM PREF (0: it was ignored). */
unsigned long wfp_build(const unsigned char *old, unsigned long oldn, const char *name,
                        unsigned char *out, unsigned long cap, int *oldok);
/* the PTRN chunk for one place: 1 if found (name: its picture name, or "" for a pattern) */
int wfp_get(const unsigned char *buf, unsigned long n, int which, char *name, unsigned long namecap, unsigned *flags);

#endif
