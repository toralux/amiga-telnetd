# amiga-telnetd

A really simple standalone telnet daemon for AmigaOS 2.04+ (tested on
AmigaOS 3.1 / A500 68000). No inetd, no config files, no user database,
no authentication. One connection at a time, real AmigaDOS shell per
connection. Needs a bsdsocket TCP/IP stack (AmiTCP, AmiTCP_NG, Roadshow,
Miami).

**License: GPLv2.** The session architecture (packet-serving filehandle,
`NewShell *` spawn recipe, ACTION_WAIT_CHAR/SCREEN_MODE handling) is
ported from **telnetd 2.0** by Peter Simons & Steve Holland (1995,
GPLv2), adapted for AmiTCP_NG 4.x: no inetd, no usergroup.library, LAN
only. See docs/DESIGN.md for the full architecture and provenance.

## Run

```amigados
1> stack 20000
1> telnetd            ; port 23
1> telnetd 2323       ; custom port
```

Then from any machine on the LAN: `telnet <amiga-ip>` and you get an
AmigaDOS shell. `EndCLI` (or `exit`) closes the session; the daemon waits
for the next connection. Ctrl-C in the starting Shell stops the daemon —
between sessions or mid-session.

### Do I need `stack 20000`?

Recommended, belt-and-braces. The daemon's large buffers are static (not
on the stack) and the shell it spawns gets an explicit 64 KB stack of its
own, so the default 4 KB CLI stack will most likely work — but bsdsocket
and DOS packet internals on a 68000 are exactly where a snug stack bites.

### Running in the background

```amigados
1> run >NIL: C:telnetd
```

With no console there is nothing to Ctrl-C, so stop it from any Shell:

```amigados
1> Status            ; find the process "Loaded as command: telnetd"
1> Break <n> C       ; n = its process number
```

## Telnet client notes

Minimal negotiation is built in: on connect the daemon asks the client
for character mode with server echo (WILL ECHO, WILL SGA, DONT LINEMODE)
and refuses every other option, so stock clients work out of the box.
Output is translated to NVT CRLF; `SetMode()` (raw mode, e.g. for
password prompts) is supported with proper echo negotiation.

WARNING: no authentication, no encryption — LAN use only, never
port-forward. A shell over telnet is total remote access.

