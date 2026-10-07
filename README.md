# amiga-telnetd

A really simple standalone telnet daemon for AmigaOS 2.04+ (tested on
AmigaOS 3.1 / A500 68000). No inetd, no config files, no user database,
no authentication. One connection at a time, real AmigaDOS shell per
connection. Needs a bsdsocket TCP/IP stack (AmiTCP, AmiTCP_NG, Roadshow,
Miami).

**License: MIT.** The session architecture (packet-serving filehandle,
`NewShell *` spawn recipe, ACTION_WAIT_CHAR/SCREEN_MODE handling) follows the approach of **telnetd 2.0** by Peter Simons & Steve
Holland (1995), reimplemented for AmiTCP_NG 4.x: no inetd, no
usergroup.library, LAN
only. See docs/DESIGN.md for the full architecture and provenance.

## Run

```amigados
1> stack 20000
1> telnetd                     ; port 23
1> telnetd 2323                ; custom port
1> telnetd LOG=Data:tdbg.log   ; also write a crash-surviving trace
```

Then from any machine on the LAN: `telnet <amiga-ip>` and you get an
AmigaDOS shell. `EndCLI` (or Ctrl-\) closes the session; the daemon waits
for the next connection. Ctrl-C in the starting Shell stops the daemon,
between sessions or mid-session (the remote shell gets EOF and ends).

### Stack

telnetd refuses to start on less than 16000 bytes of stack and says so.
bsdsocket.library calls run on the caller's stack (AmiTCP_NG documents a
~1.5 KB protocol call depth with no guard) and a 68000 has no MMU to
catch an overrun, so the default 4 KB Shell stack is not enough margin.

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

In normal (cooked) mode the daemon declines ECHO and SGA, so the client
echoes and edits the line locally (BSD/inetutils telnet "line by line",
PuTTY with local echo/editing on "Auto") and sends it on Return, as
telnetd 2.0 did. When a program calls `SetMode(fh, 1)` (raw mode) the
daemon announces WILL ECHO + WILL SGA and the client switches to
character mode; `SetMode(fh, 0)` switches back. Ctrl-C (or the client's
"interrupt process") sends a break to the command reading the console.
Output is translated to NVT CRLF and Amiga CSI (0x9B) becomes `ESC [`.

A raw TCP client without telnet negotiation (`nc`) works, but with no
echo and no line editing.

WARNING: no authentication, no encryption — LAN use only, never
port-forward. A shell over telnet is total remote access.

