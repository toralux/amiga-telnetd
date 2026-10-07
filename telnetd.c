/*
 * telnetd.c — a standalone telnet daemon for AmigaOS 2.04+
 * v0.2: session architecture ported from telnetd 2.0 (Peter Simons &
 * Steve Holland, 1995, GPLv2), adapted for AmiTCP_NG 4.x: no inetd, no
 * usergroup.library, one connection at a time, LAN-only.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * Mechanism (the telnetd 2.0 "star path", proven since 1995):
 *   For each accepted connection the daemon builds a DOS filehandle whose
 *   handler is THIS process's pr_MsgPort (fh_Type/fh_Port), with the
 *   unbuffered sentinels fh_Pos=fh_End=-1. System("NewShell *") is called
 *   asynchronously with that handle as SYS_Input/SYS_Output, NP_Cli, and
 *   NP_ConsoleTask pointed at our port. The shell's stdio then arrives as
 *   DosPackets on pr_MsgPort, which the session loop answers:
 *     ACTION_READ        — queued; completed line-oriented (CR/LF ends it)
 *     ACTION_WRITE       — sent immediately, LF -> CRLF, IAC escaped
 *     ACTION_WAIT_CHAR   — WaitForChar() via timer.device UNIT_MICROHZ
 *     ACTION_SCREEN_MODE — SetMode(): raw/cooked, telnet echo negotiated
 *     ACTION_FIND*       — handle re-wired to this session (opencount++)
 *     ACTION_END         — opencount--; zero ends the session
 *     ACTION_SEEK        — not seekable
 *     default            — ACTION_NOT_KNOWN (v0.1 wrongly used 503)
 *
 * Build (cross toolchain on x64 Linux):
 *   m68k-amigaos-gcc -Os -m68000 -Wall -Wextra -o telnetd telnetd.c -s -lamiga
 *
 * Run:  1> stack 20000
 *       1> telnetd            ; port 23
 *       1> telnetd 2323       ; custom port
 * Stop with Ctrl-C in the starting Shell.
 *
 * WARNING: no authentication. LAN use only — never expose to the internet.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/devices.h>
#include <devices/timer.h>
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
#include <errno.h>

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#ifndef NewList
#define NewList(l)  ((l)->lh_Head = (struct Node *)&(l)->lh_Tail, \
                     (l)->lh_Tail = NULL, \
                     (l)->lh_TailPred = (struct Node *)&(l)->lh_Head)
#endif

#ifndef MKBADDR
#define MKBADDR(x) ((BPTR)(((ULONG)(x)) >> 2))
#endif
#ifndef BADDR
#define BADDR(x)  ((APTR)(((ULONG)(x)) << 2))
#endif

static const char __attribute__((used)) verstag[] =
    "$VER: telnetd 0.3 (6.10.2026)";

/* Telnet protocol bytes */
#define TEL_IAC      255
#define TEL_DONT     254
#define TEL_DO       253
#define TEL_WONT     252
#define TEL_WILL     251
#define TEL_SB       250
#define TEL_SE       240
#define OPT_ECHO       1
#define OPT_SGA        3
#define OPT_LINEMODE  34

#define IOBUF        1024

struct Library *SocketBase = NULL;

/* Debug breadcrumbs that SURVIVE a crash: open-append-close per event,
 * so DOS buffering cannot eat the trail of a suspended daemon. */
static void dbglog(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    BPTR f;
    LONG len;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    len = (LONG)strlen(buf);
    f = Open((STRPTR)"Data:tdbg.log", MODE_READWRITE);
    if (!f) return;
    Seek(f, 0, OFFSET_END);
    Write(f, (APTR)buf, len);
    Close(f);
}   /* extern in proto/bsdsocket.h */
static int              gListen   = -1;
static int              gSock     = -1;
static int              gBreak    = FALSE;

/* telnetd 2.0 negotiation strings (screen-mode switches) */
static const unsigned char NoEcho[] = { TEL_IAC,TEL_WILL,OPT_ECHO, TEL_IAC,TEL_WILL,OPT_SGA, 0 };
static const unsigned char EchoOn[] = { TEL_IAC,TEL_WONT,OPT_ECHO, TEL_IAC,TEL_WONT,OPT_SGA,
                                        TEL_IAC,TEL_DONT,OPT_LINEMODE, TEL_IAC,TEL_WONT,OPT_SGA, 0 };


/* ---------------- socket / telnet helpers (ported from telnetd 2.0) ----- */

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

static void negotiate_start(void)
{
    static const unsigned char seq1[] = { TEL_IAC,TEL_WILL,OPT_ECHO };
    static const unsigned char seq2[] = { TEL_IAC,TEL_WILL,OPT_SGA };
    static const unsigned char seq3[] = { TEL_IAC,TEL_DONT,OPT_LINEMODE };
    send_all(gSock, seq1, 3);
    send_all(gSock, seq2, 3);
    send_all(gSock, seq3, 3);
}

/* Output: bare LF -> CRLF, IAC escaped. Returns 0 ok, -1 send failed. */
static LONG sock_write(const unsigned char *p, LONG n)
{
    static unsigned char buf[IOBUF];
    LONG i, w = 0;
    for (i = 0; i < n; i++) {
        unsigned char c = p[i];
        if (c == 255) {                       /* IAC escape */
            buf[w++] = 255; buf[w++] = 255;
        } else if (c == 10 && (i == 0 || p[i-1] != 13)) {
            buf[w++] = 13; buf[w++] = 10;     /* bare LF -> CRLF */
        } else {
            buf[w++] = c;
        }
        if (w >= IOBUF - 2) { if (send_all(gSock, buf, w) < 0) return -1; w = 0; }
    }
    if (w > 0) { if (send_all(gSock, buf, w) < 0) return -1; }
    return 0;
}

/* Per-character input with telnet IAC filtering.
 * Returns: 1 = char in *loc, 0 = hangup, -1 = no data right now.
 * Static state: single session at a time. */
static LONG recv_char(unsigned char *loc)
{
    static int state = 0;        /* 0 normal, 1 IAC, 2 opt-of-cmd, 3 SB, 4 SB-IAC */
    static int cmd = 0;
    static unsigned char pushback = 0;
    static int have_pushback = 0;

    for (;;) {
        unsigned char ch;
        LONG r;

        if (have_pushback) { ch = pushback; have_pushback = 0; }
        else {
            r = recv(gSock, (APTR)&ch, 1, 0);
            if (r == 0) return 0;                       /* hangup */
            if (r < 0) return -1;
        }

        switch (state) {
        case 0:
            if (ch == 255) { state = 1; continue; }
            if (ch == 13) {                             /* CR: swallow CR NUL / CR LF */
                unsigned char nx;
                r = recv(gSock, (APTR)&nx, 1, 0);
                if (r == 1 && nx != 10 && nx != 0) { pushback = nx; have_pushback = 1; }
                else if (r < 0) return -1;
                *loc = '\n'; return 1;
            }
            *loc = ch; return 1;
        case 1:                                          /* IAC command byte */
            if (ch == 255) { *loc = 255; state = 0; return 1; }  /* escaped IAC */
            if (ch == 250) { state = 3; continue; }     /* SB ... SE */
            if (ch >= 251) { cmd = ch; state = 2; continue; }
            state = 0; continue;                        /* other: ignore */
        case 2: {                                        /* option byte */
            unsigned char rep[3];
            rep[0] = 255;
            if (cmd == TEL_DO && (ch == OPT_ECHO || ch == OPT_SGA)) {
                rep[1] = TEL_WILL; rep[2] = ch; send_all(gSock, rep, 3);
            } else if (cmd == TEL_DO) {
                rep[1] = TEL_WONT; rep[2] = ch; send_all(gSock, rep, 3);
            } else if (cmd == TEL_WILL) {
                rep[1] = TEL_DONT; rep[2] = ch; send_all(gSock, rep, 3);
            }                                            /* DONT/WONT: silence */
            state = 0; continue;
        }
        case 3:                                          /* inside SB */
            if (ch == 255) state = 4;
            continue;
        case 4:                                          /* SB IAC */
            state = (ch == 240) ? 0 : 3;                /* SE ends it */
            continue;
        }
    }
}


/* ---------------- the shell spawner (telnetd 2.0 SubSubProc port) -------
 * 2.0 runs System() SYNCHRONOUSLY inside a dedicated throwaway process -
 * never with SYS_Asynch from the main daemon (that combination is the
 * instrumented crash site: the process died inside SystemTags). The
 * helper gets the filehandle BPTR through NP_Arguments as a decimal
 * string, exactly like 2.0 passes it. */
static BPTR g_spawnFH = 0;   /* set by main before CreateNewProcTags */

static int spawner_entry(void)
{
    struct Library *dosBase;
    BPTR fh = g_spawnFH;
    LONG rc;

    dosBase = OpenLibrary((STRPTR)"dos.library", 36);
    if (!dosBase) return RETURN_FAIL;

    rc = SystemTags((STRPTR)"NewShell *",
                    SYS_Input,      fh,
                    SYS_Output,     fh,
                    NP_ConsoleTask, (LONG)((struct FileHandle *)BADDR(fh))->fh_Type,
                    NP_Cli,         TRUE,
                    TAG_DONE);
    /* not reached until the shell exits; then close and die */
    Close(fh);
    CloseLibrary(dosBase);
    return (int)rc;
}

/* ---------------- the session loop (the telnetd 2.0 star path) ---------- */

static struct DosPacket *pkt_from_msg(struct Message *msg)
{
    return (struct DosPacket *)msg->mn_Node.ln_Name;
}

/* Serves one connection until the shell ends, the client hangs up, or
 * Ctrl-C. Sets gBreak if the daemon should stop afterwards. */
static BOOL session_loop(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    struct MsgPort *pktPort = &me->pr_MsgPort;
    struct MsgPort *timePort = CreateMsgPort();
    struct List readWait;                    /* queued READ / WAIT_CHAR msgs */
    struct Message *msg;
    struct DosPacket *pkt;
    LONG nextChar = -1;                      /* pushback char from IAC layer */
    BOOL hangup = FALSE, ended = FALSE;
    LONG silent = 0;                         /* seconds without packets     */
    NewList(&readWait);

    if (!timePort) return FALSE;

    while (!ended) {
        fd_set rd;
        ULONG mask = (1UL << pktPort->mp_SigBit)
                   | (1UL << timePort->mp_SigBit)
                   | SIGBREAKF_CTRL_C;
        struct timeval tv;
        memset(&tv, 0, sizeof tv);
        tv.tv_secs = 1;
        LONG selr;

        FD_ZERO(&rd);
        if (!hangup && readWait.lh_Head->ln_Succ)
            FD_SET(gSock, &rd);              /* only read while someone waits */

        selr = WaitSelect(gSock + 1, &rd, NULL, NULL, &tv, &mask);

        if (mask & SIGBREAKF_CTRL_C) {
            PutStr((STRPTR)"telnetd: break - closing session\n");
            PutStr((STRPTR)"dbg: ctrl-c in session\n"); dbglog("dbg: ctrl-c in session\n");
            gBreak = TRUE;
            ended = TRUE;
            break;
        }

        /* socket data available: service the head read request */
        if (!hangup && selr > 0 && FD_ISSET(gSock, &rd)
            && readWait.lh_Head->ln_Succ) {
            msg = (struct Message *)readWait.lh_Head;
            pkt = pkt_from_msg(msg);

            if (pkt->dp_Type == ACTION_READ) {
                unsigned char ch = 0;
                LONG got = 1;
                silent = 0;
                while (pkt->dp_Res1 < pkt->dp_Arg3) {
                    if (nextChar != -1) { ch = (unsigned char)nextChar; nextChar = -1; }
                    else { got = recv_char(&ch); }
                    if (got == 0) { hangup = TRUE; break; }
                    if (got < 0) break;      /* no more data right now */
                    *((unsigned char *)pkt->dp_Arg2 + pkt->dp_Res1) = ch;
                    pkt->dp_Res1++;
                    if (ch == '\r' || ch == '\n') break;
                }
                if (got > 0 &&
                    (pkt->dp_Res1 == pkt->dp_Arg3 || ch == '\r' || ch == '\n')) {
                    Remove(msg);
                    PutMsg(pkt->dp_Port, pkt->dp_Link);
                }
            } else if (pkt->dp_Type == ACTION_WAIT_CHAR) {
                unsigned char ch;
                LONG got = (nextChar != -1) ? (ch = (unsigned char)nextChar, nextChar = -1, 1)
                                            : recv_char(&ch);
                if (got == 0) hangup = TRUE;
                else if (got > 0) {
                    struct timerequest *tr = (struct timerequest *)pkt->dp_Res2;
                    if (tr) {
                        AbortIO((struct IORequest *)tr);
                        WaitIO((struct IORequest *)tr);
                        CloseDevice((struct IORequest *)tr);
                        DeleteIORequest((struct IORequest *)tr);
                        pkt->dp_Res2 = 0;
                    }
                    nextChar = ch;
                    pkt->dp_Res1 = DOSTRUE;
                    Remove(msg);
                    PutMsg(pkt->dp_Port, pkt->dp_Link);
                }
            } else {
                Remove(msg);                 /* invalid request on the queue */
            }
        }

        /* timer replies: WAIT_CHAR timeouts */
        {
            struct timerequest *tr;
            while ((tr = (struct timerequest *)GetMsg(timePort)) != NULL) {
                msg = (struct Message *)tr->tr_node.io_Message.mn_Node.ln_Name;
                pkt = pkt_from_msg(msg);
                CloseDevice((struct IORequest *)tr);
                DeleteIORequest((struct IORequest *)tr);
                pkt->dp_Res1 = DOSFALSE;     /* no character in time */
                pkt->dp_Res2 = 0;
                Remove(msg);
                PutMsg(pkt->dp_Port, pkt->dp_Link);
            }
        }

        /* DOS packets from the shell */
        while ((msg = GetMsg(pktPort)) != NULL) {
            silent = 0;
            pkt = pkt_from_msg(msg);
            dbglog("dbg: pkt type %ld\n", (LONG)pkt->dp_Type);
            switch (pkt->dp_Type) {

            case ACTION_FINDINPUT:
            case ACTION_FINDOUTPUT:
            case ACTION_FINDUPDATE: {
                /* The shell (re)opened its stdio: wire that handle to us. */
                struct FileHandle *fh = (struct FileHandle *)BADDR((BPTR)pkt->dp_Arg1);
                if (fh) {
                    fh->fh_Pos  = -1;
                    fh->fh_End  = -1;
                    fh->fh_Type = pktPort;
                    fh->fh_Port = pktPort;
                    fh->fh_Arg1 = (LONG)gSock;
                }
                pkt->dp_Res1 = DOSTRUE;
                PutMsg(pkt->dp_Port, pkt->dp_Link);
                break;
            }

            case ACTION_READ:
                pkt->dp_Res1 = 0;
                AddTail(&readWait, msg);     /* completed when data arrives */
                break;

            case ACTION_WRITE:
                if (sock_write((const unsigned char *)pkt->dp_Arg2,
                               pkt->dp_Arg3) < 0)
                    hangup = TRUE;
                pkt->dp_Res1 = pkt->dp_Arg3;
                PutMsg(pkt->dp_Port, pkt->dp_Link);
                break;

            case ACTION_WAIT_CHAR: {
                struct timerequest *tr;
                pkt->dp_Res1 = DOSFALSE;
                tr = (struct timerequest *)CreateIORequest(timePort,
                                                           sizeof(struct timerequest));
                if (!tr || OpenDevice((STRPTR)"timer.device", UNIT_MICROHZ,
                                      (struct IORequest *)tr, 0)) {
                    if (tr) DeleteIORequest((struct IORequest *)tr);
                    pkt->dp_Res2 = ERROR_NO_FREE_STORE;
                    PutMsg(pkt->dp_Port, pkt->dp_Link);
                    break;
                }
                tr->tr_node.io_Command = TR_ADDREQUEST;
                tr->tr_time.tv_micro   = pkt->dp_Arg1;
                tr->tr_time.tv_secs    = 0;
                tr->tr_node.io_Message.mn_Node.ln_Name = (char *)msg;
                pkt->dp_Res2 = (LONG)tr;
                SendIO((struct IORequest *)tr);
                AddTail(&readWait, msg);
                break;
            }

            case ACTION_SCREEN_MODE:
                send_all(gSock, pkt->dp_Arg1 ? NoEcho : EchoOn,
                         pkt->dp_Arg1 ? 6 : 12);
                pkt->dp_Res1 = DOSFALSE;
                PutMsg(pkt->dp_Port, pkt->dp_Link);
                break;

            case ACTION_END:
                pkt->dp_Res1 = 0;
                PutMsg(pkt->dp_Port, pkt->dp_Link);
                ended = TRUE;                /* shell is done */
                break;

            case ACTION_SEEK:
                pkt->dp_Res1 = -1;
                pkt->dp_Res2 = ERROR_OBJECT_WRONG_TYPE;
                PutMsg(pkt->dp_Port, pkt->dp_Link);
                break;

            default:
                pkt->dp_Res1 = DOSFALSE;
                pkt->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
                PutMsg(pkt->dp_Port, pkt->dp_Link);
                break;
            }
            if (ended || hangup) break;
        }

        /* watchdogs: a silent shell 10s after spawn, or a hung-up client
         * whose shell never sends END, must not park the daemon */
        if (!ended) {
            silent++;
            if (readWait.lh_Head->ln_Succ == NULL && silent > 10 && !hangup) {
                PutStr((STRPTR)"telnetd: shell produced no session - abandoning\n"); dbglog("dbg: abandon: no session\n");
                break;                       /* FALSE: abandoned */
            }
            if (hangup && silent > 5) {
                PutStr((STRPTR)"telnetd: shell did not end - abandoning\n"); dbglog("dbg: abandon: no end\n");
                break;                       /* FALSE: abandoned */
            }
        }
    }

    /* teardown: fail everything still queued */
    while ((msg = (struct Message *)RemHead(&readWait)) != NULL) {
        pkt = pkt_from_msg(msg);
        if (pkt->dp_Type == ACTION_WAIT_CHAR && pkt->dp_Res2) {
            struct timerequest *tr = (struct timerequest *)pkt->dp_Res2;
            AbortIO((struct IORequest *)tr);
            WaitIO((struct IORequest *)tr);
            CloseDevice((struct IORequest *)tr);
            DeleteIORequest((struct IORequest *)tr);
            pkt->dp_Res2 = 0;
        }
        pkt->dp_Res1 = (pkt->dp_Type == ACTION_READ) ? 0 : DOSFALSE;
        PutMsg(pkt->dp_Port, pkt->dp_Link);
    }
    DeleteMsgPort(timePort);
    return ended && !gBreak;                 /* clean only via ACTION_END */
}


/* ------------------------------- main ---------------------------------- */

int main(int argc, char **argv)
{
    LONG args[2] = { 0, 0 };
    struct RDArgs *rd;
    struct sockaddr_in sa;
    struct Process *self = (struct Process *)FindTask(NULL);
    struct Message *msg;
    ULONG port = 23;
    int one = 1;
    APTR oldwinptr = self->pr_WindowPtr;

    (void)argc; (void)argv;

    dbglog("dbg: startup: entered main\n");
    rd = ReadArgs((STRPTR)"PORT/N", args, NULL);
    if (rd == NULL) {
        PutStr((STRPTR)"telnetd: bad arguments\n");
        return RETURN_FAIL;
    }
    dbglog("dbg: startup: ReadArgs ok\n");
    if (args[0]) port = *(ULONG *)args[0];

    dbglog("dbg: startup: opening bsdsocket\n");
    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 3);
    if (!SocketBase) {
        PutStr((STRPTR)"telnetd: no bsdsocket.library - is the TCP/IP stack up?\n");
        FreeArgs(rd);
        return RETURN_FAIL;
    }
    dbglog("dbg: startup: bsdsocket open\n");

    /* No DOS requesters may ever park this headless daemon (telnetd 2.0
     * NoReq / amiagent pr_WindowPtr=-1 pattern). */
    self->pr_WindowPtr = (APTR)-1;

    dbglog("dbg: startup: creating socket\n");
    gListen = socket(AF_INET, SOCK_STREAM, 0);
    if (gListen < 0) { PutStr((STRPTR)"telnetd: socket failed\n"); goto out; }
    dbglog("dbg: startup: socket ok\n");

    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port        = htons((UWORD)port);

    setsockopt(gListen, SOL_SOCKET, SO_REUSEADDR, (APTR)&one, sizeof one);
    if (bind(gListen, (struct sockaddr *)&sa, sizeof sa) < 0) {
        PutStr((STRPTR)"telnetd: bind failed (port in use?)\n");
        goto out;
    }
    dbglog("dbg: startup: bound\n");
    if (listen(gListen, 1) < 0) { PutStr((STRPTR)"telnetd: listen failed\n"); goto out; }
    { BPTR f = Open((STRPTR)"Data:tdbg.log", MODE_NEWFILE); if (f) Close(f); }

    Printf((STRPTR)"telnetd: listening on port %ld - Ctrl-C stops\n", port);
    dbglog("dbg: listening on port %ld\n", (LONG)port);

    for (;;) {
        fd_set rdset;
        ULONG mask = SIGBREAKF_CTRL_C;
        struct FileHandle *fh;
        BPTR fhB;
        BOOL clean;

        FD_ZERO(&rdset);
        FD_SET(gListen, &rdset);
        {
            LONG selr = WaitSelect(gListen + 1, &rdset, NULL, NULL, NULL, &mask);
            if (mask & SIGBREAKF_CTRL_C) { PutStr((STRPTR)"telnetd: break\n"); break; }
            if (selr <= 0) continue;
        }
        {
            struct sockaddr_in ca;
            socklen_t calen = sizeof ca;   /* REAL buffers: NULL addr/len hits an
                                       * address error on this stack (the
                                       * 80000003 crash of v0.1-0.2.1) */
            gSock = accept(gListen, (struct sockaddr *)&ca, &calen);
        }
        if (gSock < 0) { PutStr((STRPTR)"dbg: accept failed\n"); continue; }
        PutStr((STRPTR)"dbg: accepted\n"); dbglog("dbg: accepted\n");

        negotiate_start();
        PutStr((STRPTR)"dbg: negotiated\n"); dbglog("dbg: negotiated\n");

        /* The session handle: handler = our own pr_MsgPort, unbuffered. */
        fhB = (BPTR)AllocDosObject(DOS_FILEHANDLE, NULL);
        if (!fhB) { PutStr((STRPTR)"dbg: no handle\n"); CloseSocket(gSock); gSock = -1; continue; }
        fh = (struct FileHandle *)BADDR(fhB);
        fh->fh_Type = &self->pr_MsgPort;
        fh->fh_Port = &self->pr_MsgPort;      /* interactive flag (by tradition) */
        fh->fh_Arg1 = (LONG)gSock;
        fh->fh_Pos  = -1;
        fh->fh_End  = -1;
        PutStr((STRPTR)"dbg: handle ready\n"); dbglog("dbg: handle ready\n");

#ifdef BISECT_MINIMAL
        /* Bisection build: no spawn, no session - just log and close. */
        {
            unsigned char discard[64];
            LONG got = recv(gSock, (APTR)discard, 64, 0);
            Printf((STRPTR)"bisect: accepted conn, got %ld bytes, closing\n", (LONG)got);
        }
        CloseSocket(gSock);
        gSock = -1;
        continue;
#endif
        PutStr((STRPTR)"telnetd: connection - starting shell\n");
        PutStr((STRPTR)"dbg: calling SystemTags\n"); dbglog("dbg: calling SystemTags\n");
        /* The proven telnetd 2.0 spawn: NewShell * on our handle as both
         * stdio and console task, as a CLI process. No SYS_UserShell. */
        {
            struct TagItem ptags[5];
            ptags[0].ti_Tag  = NP_Entry;
            ptags[0].ti_Data = (LONG)spawner_entry;
            ptags[1].ti_Tag  = NP_StackSize;
            ptags[1].ti_Data = 20000;
            ptags[2].ti_Tag  = NP_Name;
            ptags[2].ti_Data = (LONG)"telnetd shell";
            ptags[3].ti_Tag  = TAG_END;
            ptags[3].ti_Data = 0;

            g_spawnFH = fhB;   /* read by spawner_entry at startup */
            dbglog("dbg: spawning helper process\n");
            if (CreateNewProc((struct TagItem *)&ptags) == NULL) {
                PutStr((STRPTR)"telnetd: could not start shell\n");
                PutStr((STRPTR)"dbg: CreateNewProc FAILED\n"); dbglog("dbg: CreateNewProc FAILED\n");
                Close(fhB);
                while ((msg = GetMsg(&self->pr_MsgPort)) != NULL) {  /* drain END */
                    struct DosPacket *p = (struct DosPacket *)msg->mn_Node.ln_Name;
                    PutMsg(p->dp_Port, p->dp_Link);
                }
                continue;
            }
            dbglog("dbg: helper spawned (synchronous System inside)\n");
        }

        PutStr((STRPTR)"dbg: spawn ok - entering session loop\n"); dbglog("dbg: spawn ok - entering session loop\n");

        /* Stragglers from a previous abandoned shell must not leak in. */
        while ((msg = GetMsg(&self->pr_MsgPort)) != NULL) {
            struct DosPacket *p = (struct DosPacket *)msg->mn_Node.ln_Name;
            p->dp_Res1 = DOSFALSE;
            p->dp_Res2 = ERROR_ACTION_NOT_KNOWN;
            PutMsg(p->dp_Port, p->dp_Link);
        }

        clean = session_loop();
        if (clean) {
            FreeDosObject(DOS_FILEHANDLE, fhB);
        } else {
            /* An abandoned shell may still reference the handle: leak the
             * few bytes rather than free them under its feet. */
            PutStr((STRPTR)"telnetd: session handle kept (abandoned shell)\n");
        }
        if (gSock >= 0) { CloseSocket(gSock); gSock = -1; }
        if (gBreak) { PutStr((STRPTR)"telnetd: stopped\n"); break; }
        PutStr((STRPTR)"telnetd: session closed - waiting for next\n");
    }

out:
    if (gListen >= 0) CloseSocket(gListen);
    if (SocketBase) CloseLibrary(SocketBase);
    self->pr_WindowPtr = oldwinptr;
    FreeArgs(rd);
    return RETURN_OK;
}

