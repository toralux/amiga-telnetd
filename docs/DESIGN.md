# telnetd — Design

Architecture follows the approach of **telnetd 2.0** (Peter Simons & Steve Holland, 1995,
Aminet `comm/tcp/telnetd2_0.lha`): the shell's stdio is a DOS filehandle whose
handler is a message port served by the daemon, which relays the DosPackets over
the socket. The fakesr.device path is not used.

## Post-mortem: why v0.1.2 – v0.3.1 crashed

Two defects, both found by reading the code against the telnetd 2.0 source and
the NDK, not on hardware.

### 1. AllocDosObject() result treated as a BPTR (since v0.1.2, commit 10d6aa3)

```c
fhB = (BPTR)AllocDosObject(DOS_FILEHANDLE, NULL);   /* returns APTR! */
fh  = (struct FileHandle *)BADDR(fhB);              /* = 4 x real address */
fh->fh_Type = ...; fh->fh_Port = ...; fh->fh_Pos = fh->fh_End = -1; ...
```

`AllocDosObject()` returns a plain C pointer (`clib/dos_protos.h`: `APTR`).
telnetd 2.0 converts it with `MKBADDR` (its `telnetd.c:337`). `BADDR()` of a C
pointer multiplies it by four, and the 68000 drives only 24 address lines, so
every handle field was written to `(4 * p) & 0xFFFFFF`. For a heap pointer in
the A500's fast RAM that is anywhere in the 16 MB map: other fast RAM, chip RAM
(exception vectors, copper lists), the CIAs at `$A00000-$BFFFFF` (parallel port,
disk motor and select), or the custom chips at `$DFF000`. `SystemTags()` and
`Close()` then read and freed the same wild address. This matches every
symptom: varying crash points between runs (the heap address varies), red
gurus, and gray-screen halts with the disk stopping. The cast was added to
silence the compiler warning that pointed at the bug.

### 2. The handler port was the daemon's own pr_MsgPort (v0.2 – v0.3.1)

telnetd 2.0 serves packets on its own `pr_MsgPort`, which is legal only because
that process makes no DOS calls after the spawn. Its source says so:
"NO more DOS calls allowed after this one". `pr_MsgPort` is where dos.library
waits for replies to the process's own packets, and `WaitPkt()` takes whatever
arrives first. Our port kept calling `PutStr()` and the `dbglog()` breadcrumbs
(`Open`/`Seek`/`Write`/`Close` on `Data:`), including one per packet inside the
session loop. A shell packet could then be taken as the reply to the daemon's
own `Write()`. The real filesystem reply landed in the session loop, which
"replied" it back to the filesystem handler: `ACTION_END` and `ACTION_SEEK`
replies would be bounced straight back to FFS. The v0.2.2 trace ("died inside
`SystemTags(SYS_Asynch)`") is this collision. System() does its own DoPkt calls
on `pr_MsgPort` while the new shell is already sending to it.

### Smaller defects fixed in v0.4

- The socket was blocking. A READ waiting for the rest of a line blocked the
  whole daemon in `recv()`, including Ctrl-C and hangup detection.
- On hangup, queued READs were never answered, so the shell hung in `Read()`.
  The session was then "abandoned" with handles still pointing at the daemon.
- The session ended at the first `ACTION_END` instead of when the last handle
  closed, and v0.3 freed the spawner's handle a second time after `Close()`
  had already freed it.
- The start negotiation announced WILL ECHO, but nothing echoed.
- `WAIT_CHAR` passed timeouts over one second to timer.device unnormalised
  (`tv_micro >= 1000000`).
- `SCREEN_MODE` replied DOSFALSE, so `SetMode()` reported failure.

Red herrings from the earlier investigation:
- The NULL `accept()` address. AmiTCP_NG 4.1.7's `_accept` guards every
  dereference with `if (name)`.
- The NULL-timeout `WaitSelect`. It is handled in `amiga_generic.c`.
- 68020 opcodes. Both binaries are 68000-clean by disassembly.

## Architecture (one process, several sessions)

1. **Startup.** Refuse to run on less than 16000 bytes of stack. bsdsocket
   calls run on the caller's stack, and a 68000 has no MMU to catch an overrun.
   Allocate one signal shared by all handler ports, one for the spawn
   handshake, and a timer port with one timer.device request for every
   WaitForChar.
2. **Main loop.** One `WaitSelect` covers the listening socket (while fewer
   than `MAXSESSIONS` sessions are live), every session's socket (read when
   its console can take input, write when its output buffer holds data),
   Ctrl-C, the shared port signal and the timer signal. Each pass serves the
   timer, drains every handler port, steps every session (queued writes,
   output, input, reads, lifecycle), then accepts.
3. **Per-session state.** `struct Session` wraps a portable `struct Console`
   (input decoder, ready queue, output buffer, line editor and history, screen
   geometry - the code `make test` compiles on the host) with the Amiga parts:
   its handler port, READ and WRITE queues, handle count and break task.
4. **Handler ports.** Each session has its own port, built by hand on the
   shared signal bit (a CreateMsgPort per session would exhaust the 16 user
   signals). A packet's session is the port it arrives on, so sessions never
   see each other's traffic. When a session ends its port is retired, not
   freed: retired ports are still drained (READ gets break/EOF, WRITE is
   discarded) and reused for new sessions, because a process started from a
   session (a `run` job) keeps the port as its console task. Only such a
   leftover can ever meet a later session, after its port has been reused.
5. **Starting a shell.** Make the socket non-blocking, negotiate (character
   mode with server echo and NAWS, or nothing with `DUMBTERM`), allocate two
   filehandles (`AllocDosObject` → `MKBADDR`) with `fh_Type` = the session's
   port, `fh_Port` non-zero (interactive), `fh_Pos = fh_End = -1`. A
   short-lived helper process copies them, signals the daemon (so the next
   session cannot overwrite them first), runs `SystemTags("NewShell *",
   SYS_Input=in, SYS_Output=out, NP_ConsoleTask=port, NP_Cli)` synchronously,
   then `Forbid(); Close(); Close()` as telnetd 2.0 does (synchronous System()
   does not close them). `SystemTags(SYS_Asynch)` from the daemon itself hung
   on hardware: the daemon is the handler of those handles. NewShell opens
   `*`, and those FIND packets go to the console task, the session's port.
6. **Output.** Everything for the client goes into the session's 4 KB output
   buffer and is sent as the socket takes it. A WRITE is copied in chunks,
   keeping 1 KB free for the line editor, and answered only when all of it is
   queued - a slow client blocks only its own shell. A client that takes
   nothing for 60 s is hung up. While a WRITE is half queued no input is
   taken, so typed keys are never echoed into the middle of output.
7. **Console.** In cooked mode the daemon is the line editor (echo, cursor
   keys, 16-line history), as CON: is on a real Amiga; raw mode passes keys
   through. The editor learns the window width with telnet NAWS and follows
   the client's cursor column through everything it sends, so wrapped lines
   and typeahead during command output are drawn correctly. `DUMBTERM` turns
   the editor off for non-ANSI clients: they edit lines themselves (telnet
   line mode).
8. **Session end.** When every handle on the port is closed (only after
   NewShell has opened `*`), the remaining output is sent (up to 5 s), the
   socket closed and the port retired.
9. **Hangup or Ctrl-C.** Signal Ctrl-C to the reading process, answer every
   queued and future READ with break (0 bytes, `ERROR_BREAK`) so the shell
   ends by itself, close the socket. A shell that has not ended after 3 s is
   left detached: its session stays until its handles close, without
   counting against `MAXSESSIONS`. Ctrl-C to the daemon hangs up every session
   and waits (still serving packets) for them and for every spawn helper,
   which runs code in the daemon's seglist, up to 3 s.
10. **Exit.** Ports still in use and retired ports are left allocated with
    `PA_IGNORE`: a process that still has one as console task then blocks on
    a late packet instead of writing into freed memory.

All DOS I/O of the daemon itself (`PutStr`, `LOG=` trace) goes through its own
`pr_MsgPort` and never meets shell traffic. All socket calls stay in the
process that opened bsdsocket.library (AmiTCP_NG enforces this per SocketBase).

## License

MIT. Architecture follows the approach of telnetd 2.0 (Simons & Holland) — no code from the GPLv2 original.
