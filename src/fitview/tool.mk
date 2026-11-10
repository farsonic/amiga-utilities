# FitView (src/fitview): a MultiView replacement that fits pictures to the window.
#   build/FitView        the program (vbcc, 68000, OS 3.1+; no NativeCall)
#   make test-fitview    host tests (gcc, ASan/UBSan; numpy for the reference)
# The NativeCall test build: make fvnc|fvnctest|fvncpriv NATIVECALL=1 NC_SDK=<sdk> (src/fitview/nc.mk).
TOOLS += FitView
TESTS += test-fitview
FITVIEW_SRC := src/fitview/fitview.c src/fitview/fvload.c src/fitview/fvrender.c src/fitview/fvview.c src/fitview/fvk.c \
               src/fitview/fvprefs.c src/lib/pxweights.c src/lib/pxquant.c
FITVIEW_HDR := src/fitview/fvload.h src/fitview/fvrender.h src/fitview/fvview.h src/fitview/fvk.h src/fitview/fvprefs.h \
               src/lib/pxbase.h src/lib/pxweights.h src/lib/pxquant.h
FITVIEW_VCFLAGS := -c99 -O2 -cpu=68000 -dontwarn=153 -I$(NDK) -Isrc/fitview -Isrc/lib

build/FitView: $(FITVIEW_SRC) $(FITVIEW_HDR) src/fitview/tool.mk
	@mkdir -p build
	$(VC) $(FITVIEW_VCFLAGS) -o $@ $(FITVIEW_SRC) -lamiga
	@echo "== $@: $$(stat -c %s $@) bytes"

# the emulator runs' helper: a command to FitView's ARexx port without RexxMast
build/fvcmd: tests/fitview/fvcmd.c
	@mkdir -p build
	$(VC) -c99 -O2 -cpu=68000 -I$(NDK) -o $@ tests/fitview/fvcmd.c -lamiga

# ---- host tests (the scaler is also checked against WBFit's kernels, which are linked in and
#      own the test clock: fvk.c is built with FV_SHARED_BUDGET)
FITVIEW_HOSTCC := gcc -std=c99 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
                  -fno-omit-frame-pointer -Isrc/fitview -Isrc/lib -Isrc/wbfit -DWFK_TEST_BUDGET
FITVIEW_HOSTSRC := tests/fitview/fitview_test.c src/fitview/fvrender.c src/fitview/fvview.c src/fitview/fvprefs.c \
                   src/lib/pxweights.c src/wbfit/wfk.c
build/host/fitview_test: $(FITVIEW_HOSTSRC) src/fitview/fvk.c $(FITVIEW_HDR) src/wbfit/wfk.h
	@mkdir -p $(dir $@)
	$(FITVIEW_HOSTCC) -DFV_SHARED_BUDGET -c -o build/host/fvk_shared.o src/fitview/fvk.c
	$(FITVIEW_HOSTCC) -o $@ $(FITVIEW_HOSTSRC) build/host/fvk_shared.o
test-fitview: build/host/fitview_test
	@mkdir -p build/host/fitview
	build/host/fitview_test build/host/fitview
	python3 tests/fitview/check.py build/host/fitview

ifeq ($(NATIVECALL),1)
include src/fitview/nc.mk
endif
.PHONY: test-fitview
