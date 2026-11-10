# src/wbfit/nc.mk - WBFit's NativeCall test build, read by tool.mk only with NATIVECALL=1
# NC_SDK=<the NativeCall SDK's sdk directory>. Nothing of the SDK is in this repository, and the
# program this builds (it contains the SDK's ncsupport.c) is a private test build: never part
# of the package (tools/mkpackage.sh refuses it).
#   make nc      NATIVECALL=1 NC_SDK=...  build/nc/WBFit (68040, the module embedded)
#   make nctest  NATIVECALL=1 NC_SDK=...  the module under qemu-aarch64_be: every kernel (NEON
#                                         included) against the 68k code's C, and whole jobs
#   make ncpriv  NATIVECALL=1 NC_SDK=...  the private test archive in NC_PRIVATE
ifeq ($(NC_SDK),)
$(error NATIVECALL=1 needs NC_SDK=<path to the NativeCall SDK's sdk directory>)
endif
NC_SDK_ABS := $(abspath $(NC_SDK))
ifeq ($(wildcard $(NC_SDK_ABS)/mk/nativecall.mk),)
$(error $(NC_SDK) is not the NativeCall SDK's sdk directory (no mk/nativecall.mk))
endif
WBNC_B     := build/nc
NC_PRIVATE ?= /tmp/claude-0/-home-user/3a066da9-8230-5cea-817a-b3782336c61e/scratchpad/wbfit-private
WBNC_KSRC  := src/wbfit/nc/wfk_nc.c src/wbfit/wfk.c src/wbfit/wfk.h
WBNC_IMG   := $(WBNC_B)/mod/build/native/wbfit_image.c
WBNC_NARM  := $(WBNC_B)/mod/build/native/wbfit.narm
# -fno-tree-slp-vectorize: GCC 14.2's SLP vectorizer miscompiles some big-endian code (found by
# webp.datatype's tests); -fno-tree-loop-distribute-patterns: no calls to the SDK's byte-loop memcpy
WBNC_NATFLAGS := -O3 -fno-tree-slp-vectorize -fno-tree-loop-distribute-patterns -DWFK_NC

# ---- the module: the SDK's own rules (nativecall.mk "narm"), our kernels linked in
$(WBNC_NARM) $(WBNC_IMG): $(WBNC_KSRC) src/wbfit/nc.mk
	@mkdir -p $(WBNC_B)/mod
	ln -sf $(abspath src/wbfit/nc/wfk_nc.c) $(WBNC_B)/mod/wfk_nc.c
	$(MAKE) -B -C $(WBNC_B)/mod -f $(NC_SDK_ABS)/mk/nativecall.mk narm MODULE=wbfit KERNELS=wfk_nc.c \
	  NC_SDK=$(NC_SDK_ABS) CFLAGS_NATIVE='$(WBNC_NATFLAGS) -I$(abspath src/wbfit) -I$(abspath src/wbfit/nc)'

# ---- the 68k side (vbcc, 68040)
WBNC_VC := $(VC) -c99 -O2 -cpu=68040 -dontwarn=153 -I$(NDK) -Isrc/wbfit -Isrc/wbfit/nc -I$(NC_SDK_ABS)/include -DWFK_NC -DWBFIT_NC
WBNC_SRC := src/wbfit/wbfit.c src/wbfit/wfcore.c src/wbfit/wfprefs.c src/wbfit/nc/wfk_nc.c src/wbfit/nc/wfnc.c \
            $(NC_SDK_ABS)/lib68k/ncsupport.c $(WBNC_IMG)
$(WBNC_B)/WBFit: $(WBFIT_SRC) $(WBFIT_HDR) src/wbfit/nc/wfnc.c src/wbfit/nc/wfnc.h $(WBNC_KSRC) $(WBNC_IMG)
	@mkdir -p $(WBNC_B)
	$(WBNC_VC) -o $@ $(WBNC_SRC) -lamiga
	@echo "== $@: $$(stat -c %s $@) bytes (NativeCall test build)"
nc: $(WBNC_B)/WBFit

# ---- the module under qemu-aarch64_be (Linux user mode, no libc): nc/tests/narm_run.c loads it as
#      the hypervisor would and checks it against the kernels' plain C, then whole jobs
WBNC_A64CC := aarch64-linux-gnu-gcc-14 -mbig-endian -mcpu=cortex-a72 -O2 -fno-tree-slp-vectorize -ffreestanding -nostdlib -static -fno-pie -no-pie \
              -fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra -Wno-unused-function -Isrc/wbfit -Isrc/wbfit/nc \
              -I$(NC_SDK_ABS)/native/include -I$(NC_SDK_ABS)/include -DWFK_NC
WBNC_Q := $(WBNC_B)/qemu
$(WBNC_Q)/narm_run: src/wbfit/nc/tests/narm_run.c src/wbfit/nc/tests/narm_start.S $(WBNC_KSRC) src/wbfit/wfcore.c src/wbfit/wfcore.h $(WBNC_NARM)
	@mkdir -p $(WBNC_Q)
	$(WBNC_A64CC) -c -o $(WBNC_Q)/ref.o -DWFK_NO_NEON -O1 -fno-tree-vectorize src/wbfit/nc/wfk_nc.c
	$(WBNC_A64CC) -c -o $(WBNC_Q)/core.o -fno-tree-loop-distribute-patterns src/wbfit/wfcore.c
	$(WBNC_A64CC) -c -o $(WBNC_Q)/rt.o -fno-tree-loop-distribute-patterns $(NC_SDK_ABS)/native/narm_rt.c
	$(WBNC_A64CC) -c -o $(WBNC_Q)/run.o -fno-tree-loop-distribute-patterns src/wbfit/nc/tests/narm_run.c
	$(WBNC_A64CC) -c -o $(WBNC_Q)/start.o -DNARM_FILE_S='"$(abspath $(WBNC_NARM))"' src/wbfit/nc/tests/narm_start.S
	aarch64-linux-gnu-ld -EB -static -Ttext=0x400000 --no-warn-rwx-segments -o $@ $(WBNC_Q)/start.o $(WBNC_Q)/run.o \
	  $(WBNC_Q)/core.o $(WBNC_Q)/ref.o $(WBNC_Q)/rt.o
nctest: $(WBNC_Q)/narm_run
	qemu-aarch64_be -cpu cortex-a72 $(WBNC_Q)/narm_run

# ---- the private test archive (never the package)
ncpriv: nc
	mkdir -p $(NC_PRIVATE)/WBFit-nc
	cp $(WBNC_B)/WBFit $(NC_PRIVATE)/WBFit-nc/WBFit
	cp src/wbfit/nc/ReadMe.private $(NC_PRIVATE)/WBFit-nc/ReadMe
	cp $(NC_PRIVATE)/WBFit-nc/ReadMe $(NC_PRIVATE)/ReadMe.private
	cd $(NC_PRIVATE) && if [ -e WBFit-nc-private-test.lha ]; then mkdir -p old && mv WBFit-nc-private-test.lha old/WBFit-nc-private-test.lha.$$$$; fi && lha -aq WBFit-nc-private-test.lha WBFit-nc
	@echo "== $(NC_PRIVATE)/WBFit-nc-private-test.lha"

.PHONY: nc nctest ncpriv
