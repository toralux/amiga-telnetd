/*
 * telnetd.c — a really simple standalone telnet daemon for AmigaOS 2.04+
 *
 * No inetd. No config files. No user database. No UI. One connection at a
 * time. Listens on port 23 by default, or PORT given as the only argument.
 *
 * Mechanism: for each accepted connection the daemon builds a DOS
 * filehandle whose handler is a packet loop in this very process, backed
 * by the socket. System("NewShell *") then runs an AmigaDOS CLI attached
 * to that handle, giving a real interactive shell over telnet. When the
 * shell exits (EndCLI), the handle is closed, the socket shuts down, and
 * the daemon accepts the next connection.
 *
 * Minimal telnet negotiation: on connect the daemon requests character
 * mode with server echo (WILL ECHO, WILL SGA, DONT LINEMODE) and refuses
 * every other option, so stock clients work without manual toggling.
 * Output is translated to NVT CRLF. Ctrl-C works at any time - between
 * connections and mid-session: socket reads are interruptible, and a
 * client that vanishes (or a shell that dies without ending its session)
 * no longer hangs the daemon (the 0.1 wedge).
 *
 * Build (cross toolchain on x64 Linux):
 *   m68k-amigaos-gcc -Os -m68000 -Wall -o telnetd telnetd.c -s
 *   (or: make)
 *
 * Run (Amiga, TCP/IP stack up, e.g. AmiTCP/AmiTCP_NG/Roadshow):
 *   1> stack 20000
 *   1> telnetd          ; port 23
 *   1> telnetd 2323     ; custom port
 * Stop with Ctrl-C in the starting Shell - works between connections
 * and mid-session.
 *
 * WARNING: no authentication. LAN use only — never expose to the internet.
 *
 * Copyright 2026 Tor Anders Johansen (toralux) — MIT license.
 * The packet-handler approach follows the classic AmiTCP-era daemons
 * (telnetd 2.0 / fakesr.device by P. Simons & S. Holland; ttyhandler by
 * K. Melkko), collapsed into one small file with no extra components.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/alib.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <proto/bsdsocket.h>

#include <string.h>

#ifndef MKBADDR
#define MKBADDR(x) ((BPTR)(((ULONG)(x)) >> 2))
#endif

static const char __attribute__((used)) verstag[] =
    "$VER: telnetd 0.1.1 (6.10.2026)";

/* Telnet protocol bytes we care about (minimal NVT negotiation) */
#define TEL_IAC      255
#define TEL_DONT     254
#define TEL_DO       253
#define TEL_WONT     252
#define TEL_WILL     251
#define OPT_ECHO       1
#define OPT_SGA        3
#define OPT_LINEMODE  34

#define IOBUF        2048
#define ACT_READ     82     /* 'R' */
#define ACT_WRITE    87     /* 'W' */
#define ACT_END      1007
#define ERR_UNKNOWN  503    /* ERROR_ACTION_NOT_KNOWN */

struct Library *SocketBase = NULL;
static struct MsgPort *hdlPort   = NULL;
static int              gListen   = -1;
static int              gSock     = -1;
static BOOL             gDone     = FALSE;
static BOOL             gBreak    = FALSE;

/* Hand-built DOS FileHandle image: fh_Type = our port, fh_Arg1 = socket */
struct MiniFH {
    struct MsgPort *fh_Type;
    LONG  fh_ID, fh_Mode;
    BPTR  fh_Buf, fh_Pos;
    LONG  fh_End;
    ULONG fh_Arg1;
};

static void send_iac(int cmd, int opt);

/* Filter telnet IAC sequences; map CR and CRLF to LF. Returns kept bytes. */
static LONG filter_input(unsigned char *buf, LONG n)
{
    LONG i, w = 0, state = 0, esc = 0, cmd = 0;
    for (i = 0; i < n; i++) {
        unsigned char ch = buf[i];
        if (state == 1) {                    /* IAC command byte */
            if (ch == 250) state = 2;        /* SB ... find SE */
            else if (ch == 255) { state = 0; buf[w++] = ch; }
            else if (ch >= 251) { cmd = ch; state = 3; }
            else state = 0;
            continue;
        }
        if (state == 3) {                    /* option byte of a negotiation */
            state = 0;
            if (cmd == TEL_DO && (ch == OPT_ECHO || ch == OPT_SGA))
                send_iac(TEL_WILL, ch);      /* keep the two we asked for */
            else if (cmd == TEL_DO)
                send_iac(TEL_WONT, ch);      /* refuse everything else */
            else if (cmd == TEL_WILL)
                send_iac(TEL_DONT, ch);      /* and want nothing back */
            continue;                        /* DONT/WONT: silence is fine */
        }
        if (state == 2) {                    /* inside SB */
            if (ch == 240) state = 0;        /* SE */
            else if (ch == 255) esc = !esc;
            continue;
        }
        if (ch == 255) { state = 1; continue; }
        if (ch == 13) {                      /* CR / CRLF -> LF */
            buf[w++] = 10;
            if (i + 1 < n && buf[i + 1] == 10) i++;
            continue;
        }
        buf[w++] = ch;
    }
    return w;
}

/* Send everything, tolerate partial sends. */
static LONG send_all(int s, const unsigned char *p, LONG n)
{
    LONG sent = 0;
    while (sent < n) {
        LONG k = send(s, (APTR)(p + sent), (int)(n - sent), 0);
        if (k <= 0) return -1;
        sent += k;
    }
    return sent;
}

/* Send one 3-byte telnet command. */
static void send_iac(int cmd, int opt)
{
    unsigned char seq[3] = { TEL_IAC, cmd, opt };
    send_all(gSock, seq, 3);
}

/* Kick the client into character mode with server echo: this is what makes
 * stock telnet clients usable without manual "mode character" toggling. */
static void negotiate_start(void)
{
    send_iac(TEL_WILL, OPT_ECHO);
    send_iac(TEL_WILL, OPT_SGA);
    send_iac(TEL_DONT, OPT_LINEMODE);
}

/* ---------------- the micro handler (one connection) ------------------- */

#define RW_BREAK (-2)          /* recv_wait(): woken by SIGBREAKF_CTRL_C */

/* Close the session socket if it is still open. */
static void close_session(void)
{
    if (gSock >= 0) {
        shutdown(gSock, 2);
        CloseSocket(gSock);
        gSock = -1;
    }
}

/* Block until the socket has data or SIGBREAKF_CTRL_C arrives, then recv().
 * A plain blocking recv() sleeps inside bsdsocket and never sees Ctrl-C -
 * that is how 0.1 could wedge forever. Returns recv()'s result (data > 0,
 * 0/-1 for EOF/error) or RW_BREAK. */
static LONG recv_wait(int s, unsigned char *buf, LONG n)
{
    for (;;) {
        fd_set  rd;
        ULONG   mask = SIGBREAKF_CTRL_C;
        LONG    selr;

        FD_ZERO(&rd);
        FD_SET(s, &rd);
        selr = WaitSelect(s + 1, &rd, NULL, NULL, NULL, &mask);
        if (mask & SIGBREAKF_CTRL_C)
            return RW_BREAK;
        if (selr > 0 && FD_ISSET(s, &rd))
            return recv(s, (APTR)buf, (int)n, 0);
        /* spurious wake or transient: wait again */
    }
}

/* Serves the packet loop for one connection. Returns TRUE when the shell
 * ended the session cleanly (ACTION_END); FALSE when the session was torn
 * down (Ctrl-C, vanished client, or a shell that never sent END). In the
 * FALSE case the shell may still be alive: its handle is then leaked (a
 * few bytes) rather than freed under a live shell. */
static BOOL handle_session(void)
{
    static unsigned char buf[IOBUF];
    struct Message *msg;
    struct DosPacket *pkt;
    BOOL  ended = FALSE, gone = FALSE, clean = FALSE;
    LONG  grace = 0;

    /* Reply with errors to any packets a previously abandoned shell still
     * owes, so they cannot leak into this session. */
    while ((msg = GetMsg(hdlPort)) != NULL) {
        pkt = (struct DosPacket *)msg->mn_Node.ln_Name;
        ReplyPkt(pkt, DOSFALSE, ERR_UNKNOWN);
    }

    while (!ended) {
        if (gone) {
            /* The client is gone. A healthy shell reads EOF and sends END;
             * one that died mid-write never does, so do not wait forever:
             * poll briefly for END, then abandon the session. */
            Delay(10);                          /* 0.2 s */
            if (++grace > 25) {                 /* ~5 s cap */
                PutStr((STRPTR)"telnetd: shell did not end - abandoning\n");
                break;
            }
        } else {
            ULONG sigs = Wait(1UL << hdlPort->mp_SigBit | SIGBREAKF_CTRL_C);
            if (sigs & SIGBREAKF_CTRL_C) {
                PutStr((STRPTR)"telnetd: break - closing session\n");
                gBreak = TRUE;
                break;
            }
        }

        while ((msg = GetMsg(hdlPort)) != NULL) {
            pkt = (struct DosPacket *)msg->mn_Node.ln_Name;

            switch (pkt->dp_Type) {
            case ACT_READ: {
                LONG n = -1, k, r = 0;
                for (;;) {
                    if (gSock < 0) break;                /* already closed */
                    r = recv_wait(gSock, buf, IOBUF);
                    if (r == RW_BREAK) break;            /* Ctrl-C */
                    if (r <= 0) break;                   /* EOF / error */
                    n = filter_input(buf, r);
                    if (n > 0) break;                    /* real input */
                    /* a pure-IAC burst: keep waiting for data */
                }
                if (r == RW_BREAK) {
                    ReplyPkt(pkt, 0, 0);                 /* EOF: unblock shell */
                    gBreak = TRUE;
                    ended = TRUE;
                    break;
                }
                if (n <= 0) {
                    ReplyPkt(pkt, 0, 0);                 /* EOF: client gone */
                    gone = TRUE;
                    break;
                }
                k = send_all(gSock, buf, n);             /* server-side echo */
                if (k < 0) { ReplyPkt(pkt, DOSFALSE, Errno()); gone = TRUE; break; }
                ReplyPkt(pkt, n, 0);
                break;
            }
            case ACT_WRITE: {
                const unsigned char *p = (const unsigned char *)pkt->dp_Arg2;
                LONG n = pkt->dp_Arg3, i, w = 0, k = 0;
                for (i = 0; i < n && k >= 0; i++) {
                    unsigned char c = p[i];
                    buf[w++] = c;
                    if (c == 10 && (i == 0 || p[i - 1] != 13)) {
                        buf[w - 1] = 13;                 /* bare LF -> CRLF */
                        buf[w++] = 10;
                    }
                    if (w >= IOBUF - 2) {                /* room for a CRLF pair */
                        k = send_all(gSock, buf, w);
                        w = 0;
                    }
                }
                if (k >= 0 && w > 0) k = send_all(gSock, buf, w);
                if (k < 0) { ReplyPkt(pkt, DOSFALSE, Errno()); gone = TRUE; }
                else       ReplyPkt(pkt, n, 0);
                break;
            }
            case ACT_END:
                ReplyPkt(pkt, DOSTRUE, 0);
                ended = TRUE;
                clean = TRUE;                            /* shell is done */
                break;
            default:
                ReplyPkt(pkt, DOSFALSE, ERR_UNKNOWN);
                break;
            }
            if (ended) break;
        }
    }
    close_session();
    return clean;
}


/* ------------------------------- main ---------------------------------- */

int main(int argc, char **argv)
{
    LONG args[2] = { 0, 0 };
    struct RDArgs *rd;
    struct sockaddr_in sa;
    struct MiniFH *fh = NULL;
    BPTR fhB = 0;
    ULONG port = 23;
    int one = 1;
    struct Message *msg;
    struct Process *self = (struct Process *)FindTask(NULL);
    APTR oldwinptr = self->pr_WindowPtr;

    (void)argc; (void)argv;

    rd = ReadArgs((STRPTR)"PORT/N", args, NULL);
    if (rd == NULL) {
        PutStr((STRPTR)"telnetd: bad arguments\n");
        return RETURN_FAIL;
    }
    if (args[0]) port = *(ULONG *)args[0];

    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 3);
    if (!SocketBase) {
        PutStr((STRPTR)"telnetd: no bsdsocket.library — is the TCP/IP stack up?\n");
        FreeArgs(rd);
        return RETURN_FAIL;
    }

    /* Daemon hygiene (pattern from amiagent): a client-run command naming an
     * unmounted volume must never pop a DOS requester — nobody is at the
     * machine to click Cancel, and the daemon would park. -1 fails instead.
     * The SystemTags child inherits this. Restored on the way out. */
    self->pr_WindowPtr = (APTR)-1;

    hdlPort = CreatePort((STRPTR)"telnetd.handler", 0);
    if (!hdlPort) { PutStr((STRPTR)"telnetd: no port\n"); goto out; }

    gListen = socket(AF_INET, SOCK_STREAM, 0);
    if (gListen < 0) { PutStr((STRPTR)"telnetd: socket failed\n"); goto out; }

    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port        = htons((UWORD)port);

    setsockopt(gListen, SOL_SOCKET, SO_REUSEADDR, (APTR)&one, sizeof one);
    if (bind(gListen, (struct sockaddr *)&sa, sizeof sa) < 0) {
        PutStr((STRPTR)"telnetd: bind failed (port in use?)\n");
        goto out;
    }
    if (listen(gListen, 1) < 0) { PutStr((STRPTR)"telnetd: listen failed\n"); goto out; }

    Printf((STRPTR)"telnetd: listening on port %ld — Ctrl-C stops\n", port);

    for (;;) {
        fd_set rdset;
        ULONG mask;
        int csock;

        FD_ZERO(&rdset);
        FD_SET(gListen, &rdset);
        mask = SIGBREAKF_CTRL_C;
        {
            LONG selr = WaitSelect(gListen + 1, &rdset, NULL, NULL, NULL, &mask);
            if (mask & SIGBREAKF_CTRL_C) { PutStr((STRPTR)"telnetd: break\n"); break; }
            if (selr <= 0) continue;             /* transient — retry */
        }
        csock = accept(gListen, NULL, NULL);
        if (csock < 0) continue;

        gSock  = csock;
        gDone  = FALSE;
        gBreak = FALSE;
        negotiate_start();

        fh = AllocMem(sizeof(struct MiniFH), MEMF_CLEAR | MEMF_PUBLIC);
        if (!fh) { CloseSocket(gSock); gSock = -1; continue; }
        fh->fh_Type = hdlPort;
        fh->fh_Arg1 = (ULONG)gSock;
        fhB = MKBADDR(fh);

        PutStr((STRPTR)"telnetd: connection — starting shell\n");
        if (SystemTags((STRPTR)"NewShell *",
                       SYS_Input,   fhB,
                       SYS_Output,  fhB,
                       SYS_Asynch,  TRUE,
                       SYS_UserShell, TRUE,
                       NP_StackSize, 65536,
                       TAG_DONE) == -1) {
            PutStr((STRPTR)"telnetd: could not start shell\n");
            Close(fhB);
            continue;
        }
        /* serves packets until the shell ends (or the session is torn down) */
        if (handle_session()) {
            FreeMem(fh, sizeof(struct MiniFH));
        } else {
            /* an abandoned shell may still reference the handle: leak the
             * few bytes instead of freeing them under its feet */
            PutStr((STRPTR)"telnetd: session handle kept (abandoned shell)\n");
        }
        fh = NULL;
        /* drain straggler packets so the next session starts clean */
        while ((msg = GetMsg(hdlPort)) != NULL)
            ReplyPkt((struct DosPacket *)msg->mn_Node.ln_Name, DOSFALSE, ERR_UNKNOWN);

        if (gBreak) { PutStr((STRPTR)"telnetd: stopped\n"); break; }
        PutStr((STRPTR)"telnetd: session closed — waiting for next\n");
    }

out:
    if (gListen >= 0) CloseSocket(gListen);
    if (hdlPort) DeletePort(hdlPort);
    if (SocketBase) CloseLibrary(SocketBase);
    self->pr_WindowPtr = oldwinptr;
    FreeArgs(rd);
    return RETURN_OK;
}
