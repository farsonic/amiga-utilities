/*
 * wfprefs.c - Sys/WBPattern.prefs as bytes (wfprefs.h).
 * Copyright (C) 2026 the WBFit author.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>
#include "wfprefs.h"

typedef unsigned char u8;
typedef unsigned long ul;

static ul rd32(const u8 *p) { return (ul)p[0] << 24 | (ul)p[1] << 16 | (ul)p[2] << 8 | p[3]; }
static unsigned rd16(const u8 *p) { return (unsigned)p[0] << 8 | p[1]; }
static void wr32(u8 *p, ul v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static void wr16(u8 *p, unsigned v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

/* the FORM PREF's chunks: calls f for each (id, data, len); 0 if the file is not valid */
typedef int (*ChunkFn)(void *ctx, const u8 *id, const u8 *data, ul len);
static int walk(const u8 *b, ul n, ChunkFn f, void *ctx)
{
  ul flen, pos;
  if (!b || n < 12 || memcmp(b, "FORM", 4) || memcmp(b + 8, "PREF", 4)) return 0;
  flen = rd32(b + 4);
  if (flen < 4 || flen > n - 8) return 0;
  for (pos = 12; pos + 8 <= flen + 8; ) {
    ul len = rd32(b + pos + 4);
    if (len > flen + 8 - pos - 8) return 0;
    if (f && !f(ctx, b + pos, b + pos + 8, len)) return 0;
    pos += 8 + len + (len & 1);
  }
  return 1;
}

typedef struct { u8 *out; ul cap, pos; int bad, rootdone; const char *name; unsigned keep; } Build;
static void put(Build *b, const void *p, ul n)
{
  if (b->bad || n > b->cap - b->pos) { b->bad = 1; return; }
  memcpy(b->out + b->pos, p, n);
  b->pos += n;
}
static void chunk(Build *b, const char *id, const void *data, ul len)
{
  u8 h[8];
  memcpy(h, id, 4); wr32(h + 4, len);
  put(b, h, 8); put(b, data, len);
  if (len & 1) put(b, "", 1);
}
static void root(Build *b)
{
  u8 d[WFP_HDR + WFP_MAXNAME + 1];
  ul nl = strlen(b->name) + 1;             /* the name with its NUL */
  memset(d, 0, WFP_HDR);
  wr16(d + 16, WFP_ROOT);
  wr16(d + 18, b->keep & WFP_KEEP);        /* a picture (no WBPF_PATTERN), tiled */
  d[20] = 0; d[21] = 0;
  wr16(d + 22, (unsigned)nl);
  memcpy(d + WFP_HDR, b->name, nl);
  chunk(b, "PTRN", d, WFP_HDR + nl);
  b->rootdone = 1;
}
static int find_root(void *ctx, const u8 *id, const u8 *data, ul len)
{
  if (!memcmp(id, "PTRN", 4) && len >= WFP_HDR && rd16(data + 16) == WFP_ROOT) *(unsigned *)ctx = rd16(data + 18) | 0x10000;
  return 1;
}
static int copy(void *ctx, const u8 *id, const u8 *data, ul len)
{
  Build *b = (Build *)ctx;
  if (!memcmp(id, "PRHD", 4)) return 1;    /* (ours is written first) */
  if (!memcmp(id, "PTRN", 4) && len >= WFP_HDR && rd16(data + 16) == WFP_ROOT) {
    if (!b->rootdone) root(b);             /* (only one) */
    return 1;
  }
  { char cid[5]; memcpy(cid, id, 4); cid[4] = 0; chunk(b, cid, data, len); }
  return 1;
}

ul wfp_build(const u8 *old, ul oldn, const char *name, u8 *out, ul cap, int *oldok)
{
  static const u8 prhd[6] = { 0, 0, 0, 0, 0, 0 };   /* ph_Version 0, ph_Type 0, ph_Flags 0 */
  Build b;
  unsigned found = 0;
  int ok;
  if (oldok) *oldok = 0;
  if (!name || strlen(name) > WFP_MAXNAME || cap < 12) return 0;
  ok = old && walk(old, oldn, find_root, &found);
  memset(&b, 0, sizeof b);
  b.out = out; b.cap = cap; b.name = name; b.keep = found & 0xFFFF;
  put(&b, "FORM\0\0\0\0PREF", 12);
  chunk(&b, "PRHD", prhd, 6);
  if (ok) {
    /* a ROOT that was a pattern: its dither flags are the pattern's, not kept */
    if ((found & 0x10000) && (found & WFP_PATTERN)) b.keep = 0;
    walk(old, oldn, copy, &b);
  }
  if (!b.rootdone) root(&b);
  if (b.bad) return 0;
  wr32(out + 4, b.pos - 8);
  if (oldok) *oldok = ok;
  return b.pos;
}

typedef struct { int which; char *name; ul cap; unsigned *flags; int found; } Get;
static int get(void *ctx, const u8 *id, const u8 *data, ul len)
{
  Get *g = (Get *)ctx;
  ul dl, k;
  if (memcmp(id, "PTRN", 4) || len < WFP_HDR || (int)rd16(data + 16) != g->which || g->found) return 1;
  g->found = 1;
  if (g->flags) *g->flags = rd16(data + 18);
  dl = rd16(data + 22);
  if (dl > len - WFP_HDR) dl = len - WFP_HDR;
  if (g->name && g->cap) {
    for (k = 0; k < dl && k + 1 < g->cap && data[WFP_HDR + k]; k++) g->name[k] = (char)data[WFP_HDR + k];
    g->name[k] = 0;
    if (rd16(data + 18) & WFP_PATTERN) g->name[0] = 0;
  }
  return 1;
}
int wfp_get(const u8 *buf, ul n, int which, char *name, ul namecap, unsigned *flags)
{
  Get g;
  g.which = which; g.name = name; g.cap = namecap; g.flags = flags; g.found = 0;
  if (name && namecap) name[0] = 0;
  if (!walk(buf, n, get, &g)) return 0;
  return g.found;
}
