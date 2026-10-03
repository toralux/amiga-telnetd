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
 * Telnet option negotiation is not implemented: IAC sequences are
 * filtered on input, and typed characters are echoed by the daemon. If
 * your client line-edits oddly, switch it to character mode
 * (Ctrl-] then "mode character" in the BSD telnet client).
 *
 * Build (cross toolchain on x64 Linux):
 *   m68k-amigaos-gcc -Os -m68000 -Wall -o telnetd telnetd.c -s
 *   (or: make)
 *
 * Run (Amiga, TCP/IP stack up, e.g. AmiTCP/AmiTCP_NG/Roadshow):
 *   1> stack 20000
 *   1> telnetd          ; port 23
 *   1> telnetd 2323     ; custom port
 * Stop with Ctrl-C in the starting Shell (between connections; during a
 * session, close the telnet client first).
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

#include <proto/exec.h>
#include <proto/dos.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <proto/bsdsocket.h>

#include <string.h>

#ifndef MKBADDR
#define MKBADDR(x) ((BPTR)(((ULONG)(x)) >> 2))
#endif

#define IOBUF        2048
#define ACT_READ     82     /* 'R' */
#define ACT_WRITE    87     /* 'W' */
#define ACT_END      1007
#define ERR_UNKNOWN  503    /* ERROR_ACTION_NOT_KNOWN */

static struct Library *SocketBase = NULL;
static struct MsgPort *hdlPort   = NULL;
static int              gListen   = -1;
static int              gSock     = -1;
static BOOL             gDone     = FALSE;

/* Hand-built DOS FileHandle image: fh_Type = our port, fh_Arg1 = socket */
struct MiniFH {
    struct MsgPort *fh_Type;
    LONG  fh_ID, fh_Mode;
    BPTR  fh_Buf, fh_Pos;
    LONG  fh_End;
    ULONG fh_Arg1;
};

/* Filter telnet IAC sequences; map CR and CRLF to LF. Returns kept bytes. */
static LONG filter_input(unsigned char *buf, LONG n)
{
    LONG i, w = 0, state = 0, esc = 0;
    for (i = 0; i < n; i++) {
        unsigned char ch = buf[i];
        if (state == 1) {                    /* IAC command byte */
            if (ch == 250) state = 2;        /* SB ... find SE */
            else if (ch == 255) { state = 0; buf[w++] = ch; }
            else state = 0;
            continue;
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
        LONG k = send(s, (const char *)p + sent, (int)(n - sent), 0);
        if (k <= 0) return -1;
        sent += k;
    }
    return sent;
}

/* ---------------- the micro handler (one connection) ------------------- */

static void handle_session(void)
{
    static unsigned char buf[IOBUF];
    struct Message *msg;
    struct DosPacket *pkt;
    BOOL ended = FALSE;

    while (!ended) {
        Wait(1UL << hdlPort->mp_SigBit | SIGBREAKF_CTRL_C);

        while ((msg = GetMsg(hdlPort)) != NULL) {
            pkt = (struct DosPacket *)msg->mn_Node.ln_Name;   /* msg -> packet */

            switch (pkt->dp_Type) {
            case ACT_READ: {
                LONG n, k, r;
                for (;;) {
                    r = recv(gSock, (char *)buf, IOBUF, 0);
                    if (r < 0) { ReplyPkt(pkt, DOSFALSE, Errno()); n = -1; break; }
                    if (r == 0) { ReplyPkt(pkt, 0, 0); n = -1; break; }  /* EOF */
                    n = filter_input(buf, r);
                    if (n > 0) break;            /* skip pure-IAC packets */
                }
                if (n <= 0) break;
                k = send_all(gSock, buf, n);     /* server-side echo */
                if (k < 0) { ReplyPkt(pkt, DOSFALSE, Errno()); break; }
                ReplyPkt(pkt, n, 0);
                break;
            }
            case ACT_WRITE: {
                LONG n = pkt->dp_Arg3;
                LONG k = send_all(gSock, (const unsigned char *)pkt->dp_Arg2, n);
                if (k < 0) ReplyPkt(pkt, DOSFALSE, Errno());
                else       ReplyPkt(pkt, k, 0);
                break;
            }
            case ACT_END:
                ReplyPkt(pkt, DOSTRUE, 0);
                ended = TRUE;                     /* shell is done */
                break;
            default:
                ReplyPkt(pkt, DOSFALSE, ERR_UNKNOWN);
                break;
            }
        }
    }
    /* drain / close */
    shutdown(gSock, 2);
    CloseSocket(gSock);
    gSock = -1;
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

    hdlPort = CreatePort((STRPTR)"telnetd.handler", 0);
    if (!hdlPort) { PutStr((STRPTR)"telnetd: no port\n"); goto out; }

    gListen = socket(AF_INET, SOCK_STREAM, 0);
    if (gListen < 0) { PutStr((STRPTR)"telnetd: socket failed\n"); goto out; }

    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port        = htons((UWORD)port);

    setsockopt(gListen, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
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
                       TAG_DONE) == -1) {
            PutStr((STRPTR)"telnetd: could not start shell\n");
            Close(fhB);
            continue;
        }
        handle_session();           /* serves packets until shell exits */
        FreeMem(fh, sizeof(struct MiniFH));
        fh = NULL;
        PutStr((STRPTR)"telnetd: session closed — waiting for next\n");
    }

out:
    if (gListen >= 0) CloseSocket(gListen);
    if (hdlPort) DeletePort(hdlPort);
    if (SocketBase) CloseLibrary(SocketBase);
    FreeArgs(rd);
    return RETURN_OK;
}
