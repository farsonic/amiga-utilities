# CLAUDE.md - working on Amiga Utilities

A running collection of small AmigaOS 3.x tools, built and shipped together as one archive,
`Utilities.lha`, with an Installer script that lets the user choose what to install. GPL-2.0-only (see
`LICENSE`). This file is for anyone, person or agent, picking the work up. Read it first, then
`README.md` (each tool's section).

## What is here

| Tool | Path | What it does | Needs |
|---|---|---|---|
| **WBFit** | `src/wbfit/` | Sets the Workbench backdrop from any picture (through datatypes), scaled to the screen's exact size and pixel aspect; FILL/FIT/STRETCH/CENTER; palette ILBM with dithering for OS 3.1/AGA, 24-bit PNG or ILBM where the system can show it; writes WBPattern prefs, `RESTORE`, `WATCH` (refits after a screen-mode change). | OS 3.0+, any CPU |
| **FitView** | `src/fitview/` | A MultiView replacement: pictures always fit the window and re-fit live on resize (fast preview, then a sharp bicubic refine in bands); zoom modes, scrolling, alpha, RTG and pen-dithered AGA output; PDF pages rendered at the window size through pdf.datatype 1.1's attributes; text, AmigaGuide, sound and animation as datatype gadgets like MultiView; ARexx port, AppWindow. | OS 3.1+, any CPU |
| shared | `src/lib/` | The scaling weights (`pxweights.c`), median cut and palettes (`pxquant.c`), and the kernel base used by both tools (`pxbase.h`, `pxneon.h`). | |

The archive also bundles our datatypes, unpacked unchanged from their own packages into `DataTypes/`:
webp.datatype, png/jpeg.datatype, avif.datatype, pdf.datatype and markdown.datatype. Their sources are
separate repositories (`webpdt`, `pjdt`, `avifdt`, `pdfdt`, `mddt`); `tools/mkpackage.sh` picks up their
built `.lha` packages from sibling directories (see the script's top for the paths) and skips any that
are missing.

## Build and test

Host tests (need gcc and Python 3 with Pillow and numpy; CI runs them and they must stay green):

    make test

Expected: `wbfit_test: ... 0 failed`, `fitview_test: ... 0 failed` and the `check.py` lines with 0 failed.
They check the scalers, dithering, quantising, PNG and ILBM writers, prefs files and FitView's geometry
and rendering against independent Python references, under ASan/UBSan, with the kernels interrupted
and resumed at many points. The NDK structure check (`tests/wbfit/ndkcheck.c`) runs only where the NDK
headers are installed.

Amiga binaries: `make` (vbcc; `build/WBFit`, `build/FitView`). It needs **vbcc** 0.9h with the
m68k-amigaos target in `/opt/amiga/vbcc` (http://www.compilers.de/vbcc.html) and the **NDK 3.2**
headers in `/opt/amiga/ndk/Include_H` (Aminet `dev/misc/NDK3.2.lha`), or set `VBCC` and `NDK`.

Package: `make package` builds `build/Utilities.lha`: the tools with icons (`tools/mkicon.py`), README,
Source/, the DataTypes bundle and `Install-Utilities`, which `tools/mkinstaller.sh` generates (one
selectable option per tool and per datatype, with OS and CPU checks). `tools/srcfix.py` makes every
Installer script find its files through `@icon`, because Workbench may start Installer in another
directory. The package script refuses to finish if any file outside Source/ contains NativeCall code or
any file holds certificate or key material.

Installer scripts must run on Commodore's Installer 43, InstallerLG and AROS's Installer: no
`procedure`, no `select` for values (AROS returns the index), no `(and ...)` in conditions (nest `if`s),
`(IN #mask n)` for askoptions bits, and every relative source through `(tackon #src "...")`.

Adding a tool: give it `src/<tool>/tool.mk` (it adds itself to `TOOLS` and `TESTS` and has the rule for
`build/<Tool>`), a `pkg.sh` hook for its icon, host tests under `tests/<tool>/`, a row in the README's
table and its own README section; `mkinstaller.sh` then offers it.

## NativeCall builds (private)

WBFit and FitView also have NativeCall builds (`src/*/nc/`, `src/*/nc.mk`) that run their pixel kernels
as AArch64 code on a Raspberry Pi under Emu68, bit-identical with the 68k code. The code in this
repository is ours (GPL-2.0-only); it needs the NativeCall SDK to build (`make nc NATIVECALL=1
NC_SDK=<sdk>`), and that SDK has **no licence yet**: never commit any SDK file, and never publish a
binary built with it (the `ReadMe.private` files say the same). The normal build doesn't use it.

## Rules of the project

- vbcc C99; the plain builds are 68000-compatible with integer maths only and use only OS 3.0/3.1 calls
  in their base paths, checking for newer features at run time.
- Native kernels and the 68k code must give the same bytes; the tests check it.
- Don't use `rm -rf` in scripts or by hand on shared machines; move things aside.
- Never put a personal email address in commits, code or docs.

## Where work stands

Both tools are done and tested on the host and in an AROS emulator (68040, RTG). Still to be verified
on a real Workbench 3.1/3.2 (listed in the README): whether IPrefs applies a new backdrop at once,
WBFit's `WATCH` across a real screen-mode change, FitView's menus, Print, Save As and Copy, pens on
AGA/ECS screens, and the Installer script on Commodore's Installer.

## Issues

Bugs and requests come in as GitHub issues (templates in `.github/ISSUE_TEMPLATE/`). To work on them:

1. `gh issue list --repo farsonic/amiga-utilities` and `gh issue view N --repo farsonic/amiga-utilities --comments`.
2. Reproduce first. Scaling, dithering, file writing and geometry bugs can be reproduced on the host
   (`tests/wbfit/`, `tests/fitview/`); write a failing test, then fix it. Bugs in a bundled datatype
   belong in that datatype's own repository. Interface bugs need the Amiga build; say what you could and
   couldn't verify.
3. Fix on a branch named `issue-N-short-name`, keep `make test` green, and open a pull request whose
   description says `Fixes #N`, what changed and how it was tested. Ask on the issue when information is
   missing rather than guessing. Don't close issues by hand unless asked; merging the PR closes them.
4. Attached pictures may be private: never commit them; make a small test picture of your own.

`.github/workflows/claude.yml` lets people mention `@claude` in an issue or pull request to have Claude
Code look at it (it needs the `ANTHROPIC_API_KEY` repository secret or the Claude GitHub App).
