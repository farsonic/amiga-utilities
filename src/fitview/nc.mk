# src/fitview/nc.mk - FitView's NativeCall test build, read by tool.mk only with NATIVECALL=1
# NC_SDK=<the NativeCall SDK's sdk directory>. Nothing of the SDK is in this repository, and the
# program this builds (it contains the SDK's ncsupport.c) is a private test build: never part
# of the package (tools/mkpackage.sh refuses it).
#   make fvnc      NATIVECALL=1 NC_SDK=...  build/fvnc/FitView (68040, the module embedded)
#   make fvnctest  NATIVECALL=1 NC_SDK=...  the module under qemu-aarch64_be: every kernel (NEON
#                                           included) against the 68k code's C, and whole renders
#   make fvncpriv  NATIVECALL=1 NC_SDK=...  the private test build in FV_PRIVATE
ifeq ($(NC_SDK),)
$(error NATIVECALL=1 needs NC_SDK=<path to the NativeCall SDK's sdk directory>)
endif
NC_SDK_ABS := $(abspath $(NC_SDK))
FVNC_B     := build/fvnc
FV_PRIVATE ?= /tmp/claude-0/-home-user/3a066da9-8230-5cea-817a-b3782336c61e/scratchpad/fitview-private
FVNC_KSRC  := src/fitview/nc/fvk_nc.c src/fitview/fvk.c src/fitview/fvk.h src/lib/pxbase.h src/lib/pxneon.h
FVNC_IMG   := $(FVNC_B)/mod/build/native/fitview_image.c
FVNC_NARM  := $(FVNC_B)/mod/build/native/fitview.narm
# (as WBFit's: no SLP vectorizer, no calls to the SDK's byte-loop memcpy)
FVNC_NATFLAGS := -O3 -fno-tree-slp-vectorize -fno-tree-loop-distribute-patterns -DWFK_NC

$(FVNC_NARM) $(FVNC_IMG): $(FVNC_KSRC) src/fitview/nc.mk
	@mkdir -p $(FVNC_B)/mod
	ln -sf $(abspath src/fitview/nc/fvk_nc.c) $(FVNC_B)/mod/fvk_nc.c
	$(MAKE) -B -C $(FVNC_B)/mod -f $(NC_SDK_ABS)/mk/nativecall.mk narm MODULE=fitview KERNELS=fvk_nc.c \
	  NC_SDK=$(NC_SDK_ABS) CFLAGS_NATIVE='$(FVNC_NATFLAGS) -I$(abspath src/fitview) -I$(abspath src/lib) -I$(abspath src/fitview/nc)'

# ---- the 68k side (vbcc, 68040)
FVNC_VC := $(VC) -c99 -O2 -cpu=68040 -dontwarn=153 -I$(NDK) -Isrc/fitview -Isrc/lib -Isrc/fitview/nc -I$(NC_SDK_ABS)/include -DWFK_NC -DFITVIEW_NC
FVNC_SRC := src/fitview/fitview.c src/fitview/fvload.c src/fitview/fvrender.c src/fitview/fvview.c src/fitview/fvprefs.c \
            src/fitview/nc/fvk_nc.c src/fitview/nc/fvnc.c src/lib/pxweights.c src/lib/pxquant.c \
            $(NC_SDK_ABS)/lib68k/ncsupport.c $(FVNC_IMG)
$(FVNC_B)/FitView: $(FITVIEW_SRC) $(FITVIEW_HDR) src/fitview/nc/fvnc.c src/fitview/nc/fvnc.h $(FVNC_KSRC) $(FVNC_IMG)
	@mkdir -p $(FVNC_B)
	$(FVNC_VC) -o $@ $(FVNC_SRC) -lamiga
	@echo "== $@: $$(stat -c %s $@) bytes (NativeCall test build)"
fvnc: $(FVNC_B)/FitView

# ---- the module under qemu-aarch64_be (Linux user mode, no libc): nc/tests/narm_run.c loads it as
#      the hypervisor would and checks it against the kernels' plain C, then whole renders
FVNC_A64CC := aarch64-linux-gnu-gcc-14 -mbig-endian -mcpu=cortex-a72 -O2 -fno-tree-slp-vectorize -ffreestanding -nostdlib -static -fno-pie -no-pie \
              -fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra -Wno-unused-function -Isrc/fitview -Isrc/lib -Isrc/fitview/nc \
              -I$(NC_SDK_ABS)/native/include -I$(NC_SDK_ABS)/include -DWFK_NC
FVNC_Q := $(FVNC_B)/qemu
$(FVNC_Q)/narm_run: src/fitview/nc/tests/narm_run.c src/fitview/nc/tests/narm_start.S $(FVNC_KSRC) src/fitview/fvrender.c src/fitview/fvrender.h \
                    src/fitview/fvview.c src/lib/pxweights.c $(FVNC_NARM)
	@mkdir -p $(FVNC_Q)
	$(FVNC_A64CC) -c -o $(FVNC_Q)/ref.o -DWFK_NO_NEON -O1 -fno-tree-vectorize src/fitview/nc/fvk_nc.c
	$(FVNC_A64CC) -c -o $(FVNC_Q)/render.o -fno-tree-loop-distribute-patterns src/fitview/fvrender.c
	$(FVNC_A64CC) -c -o $(FVNC_Q)/view.o -fno-tree-loop-distribute-patterns src/fitview/fvview.c
	$(FVNC_A64CC) -c -o $(FVNC_Q)/pxw.o -fno-tree-loop-distribute-patterns src/lib/pxweights.c
	$(FVNC_A64CC) -c -o $(FVNC_Q)/rt.o -fno-tree-loop-distribute-patterns $(NC_SDK_ABS)/native/narm_rt.c
	$(FVNC_A64CC) -c -o $(FVNC_Q)/run.o -fno-tree-loop-distribute-patterns src/fitview/nc/tests/narm_run.c
	$(FVNC_A64CC) -c -o $(FVNC_Q)/start.o -DNARM_FILE_S='"$(abspath $(FVNC_NARM))"' src/fitview/nc/tests/narm_start.S
	aarch64-linux-gnu-ld -EB -static -Ttext=0x400000 --no-warn-rwx-segments -o $@ $(FVNC_Q)/start.o $(FVNC_Q)/run.o \
	  $(FVNC_Q)/render.o $(FVNC_Q)/view.o $(FVNC_Q)/pxw.o $(FVNC_Q)/ref.o $(FVNC_Q)/rt.o
fvnctest: $(FVNC_Q)/narm_run
	qemu-aarch64_be -cpu cortex-a72 $(FVNC_Q)/narm_run

# ---- the private test build (never the package)
fvncpriv: fvnc
	mkdir -p $(FV_PRIVATE)/FitView-nc
	cp $(FVNC_B)/FitView $(FV_PRIVATE)/FitView-nc/FitView
	cp src/fitview/nc/ReadMe.private $(FV_PRIVATE)/FitView-nc/ReadMe
	cp $(FV_PRIVATE)/FitView-nc/ReadMe $(FV_PRIVATE)/ReadMe.private
	sh src/fitview/nc/mkpriv.sh $(FV_PRIVATE)
	@echo "== $(FV_PRIVATE)/FitView-nc-private-test.lha"

.PHONY: fvnc fvnctest fvncpriv
