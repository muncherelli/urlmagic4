# urlmagic4 - built with the MinGW-w64 i686 cross-compiler, for Windows NT 4.0 on 486+ CPUs.
#   make            build/urlmagic4.dll (the filter), build/urlmagic4-mmc-snap-in.dll (the tab) and
#                   build/urlmagic<version>.exe, e.g. urlmagic4.0.0.exe (the installer)
#   make test       run the rewrite engine's tests natively
#   make dist       build/urlmagic<version>.zip (the installer + README, LICENSE, CHANGELOG)
#   make install    copy the DLLs to $(DEST) (default ~/Development/files)
comma := ,
VERSION := $(strip $(shell cat version.txt))
SETUP_EXE   = urlmagic$(VERSION).exe
VERSION_NUM := $(subst .,$(comma),$(firstword $(subst -, ,$(VERSION))))$(comma)0
CC      = i686-w64-mingw32-gcc
WINDRES = i686-w64-mingw32-windres
DEST   ?= $(HOME)/Development/files
B       = build

# -march=i486: NT 4.0 machines (and emulators of them) often lack P6 instructions such as
# CMOV; MinGW's default (pentiumpro) crashes there with 0xC000001E.
CFLAGS  = -Os -march=i486 -mtune=pentium -std=c99 -Wall -Wextra \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables \
          -fno-ident -mno-stack-arg-probe \
          -DUNICODE -D_UNICODE -DWINVER=0x0400 -D_WIN32_WINNT=0x0400 -D_WIN32_IE=0x0300
# No C runtime; NT 4.0 version stamps; no ASLR/DEP flags (NT 4.0 predates them).
LDFLAGS = -shared -nostdlib -s -Wl,-e,_DllMain@12 -Wl,--enable-stdcall-fixup \
          -Wl,--major-os-version,4,--minor-os-version,0 \
          -Wl,--major-subsystem-version,4,--minor-subsystem-version,0 \
          -Wl,--disable-dynamicbase,--disable-nxcompat

TAB_OBJS = $(B)/tab.o $(B)/tab.res.o $(B)/settings.o $(B)/nocrt.o
TAB_LIBS = -lkernel32 -luser32 -ladvapi32 -lole32 -lcomctl32 -lcomdlg32

PCRE2_SRC   = $(wildcard third_party/pcre2/src/*.c)
PCRE2_DEFS  = -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8 -DPCRE2_STATIC -DMATCH_LIMIT=200000 -DHEAP_LIMIT=8192
PCRE2_OBJS  = $(patsubst third_party/pcre2/src/%.c,$(B)/pcre2/%.o,$(PCRE2_SRC))

FILTER_OBJS = $(B)/filter.o $(B)/filter.res.o $(B)/rewrite.o $(B)/settings.o $(B)/nocrt.o $(PCRE2_OBJS)
FILTER_LIBS = -lkernel32 -luser32 -ladvapi32 -lole32 -lgcc

SETUP_OBJS  = $(B)/setup.o $(B)/setup.res.o $(B)/nocrt.o
SETUP_LIBS  = -lkernel32 -luser32 -ladvapi32 -lole32 -lshell32

all: $(B)/urlmagic4.dll $(B)/urlmagic4-mmc-snap-in.dll $(B)/$(SETUP_EXE)

$(B):
	mkdir -p $@

# version stamped into the DLLs' VERSIONINFO (version.txt, bumped by release-please)
$(B)/version.h: version.txt | $(B)
	printf '#define UM_VERSION_STR "%s"\n#define UM_VERSION_WSTR L"%s"\n#define UM_VERSION_NUM %s\n' '$(VERSION)' '$(VERSION)' '$(VERSION_NUM)' > $@

$(B)/urlmagic4-mmc-snap-in.dll: $(TAB_OBJS) src/tab/tab.def
	$(CC) $(LDFLAGS) -o $@ $(TAB_OBJS) src/tab/tab.def $(TAB_LIBS)
	@if i686-w64-mingw32-objdump -d $@ | grep -qE '\bcmov|\bf(u)?comi'; then \
	  echo "error: $@ uses P6-only instructions"; rm -f $@; exit 1; fi

$(B)/urlmagic4.dll: $(FILTER_OBJS) src/filter/filter.def
	$(CC) $(LDFLAGS) -o $@ $(FILTER_OBJS) src/filter/filter.def $(FILTER_LIBS)
	@if i686-w64-mingw32-objdump -d $@ | grep -qE '\bcmov|\bf(u)?comi'; then \
	  echo "error: $@ uses P6-only instructions"; rm -f $@; exit 1; fi

$(B)/filter.o: src/filter/filter.c src/common/settings.h src/engine/rewrite.h $(B)/version.h | $(B)
	$(CC) $(CFLAGS) -I $(B) -c -o $@ $<
$(B)/filter.res.o: src/filter/filter.rc $(B)/version.h | $(B)
	$(WINDRES) -O coff -I $(B) -o $@ $<
$(B)/rewrite.o: src/engine/rewrite.c src/engine/rewrite.h | $(B)
	$(CC) $(CFLAGS) -Ithird_party/pcre2/src $(PCRE2_DEFS) -c -o $@ $<
$(B)/pcre2/%.o: third_party/pcre2/src/%.c | $(B)
	@mkdir -p $(B)/pcre2
	$(CC) $(CFLAGS) -w -Ithird_party/pcre2/src $(PCRE2_DEFS) -c -o $@ $<

$(B)/$(SETUP_EXE): $(SETUP_OBJS)
	rm -f $(B)/urlmagic[0-9]*.exe
	$(CC) -nostdlib -s -Wl,-e,_start@0 -Wl,--subsystem,console:4.0 \
	  -Wl,--major-os-version,4,--minor-os-version,0 \
	  -Wl,--disable-dynamicbase,--disable-nxcompat -o $@ $(SETUP_OBJS) $(SETUP_LIBS)
	@if i686-w64-mingw32-objdump -d $@ | grep -qE '\bcmov|\bf(u)?comi'; then \
	  echo "error: $@ uses P6-only instructions"; rm -f $@; exit 1; fi
$(B)/setup.o: src/setup/setup.c src/setup/payload.h $(B)/version.h | $(B)
	$(CC) $(CFLAGS) -I $(B) -c -o $@ $<
# docs for NT 4.0: .txt names, Windows-1252 and CRLF line endings, so Notepad shows them properly
DOCS = $(B)/README.txt $(B)/LICENSE.txt $(B)/CHANGELOG.txt
$(B)/README.txt: README.md tools/totxt.py | $(B)
	python3 tools/totxt.py $< $@
$(B)/LICENSE.txt: LICENSE tools/totxt.py | $(B)
	python3 tools/totxt.py $< $@
$(B)/CHANGELOG.txt: CHANGELOG.md tools/totxt.py | $(B)
	python3 tools/totxt.py $< $@

# the installer carries everything it installs
$(B)/payload.rc: $(B)/urlmagic4.dll $(B)/urlmagic4-mmc-snap-in.dll $(DOCS) | $(B)
	printf '%s RCDATA "%s"\n' \
	  ID_FILTER_DLL '$(abspath $(B)/urlmagic4.dll)' ID_TAB_DLL '$(abspath $(B)/urlmagic4-mmc-snap-in.dll)' \
	  ID_README '$(abspath $(B)/README.txt)' ID_LICENSE '$(abspath $(B)/LICENSE.txt)' \
	  ID_CHANGELOG '$(abspath $(B)/CHANGELOG.txt)' > $@
$(B)/setup.res.o: src/setup/setup.rc src/setup/payload.h res/urlmagic4.ico $(B)/version.h $(B)/payload.rc | $(B)
	$(WINDRES) -O coff -I $(B) -I src/setup -o $@ $<

$(B)/tab.o: src/tab/tab.c src/tab/resource.h src/common/settings.h | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<
$(B)/tab.res.o: src/tab/tab.rc src/tab/resource.h res/urlmagic4.ico $(B)/version.h | $(B)
	$(WINDRES) -O coff -I src/tab -I $(B) -o $@ $<
$(B)/settings.o: src/common/settings.c src/common/settings.h | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<
$(B)/nocrt.o: src/common/nocrt.c | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

dist: all $(DOCS)
	rm -rf $(B)/dist $(B)/urlmagic[0-9]*.zip
	mkdir -p $(B)/dist
	cp $(B)/$(SETUP_EXE) $(DOCS) $(B)/dist/
	cd $(B)/dist && python3 -m zipfile -c ../urlmagic$(VERSION).zip *

# ---- native tests of the rewrite engine (gcc + libc on Linux)
HOSTCC     ?= gcc
TEST_FLAGS  = -O1 -g -std=c99 -Wall -Wextra -D_DEFAULT_SOURCE -Ithird_party/pcre2/src $(PCRE2_DEFS)

$(B)/test_engine: tests/test_engine.c src/engine/rewrite.c src/engine/rewrite.h $(PCRE2_SRC) | $(B)
	$(HOSTCC) $(TEST_FLAGS) -fsanitize=address,undefined -o $@ tests/test_engine.c src/engine/rewrite.c $(PCRE2_SRC)

test: $(B)/test_engine
	$(B)/test_engine

install: all
	rm -f $(DEST)/setup.exe $(DEST)/urlmagic4-v*.exe $(DEST)/urlmagic[0-9]*.exe
	rm -f $(DEST)/urlmagic4.dll $(DEST)/urlmagic4tab.dll $(DEST)/urlmagic4-mmc-snap-in.dll $(DEST)/README.md $(DEST)/LICENSE $(DEST)/CHANGELOG.md
	@# executable, or Samba refuses to let Windows run it
	install -m 0755 $(B)/$(SETUP_EXE) $(DEST)/

clean:
	rm -rf $(B)

.PHONY: all dist test install clean
