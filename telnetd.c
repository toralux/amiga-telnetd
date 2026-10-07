/*
 * telnetd.c — a standalone telnet daemon for AmigaOS 2.04+
 * v0.4.1: shell spawned from a helper process (sync SystemTags + NP_Cli,
 *   the telnetd 2.0 recipe) - direct SystemTags(SYS_Asynch) from the
 *   daemon hung forever on real hardware (v0.4 field test 07-Oct-2026).
 * v0.4: session architecture follows the approach of telnetd 2.0 (Peter
 * Simons & Steve Holland, 1995), reimplemented for AmiTCP_NG 4.x: no
 * inetd, no usergroup.library, one connection at a time, LAN-only.
 * No code from the GPLv2 original is used.
 *
 * Copyright (c) 2026 Tor Anders Johansen. MIT License — see LICENSE.
 *
 * Mechanism:
 *   The daemon owns one PRIVATE handler port (CreateMsgPort) for its whole
 *   life. For each connection it makes two DOS filehandles whose fh_Type
 *   is that port, then a short-lived helper process runs SystemTags(
 *   "NewShell *") synchronously - it blocks until the shell exits - with
 *   them as stdio and NP_ConsoleTask = the port. The shell's console
 *   traffic then
 *   arrives as DosPackets on the private port and is served here:
 *     ACTION_FIND*         — "*" opened again: opencount++
 *     ACTION_READ          — queued; cooked: completed per line,
 *                            raw: completed with what is available
 *     ACTION_WRITE         — sent at once, LF -> CRLF, CSI -> ESC [,
 *                            IAC escaped
 *     ACTION_WAIT_CHAR     — timer.device UNIT_MICROHZ
 *     ACTION_SCREEN_MODE   — raw/cooked + telnet ECHO/SGA negotiation
 *     ACTION_CHANGE_SIGNAL — who gets Ctrl-C
 *     ACTION_END           — opencount--; zero ends the session
 *   Every handle carries the session number in fh_Arg1, so packets from a
 *   process that outlived its session (a "run" job, an abandoned shell)
 *   are recognised and answered harmlessly: READ gets EOF, WRITE is
 *   discarded. The port is never freed while anything may still use it.
 *
 * Why a private port (the v0.2-v0.3.1 crashes): telnetd 2.0 serves the
 * packets on its own pr_MsgPort, which is only legal because that
 * process makes NO DOS calls of its own after the spawn ("NO more DOS
 * calls allowed after this one", telnetd.c rev 2.0). pr_MsgPort is where
 * dos.library waits for the replies to the process's own packets, and
 * WaitPkt() takes whatever arrives first. Every Open/Write/Close/PutStr
 * the daemon made while a shell was talking to pr_MsgPort could swallow
 * a shell packet as its own reply; the real filesystem reply then landed
 * in the session loop and was "replied" back to the filesystem handler.
 * With a private port the daemon's own DOS I/O and the shell's console
 * traffic never meet.
 *
 * Build (cross toolchain):
 *   m68k-amigaos-gcc -Os -m68000 -Wall -Wextra -o telnetd telnetd.c -s -lamiga
 *
 * Run:  1> stack 20000
 *       1> telnetd                         ; port 23
 *       1> telnetd 2323                    ; custom port
 *       1> telnetd LOG=T:tdbg.log            ; crash-surviving trace (any path)
 * Stop with Ctrl-C in the starting Shell.
 *
 * WARNING: no authentication. LAN use only — never expose to the internet.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
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
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <proto/bsdsocket.h>

#include <string.h>

#ifndef NewList
#define NewList(l)  ((l)->lh_Head = (struct Node *)&(l)->lh_Tail, \
                     (l)->lh_Tail = NULL, \
                     (l)->lh_TailPred = (struct Node *)&(l)->lh_Head)
#endif

#ifndef BADDR
#define BADDR(x)  ((APTR)(((ULONG)(x)) << 2))
#endif
#ifndef MKBADDR
#define MKBADDR(x) ((BPTR)(((ULONG)(x)) >> 2))
#endif

#ifndef ACTION_CHANGE_SIGNAL
#define ACTION_CHANGE_SIGNAL 995
#endif

static const char __attribute__((used)) verstag[] =
    "$VER: telnetd 0.4.6 (7.10.2026)";

/* Telnet protocol bytes */
#define TEL_IAC      255
#define TEL_DONT     254
#define TEL_DO       253
#define TEL_WONT     252
#define TEL_WILL     251
#define TEL_SB       250
#define TEL_IP       244
#define TEL_BRK      243
#define TEL_SE       240
#define OPT_ECHO       1
#define OPT_SGA        3

/* bsdsocket errno values (BSD numbering, as returned by Errno()) */
#define SOCK_EINTR         4   /* AmiTCP BSD numbering */
#define SOCK_EWOULDBLOCK   6   /* AmiTCP: 6, NOT Linux 35 - v0.4.3 had 35 and every would-block was fatal */

#define IOBUF        1024
#define MIN_STACK    16000            /* refuse to run on a smaller stack */
#define DRAIN_SECS   30               /* after hangup the shell must end by then */
#define SPAWN_SECS   10               /* NewShell must open "*" by then */

struct Library *SocketBase = NULL;    /* extern in proto/bsdsocket.h */

static STRPTR           gLogName  = NULL;
static int              gListen   = -1;
static int              gSock     = -1;
static BOOL             gBreak    = FALSE;

/* the handler port and its timer port live as long as the daemon */
static struct MsgPort  *gPort     = NULL;
static struct MsgPort  *gTimePort = NULL;
static LONG             gSessions = 0;      /* sessions started so far */

/* per-session state (one session at a time; gCookie 0 = no session) */
static LONG             gCookie   = 0;      /* fh_Arg1 of this session's handles */
static struct List      gReadWait;          /* queued READ / WAIT_CHAR messages */
static LONG             gOpens    = 0;      /* this session's handles not yet ENDed */
static BOOL             gSawOpen  = FALSE;  /* NewShell has opened "*" */
static BOOL             gHangup   = FALSE;  /* client gone or daemon stopping */
static BOOL             gRaw      = FALSE;  /* SetMode(fh, 1) in effect */
static struct Task     *gBreakTask = NULL;  /* receives Ctrl-C from the client */
static BOOL             gWill[256];         /* options we currently have WILL'd */

/* input decoding state */
static unsigned char    gIn[256];
static LONG             gInHead = 0, gInTail = 0;
static int              gTelState = 0;      /* 0 data, 1 IAC, 2 option, 3 SB, 4 SB-IAC */
static int              gTelCmd = 0;
static BOOL             gLastCR = FALSE;
static LONG             gPeek = -1;         /* char seen by WAIT_CHAR, not yet read */


/* ---------------- logging ----------------------------------------------
 * Only with LOG=<file>. Open-append-close per event so the trail survives
 * a crash. Safe at any time: the daemon's own pr_MsgPort carries only its
 * own packets. Formatting is RawDoFmt style (%ld, %lx, %s; LONG args). */
static void logmsg(const char *fmt, LONG arg)
{
    BPTR f;
    if (!gLogName) return;
    f = Open(gLogName, MODE_READWRITE);
    if (!f) return;
    Seek(f, 0, OFFSET_END);
    VFPrintf(f, (STRPTR)fmt, (APTR)&arg);
    Close(f);
}

static LONG now_secs(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return ds.ds_Days * 86400 + ds.ds_Minute * 60 + ds.ds_Tick / TICKS_PER_SECOND;
}


/* ---------------- socket output ---------------------------------------- */

/* The socket is non-blocking: wait for room (bounded) when it is full. */
static LONG send_all(const unsigned char *p, LONG n)
{
    LONG sent = 0;
    int stalls = 0;
    if (gSock < 0 || gHangup) return -1;
    while (sent < n) {
        LONG k = send(gSock, (APTR)(p + sent), (int)(n - sent), 0);
        if (k > 0) { sent += k; stalls = 0; continue; }
        if (k < 0) {
            LONG se = Errno();
            logmsg("telnetd: sendfail e %ld\n", se);
            if (se != SOCK_EWOULDBLOCK && se != 35 && se != SOCK_EINTR) return -1;
            if (++stalls > 30) return -1;        /* 30 s without progress */
            {
                fd_set wr;
                struct timeval tv;
                ULONG wmask = 0;
                FD_ZERO(&wr);
                FD_SET(gSock, &wr);
                tv.tv_secs = 1;
                tv.tv_micro = 0;
                WaitSelect(gSock + 1, NULL, &wr, NULL, &tv, &wmask);
            }
            continue;
        }
        return -1;                               /* k == 0 */
    }
    return sent;
}

static void send_opt(int cmd, int opt)
{
    unsigned char seq[3];
    seq[0] = TEL_IAC; seq[1] = (unsigned char)cmd; seq[2] = (unsigned char)opt;
    send_all(seq, 3);
}

/* Output: bare LF -> CRLF, CSI -> ESC [, IAC escaped. 0 ok, -1 failed. */
static LONG sock_write(const unsigned char *p, LONG n)
{
    static unsigned char buf[IOBUF];
    LONG i, w = 0;
    for (i = 0; i < n; i++) {
        unsigned char c = p[i];
        if (c == 255) {                       /* IAC escape */
            buf[w++] = 255; buf[w++] = 255;
        } else if (c == 0x9b) {               /* Amiga CSI -> ANSI ESC [ */
            buf[w++] = 27; buf[w++] = '[';
        } else if (c == 10 && (i == 0 || p[i-1] != 13)) {
            buf[w++] = 13; buf[w++] = 10;     /* bare LF -> CRLF */
        } else {
            buf[w++] = c;
        }
        if (w >= IOBUF - 2) { if (send_all(buf, w) < 0) return -1; w = 0; }
    }
    if (w > 0) { if (send_all(buf, w) < 0) return -1; }
    return 0;
}


/* ---------------- Ctrl-C forwarding ------------------------------------ */

static BOOL task_alive(struct Task *t)
{
    struct Node *n;
    BOOL found = FALSE;
    if (!t) return FALSE;
    Forbid();
    for (n = SysBase->TaskWait.lh_Head; n->ln_Succ && !found; n = n->ln_Succ)
        if ((struct Task *)n == t) found = TRUE;
    for (n = SysBase->TaskReady.lh_Head; n->ln_Succ && !found; n = n->ln_Succ)
        if ((struct Task *)n == t) found = TRUE;
    Permit();
    return found;
}

static void send_break(void)
{
    if (task_alive(gBreakTask)) Signal(gBreakTask, SIGBREAKF_CTRL_C);
}


/* ---------------- telnet option handling ------------------------------- */

/* We only ever agree to ECHO and SGA, and only in raw mode (cooked mode
 * leaves echo and line editing to the client, as telnetd 2.0 does).
 * gWill[] tracks what we have announced, so acknowledgements are never
 * answered (RFC 854 loop rule). */
static void answer_option(int cmd, int opt)
{
    BOOL want = gRaw && (opt == OPT_ECHO || opt == OPT_SGA);
    switch (cmd) {
    case TEL_DO:
        if (want) { if (!gWill[opt]) { gWill[opt] = TRUE; send_opt(TEL_WILL, opt); } }
        else      { gWill[opt] = FALSE; send_opt(TEL_WONT, opt); }
        break;
    case TEL_DONT:
        if (gWill[opt]) { gWill[opt] = FALSE; send_opt(TEL_WONT, opt); }
        break;
    case TEL_WILL:
        send_opt(TEL_DONT, opt);              /* we want nothing from the client */
        break;
    default:                                  /* WONT: nothing to do */
        break;
    }
}

static void set_mode(BOOL raw)
{
    static const int opts[2] = { OPT_ECHO, OPT_SGA };
    int i;
    gRaw = raw;
    for (i = 0; i < 2; i++) {
        if (raw && !gWill[opts[i]])      { gWill[opts[i]] = TRUE;  send_opt(TEL_WILL, opts[i]); }
        else if (!raw && gWill[opts[i]]) { gWill[opts[i]] = FALSE; send_opt(TEL_WONT, opts[i]); }
    }
}


/* ---------------- socket input ----------------------------------------- */

/* One raw byte from the socket. 1 = byte, 0 = hangup, -1 = none right now. */
static LONG in_byte(unsigned char *c)
{
    if (gInHead == gInTail) {
        LONG n = recv(gSock, (APTR)gIn, sizeof gIn, 0);
        if (n > 0) logmsg("telnetd: recv %ld\n", n);
        if (n == 0) { logmsg("telnetd: recv eof\n", 0); return 0; }
        if (n < 0) {
            LONG e = Errno();
            logmsg("telnetd: recv err %ld\n", e);
            return (e == SOCK_EWOULDBLOCK || e == 35 || e == SOCK_EINTR) ? -1 : 0;
        }
        gInHead = 0;
        gInTail = n;
    }
    *c = gIn[gInHead++];
    return 1;
}

/* One data character with telnet commands removed and NVT line ends
 * folded (CR LF / CR NUL / bare CR -> '\n' cooked, CR raw).
 * 1 = char in *loc, 0 = hangup, -1 = none right now. */
static LONG next_char(unsigned char *loc)
{
    unsigned char ch;
    LONG r;

    if (gPeek >= 0) { *loc = (unsigned char)gPeek; gPeek = -1; return 1; }

    for (;;) {
        r = in_byte(&ch);
        if (r <= 0) return r;

        switch (gTelState) {
        case 0:
            if (ch == TEL_IAC) { gTelState = 1; continue; }
            if (gLastCR) {
                gLastCR = FALSE;
                if (ch == 10 || ch == 0) continue;
            }
            if (ch == 13) { gLastCR = TRUE; *loc = gRaw ? 13 : 10; return 1; }
            if (ch == 3) {                     /* Ctrl-C: break the foreground */
                send_break();
                if (!gRaw) continue;
            }
            *loc = ch;
            return 1;
        case 1:                                /* IAC command byte */
            gTelState = 0;
            if (ch == TEL_IAC) { gLastCR = FALSE; *loc = 255; return 1; }
            if (ch == TEL_SB) { gTelState = 3; continue; }
            if (ch >= TEL_WILL) { gTelCmd = ch; gTelState = 2; continue; }
            if (ch == TEL_IP || ch == TEL_BRK) send_break();
            continue;
        case 2:                                /* option byte */
            gTelState = 0;
            answer_option(gTelCmd, ch);
            continue;
        case 3:                                /* inside SB */
            if (ch == TEL_IAC) gTelState = 4;
            continue;
        default:                               /* SB IAC */
            gTelState = (ch == TEL_SE) ? 0 : 3;
            continue;
        }
    }
}


/* ---------------- packet plumbing -------------------------------------- */

static struct DosPacket *pkt_of(struct Message *msg)
{
    return (struct DosPacket *)msg->mn_Node.ln_Name;
}

static void reply(struct DosPacket *pkt, LONG res1, LONG res2)
{
    pkt->dp_Res1 = res1;
    pkt->dp_Res2 = res2;
    PutMsg(pkt->dp_Port, pkt->dp_Link);
}

/* The process that reads is the one that should get Ctrl-C. */
static void note_reader(struct DosPacket *pkt)
{
    struct MsgPort *p = pkt->dp_Port;
    if (p && (p->mp_Flags & PF_ACTION) == PA_SIGNAL && p->mp_SigTask)
        gBreakTask = (struct Task *)p->mp_SigTask;
}

/* WAIT_CHAR keeps its timerequest in dp_Res2 until it is answered. */
static void stop_timer(struct DosPacket *pkt)
{
    struct timerequest *tr = (struct timerequest *)pkt->dp_Res2;
    if (tr) {
        AbortIO((struct IORequest *)tr);
        WaitIO((struct IORequest *)tr);
        CloseDevice((struct IORequest *)tr);
        DeleteIORequest((struct IORequest *)tr);
        pkt->dp_Res2 = 0;
    }
}

/* Answer every queued READ / WAIT_CHAR: READ gets what it has so far
 * (0 = EOF), WAIT_CHAR gets "no character". */
static void flush_waiters(void)
{
    struct Message *msg;
    while ((msg = (struct Message *)RemHead(&gReadWait)) != NULL) {
        struct DosPacket *pkt = pkt_of(msg);
        if (pkt->dp_Type == ACTION_WAIT_CHAR) {
            stop_timer(pkt);
            reply(pkt, DOSFALSE, 0);
        } else {
            reply(pkt, pkt->dp_Res1, 0);
        }
    }
}

/* Client gone (or daemon stopping): the foreground command gets Ctrl-C and
 * every pending and future READ gets EOF, so the shell ends by itself and
 * closes its handles. */
static void do_hangup(void)
{
    if (gHangup) return;
    gHangup = TRUE;
    logmsg("telnetd: hangup, %ld handles open\n", gOpens);
    send_break();
    flush_waiters();
}

/* Complete queued READ / WAIT_CHAR packets from the socket. */
static void service_reads(void)
{
    struct Message *msg;
    while (!gHangup && (msg = (struct Message *)gReadWait.lh_Head)->mn_Node.ln_Succ) {
        struct DosPacket *pkt = pkt_of(msg);
        unsigned char ch;
        LONG r;

        if (pkt->dp_Type == ACTION_WAIT_CHAR) {
            r = next_char(&ch);
            if (r == 0) { logmsg("telnetd: hangup wait eof\n", 0); do_hangup(); return; }
            if (r < 0) return;
            gPeek = ch;                        /* the next READ gets it */
            stop_timer(pkt);
            Remove(&msg->mn_Node);
            reply(pkt, DOSTRUE, 0);
            continue;
        }

        /* ACTION_READ: dp_Arg2 = buffer, dp_Arg3 = length, dp_Res1 = filled */
        for (;;) {
            if (pkt->dp_Res1 >= pkt->dp_Arg3) break;
            r = next_char(&ch);
            if (r == 0) { logmsg("telnetd: hangup read eof\n", 0); do_hangup(); return; }
            if (r < 0) {
                if (gRaw && pkt->dp_Res1 > 0) break;
                return;                        /* wait for more input */
            }
            if (!gRaw && ch == 0x1c) break;    /* Ctrl-\ = EOF, as on CON: */
            ((unsigned char *)pkt->dp_Arg2)[pkt->dp_Res1++] = ch;
            if (!gRaw && ch == '\n') break;
        }
        Remove(&msg->mn_Node);
        logmsg("telnetd: read done %ld\n", pkt->dp_Res1);
        reply(pkt, pkt->dp_Res1, 0);
    }
}

/* v0.4.3: serve READ/WRITE/END unconditionally while a session is live -
 * exactly what telnetd 2.0 does. The instrumented v0.4.2 run proved why
 * per-packet identity cannot work here: dos.library stores the FIND
 * reply's dp_Arg1 into fh_Arg1, so every later packet carries
 * dp_Arg1 = whatever FIND replied (we replied DOSTRUE = 1) - neither a
 * handle BPTR nor a session number. Stale packets are still rejected by
 * the state machine: between sessions gCookie is 0 and gHangup gates,
 * as in 2.0. */

/* Serves one packet from the handler port. Packets from handles of an
 * earlier session (stale fh_Arg1) or arriving between sessions are
 * answered without touching the current connection. */
static void handle_packet(struct Message *msg)
{
    struct DosPacket *pkt = pkt_of(msg);
    BOOL live = (gCookie != 0 && !gHangup);

    switch (pkt->dp_Type) {

    case ACTION_FINDINPUT:
    case ACTION_FINDOUTPUT:
    case ACTION_FINDUPDATE: {
        /* Open("*") by a process whose console task is our port. */
        struct FileHandle *fh = (struct FileHandle *)BADDR((BPTR)pkt->dp_Arg1);
        fh->fh_Type = gPort;
        fh->fh_Port = gPort;                   /* non-zero = interactive */
        fh->fh_Arg1 = gCookie;                 /* 0 between sessions: a dead handle */
        if (gCookie) { gOpens++; gSawOpen = TRUE; }
        logmsg("telnetd: find, opens %ld\n", gOpens);
        reply(pkt, DOSTRUE, 0);
        break;
    }

    case ACTION_READ:
        if (!live) { reply(pkt, 0, 0); break; }   /* EOF: 2.0 semantics, no per-packet identity */
        note_reader(pkt);
        pkt->dp_Res1 = 0;
        logmsg("telnetd: read queued\n", 0);
        AddTail(&gReadWait, &msg->mn_Node);
        break;

    case ACTION_WRITE:
        logmsg("telnetd: write %ld\n", pkt->dp_Arg3);
        if (live && sock_write((const unsigned char *)pkt->dp_Arg2, pkt->dp_Arg3) < 0) {
            logmsg("telnetd: hangup write fail\n", 0);
            do_hangup();
        }
        reply(pkt, pkt->dp_Arg3, 0);          /* output nobody can see is discarded */
        break;

    case ACTION_WAIT_CHAR: {                   /* dp_Arg1 = timeout, no cookie */
        struct timerequest *tr;
        ULONG us = (ULONG)pkt->dp_Arg1;
        if (!live) { reply(pkt, DOSFALSE, 0); break; }
        note_reader(pkt);
        tr = (struct timerequest *)CreateIORequest(gTimePort, sizeof(struct timerequest));
        if (!tr || OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)tr, 0)) {
            if (tr) DeleteIORequest((struct IORequest *)tr);
            reply(pkt, DOSFALSE, ERROR_NO_FREE_STORE);
            break;
        }
        tr->tr_node.io_Command = TR_ADDREQUEST;
        tr->tr_time.tv_secs    = us / 1000000;     /* tv_micro must stay < 1e6 */
        tr->tr_time.tv_micro   = us % 1000000;
        tr->tr_node.io_Message.mn_Node.ln_Name = (char *)msg;
        pkt->dp_Res1 = DOSFALSE;
        pkt->dp_Res2 = (LONG)tr;
        SendIO((struct IORequest *)tr);
        AddTail(&gReadWait, &msg->mn_Node);
        break;
    }

    case ACTION_SCREEN_MODE:                   /* dp_Arg1 = mode, no cookie */
        if (live) set_mode(pkt->dp_Arg1 != 0);
        reply(pkt, DOSTRUE, 0);
        break;

    case ACTION_CHANGE_SIGNAL: {
        struct MsgPort *np = (struct MsgPort *)pkt->dp_Arg2;
        if (live && np && np->mp_SigTask) gBreakTask = (struct Task *)np->mp_SigTask;
        reply(pkt, DOSTRUE, 0);
        break;
    }

    case ACTION_END:
        reply(pkt, DOSTRUE, 0);
        gOpens--;
        logmsg("telnetd: END, %ld handles left\n", gOpens);
        break;

    case ACTION_SEEK:
        reply(pkt, -1, ERROR_OBJECT_WRONG_TYPE);
        break;

    case ACTION_IS_FILESYSTEM:
        reply(pkt, DOSFALSE, 0);
        break;

    default:
        logmsg("telnetd: unknown packet %ld\n", pkt->dp_Type);
        reply(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
        break;
    }
}

static void drain_port(void)
{
    struct Message *msg;
    while ((msg = GetMsg(gPort)) != NULL)
        handle_packet(msg);
}


/* ---------------- one session ------------------------------------------ */

/* AllocDosObject() returns a plain C pointer, NOT a BPTR (dos_protos.h:
 * APTR). v0.1.2-v0.3.1 cast it to BPTR and then BADDR()ed it, so every
 * field below was written to 4x the real address - on a 24-bit 68000 bus
 * that is anywhere from chip RAM to the CIAs and custom chips. telnetd
 * 2.0 converts with MKBADDR, as done here. */
/* v0.4.1: the spawn runs in this short-lived helper, never in the
 * daemon. telnetd 2.0's rule: the process that calls System must be the
 * one willing to block until the shell exits. A direct
 * SystemTags(SYS_Asynch) from the daemon hung forever on real hardware
 * (v0.4 field test, 07-Oct-2026: trail ends inside the call, no
 * "shell started", no "SystemTags failed"). */
static BPTR g_spawnIn = 0, g_spawnOut = 0;

static int spawner_entry(void)
{
    struct Library *dosBase;
    LONG rc;

    dosBase = OpenLibrary((STRPTR)"dos.library", 36);
    if (!dosBase) return RETURN_FAIL;

    rc = SystemTags((STRPTR)"NewShell *",
                    SYS_Input,      g_spawnIn,
                    SYS_Output,     g_spawnOut,
                    NP_ConsoleTask, (LONG)gPort,
                    NP_Cli,         TRUE,
                    TAG_DONE);
    /* Blocked here until the shell exits. The child's process cleanup
     * closes the two handles (ACTION_END each); never touch them here. */
    logmsg("telnetd: helper rc %ld\n", rc);
    CloseLibrary(dosBase);
    return (int)rc;
}

static BPTR make_handle(void)
{
    struct FileHandle *fh = (struct FileHandle *)AllocDosObject(DOS_FILEHANDLE, NULL);
    if (!fh) return 0;
    fh->fh_Type = gPort;
    fh->fh_Port = gPort;                       /* non-zero = interactive */
    fh->fh_Arg1 = gCookie;
    fh->fh_Pos  = -1;                          /* unbuffered */
    fh->fh_End  = -1;
    return MKBADDR(fh);
}

/* Serves the accepted connection gSock until every handle of the session
 * is closed (or the shell is given up on). */
static void run_session(void)
{
    BPTR in, out;
    LONG start = now_secs(), hangupAt = 0;
    ULONG one = 1;

    gSessions++;
    gCookie = gSessions;
    NewList(&gReadWait);
    gOpens = 0;
    gSawOpen = FALSE;
    gHangup = FALSE;
    gRaw = FALSE;
    gBreakTask = NULL;
    memset(gWill, 0, sizeof gWill);
    gInHead = gInTail = 0;
    gTelState = 0;
    gLastCR = FALSE;
    gPeek = -1;

    IoctlSocket(gSock, FIONBIO, (APTR)&one);

    in  = make_handle();
    out = make_handle();
    if (!in || !out) {
        if (in)  FreeDosObject(DOS_FILEHANDLE, (APTR)BADDR(in));
        if (out) FreeDosObject(DOS_FILEHANDLE, (APTR)BADDR(out));
        goto done;
    }

    /* Two distinct handles: when the spawned shell exits, its process
     * cleanup closes them (one ACTION_END each; if DOS skips that the
     * hangup path still ends the session - leaked handles, not freed
     * under a live shell). NewShell opens "*" on NP_ConsoleTask = gPort
     * for the interactive shell it starts. */
    gOpens = 2;
    logmsg("telnetd: session %ld, spawning shell\n", gCookie);
    g_spawnIn  = in;
    g_spawnOut = out;
    {
        struct TagItem ptags[4];
        ptags[0].ti_Tag  = NP_Entry;
        ptags[0].ti_Data = (LONG)spawner_entry;
        ptags[1].ti_Tag  = NP_StackSize;
        ptags[1].ti_Data = 20000;
        ptags[2].ti_Tag  = NP_Name;
        ptags[2].ti_Data = (LONG)"telnetd shell";
        ptags[3].ti_Tag  = TAG_END;
        ptags[3].ti_Data = 0;
        if (CreateNewProc(ptags) == NULL) {
            logmsg("telnetd: CreateNewProc failed\n", 0);
            FreeDosObject(DOS_FILEHANDLE, (APTR)BADDR(in));
            FreeDosObject(DOS_FILEHANDLE, (APTR)BADDR(out));
            gOpens = 0;
            goto done;
        }
    }
    logmsg("telnetd: shell started\n", 0);

    for (;;) {
        fd_set rd;
        struct timeval tv;
        ULONG mask = (1UL << gPort->mp_SigBit)
                   | (1UL << gTimePort->mp_SigBit)
                   | SIGBREAKF_CTRL_C;

        /* Over when every handle is closed - but not before NewShell has
         * opened "*", or its late FINDINPUT would find no session. */
        if (gOpens <= 0 && (gSawOpen || now_secs() - start > SPAWN_SECS)) break;
        if (gHangup) {
            if (!hangupAt) hangupAt = now_secs();
            else if (now_secs() - hangupAt > DRAIN_SECS) break;
        }

        FD_ZERO(&rd);
        if (!gHangup && gReadWait.lh_Head->ln_Succ)
            FD_SET(gSock, &rd);                 /* only read while someone waits */
        tv.tv_secs = 1;
        tv.tv_micro = 0;
        WaitSelect(gSock + 1, &rd, NULL, NULL, &tv, &mask);

        if ((mask & SIGBREAKF_CTRL_C) && !gHangup) {
            PutStr((STRPTR)"telnetd: break - ending session\n");
            gBreak = TRUE;
            logmsg("telnetd: hangup break\n", 0);
            do_hangup();
        }

        /* WAIT_CHAR timeouts */
        {
            struct timerequest *tr;
            while ((tr = (struct timerequest *)GetMsg(gTimePort)) != NULL) {
                struct Message *msg = (struct Message *)tr->tr_node.io_Message.mn_Node.ln_Name;
                struct DosPacket *pkt = pkt_of(msg);
                CloseDevice((struct IORequest *)tr);
                DeleteIORequest((struct IORequest *)tr);
                pkt->dp_Res2 = 0;
                Remove(&msg->mn_Node);
                reply(pkt, DOSFALSE, 0);
            }
        }

        drain_port();
        service_reads();
    }

done:
    gHangup = TRUE;
    flush_waiters();
    if (gOpens > 0) {
        PutStr((STRPTR)"telnetd: shell did not exit - left detached\n");
        logmsg("telnetd: session %ld left detached\n", gCookie);
    }
    gCookie = 0;                               /* its packets are stale from now on */
}


/* ------------------------------- main ---------------------------------- */

int main(int argc, char **argv)
{
    LONG args[2] = { 0, 0 };
    struct RDArgs *rd;
    struct sockaddr_in sa;
    struct Process *self = (struct Process *)FindTask(NULL);
    ULONG port = 23;
    ULONG stack = (ULONG)self->pr_Task.tc_SPUpper - (ULONG)self->pr_Task.tc_SPLower;
    int one = 1;
    APTR oldwinptr = self->pr_WindowPtr;

    (void)argc; (void)argv;

    if (stack < MIN_STACK) {
        Printf((STRPTR)"telnetd: stack is %lu bytes, needs %ld - run 'stack 20000' first\n",
               stack, (LONG)MIN_STACK);
        return RETURN_FAIL;
    }

    rd = ReadArgs((STRPTR)"PORT/N,LOG/K", args, NULL);
    if (rd == NULL) {
        PutStr((STRPTR)"usage: telnetd [PORT <n>] [LOG <file>]\n");
        return RETURN_FAIL;
    }
    if (args[0]) port = *(ULONG *)args[0];
    gLogName = (STRPTR)args[1];
    if (gLogName) {                          /* fresh trace per run */
        BPTR f = Open(gLogName, MODE_NEWFILE);
        if (f) Close(f);
    }
    logmsg("telnetd: started, stack %ld\n", (LONG)stack);

    gPort = CreateMsgPort();
    gTimePort = CreateMsgPort();
    if (!gPort || !gTimePort) {
        PutStr((STRPTR)"telnetd: out of signals/memory\n");
        if (gPort) DeleteMsgPort(gPort);
        if (gTimePort) DeleteMsgPort(gTimePort);
        FreeArgs(rd);
        return RETURN_FAIL;
    }

    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 3);
    if (!SocketBase) {
        PutStr((STRPTR)"telnetd: no bsdsocket.library - is the TCP/IP stack up?\n");
        goto out;
    }

    /* No DOS requesters may ever park this headless daemon. */
    self->pr_WindowPtr = (APTR)-1;

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

    Printf((STRPTR)"telnetd: listening on port %ld - Ctrl-C stops\n", port);
    logmsg("telnetd: listening on port %ld\n", (LONG)port);

    while (!gBreak) {
        fd_set rdset;
        ULONG mask = SIGBREAKF_CTRL_C | (1UL << gPort->mp_SigBit);
        struct timeval tv;
        struct sockaddr_in ca;
        socklen_t calen = sizeof ca;
        LONG selr;

        FD_ZERO(&rdset);
        FD_SET(gListen, &rdset);
        tv.tv_secs = 2;
        tv.tv_micro = 0;
        selr = WaitSelect(gListen + 1, &rdset, NULL, NULL, &tv, &mask);
        drain_port();                        /* stragglers of earlier sessions */
        if (mask & SIGBREAKF_CTRL_C) break;
        if (selr <= 0 || !FD_ISSET(gListen, &rdset)) continue;

        if (gOpens > 0) {                     /* zombie shell of last session */
            LONG zw = 0;
            BOOL zbreak = FALSE;
            while (gOpens > 0 && zw < 15) {
                struct timeval zt;
                ULONG zm = SIGBREAKF_CTRL_C;
                zt.tv_secs = 1;
                zt.tv_micro = 0;
                WaitSelect(0, NULL, NULL, NULL, &zt, &zm);
                drain_port();
                zw++;
                if (zm & SIGBREAKF_CTRL_C) { zbreak = TRUE; break; }
            }
            if (gOpens <= 0) logmsg("telnetd: zombie drained\n", 0);
            else logmsg("telnetd: zombie stuck, handles %ld\n", (LONG)gOpens);
            if (zbreak) break;
        }

        gSock = accept(gListen, (struct sockaddr *)&ca, &calen);
        if (gSock < 0) { logmsg("telnetd: accept failed, errno %ld\n", Errno()); continue; }
        logmsg("telnetd: accepted from %lx\n", (LONG)ca.sin_addr.s_addr);
        PutStr((STRPTR)"telnetd: connection - starting shell\n");

        run_session();

        CloseSocket(gSock);
        gSock = -1;
        logmsg("telnetd: session closed\n", 0);
        if (!gBreak) PutStr((STRPTR)"telnetd: session closed - waiting for next\n");
    }
    PutStr((STRPTR)"telnetd: stopped\n");

out:
    if (gListen >= 0) CloseSocket(gListen);
    if (SocketBase) CloseLibrary(SocketBase);
    self->pr_WindowPtr = oldwinptr;

    drain_port();
    DeleteMsgPort(gTimePort);                /* no timer is pending outside a session */
    if (gSessions == 0) {
        DeleteMsgPort(gPort);
    } else {
        /* Processes started from a session (a "run" job, a detached shell)
         * keep this port as their console task and may PutMsg to it after
         * we are gone. Leave it allocated but inert: PA_IGNORE queues
         * without signalling, so a late packet waits forever instead of
         * crashing the machine on freed memory. */
        Forbid();
        gPort->mp_Flags = PA_IGNORE;
        gPort->mp_SigTask = NULL;
        Permit();
        FreeSignal(gPort->mp_SigBit);
    }
    FreeArgs(rd);
    return RETURN_OK;
}
