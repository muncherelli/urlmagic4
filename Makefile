# urlmagic4 - built with the MinGW-w64 i686 cross-compiler, for Windows NT 4.0 on 486+ CPUs.
#   make            build/urlmagic4tab.dll
#   make install    copy the DLLs to $(DEST) (default ~/Development/files)
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

all: $(B)/urlmagic4tab.dll

$(B):
	mkdir -p $@

$(B)/urlmagic4tab.dll: $(TAB_OBJS) src/tab/tab.def
	$(CC) $(LDFLAGS) -o $@ $(TAB_OBJS) src/tab/tab.def $(TAB_LIBS)
	@if i686-w64-mingw32-objdump -d $@ | grep -qE '\bcmov|\bf(u)?comi'; then \
	  echo "error: $@ uses P6-only instructions"; rm -f $@; exit 1; fi

$(B)/tab.o: src/tab/tab.c src/tab/resource.h src/common/settings.h | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<
$(B)/tab.res.o: src/tab/tab.rc src/tab/resource.h res/urlmagic4.ico | $(B)
	$(WINDRES) -O coff -I src/tab -o $@ $<
$(B)/settings.o: src/common/settings.c src/common/settings.h | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<
$(B)/nocrt.o: src/common/nocrt.c | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	install -m 0644 $(B)/urlmagic4tab.dll $(DEST)/

clean:
	rm -rf $(B)

.PHONY: all install clean
