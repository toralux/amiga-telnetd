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

clean:
	rm -f telnetd telnetd.020

.PHONY: all clean
