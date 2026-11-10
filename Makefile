# Utilities for AmigaOS 3.x (vbcc + NDK 3.2)
#   make            every tool into build/
#   make package    build/Utilities.lha (tools, icons, ReadMe, source)
# Each tool adds itself to TOOLS and gives a rule for build/<Tool> (src/<tool>/tool.mk).
VBCC ?= /opt/amiga/vbcc
NDK  ?= /opt/amiga/ndk/Include_H
VC   := PATH=$(VBCC)/bin:$$PATH VBCC=$(VBCC) $(VBCC)/bin/vc +aos68k
CFLAGS := -c99 -O2 -cpu=68020 -dontwarn=153 -I$(NDK)
TOOLS :=
-include src/*/tool.mk

all: $(addprefix build/,$(TOOLS))

package: all
	sh tools/mkpackage.sh $(TOOLS)

.PHONY: all package
