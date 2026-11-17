# Utilities

[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20me%20a%20coffee-farsonic-FFDD00?logo=buymeacoffee&logoColor=black)](https://buymeacoffee.com/farsonic)

Small utilities for AmigaOS 3.x, collected in one archive (`build/Utilities.lha`), like the
Commodities archive. Each tool has its own drawer in `src/`, its own section below, and is built by
`make`; `make package` builds the archive with icons, this ReadMe, the source, our datatypes and
an Installer script (`Install-Utilities`) that installs whichever of them you choose.

| Tool | What it does | Needs |
|---|---|---|
| WBFit | sets the Workbench backdrop from any picture, scaled to the screen's exact resolution (and pixel aspect) | OS 3.0/3.1+, any 68000; PNG/24-bit backdrops need picture.datatype 43 |
| FitView | a MultiView replacement whose pictures always fit the window, re-fitted live as it is resized; PDFs a page at a time, drawn sharp for the window | OS 3.1+, any 68000; true colour needs picture.datatype 43; RTG or AGA/ECS |
| DataTypes/ | our datatypes, each its own unchanged public package: WebP, PNG + JPEG, AVIF, PDF, Markdown (each one included when its package has been built) | the picture ones picture.datatype 43 (OS 3.5+), AVIF and PDF a 68040/060 (PDF with FPU); Markdown (a text datatype) OS 3.0+, any 68000 |

Private NativeCall builds of tools that have one (WBFit, FitView) are never part of this archive (the NativeCall
SDK has no licence yet); they are built separately with `NATIVECALL=1 NC_SDK=...`. The packaging
refuses any binary that contains NativeCall code, and any certificate or key.

`make test` runs every tool's host tests.

Licence: GPL-2.0-only (see LICENSE), unless a tool's section says otherwise.

## WBFit

    WBFit FILE,MODE/K,BG/K,OUT/K,FORMAT/K,PALETTE/K,DITHER/S,RESTORE/S,WATCH/S,QUIET/S,TIMES/S,BENCH/S,V40/S

WBFit makes the Workbench backdrop from any picture the system has a datatype for (ILBM, GIF,
PNG, JPEG, WebP, AVIF, ...), scaled to exactly the size of the Workbench screen, so Workbench never
tiles or crops it:

    WBFit Pics:holiday.jpg                   fill the screen (the default)
    WBFit Pics:logo.png MODE=FIT BG=203040   the whole picture, bars of that colour
    WBFit RESTORE                            back to the backdrop you had before WBFit

From Workbench: drop a picture's icon on WBFit's icon (or make WBFit a picture's default tool).
The icon's tool types are the arguments (`MODE=FIT`, `BG=000000`, `FORMAT=ILBM`, `DITHER`,
`OUT=...`, `FILE=...` for a double-click, `WATCH`, `QUIET`); the ones in brackets are off.

It prints what it did: the picture's size and the screen's, the mode, how it was scaled and
cropped, the screen's pixel aspect, the file written, and the time each stage took.

### Modes

- `FILL` (default): the picture covers the whole screen; what is over is cut off, centred.
- `FIT`: the whole picture, centred; the rest is `BG=RRGGBB`, or the average colour of the
  picture's border when `BG` is not given.
- `STRETCH`: the picture is scaled to the screen's size, its shape ignored.
- `CENTER`: not scaled: cut off or padded (with the `FIT` background), centred.

**Pixel aspect.** The screen's pixel shape comes from the display database: `GetVPModeID()` of the
Workbench screen, then `GetDisplayInfoData(DTAG_DISP)`, its `Resolution` (how many "ticks" a pixel
is wide and high). A picture shown on PAL hires (640x256, Resolution 22:44: pixels twice as tall
as wide) is made half as many rows high, so it looks right. Ratios within 5% of square (RTG modes
report things like 22:23) count as square.

**Scaling** is integer arithmetic only, the same bit for bit on every CPU: two separable passes
(across, then down) with a bicubic (Catmull-Rom) filter. When it shrinks, the filter is widened by
the factor, so every source pixel counts (an average over the area: no aliasing, no moire); when
it enlarges, it is plain bicubic. Weights are 14-bit fixed point that add up exactly to one; it is
not gamma-aware (like most scalers). Only the source rows and output pixels that show are worked
out.

### What it writes, and how Workbench gets it

The fitted picture goes to `SYS:Prefs/Presets/WBFit/backdrop-<W>x<H>.<png|ilbm>` (made if missing),
or `OUT=drawer/` or `OUT=file`. Each run alternates with `backdrop-<W>x<H>-2.<ext>`, so the prefs
always change and Workbench never reads a file while it is being replaced. Files are written to a
temporary name and renamed.

`FORMAT=AUTO` (the default) picks by what the system can show:

- picture.datatype takes true-colour pictures (version 43+: OS 3.5, 3.9, 3.1.4, 3.2, or P96's /
  CyberGraphX's on 3.1; WBFit also counts it when the datatype gave it RGB pixels) **and** the
  screen is deeper than 8 bits (RTG): a 24-bit **PNG** if `DEVS:DataTypes/PNG` exists, else a
  24-bit **ILBM**.
- otherwise (OS 3.0/3.1's picture.datatype 40, or a screen of 8 bits or fewer, AGA/ECS): a
  **palette ILBM** (ByteRun1, BMHD/CMAP/BODY), which every picture.datatype reads.

`FORMAT=PNG`, `ILBM24` or `ILBM` forces one. A 24-bit ILBM is about twice the size of the PNG and
needs no unpacking beyond ByteRun1, so it loads a little faster on slow CPUs; the PNG is smaller and
needs a png.datatype. The PNG is our own encoder (each row filtered with the best of the five PNG
filters, then deflate: Folio's encoder, greedy, made resumable).

**Few colours.** For a palette ILBM, `PALETTE=AUTO` (the default) uses the Workbench screen's own
palette (every pen, read with `GetRGB32`) on screens of 8 bits or fewer, and a median cut of the
picture (up to 2^depth colours, at most 256) otherwise; `PALETTE=WB` or `PICTURE` chooses.
`DITHER` adds Floyd-Steinberg error diffusion (else each pixel takes the nearest colour). With the
screen's palette, every colour of the picture is one Workbench already has, so when Workbench
remaps the picture to its pens (as it always does) it finds exact matches. WBFit does not set
`WBPF_NOREMAP`: pens other programs hold can change, and remapping is what 3.1 does reliably.

**The prefs.** WBFit writes `ENVARC:Sys/WBPattern.prefs` (kept over a reboot) and
`ENV:Sys/WBPattern.prefs`: an IFF `FORM PREF` with a `PRHD` chunk and `PTRN` chunks
(`struct WBPatternPrefs` from the NDK's `prefs/wbpattern.h`, 24 bytes, then the data). The
`WBP_ROOT` (Workbench backdrop) entry becomes a picture (no `WBPF_PATTERN`), its data the picture's
full name with its NUL; the drawer (`WBP_DRAWER`) and screen (`WBP_SCREEN`) entries, and any other
chunk, are kept as they were, and so are the old root entry's dither and precision flags. The first
time, the old ENVARC: file is saved as `ENVARC:Sys/WBPattern.prefs.wbfit-backup` (an empty file if
there was none); `RESTORE` puts it back in both places and removes the backup. ENVARC: is written
through a temporary file and a rename; ENV: is written whole with one `Write()` and `Close()`, as
the Prefs editors do it.

That last part is how 3.x applies it without a reboot: IPrefs watches the files in `ENV:Sys/` with
DOS notification, and when `WBPattern.prefs` changes it reads it again and gives Workbench the new
backdrop (Workbench loads the picture through datatypes and remaps it to its screen). This is how
the WBPattern Prefs editor's "Save" and "Use" work on 3.0 to 3.2.

### WATCH

`WATCH` fits the picture, then stays as a small commodity (it shows in Exchange; Exchange's "Remove"
or starting `WBFit ... WATCH` again stops it). It waits on DOS notification of
`ENV:Sys/ScreenMode.prefs` and `ENV:Sys/Overscan.prefs`, so it takes no CPU while nothing happens.
When either changes, IPrefs reopens Workbench; WBFit waits for the screen to come back (a check a
second, at most 30 seconds), and if its size, depth or pixel shape changed, fits the original
picture again for it, or points the prefs at the file it already made for that screen in this
session.

### Requirements, speed, limits

- OS 3.0 or newer (intuition, graphics, datatypes.library 39); only OS 3.1 (V40) calls in the base
  path. Any 68000; no FPU (integer arithmetic only). PNG and 24-bit backdrops need a
  picture.datatype that takes true colour (see above).
- Old picture.datatype (40): WBFit reads the class's bitmap (`ReadPixelLine8`) and colour registers
  (`PDTA_CRegs`), with HAM6/HAM8 and Extra Half-Brite. Pictures deeper than 8 bits need
  picture.datatype 43. `V40` forces this way (for testing).
- Memory: the picture at 3 bytes a pixel, the screen at 3 (and 1 for a palette), the rows in
  between; a PNG needs about another screenful while it is packed.
- Pictures and screens up to 32767 pixels a side; at most a 250-fold reduction in one go.
- `TIMES` adds a line per stage with its result's CRC; `BENCH` is the same in this build (the
  NativeCall test build compares the native and 68k code).

### How it was tested

`make test-wbfit` (gcc, ASan and UBSan): the scaler, quantiser, ditherer, encoders and prefs
writer on generated pictures only (gradients, fine checks for aliasing, noise, rings). Every job
also runs with the kernels interrupted after 1, 2, 3, 7 and 50 checks and called again (as the
NativeCall build does): the same bytes each time. `tests/wbfit/check.py` then checks the results
against references written independently in Python: the resampler from exact fractions, the
median cut and Floyd-Steinberg, the old datatype's colour lookup and HAM; the PNG files decoded
with Pillow (and their CRCs and zlib stream checked); the ILBM files with a reader of its own; the
prefs files against the NDK layout (`tests/wbfit/ndkcheck.c` also compiles the NDK's header and
compares every field offset). On AROS (m68k, 68040, a 640x480 RTG screen) under FS-UAE:
pictures read through datatypes both ways (the RGB pixel array, and the bitmap with colour
registers for ILBM and GIF), every mode and format written at exactly the screen's size, the two
alternating names, our palette and 24-bit ILBMs read back by AROS's ilbm.datatype to the same
pixels, the prefs written with the drawer and screen entries kept, RESTORE giving back the
original file byte for byte, WATCH (idle through a prefs change, stopped by a second start), the
error messages, and the NativeCall build falling back to the 68k code with the same bytes.

Not verified here, needs a real Workbench 3.1/3.2: that IPrefs and Workbench pick up the backdrop
at once (no IPrefs or Workbench ran in these tests; AROS's Wanderer has prefs of its own), the
look of the remapped picture on AGA/ECS screens, WATCH across a real ScreenMode change, and
picture.datatype 40's own behaviour (the V40 path was run on AROS's class with `V40`).

## FitView

    FitView FILE/M,BASENAME/K,CLIPBOARD/S,CLIPUNIT/K/N,SCREEN/S,PUBSCREEN/K,REQUESTER/S,BOOKMARK/S,
            FONTNAME/K,FONTSIZE/K/N,BACKDROP/S,WINDOW/S,PORTNAME/K,FIT/K,BENCH/S,V40/S,PENS/S

FitView shows any file the system has a datatype for, as MultiView does, but draws pictures
itself so that they always fit the window: OS 3.2's MultiView and picture.datatype have no fit to
window (picture.datatype 45's `PDTM_SCALE` works only between `OM_NEW` and the first layout).
Resize the window and the picture is re-fitted at once with a fast preview, then refined with a
high-quality scaler when the window stops changing. PDFs are shown a page at a time, each page
drawn by pdf.datatype for the window's size, so text stays sharp at any size.

    FitView Pics:holiday.jpg                 fit to the window (the default)
    FitView Pics:#?.jpg FIT=WIDTH            several files: N and B (or the View menu) go through them
    FitView Docs:manual.pdf                  a page at a time: Space, Backspace, Home, End
    FitView Docs:ReadMe                      text, AmigaGuide, sound, ...: the datatype's own gadget

From Workbench: double-click its icon (a file requester), drop files on its icon or on its window
(an AppWindow), or make it a project's default tool; several icons selected together are a list
to go through. The icon's tool types are the arguments (`FIT=WIDTH`, `BG=203040`,
`CHECKER=NO`, `DITHER=NO`, `PUBSCREEN=...`, `FONTNAME=...`, `FONTSIZE=...`, `SCREEN`, `BACKDROP`).

### Zoom modes and keys

| Mode | | Key |
|---|---|---|
| Fit to window | the whole picture, its shape kept, centred on the background colour (the default) | `0` |
| Fit width | as wide as the window; scroll down | `W` |
| 1:1 | one picture pixel to one screen pixel | `1` |
| Zoom in / out | in steps (25% ... 1600%), about the middle of the window | `+` `-` |
| Fill | covers the window; what is over can be scrolled to | `F` |

The mode is remembered: in `ENV:FitView.prefs` and `ENVARC:FitView.prefs` when FitView quits,
and "Settings / Save as default" also keeps the window's place and size there. `FIT=` and the
tool type override it.

Bigger than the window, the picture scrolls: the proportional scrollers and arrows in the
window's border, the cursor keys (with Shift: a page), Page Up/Down, Home/End, the mouse wheel,
and dragging the picture with the left mouse button. Space and Backspace go a page on in a
document, else a screenful down a tall picture, else to the next file; `N`/`B` next and previous
file (with one file given: the files of its drawer); `Esc`/`Q` quit; Help: About.
The window's title says the file, the picture's size, the zoom (or "fit", "width" for a
document) and the page (`page 3/12`).

Menus: Project (Open, Save As IFF through the datatype's `DTM_WRITE`, Print with `DTM_PRINT`,
About, Quit), Edit (Copy with `DTM_COPY`), View (the modes, zoom, next/previous/first/last page,
next/previous file, Play for sounds and animations), Settings (dithering, the checkerboard under
transparent pictures, Benchmark, Save as default). For a picture, Save, Print and Copy make the
datatype object again (FitView does not keep it: its pixels are FitView's own).

An ARexx port (`FITVIEW.1`, or `PORTNAME`) takes `OPEN file`, `QUIT`, `NEXTPAGE`, `PREVPAGE`,
`FIRSTPAGE`, `LASTPAGE`, `NEXTFILE`, `PREVFILE`, `FIT`, `FITWIDTH`, `ONETOONE`, `FILL`,
`ZOOMIN`, `ZOOMOUT`, `CHANGEWINDOW left top width height`, `WINDOWTOFRONT`, `SCREENTOFRONT`,
`ACTIVATE` and `BENCH` (return code 0, or 10 for an unknown command; no result strings).

### How pictures are drawn

- **Reading** is WBFit's, in a process of its own so the window never waits for a decoder: the
  datatype is asked for the whole picture as ARGB (`PDTM_READPIXELARRAY`, picture.datatype
  43+), so alpha comes too; if that gives nothing (OS 3.1's picture.datatype 40) the class's
  bitmap is read with its colour registers, HAM6/HAM8 and Extra Half-Brite by the ModeID. FitView
  keeps the pixels: 32 bits a pixel when memory allows, else 8-bit indices with a palette (an old
  datatype's picture as it is; a true-colour picture cut to 256 colours by a median cut and
  Floyd-Steinberg, read in bands twice). Pictures over 40 million pixels are refused.
- **Scaling**: the preview is nearest neighbour; the refined picture is WBFit's integer bicubic
  scaler (Catmull-Rom, widened by the factor when shrinking, so every source pixel counts: no
  aliasing), for four channels. Only the window's part of the scaled picture is ever worked out,
  a band of rows at a time from the event loop (about 20 ms of work each), so FitView answers
  the window, menus and keys all the time; a resize or scroll starts again from the preview.
  The window is "still" after 200 ms without a change (timer.device: IntuiTicks reach only the
  active window). The pixel shape of the screen (and of old ILBMs) is taken into account.
- **On RTG screens** (more than 8 bits, cybergraphics.library present) the window's picture goes
  out with `WritePixelArray()` as ARGB. **On screens of 8 bits or fewer** (AGA, ECS, 8-bit RTG)
  FitView takes a cube of pens with `ObtainBestPen()` (216 colours on 8 bits, 64 on 6, 27 on 5,
  8 below; shared pens, released on exit), dithers to them (Floyd-Steinberg for the refined
  picture, nearest colour for the preview) and draws with `WriteChunkyPixels()` on graphics 45+,
  else `WritePixelArray8()` (on V40 its emulation of WriteChunkyPixels is slower). `PENS` uses the
  pens on an RTG screen too.
- **No flicker**: the whole window's picture, background and checkerboard included, is made in
  memory and goes to the window a band of whole rows at a time; nothing is cleared first.
- **Alpha**: pictures with an alpha channel are drawn over a grey checkerboard (or the background
  colour, `CHECKER=NO`); the colour is premultiplied, so the scaler leaves no fringes.

### Pages (PDF)

pdf.datatype 1.1 (in DataTypes/PDF) has attributes for this (its `include/datatypes/pdfclass.h`):
FitView asks for one page (`PDFDTA_Page`) drawn to fit a box (`PDFDTA_FitWidth`/`Height`: the
window's inside for Fit, its width for Fit width, the covering size for Fill) and reads the
number of pages (`PDFDTA_PageCount`). Other datatypes ignore these tags, so FitView passes them
for every picture. When the window settles at a new size the page is drawn again for it
(meanwhile the old drawing is shown scaled); the next and previous pages are drawn in the
background and kept, so turning a page is immediate. In 1:1 and zoom the page is drawn at the
datatype's own size (`ENV:pdfdt/size`) and scaled. With an older pdf.datatype (no
`PDFDTA_PageCount`) FitView shows its stacked pages in Fit width.

### Everything else

Text, AmigaGuide, sounds, animations and every other group are the datatype's own gadget in the
window, as MultiView shows them (`AddDTObject`, the datatype's own layout on resize, the
scrollers following `DTA_TopVert`/`DTA_TotalVert`/... and moving it back, the busy pointer
during `DTA_Busy`). `FONTNAME`/`FONTSIZE` set the text font (`DTA_TextAttr`), `BASENAME` a
datatype by name, `CLIPBOARD`/`CLIPUNIT` read the clipboard.

### What differs from MultiView

- Pictures are FitView's own drawing, not picture.datatype's gadget: scaling, dithering and
  colours are FitView's; picture.datatype's own remapping preferences are not used.
- No "Mark" / selection of a picture region, no "Screen" mode menu, no "Display Mode" choice;
  `SCREEN` opens a screen like Workbench's (same mode and depth) with a borderless window;
  `BOOKMARK` and `WINDOW` are accepted and ignored.
- The ARexx port takes FitView's own commands (above), not MultiView's set, and gives no
  result strings.
- Save As saves IFF through the datatype only; Print uses the datatype's own printing.

### Requirements, speed, limits

- OS 3.1 (V40: intuition, graphics, datatypes, gadtools); any 68000 (the plain build is 68000
  code; the multiplies of the scaler are 16 x 16 bits, one `muls.w`, so a 68020+ loses nothing).
  True-colour pictures need a picture.datatype that gives them (43+); PDFs need pdf.datatype
  (a 68040/060 with FPU).
- Memory: the picture at 4 bytes a pixel (1 with a palette), the window at 4 (plus 1 on screens
  of 8 bits or fewer), the scaler's rows in between (about 256 KB), a few pages of a document.
- `BENCH` (or Settings / Benchmark) times the preview and the refined picture of the window
  once (the NativeCall test build compares its native and 68k code).

### How it was tested

`make test-fitview` (gcc, ASan and UBSan): the geometry of every zoom mode (3040 cases, also
recomputed by `tests/fitview/check.py` from exact fractions), placement, scroll ranges, keeping
the middle on zoom and resize, the zoom steps and the scroller values; rendered windows of
generated pictures (gradients, the finest checks, noise, alpha; 32-bit and palette; shrinking,
enlarging, one axis each, centred and scrolled, very wide rows done in column chunks), each
rendered with many band sizes and with the kernels stopped after 1, 2, 3, 7 and 50 clock checks
(the same bytes every time) and checked by `check.py` against a reference written from the
description (preview, refined picture, background, alpha); the refined scaler against WBFit's
own kernels with the same weights (the colour channels byte for byte the same); the ditherer to
pens and the alpha composite. `make fvnctest NATIVECALL=1 ...`: every kernel of the NativeCall
module under qemu-aarch64_be against the 68k code's C, and whole windows at real sizes.

On AROS (m68k, 68040, a 640x480 RTG screen) under FS-UAE, steered through the ARexx port:
a photo fitted to the window, re-fitted after the window was resized (`CHANGEWINDOW`), 1:1 and
zoom with working scrollers, a transparent PNG over the checkerboard, the pens path (`PENS`)
dithered with Floyd-Steinberg, a palette ILBM through the old bitmap path (`V40`), a text file
through the datatype's gadget with its scrollers, PDFs a page at a time (pages turned with
`NEXTPAGE`/`LASTPAGE`, a page drawn again sharp for a new window size, the old drawing scaled
meanwhile), the NativeCall test build falling back to the 68k code with BENCH's native and 68k
pixels the same, and the error requester for a missing file.

Not verified here, needs a real Amiga: OS 3.1/3.2's own picture.datatype and Intuition (border
scrollers' look, menus, AppWindow drops from Workbench, `ObtainBestPen` on an AGA/ECS screen and
`WritePixelArray8` there, `SCREEN`/`BACKDROP`, printing, Save As and Copy), and the NativeCall
build's speed on an Emu68 Pi.
