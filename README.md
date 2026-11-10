# Utilities

Small utilities for AmigaOS 3.x, collected in one archive (`build/Utilities.lha`), like the
Commodities archive. Each tool has its own drawer in `src/`, its own section below, and is built by
`make`; `make package` builds the archive with icons, this ReadMe and the source.

| Tool | What it does | Needs |
|---|---|---|
| WBFit | sets the Workbench backdrop from any picture, scaled to the screen's exact resolution (and pixel aspect) | OS 3.0+, datatypes; PNG backdrops OS 3.5+ |

Private NativeCall builds of tools that have one are never part of this archive (the NativeCall
SDK has no licence yet); they are built separately with `NATIVECALL=1 NC_SDK=...`.

Licence: GPL-2.0-only (see LICENSE), unless a tool's section says otherwise.
