# WBFit (src/wbfit): the Workbench backdrop from any picture, fitted to the screen.
#   build/WBFit        the program (vbcc, 68000, OS 3.0+; no NativeCall)
#   make test-wbfit    host tests (gcc, ASan/UBSan; Pillow and libpng for the decoders)
# The NativeCall test build: make nc|nctest|ncpriv NATIVECALL=1 NC_SDK=<sdk> (src/wbfit/nc.mk).
TOOLS += WBFit
TESTS += test-wbfit
WBFIT_SRC := src/wbfit/wbfit.c src/wbfit/wfcore.c src/wbfit/wfk.c src/wbfit/wfprefs.c
WBFIT_HDR := src/wbfit/wfcore.h src/wbfit/wfk.h src/wbfit/wfprefs.h
WBFIT_VCFLAGS := -c99 -O2 -cpu=68000 -dontwarn=153 -I$(NDK) -Isrc/wbfit

build/WBFit: $(WBFIT_SRC) $(WBFIT_HDR) src/wbfit/tool.mk
	@mkdir -p build
	$(VC) $(WBFIT_VCFLAGS) -o $@ $(WBFIT_SRC) -lamiga
	@echo "== $@: $$(stat -c %s $@) bytes"

# ---- host tests
WBFIT_HOSTCC := gcc -std=c99 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
                -fno-omit-frame-pointer -Isrc/wbfit -DWFK_TEST_BUDGET
WBFIT_HOSTSRC := src/wbfit/wfcore.c src/wbfit/wfk.c src/wbfit/wfprefs.c
build/host/wbfit_test: tests/wbfit/wbfit_test.c $(WBFIT_HOSTSRC) $(WBFIT_HDR)
	@mkdir -p $(dir $@)
	$(WBFIT_HOSTCC) -o $@ tests/wbfit/wbfit_test.c $(WBFIT_HOSTSRC)
build/host/wbfit_ndk: tests/wbfit/ndkcheck.c
	@mkdir -p $(dir $@)
	gcc -m32 -std=c99 -Wall -I$(NDK) -o $@ tests/wbfit/ndkcheck.c
test-wbfit: build/host/wbfit_test build/host/wbfit_ndk
	build/host/wbfit_ndk
	@mkdir -p build/host/wbfit
	build/host/wbfit_test build/host/wbfit
	python3 tests/wbfit/check.py build/host/wbfit

ifeq ($(NATIVECALL),1)
include src/wbfit/nc.mk
endif
.PHONY: test-wbfit
