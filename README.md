# amiga-telnetd

A really simple standalone telnet daemon for classic AmigaOS.

- **No inetd** — it listens by itself
- **No config files, no user database, no authentication**
- **No UI** — plain CLI program
- One connection at a time, real AmigaDOS shell per connection
- 68000 baseline, AmigaOS 2.04+ (tested target: 3.1), needs a bsdsocket TCP/IP stack (AmiTCP, AmiTCP_NG, Roadshow, Miami)

## Run

First run: foreground, from a Shell, so you can watch the log lines:

```amigados
1> stack 20000
1> telnetd            ; port 23
1> telnetd 2323       ; custom port
```

Then from any machine on the LAN: `telnet <amiga-ip>` and you get an
AmigaDOS shell. `EndCLI` (or `exit`) closes the session; the daemon waits
for the next connection. Ctrl-C in the starting Shell stops the daemon
(between sessions; during a session, close the telnet client first).

### Do I need `stack 20000`?

Recommended, belt-and-braces. The daemon's large buffers are static (not on
the stack) and the shell it spawns gets an explicit 64 KB stack of its own,
so the default 4 KB CLI stack will most likely work — but bsdsocket and DOS
packet internals on a 68000 are exactly where a snug stack bites, and the
one-line insurance is free. Set it once in the Shell (or in User-Startup
before `run`) and forget about it.

### Running in the background

```amigados
1> run >NIL: C:telnetd
```

With no console there is nothing to Ctrl-C, so stop it from any Shell:

```amigados
1> Status            ; find the process "Loaded as command: telnetd"
1> Break <n> C       ; n = its process number
```

### Starting at boot

Add to `S:User-Startup` — **after** the line(s) that bring up the TCP/IP
stack (AmiTCP / AmiTCP_NG / Roadshow), or the daemon exits with
"no bsdsocket.library":

```amigados
if exists C:telnetd
  run >NIL: C:telnetd
endif
```

## Telnet client notes

Option negotiation is not implemented: IAC sequences are filtered on input
and the daemon echoes what you type. If your client misbehaves in line mode,
switch to character mode: Ctrl-] then `mode character` (BSD telnet).

## Build (x64 Linux cross toolchain)

```bash
# toolchain: https://github.com/AmigaPorts/m68k-amigaos-gcc
m68k-amigaos-gcc -Os -m68000 -Wall -o telnetd telnetd.c -s -lamiga
# or
make
```

GitHub Actions builds the binary on every push (see .github/workflows).

## How it works

For each accepted connection the daemon builds a DOS filehandle whose
handler is a packet loop inside the daemon itself, backed by the socket.
`System("NewShell *")` then starts a CLI attached to that handle. When the
shell exits, ACTION_END arrives, the socket closes, the next connection is
accepted. The packet-handler approach follows the classic AmiTCP-era
daemons (telnetd 2.0 / fakesr.device, ttyhandler), collapsed into one file
with no extra components.

## Test plan (hardware)

1. `stack 20000` then `telnetd` — expect "listening on port 23"
2. From the LAN: `telnet <amiga-ip>` — expect an AmigaDOS prompt with echo
3. `dir`, `version`, `avail` — output should stream back
4. `endcli` — session closes, daemon waits for next connection
5. Ctrl-C the daemon between connections

## Security

No authentication, plaintext protocol. LAN use only — never port-forward.

## License

MIT — see LICENSE. The design follows the public AmiTCP-era daemons
(telnetd 2.0 / fakesr.device by P. Simons & S. Holland; ttyhandler by K. Melkko).
