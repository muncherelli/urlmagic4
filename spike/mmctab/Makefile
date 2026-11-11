# Spike: MMC property-sheet extension for IIS 4.0's Internet Service Manager.
#   make            -> umtab.dll
#   make install    -> copy to $(DEST) (default ~/Development/files)
CC      = i686-w64-mingw32-gcc
WINDRES = i686-w64-mingw32-windres
DEST   ?= $(HOME)/Development/files

CFLAGS  = -Os -Wall -Wextra -std=c99 -ffreestanding -fno-builtin -fno-stack-protector \
          -fno-asynchronous-unwind-tables -fno-ident -mno-stack-arg-probe -DUNICODE -D_UNICODE \
          -D_WIN32_WINNT=0x0400 -D_WIN32_IE=0x0300 -DWINVER=0x0400
# NT 4.0 loader: no CRT, kernel32/user32/advapi32/ole32/comctl32 only, NT 4.0 versions
LDFLAGS = -shared -nostdlib -s -Wl,-e,_DllMain@12 -Wl,--enable-stdcall-fixup \
          -Wl,--major-os-version,4,--minor-os-version,0 \
          -Wl,--major-subsystem-version,4,--minor-subsystem-version,0 \
          -Wl,--disable-dynamicbase,--disable-nxcompat
LIBS    = -lkernel32 -luser32 -ladvapi32 -lole32 -lcomctl32

umtab.dll: umtab.o umtab.res.o umtab.def
	$(CC) $(LDFLAGS) -o $@ umtab.o umtab.res.o umtab.def $(LIBS)

umtab.o: umtab.c resource.h
	$(CC) $(CFLAGS) -c -o $@ umtab.c

umtab.res.o: umtab.rc resource.h
	$(WINDRES) -O coff -o $@ umtab.rc

install: umtab.dll
	install -m 0644 umtab.dll $(DEST)/umtab.dll

clean:
	rm -f *.o umtab.dll

.PHONY: install clean
