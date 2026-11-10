# Utilities

Small utilities for AmigaOS 3.x, collected in one archive (`build/Utilities.lha`), like the
Commodities archive. Each tool has its own drawer in `src/`, its own section below, and is built by
`make`; `make package` builds the archive with icons, this ReadMe, the source, our datatypes and
an Installer script (`Install-Utilities`) that installs whichever of them you choose.

| Tool | What it does | Needs |
|---|---|---|
| WBFit | sets the Workbench backdrop from any picture, scaled to the screen's exact resolution (and pixel aspect) | OS 3.0/3.1+, any 68000; PNG/24-bit backdrops need picture.datatype 43 |
| DataTypes/ | our datatypes, each its own unchanged public package: WebP, PNG + JPEG, AVIF (and PDF once it is built) | picture.datatype 43 (OS 3.5+); AVIF and PDF a 68040/060 |

Private NativeCall builds of tools that have one are never part of this archive (the NativeCall
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
compares every field offset). On AROS (68040, RTG) under FS-UAE: pictures read through datatypes
both ways, every mode and format written at the screen's exact size, our ILBMs read back by
AROS's ilbm.datatype, the prefs written and kept, RESTORE, and the errors.

Not verified here, needs a real Workbench 3.1/3.2: that IPrefs and Workbench pick up the backdrop
at once (AROS's Wanderer does not use WBPattern.prefs), the look of the remapped picture on
AGA/ECS screens, and WATCH across a real ScreenMode change.
