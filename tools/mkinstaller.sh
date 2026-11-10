#!/bin/sh
# Utilities - write Install-Utilities (an Amiga Installer script) for the package drawer PKG:
#   sh tools/mkinstaller.sh PKG Tool...
# It is made from what is in the package: the tools given, and each DataTypes/<Name> drawer
# there (its .datatype files, its descriptors and their icons, an optional <X>Info tool, PDF's
# Fonts drawer), so a datatype that was not bundled is not offered.
#
# The script keeps to what Commodore's Installer 43, InstallerLG and the AROS Installer all do
# the same way (found with Install-Commodities on AROS): no procedure, no select for values
# (plain ifs), no (and ...) (nested ifs), askoptions bits tested with (IN mask n), one action
# per if.
# SPDX-License-Identifier: GPL-2.0-only
set -e
pkg=$1; shift
out=$pkg/Install-Utilities
q() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }   # an Installer string's inside

ntools=$#
dts=""
for n in WebP PNG-JPEG AVIF PDF; do [ -d "$pkg/DataTypes/$n" ] && dts="$dts $n"; done

{
cat <<'EOF'
; $VER: Install-Utilities 1.0 (10.11.2026)
;
; Installer script for the Utilities archive: our small tools for AmigaOS 3.x and the
; datatypes bundled with them. You choose what to install; the tools go to a drawer of your
; choice, the datatypes to SYS:Classes/DataTypes and DEVS:DataTypes (with AddDataTypes, so
; they work at once). Written for the Commodore/Amiga Installer V43 language; it also runs
; with InstallerLG and the AROS Installer. (Made by tools/mkinstaller.sh.)
; SPDX-License-Identifier: GPL-2.0-only

(set #version "1.0")
(if (= @app-name "") (set @app-name "Utilities"))

(welcome
  (cat "Utilities " #version "\n\n"
       "Small tools for AmigaOS 3.x, and our datatypes. You can choose which "
       "of them to install on the next page."))

; what works here: picture.datatype 43+ (true-colour pictures) and a 68040/68060
(set #pdt (/ (getversion "SYS:Classes/DataTypes/picture.datatype") 65536))
(set #v43 0)
(if (>= #pdt 43) (set #v43 1))
(set #cpu (database "cpu"))
(set #c40 0)
(if (= #cpu "68040") (set #c40 1))
(if (= #cpu "68060") (set #c40 1))
(if (= #cpu "68080") (set #c40 1))

EOF
# ---- the default selection: every tool, every datatype that works here
echo "(set #def 0)"
bit=0; val=1
for t in "$@"; do echo "(set #def (+ #def $val))   ; $t"; bit=$((bit + 1)); val=$((val * 2)); done
for n in $dts; do
	case $n in
	AVIF|PDF) echo "(if #v43 (if #c40 (set #def (+ #def $val))))   ; $n" ;;
	*) echo "(if #v43 (set #def (+ #def $val)))   ; $n" ;;
	esac
	val=$((val * 2))
done
# ---- the labels
for n in $dts; do
	case $n in
	WebP) l="WebP (webp.datatype)"; v="#v43"; need="needs OS 3.5+" ;;
	PNG-JPEG) l="PNG + JPEG (png/jpeg.datatype)"; v="#v43"; need="needs OS 3.5+" ;;
	AVIF) l="AVIF (avif.datatype)"; v="#c40"; need="needs a 68040+" ;;
	PDF) l="PDF (pdf.datatype)"; v="#c40"; need="needs a 68040+ with FPU" ;;
	esac
	echo "(set #l$(echo $n | tr -d -) \"$(q "$l")\")"
	echo "(if (= $v 0) (set #l$(echo $n | tr -d -) \"$(q "$l") ($need)\"))"
	case $n in AVIF|PDF) echo "(if (= #v43 0) (set #l$n \"$(q "$l") (needs OS 3.5+)\"))" ;; esac
done
echo
echo "(set #which"
echo "  (askoptions"
echo "    (prompt \"What do you want to install?\")"
printf '    (help (cat "The tools go to a drawer you choose next. Each datatype goes to SYS:Classes/DataTypes, its descriptor to DEVS:DataTypes.\\n\\n"\n'
printf '%s\n' '               "OS 3.5+ means picture.datatype 43 or newer (OS 3.5, 3.9, 3.1.4, 3.2, or P96/CGX on 3.1): every datatype here needs it; yours is version " #pdt ".\n\n"'
printf '%s\n' '               "AVIF and PDF are heavy work: they need a 68040 or 68060 (PDF one with its FPU); this CPU is a " #cpu ".\n\n"'
echo "               @askoptions-help))"
printf '    (choices'
for t in "$@"; do
	case $t in
	WBFit) printf ' "WBFit (backdrop from any picture)"' ;;
	*) printf ' "%s"' "$(q "$t")" ;;
	esac
done
for n in $dts; do printf ' #l%s' "$(echo $n | tr -d -)"; done
echo ")"
echo "    (default #def)))"
echo
echo '(if (= #which 0) (exit "Nothing was selected, so nothing was installed." (quiet)))'
echo
# ---- the tools
if [ $ntools -gt 0 ]; then
	echo "(set #anytool 0)"
	bit=0
	for t in "$@"; do echo "(if (IN #which $bit) (set #anytool 1))"; bit=$((bit + 1)); done
	cat <<'EOF'
(set #where 1)
(set #dest "SYS:Tools")
(if #anytool
  (set #where
    (askchoice
      (prompt "Where should the tools go?")
      (help (cat "C: : Shell commands everywhere (their icons are not copied there).\n\n"
                 "SYS:Tools: a drawer of the Workbench; double-click or drop a picture on WBFit's icon.\n\n"
                 "Another drawer: one of your choice.\n\n"
                 @askchoice-help))
      (choices "C:" "SYS:Tools" "Another drawer...")
      (default 1))))
(if #anytool (if (= #where 0) (set #dest "C:")))
(if #anytool (if (= #where 2)
  (set #dest
    (askdir
      (prompt "Choose the drawer for the tools")
      (help @askdir-help)
      (default "SYS:Tools")))))
(if #anytool (if (not (exists #dest (noreq))) (makedir #dest (infos))))
EOF
	bit=0
	for t in "$@"; do
		# (with its icon, but not into C:)
		echo "(if (IN #which $bit)"
		echo "  (if (= #where 0)"
		echo "    (copyfiles (prompt \"Copying $(q "$t") to \" #dest) (help @copyfiles-help) (source \"$(q "$t")\") (dest #dest))"
		echo "    (copyfiles (prompt \"Copying $(q "$t") to \" #dest) (help @copyfiles-help) (source \"$(q "$t")\") (dest #dest) (infos))))"
		bit=$((bit + 1))
	done
fi
echo
# ---- the datatypes
infos=""
for n in $dts; do
	d=DataTypes/$n
	echo "; ---- $n"
	for f in $(cd "$pkg/$d" && find Classes/DataTypes -maxdepth 1 -type f -name '*.datatype' 2>/dev/null | LC_ALL=C sort); do
		echo "(if (IN #which $bit)"
		echo "  (copylib"
		echo "    (prompt \"Copying $(basename $f) to SYS:Classes/DataTypes\")"
		echo "    (help @copylib-help)"
		echo "    (source \"$d/$f\")"
		echo "    (dest \"SYS:Classes/DataTypes\")"
		echo "    (confirm)))"
	done
	descs=""
	for f in $(cd "$pkg/$d/Devs/DataTypes" 2>/dev/null && ls | grep -v '\.info$' | LC_ALL=C sort); do descs="$descs $f"; done
	if [ -n "$descs" ]; then
		echo "(if (IN #which $bit)"
		echo "  (copyfiles"
		echo "    (prompt \"Copying the DataTypes descriptor(s)$descs to DEVS:DataTypes\")"
		echo "    (help @copyfiles-help)"
		echo "    (source \"$d/Devs/DataTypes\")"
		echo "    (dest \"DEVS:DataTypes\")"
		printf '    (choices'; for f in $descs; do printf ' "%s"' "$f"; done; echo ")"
		echo "    (infos)))"
		printf '(if (IN #which %s)\n  (run "C:AddDataTypes' "$bit"; for f in $descs; do printf ' DEVS:DataTypes/%s' "$f"; done; echo ' QUIET"'
		echo "    (prompt \"Telling datatypes.library about the new $n datatype\")"
		echo "    (help \"AddDataTypes reads the descriptor, so the new datatype is used without a reboot.\")))"
	fi
	# pdf.datatype's fonts: SYS:Classes/DataTypes/PDF/Fonts (its dt_lib.h, PDFDT_FONTDIR)
	fonts=$(cd "$pkg/$d" && find . -type d -name Fonts ! -path './Source/*' 2>/dev/null | head -1 | sed 's|^\./||')
	if [ -n "$fonts" ]; then
		echo "(if (IN #which $bit) (if (not (exists \"SYS:Classes/DataTypes/PDF\" (noreq))) (makedir \"SYS:Classes/DataTypes/PDF\" (infos))))"
		echo "(if (IN #which $bit) (if (not (exists \"SYS:Classes/DataTypes/PDF/Fonts\" (noreq))) (makedir \"SYS:Classes/DataTypes/PDF/Fonts\")))"
		echo "(if (IN #which $bit)"
		echo "  (copyfiles"
		echo "    (prompt \"Copying pdf.datatype's fonts to SYS:Classes/DataTypes/PDF/Fonts\")"
		echo "    (help @copyfiles-help)"
		echo "    (source \"$d/$fonts\")"
		echo "    (dest \"SYS:Classes/DataTypes/PDF/Fonts\")"
		echo "    (all)))"
	fi
	for i in $(cd "$pkg/$d" && ls | grep -E '^[A-Za-z]+Info$' || true); do infos="$infos $bit:$d/$i"; done
	bit=$((bit + 1))
done
# ---- the Info tools, behind one question
if [ -n "$infos" ]; then
	echo
	echo "(set #anyinfo 0)"
	for e in $infos; do echo "(if (IN #which ${e%%:*}) (set #anyinfo 1))"; done
	echo "(set #info 0)"
	echo "(if #anyinfo"
	echo "  (set #info"
	echo "    (askbool"
	printf '      (prompt "Copy the Info tools of the datatypes you chose (%s) to C:?")\n' "$(for e in $infos; do printf '%s ' "$(basename ${e#*:})"; done | sed 's/ $//')"
	echo "      (help \"Shell tools that show what a picture file holds and how long it takes to decode; see each datatype's ReadMe.\")"
	echo "      (default 0))))"
	for e in $infos; do
		echo "(if #info (if (IN #which ${e%%:*})"
		echo "  (copyfiles"
		echo "    (prompt \"Copying $(basename ${e#*:}) to C:\")"
		echo "    (help @copyfiles-help)"
		echo "    (source \"${e#*:}\")"
		echo "    (dest \"C:\"))))"
	done
fi
cat <<'EOF'

(if #anytool (set @default-dest #dest))
(exit (cat "Utilities " #version " is installed.\n\n"
           "If an older version of a datatype was in use, reboot to be sure the new one is loaded. "
           "Each tool's and datatype's ReadMe says more."))
EOF
} > "$out"
# with no tools #anytool must still exist
[ $ntools -gt 0 ] || sed -i 's/^(if #anytool (set @default-dest #dest))$//' "$out"
echo "mkinstaller: $out"
