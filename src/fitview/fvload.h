/*
 * fvload.h - FitView's pictures read through datatypes.library, in a process of their own so the
 * window never waits for a decoder (a photo, a PDF page). The pixels: 32-bit premultiplied ARGB
 * when the datatype gives true colour (picture.datatype 43+, PDTM_READPIXELARRAY) and memory
 * allows; else 8-bit indices with a palette (the class's bitmap and colour registers on OS 3.1's
 * picture.datatype, with HAM and Extra Half-Brite; or a true-colour picture cut to 256 colours
 * when 32 bits don't fit).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef FVLOAD_H
#define FVLOAD_H
#include <exec/types.h>
#include <exec/ports.h>

/* pdf.datatype's own attributes (pdfdt's include/datatypes/pdfclass.h); other datatypes
   ignore them */
#define PDFDTA_Dummy     (TAG_USER + 0x50440000UL)
#define PDFDTA_Page      (PDFDTA_Dummy + 1)     /* ULONG: the page (1 = first), OM_NEW and OM_GET */
#define PDFDTA_FitWidth  (PDFDTA_Dummy + 2)     /* ULONG: drawn to fit this box (0: no limit), OM_NEW */
#define PDFDTA_FitHeight (PDFDTA_Dummy + 3)
#define PDFDTA_PageCount (PDFDTA_Dummy + 4)     /* ULONG: pages in the document, OM_GET */

#define FV_MAXPIXELS (40UL << 20)               /* the source picture, at most */

typedef struct FvPic {
  UBYTE *px; ULONG pxn;                         /* ARGB (premultiplied) or indices */
  UBYTE *pal;                                   /* 1024 bytes of ARGB with indices, else NULL */
  ULONG w, h, stride;
  int alpha;                                    /* transparent pixels */
  ULONG xasp, yasp;                             /* the picture's pixel shape (BitMapHeader) */
  int via;                                      /* 1 true colour, 2 bitmap + colours, 3 cut to 256 colours */
  ULONG depth;                                  /* as the datatype says */
  ULONG pages, page;                            /* a paged document (pdf.datatype): count (0: not one), page */
  ULONG boxw, boxh;                             /* the box it was drawn to fit (0: none) */
  char dtname[32];                              /* the datatype ("PNG", "PDF", ...) */
} FvPic;

enum { FVL_OK, FVL_NOTPIC, FVL_ERR, FVL_QUIT };
typedef struct FvLoadReq {
  struct Message msg;
  /* in */
  int quit;
  char path[512];
  ULONG srctype, unit;                          /* DTST_FILE (path) or DTST_CLIPBOARD (unit) */
  ULONG page, boxw, boxh;                       /* 0: not asked */
  int v40;                                      /* the old way (bitmap + colours) only */
  int keep;                                     /* (cache a neighbouring page: not shown) */
  ULONG gen;
  /* out */
  int result;
  char err[240];
  FvPic pic;
} FvLoadReq;

/* memory for pictures and the kernels (any task); the NativeCall build's from nc_alloc() */
void *fv_alloc(ULONG n);
void fv_free(void *p);
void fv_pic_free(FvPic *p);

/* the loader process: started once; requests go to fvl_port() and come back to their
   reply port */
int fvl_start(void);
struct MsgPort *fvl_port(void);
void fvl_stop(void);
/* the same work in the calling task (no process) */
void fvl_do(FvLoadReq *rq);
#endif
