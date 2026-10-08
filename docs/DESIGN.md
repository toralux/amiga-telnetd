# telnetd v0.4 — Design

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

## v0.4 architecture (one process, one session at a time)

1. **Startup.** Refuse to run on less than 16000 bytes of stack. bsdsocket
   calls run on the caller's stack, and a 68000 has no MMU to catch an overrun.
   Allocate one signal shared by all handler ports, and a timer port with one
   timer.device request for every WaitForChar.
2. **Accept loop.** `WaitSelect` on the listener with a 2 s timeout, Ctrl-C and
   the handler-port signal. Packets on retired ports (below) are answered
   here: READ gets break/EOF, WRITE is discarded.
3. **Session.** Make the socket non-blocking (FIONBIO) and put the client in
   character mode (WILL ECHO, WILL SGA). Allocate two filehandles
   (`AllocDosObject` → `MKBADDR`) with `fh_Type` = the session's own handler
   port (a retired one with no handles left on it, or a new one), `fh_Port`
   non-zero (interactive), `fh_Pos = fh_End = -1`. A short-lived helper process
   runs `SystemTags("NewShell *", SYS_Input=in, SYS_Output=out,
   NP_ConsoleTask=port, NP_Cli)` synchronously, then closes both handles
   (`Forbid(); Close(); Close()`, as telnetd 2.0 does: synchronous System()
   does not close them). Calling `SystemTags(SYS_Asynch)` from the daemon
   itself hung on hardware (v0.4). NewShell opens `*`, and those FIND packets
   go to the console task, the session's port.
4. **Packet loop.** It serves FIND*/READ/WRITE/WAIT_CHAR/SCREEN_MODE/
   CHANGE_SIGNAL/END/SEEK/DISK_INFO/IS_FILESYSTEM. Handles are counted, and
   the session ends when the count reaches zero (only after NewShell has
   opened `*`, so a late FIND cannot miss the session). In cooked mode the
   daemon is the line editor (echo, cursor keys, 16-line history), as CON: is
   on a real Amiga; raw mode passes keys through. The editor learns the
   window width with telnet NAWS and follows the client's cursor column
   through everything it sends, so wrapped lines and typeahead during
   command output are drawn correctly. `DUMBTERM` turns the editor off for
   non-ANSI clients: they edit lines themselves (telnet line mode). The
   editor and input decoder are tested on the host (`make test`,
   tests/edtest.c). One timer.device request,
   opened at startup, times every pending WaitForChar.

   A packet's session is the port it arrives on, so a process left over
   from an earlier session can never be taken for the current one.
5. **Hangup or Ctrl-C.** Signal Ctrl-C to the reading process and answer
   every queued READ with break (0 bytes, `ERROR_BREAK`), so the shell ends
   by itself - then end the session at once, without waiting for it: the
   port is retired with the number of handles still open on it. A retired
   port keeps answering (READ break/EOF, WRITE discarded, FIND/END counted on
   the port) and is reused only once no handle is left on it, so a shell
   that never reacts (a command that ignores Ctrl-C and never reads) costs
   one port and delays nobody: the next client is served at once.
6. **Exit.** Ctrl-C ends the session as above, then waits (still answering
   packets) up to 5 s for hung-up shells to end, and with no time limit for
   the spawn helper: it runs code in the daemon's seglist, which must not be
   unloaded under it. Every port is left allocated with `PA_IGNORE`: a
   process that still has one as console task then blocks on a late packet,
   which is better than writing into freed memory.

All DOS I/O of the daemon itself (`PutStr`, `LOG=` trace) goes through its own
`pr_MsgPort` and can no longer meet shell traffic. All socket calls stay in the
process that opened bsdsocket.library (AmiTCP_NG enforces this per SocketBase).

## License

MIT. Architecture follows the approach of telnetd 2.0 (Simons & Holland) — no code from the GPLv2 original.
