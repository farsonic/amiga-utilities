#!/bin/sh
# WBFit's part of tools/mkpackage.sh: the program and its icon (a tool; drop a picture's icon on
# it). Called with TOP (the repository), PKG (the package drawer), ICON (tools/mkicon.py) and POS
# (the icon's place). The tool types are those of the Shell arguments; the ones in brackets are
# off until the brackets are taken away.
# SPDX-License-Identifier: GPL-2.0-only
set -e
cp "$TOP/build/WBFit" "$PKG/WBFit"
$ICON tool "$PKG/WBFit.info" --image wbfit --stack 65536 --pos "$POS" \
	--tooltype "MODE=FILL" \
	--tooltype "(BG=000000)" \
	--tooltype "(FORMAT=AUTO)" \
	--tooltype "(PALETTE=AUTO)" \
	--tooltype "(DITHER)" \
	--tooltype "(OUT=SYS:Prefs/Presets/WBFit)" \
	--tooltype "(FILE=SYS:Prefs/Presets/picture.jpg)" \
	--tooltype "(WATCH)" \
	--tooltype "(QUIET)" \
	--tooltype "(TIMES)"
