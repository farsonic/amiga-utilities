#!/bin/sh
# FitView's private NativeCall test archive (make fvncpriv): DIR/FitView-nc-private-test.lha from
# DIR/FitView-nc; an older archive is moved to DIR/old/ first. Never the package.
# SPDX-License-Identifier: GPL-2.0-only
set -e
d=$1
cd "$d"
if [ -e FitView-nc-private-test.lha ]; then
	mkdir -p old
	mv FitView-nc-private-test.lha "old/FitView-nc-private-test.lha.$(date +%s)"
fi
lha -aq FitView-nc-private-test.lha FitView-nc
