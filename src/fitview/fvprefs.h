/*
 * fvprefs.h - FitView's settings as text (ENVARC:FitView.prefs and ENV:FitView.prefs, and the
 * same words as tool types), without AmigaOS:
 *   FIT=FIT|WIDTH|1:1|FILL|ZOOM   ZOOM=<permille>   BG=RRGGBB   CHECKER=YES|NO
 *   DITHER=YES|NO   WINDOW=<left>,<top>,<width>,<height>
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef FVPREFS_H
#define FVPREFS_H

typedef struct {
  int mode;                       /* FV_FIT ... (fvview.h) */
  unsigned long zoom;             /* permille */
  unsigned long bg;               /* 0x00RRGGBB */
  int checker, dither;
  int winset;                     /* WINDOW was given */
  long wx, wy, ww, wh;
} FvPrefs;

void fvp_default(FvPrefs *p);
/* FIT's word to a mode: 0 if it isn't one */
int fvp_mode(const char *s, int *mode);
const char *fvp_modename(int mode);
/* one setting (NAME=VALUE, or the tool type's words): 1 used, 0 not ours, -1 a bad value */
int fvp_set(FvPrefs *p, const char *name, const char *value);
/* a whole file's text (lines; ';' and '#' start comments) */
void fvp_parse(FvPrefs *p, const char *text, unsigned long n);
/* the file's text into out (at least 200 bytes): its length */
unsigned long fvp_format(const FvPrefs *p, char *out, unsigned long cap);
#endif
