#!/bin/sh
# Utilities - build build/Utilities.lha: each tool given (sh tools/mkpackage.sh WBFit ...) with
# its icon, the ReadMe, the source, a DataTypes drawer with our public datatype packages, and
# Install-Utilities (tools/mkinstaller.sh) to install any of them.
#
# A tool's part: src/<tool>/pkg.sh if it has one (called with TOP, PKG, ICON and POS: it copies
# the program and makes its icon), else build/<Tool> with a plain tool icon. The source is the
# repository's files (git ls-files) without the NativeCall glue (src/*/nc/, src/*/nc.mk).
#
# DataTypes/: the finished public packages of the sibling repositories, each unpacked as it is
# (its Installer script, ReadMe, icons and Licenses unchanged) into a drawer of its own; read
# only, never rebuilt. A package that isn't there yet is left out, with a note.
#
# The package is refused when a binary (any file outside Source/ with a NUL byte: programs,
# datatypes, icons) contains "nativecall", "ncsupport" or "NC_Call" (a private NativeCall build
# must never get in), and when there is certificate or key material anywhere.
# SPDX-License-Identifier: GPL-2.0-only
set -e
top=$(cd "$(dirname "$0")/.." && pwd)
far=$(cd "$top/.." && pwd)
[ $# -ge 1 ] || { echo "usage: sh tools/mkpackage.sh Tool..." >&2; exit 1; }
mkdir -p "$top/build"
stage=$(mktemp -d "$top/build/pkg.XXXXXX")
pkg=$stage/Utilities
mkdir -p "$pkg/Source" "$pkg/DataTypes"
icon="python3 $top/tools/mkicon.py"

# ---- the tools
x=10
for t in "$@"; do
	low=$(echo "$t" | tr 'A-Z' 'a-z')
	[ -f "$top/build/$t" ] || { echo "mkpackage: build/$t is missing (make first)" >&2; exit 1; }
	if [ -f "$top/src/$low/pkg.sh" ]; then
		TOP=$top PKG=$pkg ICON=$icon POS=$x,10 sh "$top/src/$low/pkg.sh"
	else
		cp "$top/build/$t" "$pkg/$t"
		$icon tool "$pkg/$t.info" --image install --stack 16384 --pos $x,10
	fi
	x=$((x + 80))
done
cp "$top/README.md" "$pkg/ReadMe"
$icon project "$pkg/ReadMe.info" --image doc --tool SYS:Utilities/MultiView --pos $x,10
x=$((x + 80))

# ---- the source (tracked files, no NativeCall glue)
(cd "$top" && git ls-files) | grep -v -E '(^|/)nc/|(^|/)nc\.mk$' | while read -r f; do
	mkdir -p "$pkg/Source/$(dirname "$f")"
	cp "$top/$f" "$pkg/Source/$f"
done
$icon drawer "$pkg/Source.info" --image drawer --window 80,50,400,150 --pos $x,10
x=$((x + 80))

# ---- the datatypes
notes=""
dtpkg() {   # drawer name, archive
	name=$1; arc=$2
	if [ ! -f "$arc" ]; then
		notes="$notes $name"
		echo "mkpackage: note: $arc is not there (yet): DataTypes/$name left out"
		return 0
	fi
	tmp=$stage/unpack.$name
	mkdir -p "$tmp"
	(cd "$tmp" && lha -xq "$arc" >/dev/null 2>&1)
	d=$(cd "$tmp" && find . -mindepth 1 -maxdepth 1 -type d | head -1 | sed 's|^\./||')
	[ -n "$d" ] || { echo "mkpackage: $arc has no drawer" >&2; exit 1; }
	mv "$tmp/$d" "$pkg/DataTypes/$name"
	[ -f "$tmp/$d.info" ] && mv "$tmp/$d.info" "$pkg/DataTypes/$name.info"
	echo "mkpackage: DataTypes/$name from $(basename "$arc")"
}
dtpkg WebP "$far/webpdt/build/webpdt-1.0.lha"
dtpkg PNG-JPEG "$far/pjdt/build/pjdt-1.0.lha"
dtpkg AVIF "$far/avifdt/build/avifdt-1.0.lha"
pdf=$(ls "$far"/pdfdt/build/pdfdt-*.lha 2>/dev/null | LC_ALL=C sort | tail -1 || true)
dtpkg PDF "${pdf:-$far/pdfdt/build/pdfdt-(not built).lha}"
{
	cat <<'EOF'
DataTypes - our picture and document datatypes
==============================================

Each drawer is that datatype's own package, unchanged: double-click its
Installer script (the standard Amiga Installer) or follow its ReadMe. They are
the plain builds for every Amiga (no accelerator needed, but see the CPU).

EOF
	[ -d "$pkg/DataTypes/WebP" ] && cat <<'EOF'
WebP      webp.datatype: WebP pictures (lossy and lossless, transparency)
          Install: double-click WebP/Install_webpdt
          Needs: picture.datatype 43 or newer (OS 3.5, 3.9, 3.1.4 or 3.2);
          a 68020 or better, no FPU.

EOF
	[ -d "$pkg/DataTypes/PNG-JPEG" ] && cat <<'EOF'
PNG-JPEG  png.datatype and jpeg.datatype: PNG and JPEG pictures (they replace
          the ones you have)
          Install: double-click PNG-JPEG/Install_pjdt (it asks which)
          Needs: picture.datatype 43 or newer (OS 3.5, 3.9, 3.1.4 or 3.2);
          a 68020 or better, no FPU.

EOF
	[ -d "$pkg/DataTypes/AVIF" ] && cat <<'EOF'
AVIF      avif.datatype: AVIF pictures (AV1 in HEIF)
          Install: double-click AVIF/Install_avifdt
          Needs: picture.datatype 43 or newer (OS 3.5, 3.9, 3.1.4 or 3.2);
          runs on a 68020, but a 68040 or 68060 is the practical minimum
          (AV1 is heavy: seconds for a small picture, minutes for a photo).

EOF
	[ -d "$pkg/DataTypes/PDF" ] && cat <<'EOF'
PDF       pdf.datatype: PDF documents (and stand-in fonts, 8 MB, for text
          in fonts a PDF doesn't embed)
          Install: double-click PDF/Install_pdfdt
          Needs: picture.datatype 43 or newer (OS 3.5, 3.9, 3.1.4 or 3.2),
          or AROS 68k; a 68040 or 68060 with its FPU; about 8 MB free.

EOF
	cat <<'EOF'
OS 3.0 and 3.1's own picture.datatype (version 40) takes only palette
pictures, so these datatypes need a newer one. WBFit itself does not: it
works on OS 3.1 with any datatype the system has (ILBM is always there).
EOF
} > "$pkg/DataTypes/ReadMe"
$icon drawer "$pkg/DataTypes.info" --image drawer --window 90,60,420,160 --pos $x,10
$icon project "$pkg/DataTypes/ReadMe.info" --image doc --tool SYS:Utilities/MultiView --pos 330,10
# ---- the Installer script, made from what is in the package
sh "$top/tools/mkinstaller.sh" "$pkg" "$@"
$icon project "$pkg/Install-Utilities.info" --image install --tool Installer --tooltype "APPNAME=Utilities" --pos 410,10
$icon drawer "$stage/Utilities.info" --image drawer --window 60,40,520,100

# ---- the safety checks, before the archive is made
bad=0
for f in $(cd "$stage" && find Utilities -type f ! -path 'Utilities/Source/*' | LC_ALL=C sort); do
	# (binary: it has NUL bytes, so removing them makes it shorter)
	if [ "$(tr -d '\000' < "$stage/$f" | wc -c)" -ne "$(wc -c < "$stage/$f")" ] &&
	   LC_ALL=C grep -q -a -i -E 'nativecall|ncsupport|nc_call' "$stage/$f"; then
		echo "mkpackage: $f contains NativeCall code (a private build?), refusing" >&2
		bad=1
	fi
done
# PEM armour anywhere (real certificates and keys have it; packaging scripts that only name the
# words, ours and the datatypes', do not), the test marker outside Source/, and the file names.
# (The patterns in pieces, so this script's own copy in Source/ does not match them.)
pem="-----""BEGIN [A-Z0-9 ]*(CERTIFICATE|PRIVATE KEY)-----"
mark="sandbox-""test-only"
if (cd "$stage" && grep -rlE -a -e "$pem" Utilities Utilities.info) ||
   (cd "$stage" && find Utilities -type f ! -path 'Utilities/Source/*' ! -path 'Utilities/DataTypes/*/Source/*' -exec grep -l -a "$mark" {} +) ||
   find "$stage/Utilities" -iname '*.crt' -o -iname '*.pem' -o -iname '*.key' | grep -q .; then
	echo "mkpackage: certificate or key material found, refusing" >&2
	bad=1
fi
[ $bad = 0 ] || exit 1

out=$top/build/Utilities.lha
[ -e "$out" ] && mv "$out" "$out.old"
# jlha (as in Lagoon's and Commodities' packaging): level-2 headers, every file listed
(cd "$stage" && jlha a2o5q "$out" Utilities.info $(find Utilities -type f | LC_ALL=C sort) >/dev/null 2>&1)
[ -s "$out" ] || { echo "mkpackage: jlha failed" >&2; exit 1; }
[ -n "$notes" ] && echo "mkpackage: left out (not built yet):$notes"
echo "mkpackage: $out"
