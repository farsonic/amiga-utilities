/*
 * fvview.h - FitView's view geometry, without AmigaOS: how big the scaled picture is in each
 * zoom mode, where it goes in the window, how far it scrolls, and the zoom steps.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef FVVIEW_H
#define FVVIEW_H
#include "pxbase.h"
typedef wfk_u32 fv_u32;
typedef wfk_s32 fv_s32;

#define FV_MAXDIM 32767              /* pictures and scaled pictures, either side */

enum { FV_FIT, FV_WIDTH, FV_ONE, FV_ZOOM, FV_FILL, FV_NMODES };

typedef struct {
  /* in */
  fv_u32 sw, sh;                     /* the picture */
  fv_u32 anum, aden;                 /* its height at 100% is sh * anum / aden (pixel aspect; 1, 1: square) */
  fv_u32 W, H;                       /* the window's inside */
  int mode;
  fv_u32 zoom;                       /* FV_ZOOM: permille of 100% */
  int snap;                          /* within a pixel of 100% counts as 100% (a page drawn for this size) */
  /* out */
  fv_u32 h100;                       /* the picture's height at 100% */
  fv_u32 dw, dh;                     /* the scaled picture */
  fv_u32 maxx, maxy;                 /* it scrolls 0..max each way */
  fv_u32 pm;                         /* the zoom it comes to, permille (of the width) */
} FvGeom;

/* the scaled size and the scroll range; 0, or -1 for a bad picture or window */
int fv_geom(FvGeom *g);
/* where the picture's top left goes in the window for scroll position (sx, sy) (clamped):
   centred when it is smaller than the window */
void fv_place(const FvGeom *g, fv_u32 sx, fv_u32 sy, fv_s32 *px, fv_s32 *py);
/* the scroll position that keeps the point at the middle of the window where it was, after the
   geometry changed from o to n (zoom, window size) */
void fv_rescroll(const FvGeom *o, const FvGeom *n, fv_u32 *sx, fv_u32 *sy);
/* the next zoom step above (dir > 0) or below (dir < 0) permille cur */
fv_u32 fv_zoom_step(fv_u32 cur, int dir);
/* the pixel aspect correction for a picture (pixel width:height px:py, 0 unknown) on a screen
   whose pixels are rx:ry ticks wide:high; square when within 5% */
void fv_aspect(fv_u32 px, fv_u32 py, fv_u32 rx, fv_u32 ry, fv_u32 *anum, fv_u32 *aden);
/* a proportional gadget's values for a scroll range: total, visible, top as 16-bit-safe units */
void fv_prop(fv_u32 total, fv_u32 visible, fv_u32 top, fv_u32 *ptotal, fv_u32 *pvis, fv_u32 *ptop);
/* and back: the scroll position for a gadget top */
fv_u32 fv_unprop(fv_u32 total, fv_u32 visible, fv_u32 ptop);
#endif
