# telnetd v0.2 — Design

Architecture ported from **telnetd 2.0** (Peter Simons & Steve Holland, 1995, GPLv2) —
the "star path": telnetd redirects the shell's stdio over the socket via a DosPacket
filehandle served by a dedicated session process. The fakesr.device path is NOT used
(2.0's own docs call it experimental).

## Why 0.1 failed

0.1-0.1.5 served DosPackets on the main daemon's port and spawned the shell with
`System("NewShell *", SYS_Asynch, SYS_UserShell)` from the daemon itself. The proven
program does it differently in every dimension:

| aspect | 0.1 (failed) | 2.0 (proven) |
|---|---|---|
| packet server | main daemon | dedicated session process |
| handle port | custom port | session process pr_MsgPort |
| shell spawn | daemon, SYS_Asynch + SYS_UserShell | dedicated spawner process, synchronous, NP_Cli + NP_ConsoleTask=fh port |
| packets answered | READ/WRITE/END only | + FIND* (re-wire), WAIT_CHAR (timer.device), SCREEN_MODE (raw/echo), SEEK |
| read semantics | raw fill | line-oriented (CR/LF completes the read) |
| WaitChar | none | timer.device UNIT_MICROHZ |

## v0.2 architecture (per connection: 3 processes)

1. **Main daemon** — proven accept loop from 0.1.4/0.1.5 (WaitSelect + CTRL-C,
   interruptible everywhere, watchdogs kept). On accept: CreateNewProcTags the session
   process (NP_Entry, NP_Arguments "<socket fd>", NP_StackSize 64K). Never blocks on
   sessions. AmiTCP socket IDs are system-global, so the child uses the fd directly
   (as inetd passes sockets to 2.0).
2. **Session process** — opens its own bsdsocket; telnet negotiation; creates the
   DOS FileHandle (AllocDosObject, fh_Pos/fh_End = -1, fh_Type = fh_Port = own
   pr_MsgPort, fh_Arg1 = socket); spawns the shell-spawner process; runs the packet
   loop ported from 2.0:
   - FINDINPUT/FINDOUTPUT/FINDUPDATE: re-wire the passed handle to this session, reply DOSTRUE
   - READ: queue async; complete line-oriented from socket (CR/LF ends the read)
   - WRITE: immediate send with CRLF translation + IAC escaping, reply length
   - WAIT_CHAR: timer.device TR_ADDREQUEST; satisfied by incoming char or timeout
   - SCREEN_MODE: toggle raw mode + telnet echo negotiation
   - END: opencount--; zero = teardown (abort pending timers, close, exit)
   - SEEK: ERROR_OBJECT_WRONG_TYPE; default: ACTION_NOT_KNOWN
   - hangup (recv 0): stop servicing reads, tear down
3. **Shell spawner process** — SystemTags("NewShell *", SYS_Input=fhB, SYS_Output=fhB,
   NP_ConsoleTask=fh->fh_Type, NP_Cli=TRUE) synchronously in its own process; then
   Close(fhB) and exit. (System() blocks here for the lifetime of the shell — that is
   fine and intended; it is a throwaway process.)

No login, no usergroup.library (LAN-only, like the token-less amiagent).
Ctrl-C stops the daemon; sessions detect hangup/END and exit on their own.

## License

GPLv2 (telnetd 2.0 port). Credit: Peter Simons & Steve Holland.

