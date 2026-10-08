# amiga-telnetd — cross build with m68k-amigaos-gcc
# Toolchain: https://github.com/AmigaPorts/m68k-amigaos-gcc
CC      = m68k-amigaos-gcc
CPU     = 68000
CFLAGS  = -Os -fomit-frame-pointer -m$(CPU) -Wall -Wextra
LDFLAGS = -s -lamiga

all: telnetd telnetd.020

telnetd: telnetd.c
	$(CC) $(CFLAGS) -o $@ telnetd.c $(LDFLAGS)

# 68020/030/040/060 accelerators (integer code only - no FPU required)
telnetd.020: telnetd.c
	$(CC) -Os -fomit-frame-pointer -m68020-60 -Wall -Wextra -o $@ telnetd.c $(LDFLAGS)

# Host-side test of the input decoder and line editor (no Amiga needed):
# the portable section of telnetd.c is extracted and compiled against the
# stubs and terminal model in tests/edtest.c with the host's cc.
HOSTCC   ?= cc
SANITIZE ?= -fsanitize=address,undefined

test: tests/edtest
	./tests/edtest

tests/editor_part.c: telnetd.c
	awk '/=== BEGIN portable input\/editor section/{p=1} /=== END portable input\/editor section/{p=0} p' telnetd.c > $@

tests/edtest: tests/edtest.c tests/editor_part.c
	$(HOSTCC) -Wall -Wextra -Wno-unused-function -g $(SANITIZE) -o $@ tests/edtest.c

clean:
	rm -f telnetd telnetd.020 tests/edtest tests/editor_part.c
	rm -rf tests/edtest.dSYM

.PHONY: all clean test
