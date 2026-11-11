#!/usr/bin/env python3
"""srcfix.py SCRIPT... - make Installer scripts find their files wherever Installer's current
directory is. Workbench may start Installer with a current directory other than the script's
drawer, so every relative (source "x") becomes (source (tackon #src "x")), #src being the
drawer of the icon that started it (@icon). Idempotent.
SPDX-License-Identifier: GPL-2.0-only"""
import re, sys

PRE = ('; the drawer this script is in: Workbench may start Installer with another current\n'
       '; directory, so sources are found through the icon\'s path (#src)\n'
       '(if (= @icon "") (set #src "") (set #src (pathonly @icon)))\n')

def fix(text):
    if '(set #src (pathonly @icon))' not in text:
        lines = text.split('\n')
        i = 0
        while i < len(lines) and (lines[i].startswith(';') or not lines[i].strip()):
            i += 1
        lines.insert(i, PRE)
        text = '\n'.join(lines)
    return re.sub(r'\(source "([^":]+)"\)', r'(source (tackon #src "\1"))', text)

for p in sys.argv[1:]:
    s = open(p, encoding='latin-1').read()
    t = fix(s)
    if t != s:
        open(p, 'w', encoding='latin-1').write(t)
        print('srcfix:', p)
