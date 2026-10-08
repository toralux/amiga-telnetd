# amiga-telnetd

A really simple standalone telnet daemon for AmigaOS 2.04+ (tested on
AmigaOS 3.1 / A500 68000). No inetd, no config files, no user database,
no authentication. Several sessions at once (4 by default), each with its
own real AmigaDOS shell. Needs a bsdsocket TCP/IP stack (AmiTCP, AmiTCP_NG, Roadshow,
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
1> telnetd MAXSESSIONS=2       ; at most 2 sessions at once (1-8, default 4)
1> telnetd SHELLSTACK=40000    ; stack for commands in the sessions (default 20000)
1> telnetd LOG=T:tdbg.log      ; also write a crash-surviving trace (any path)
1> telnetd DUMBTERM            ; for clients without ANSI support (see below)
```

Then from any machine on the LAN: `telnet <amiga-ip>` and you get an
AmigaDOS shell. `EndCLI` (or Ctrl-\) closes the session. Other clients can
connect at the same time, up to `MAXSESSIONS`; one more gets a "too many
sessions" message and is disconnected. Ctrl-C in the starting Shell stops
the daemon and ends every session (the remote shells get EOF and end).

Each session costs about 11 KB in the daemon plus its own shell process.
A slow client only slows its own shell: output is buffered per session,
and a client that takes nothing for 60 seconds is disconnected.

### Stack

Two different stacks matter.

**The daemon's own** (`stack 20000` before starting it): telnetd refuses
to start on less than 16000 bytes and says so. 20000 is ample, also with
several sessions: all session state is on the heap, telnetd's own call
depth stays under 1 KB, and the rest is headroom for bsdsocket.library and
dos.library, which run on the caller's stack (AmiTCP_NG documents a ~1.5 KB
protocol call depth with no guard; a 68000 has no MMU to catch an overrun).
The default 4 KB Shell stack is not enough margin.

**The remote shells'**: commands typed in a telnet session run with the
session shell's stack, not the daemon's - the shell is started by a helper
process that has no `stack` setting to pass on, so without help it would
get the DOS default of about 4 KB. telnetd asks for `SHELLSTACK` bytes
(default 20000) when it starts each shell. Type `stack` in a session to see
what a shell actually got; `stack <n>` there, or a `Stack` line in
`S:Shell-Startup`, changes it like in any Shell.

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

The daemon puts the client in character mode with server echo (WILL ECHO,
WILL SGA) and plays the console itself, as CON: does on a real Amiga: in
normal (cooked) mode it echoes and edits the line and keeps a command
history, then hands the finished line to the shell. Keys:

- Up / Down: history (16 lines per session); Left / Right: move in the line
- Home / End, Delete, Backspace
- Ctrl-X or Ctrl-U: kill line; Ctrl-K: kill to end of line
- Ctrl-C (or the client's "interrupt process"): break the running command;
  Ctrl-D / Ctrl-E / Ctrl-F send the other Amiga break signals
- Ctrl-\ on an empty line: EOF

Lines longer than the window wrap correctly: the daemon asks the client for
its window size (telnet NAWS) and follows resizes; a client that does not
say is assumed to be 80 columns wide. If a command prints while you are
typing ahead, your half-typed line is taken off the screen and drawn again
below the output.

When a program calls `SetMode(fh, 1)` (raw mode) keystrokes go to it
unedited and unechoed, as with a RAW: console. Output is translated to NVT
CRLF and Amiga CSI (0x9B) becomes `ESC [`.

Any ANSI/VT100 terminal works (macOS Terminal, iTerm2, PuTTY, xterm) with a
real telnet client. macOS no longer ships one: `brew install telnet`.

### DUMBTERM: clients without ANSI support

The line editor draws with ANSI escape sequences (cursor left/right/up/down,
erase to end of line/screen). For a client whose terminal does not
understand them — a hardware terminal, a very old telnet program, or `nc`,
which is not a telnet client and never switches to character mode — start
the daemon with `DUMBTERM`. The client then edits each line itself and
sends it whole (telnet line mode, local echo), as telnetd 2.0 did: only
plain text is ever sent to it, and there is no command history or cursor
key editing. Raw mode works as before.

WARNING: no authentication, no encryption — LAN use only, never
port-forward. A shell over telnet is total remote access.

