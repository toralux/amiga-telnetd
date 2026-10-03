# amiga-telnetd — cross build with m68k-amigaos-gcc
# Toolchain: https://github.com/AmigaPorts/m68k-amigaos-gcc
CC      = m68k-amigaos-gcc
CPU     = 68000
CFLAGS  = -Os -fomit-frame-pointer -m$(CPU) -Wall -Wextra
LDFLAGS = -s -lamiga

all: telnetd

telnetd: telnetd.c
	$(CC) $(CFLAGS) -o $@ telnetd.c $(LDFLAGS)

clean:
	rm -f telnetd

.PHONY: all clean
