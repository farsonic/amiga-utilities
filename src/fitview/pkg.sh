#!/bin/sh
# FitView's part of tools/mkpackage.sh: the program and its icon (a tool: double-click it for a
# file requester, drop pictures on it, or make it a project's default tool, as MultiView). Called
# with TOP (the repository), PKG (the package drawer), ICON (tools/mkicon.py) and POS (the icon's
# place). The tool types are those of the Shell arguments; the ones in brackets are off until the
# brackets are taken away.
# SPDX-License-Identifier: GPL-2.0-only
set -e
cp "$TOP/build/FitView" "$PKG/FitView"
$ICON tool "$PKG/FitView.info" --image fitview --stack 65536 --pos "$POS" \
	--tooltype "FIT=FIT" \
	--tooltype "(BG=000000)" \
	--tooltype "(CHECKER=YES)" \
	--tooltype "(DITHER=YES)" \
	--tooltype "(PUBSCREEN=Workbench)" \
	--tooltype "(FONTNAME=topaz.font)" \
	--tooltype "(FONTSIZE=8)" \
	--tooltype "(PORTNAME=FITVIEW.1)" \
	--tooltype "(SCREEN)" \
	--tooltype "(BACKDROP)"
