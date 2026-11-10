/*
 * fvprefs.c - FitView's settings as text (fvprefs.h).
 * Copyright (C) 2026 the WBFit author.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <string.h>
#include "fvprefs.h"
#include "fvview.h"

void fvp_default(FvPrefs *p)
{
  memset(p, 0, sizeof *p);
  p->mode = FV_FIT; p->zoom = 1000; p->bg = 0x000000; p->checker = 1; p->dither = 1;
}

static int up(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int eq(const char *a, const char *b)
{
  while (*a && *b && up((unsigned char)*a) == up((unsigned char)*b)) a++, b++;
  return !*a && !*b;
}

static const char *const names[FV_NMODES] = { "FIT", "WIDTH", "1:1", "ZOOM", "FILL" };
int fvp_mode(const char *s, int *mode)
{
  int i;
  for (i = 0; i < FV_NMODES; i++) if (eq(s, names[i])) { *mode = i; return 1; }
  if (eq(s, "ONE") || eq(s, "100%") || eq(s, "1")) { *mode = FV_ONE; return 1; }
  if (eq(s, "FITWIDTH")) { *mode = FV_WIDTH; return 1; }
  return 0;
}
const char *fvp_modename(int mode) { return mode >= 0 && mode < FV_NMODES ? names[mode] : "FIT"; }

static int hexval(const char *s, unsigned long *v)
{
  unsigned long r = 0;
  int n = 0;
  if (*s == '#') s++;
  if (s[0] == '$') s++;
  else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  for (; *s; s++, n++) {
    int c = up((unsigned char)*s);
    if (c >= '0' && c <= '9') r = r * 16 + (unsigned long)(c - '0');
    else if (c >= 'A' && c <= 'F') r = r * 16 + (unsigned long)(c - 'A' + 10);
    else return 0;
  }
  if (n != 6) return 0;
  *v = r;
  return 1;
}
static int decval(const char **s, long *v)
{
  long r = 0;
  int neg = 0, n = 0;
  while (**s == ' ') (*s)++;
  if (**s == '-') { neg = 1; (*s)++; }
  while (**s >= '0' && **s <= '9' && n < 9) { r = r * 10 + (**s - '0'); (*s)++; n++; }
  if (!n) return 0;
  *v = neg ? -r : r;
  return 1;
}
static int yesno(const char *s, int *v)
{
  if (!*s || eq(s, "YES") || eq(s, "ON") || eq(s, "1") || eq(s, "TRUE")) { *v = 1; return 1; }
  if (eq(s, "NO") || eq(s, "OFF") || eq(s, "0") || eq(s, "FALSE")) { *v = 0; return 1; }
  return 0;
}

int fvp_set(FvPrefs *p, const char *name, const char *value)
{
  if (eq(name, "FIT")) return fvp_mode(value, &p->mode) ? 1 : -1;
  if (eq(name, "ZOOM")) {
    long v;
    const char *s = value;
    if (!decval(&s, &v) || *s || v < 10 || v > 32000) return -1;
    p->zoom = (unsigned long)v;
    return 1;
  }
  if (eq(name, "BG")) return hexval(value, &p->bg) ? 1 : -1;
  if (eq(name, "CHECKER")) return yesno(value, &p->checker) ? 1 : -1;
  if (eq(name, "DITHER")) return yesno(value, &p->dither) ? 1 : -1;
  if (eq(name, "WINDOW")) {
    long v[4];
    const char *s = value;
    int i;
    for (i = 0; i < 4; i++) {
      if (!decval(&s, &v[i])) return -1;
      if (i < 3) { if (*s != ',') return -1; s++; }
    }
    if (*s || v[0] < 0 || v[1] < 0 || v[2] < 40 || v[3] < 30 || v[2] > 32767 || v[3] > 32767) return -1;
    p->wx = v[0]; p->wy = v[1]; p->ww = v[2]; p->wh = v[3]; p->winset = 1;
    return 1;
  }
  return 0;
}

void fvp_parse(FvPrefs *p, const char *t, unsigned long n)
{
  unsigned long i = 0;
  while (i < n) {
    char line[200], *eqp, *e;
    unsigned long k = 0;
    while (i < n && t[i] != '\n' && t[i] != '\r') { if (k < sizeof line - 1) line[k++] = t[i]; i++; }
    while (i < n && (t[i] == '\n' || t[i] == '\r')) i++;
    line[k] = 0;
    if ((e = strchr(line, ';'))) *e = 0;
    if ((e = strchr(line, '#')) && (e == line || e[-1] != '=')) *e = 0;   /* (BG=#RRGGBB is a value) */
    for (e = line + strlen(line); e > line && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
    e = line;
    while (*e == ' ' || *e == '\t') e++;
    if (!*e) continue;
    if ((eqp = strchr(e, '='))) { *eqp = 0; fvp_set(p, e, eqp + 1); }
    else fvp_set(p, e, "");
  }
}

unsigned long fvp_format(const FvPrefs *p, char *out, unsigned long cap)
{
  int n = snprintf(out, cap, "; FitView settings\nFIT=%s\nZOOM=%lu\nBG=%06lX\nCHECKER=%s\nDITHER=%s\n",
                   fvp_modename(p->mode), p->zoom, p->bg & 0xFFFFFFUL, p->checker ? "YES" : "NO", p->dither ? "YES" : "NO");
  if (n < 0) return 0;
  if (p->winset && (unsigned long)n < cap) {
    int m = snprintf(out + n, cap - (unsigned long)n, "WINDOW=%ld,%ld,%ld,%ld\n", p->wx, p->wy, p->ww, p->wh);
    if (m > 0) n += m;
  }
  return (unsigned long)n < cap ? (unsigned long)n : cap - 1;
}
