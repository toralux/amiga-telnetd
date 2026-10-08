/*
 * telnetd.c — a standalone telnet daemon for AmigaOS 2.04+
 *
 * The session architecture follows the approach of telnetd 2.0 (Peter
 * Simons & Steve Holland, 1995), reimplemented for AmiTCP_NG 4.x: no
 * inetd, no usergroup.library, one connection at a time, LAN-only.
 * No code from the GPLv2 original is used.
 *
 * Copyright (c) 2026 Tor Anders Johansen. MIT License — see LICENSE.
 *
 * Mechanism:
 *   Each session gets a PRIVATE handler port (all on one shared signal).
 *   For the connection the daemon makes two DOS filehandles whose fh_Type
 *   is that port, then a short-lived helper process runs SystemTags(
 *   "NewShell *") synchronously with them as stdio and NP_ConsoleTask =
 *   the port, and closes them when NewShell returns. The interactive
 *   shell NewShell starts opens "*" on the port, so its console traffic
 *   arrives as DosPackets on the private port and is served here:
 *     ACTION_FIND*         — "*" opened again: opencount++
 *     ACTION_READ          — queued; cooked: one line from the line
 *                            editor (echo, cursor keys, history, as
 *                            CON: does), raw: what is available
 *     ACTION_WRITE         — sent at once, LF -> CRLF, CSI -> ESC [,
 *                            IAC escaped
 *     ACTION_WAIT_CHAR     — one timer.device request serves all waiters
 *     ACTION_SCREEN_MODE   — raw/cooked + telnet ECHO/SGA negotiation
 *     ACTION_CHANGE_SIGNAL — who gets Ctrl-C
 *     ACTION_END           — opencount--; zero ends the session
 *   A packet's session is the port it arrives on. When a session ends its
 *   port is retired, not freed: retired ports are still served - READ gets
 *   break/EOF, WRITE is discarded - and count the handles still open on
 *   them, so a shell that outlives its client (a command that ignores
 *   Ctrl-C) or a "run" job cannot hurt the daemon or a later session. A
 *   retired port is reused only once no handle is left on it, and no port
 *   is freed while the daemon runs.
 *
 * Why a private port: pr_MsgPort is where dos.library waits for the
 * replies to the process's own packets, and WaitPkt() takes whatever
 * arrives first. Serving console packets on pr_MsgPort (as telnetd 2.0
 * does) is only safe for a process that makes no DOS calls of its own
 * once the shell runs; otherwise any Open/Write/PutStr can swallow a
 * shell packet as its reply, and the real filesystem reply lands in the
 * packet loop and gets bounced back to the filesystem handler. With a
 * private port the daemon's own DOS I/O and the shell's console traffic
 * never meet. docs/DESIGN.md has the full account.
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
#include <proto/timer.h>

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

#ifndef ACTION_DISK_INFO
#define ACTION_DISK_INFO 25              /* wiki.amigaos.net: AmigaDOS_Packets */
#endif

/* Console-flavoured InfoData for ACTION_DISK_INFO (frozen ABI: 9 LONGs). */
struct td_InfoData {
    LONG id_NumSoftErrors;
    LONG id_UnitNumber;
    LONG id_DiskState;
    LONG id_NumBlocks;
    LONG id_NumBlocksUsed;
    LONG id_BytesPerBlock;
    LONG id_DiskType;
    LONG id_VolumeNode;
    LONG id_InUse;
};

static const char __attribute__((used)) verstag[] =
    "$VER: telnetd 0.4.9 (8.10.2026)";

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
#define OPT_NAWS      31             /* window size (RFC 1073) */

/* bsdsocket errno values (BSD numbering, as returned by Errno()) */
#define SOCK_EINTR         4
#define SOCK_EWOULDBLOCK  35   /* AmiTCP_NG netinclude/sys/errno.h: EWOULDBLOCK = EAGAIN = 35 */
#define SOCK_ENXIO         6   /* seen from Errno() on hardware, cause unknown - the
                                * stack's send/recv paths never set it; tolerated */

#define IOBUF        1024
#define MIN_STACK    16000            /* refuse to run on a smaller stack */
#define EXIT_SECS    5                /* on Ctrl-C: time for hung-up shells to end */
#define SPAWN_SECS   10               /* NewShell must open "*" by then */

struct Library *SocketBase = NULL;    /* extern in proto/bsdsocket.h */

static STRPTR           gLogName  = NULL;
static int              gListen   = -1;
static int              gSock     = -1;
static BOOL             gBreak    = FALSE;

/* handler ports: the current session's, and the retired ones */
static struct MsgPort  *gPort     = NULL;   /* NULL between sessions */
static struct List      gRetired;           /* ports of ended sessions (mp_Node) */
static BYTE             gPortSig  = -1;     /* shared by all handler ports */
static volatile LONG    gHelpers  = 0;      /* spawn helpers that may still run our code */
static struct MsgPort  *gTimePort = NULL;
static struct timerequest *gTimer = NULL;   /* the one WAIT_CHAR timer */
static BOOL             gTimerBusy = FALSE;
struct Device          *TimerBase = NULL;   /* for GetSysTime() */

/* Pending WaitForChar() calls: answered TRUE as soon as input is ready,
 * FALSE when their deadline passes. gTimer is armed for the earliest. */
#define MAXCW        4
static struct {
    struct DosPacket *pkt;
    struct timeval    due;
}                       gCW[MAXCW];
static int              gNCW = 0;
static LONG             gSessions = 0;      /* sessions started so far */

/* per-session state (one session at a time; gCookie 0 = no session) */
static LONG             gCookie   = 0;      /* fh_Arg1 of this session's handles */
static struct List      gReadWait;          /* queued READ messages */
static LONG             gOpens    = 0;      /* this session's handles not yet ENDed */
static BOOL             gSawOpen  = FALSE;  /* NewShell has opened "*" */
static BOOL             gHangup   = FALSE;  /* client gone or daemon stopping */
static BOOL             gRaw      = FALSE;  /* SetMode(fh, 1) in effect */
static struct Task     *gBreakTask = NULL;  /* receives Ctrl-C from the client */
static BOOL             gWill[256];         /* options we currently have WILL'd */

/* input decoding state */
static unsigned char    gIn[256];
static LONG             gInHead = 0, gInTail = 0;
static int              gTelState = 0;      /* 0 data, 1 IAC, 2 option, 3 SB option, 4 SB IAC, 5 SB data */
static int              gTelCmd = 0;
static BOOL             gLastCR = FALSE;

/* Input that is ready for READ: whole edited lines in cooked mode, raw
 * bytes in raw mode. The socket is drained into it whether or not a READ
 * is waiting, so typeahead is echoed and a disconnect is seen at once. */
#define READYSZ      1024
static unsigned char    gReady[READYSZ];
static LONG             gRHead = 0, gRCount = 0;
static BOOL             gEof = FALSE;       /* Ctrl-\ on an empty line */

/* Cooked-mode line editor with history. On a real Amiga this is CON:'s
 * job, not the shell's: the shell only reads finished lines, so cursor
 * keys and history must be done by whoever plays the console - us. */
#define LINEMAX      255
#define HISTN        16                     /* power of two */
static unsigned char    gLine[LINEMAX];
static LONG             gLen = 0, gCur = 0;
static unsigned char    gSaved[LINEMAX];    /* the line being typed before Up */
static LONG             gSavedLen = 0;
static unsigned char    gHist[HISTN][LINEMAX];
static LONG             gHistLen[HISTN];
static LONG             gHistCount = 0, gHistNext = 0, gHistPos = 0;
static int              gEsc = 0;           /* 0 none, 1 ESC, 2 ESC [ / ESC O / CSI */
static LONG             gEscParam = 0;
static BOOL             gEdit = TRUE;       /* FALSE with DUMBTERM: no ANSI, client edits lines */

/* Client screen geometry, for drawing lines that wrap */
static LONG             gCols = 80;         /* from NAWS; 80 if the client never says */
static BOOL             gDoNaws = FALSE;    /* we have sent DO NAWS */
static LONG             gTermCol = 0;       /* client cursor column (see term_track) */
static int              gTrkEsc = 0;
static LONG             gTrkN = 0, gTrkN1 = -1;
static LONG             gStartCol = 0;      /* column where the edited line begins */
static LONG             gPos = 0;           /* line offset of the client's cursor */
static int              gSbOpt = 0;         /* subnegotiation being received */
static unsigned char    gSbBuf[8];
static int              gSbLen = 0;


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
            if (se != SOCK_EWOULDBLOCK && se != SOCK_ENXIO && se != SOCK_EINTR) return -1;
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
static void term_track(unsigned char c);

/* Every byte the client's terminal will see also goes through
 * term_track(), so the line editor knows the cursor column. */
static LONG sock_write(const unsigned char *p, LONG n)
{
    static unsigned char buf[IOBUF];
    LONG i, w = 0;
    for (i = 0; i < n; i++) {
        unsigned char c = p[i];
        if (c == 255) {                       /* IAC escape */
            buf[w++] = 255; buf[w++] = 255;
            term_track(255);
        } else if (c == 0x9b) {               /* Amiga CSI -> ANSI ESC [ */
            buf[w++] = 27; buf[w++] = '[';
            term_track(27); term_track('[');
        } else if (c == 10 && (i == 0 || p[i-1] != 13)) {
            buf[w++] = 13; buf[w++] = 10;     /* bare LF -> CRLF */
            term_track(13); term_track(10);
        } else {
            buf[w++] = c;
            term_track(c);
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

static void send_signal(ULONG sig)
{
    if (task_alive(gBreakTask)) Signal(gBreakTask, sig);
}

static void send_break(void)
{
    send_signal(SIGBREAKF_CTRL_C);
}


/* === BEGIN portable input/editor section ===============================
 * Everything from here to the END marker is plain C over the globals
 * above plus send_opt/sock_write/send_signal/recv/Errno/logmsg, so
 * tests/edtest.c compiles it on the host (make test). Keep it that way. */

/* ---------------- telnet option handling ------------------------------- */

/* With the line editor (the default) we always agree to ECHO and SGA:
 * the client runs in character mode and the daemon echoes and edits lines
 * itself (like CON:), which is the only way to give the shell cursor keys
 * and history; we also ask for the window size (NAWS) to draw lines that
 * wrap. A client that refuses ECHO (DONT ECHO) echoes locally and gets no
 * editor output from us. With DUMBTERM, ECHO and SGA are only agreed in raw
 * mode and the client edits cooked lines itself, as telnetd 2.0 does.
 * gWill[] tracks what we have announced, so acknowledgements are never
 * answered (RFC 854 loop rule). */
static void answer_option(int cmd, int opt)
{
    BOOL want = (opt == OPT_ECHO || opt == OPT_SGA) && (gEdit || gRaw);
    switch (cmd) {
    case TEL_DO:
        if (want) { if (!gWill[opt]) { gWill[opt] = TRUE; send_opt(TEL_WILL, opt); } }
        else      { gWill[opt] = FALSE; send_opt(TEL_WONT, opt); }
        break;
    case TEL_DONT:
        if (gWill[opt]) { gWill[opt] = FALSE; send_opt(TEL_WONT, opt); }
        break;
    case TEL_WILL:
        if (opt == OPT_NAWS && gEdit) {       /* usually the answer to our DO */
            if (!gDoNaws) { gDoNaws = TRUE; send_opt(TEL_DO, OPT_NAWS); }
        } else {
            send_opt(TEL_DONT, opt);          /* nothing else wanted from the client */
        }
        break;
    default:                                  /* WONT */
        if (opt == OPT_NAWS) gDoNaws = FALSE;
        break;
    }
}

/* IAC SB <opt> ... IAC SE received. NAWS: width (16 bit), height. */
static void sb_done(void)
{
    if (gSbOpt == OPT_NAWS && gSbLen >= 4) {
        LONG cols = ((LONG)gSbBuf[0] << 8) | gSbBuf[1];
        if (cols >= 20 && cols <= 1000) gCols = cols;
        logmsg("telnetd: window width %ld\n", gCols);
    }
}

/* Raw: bytes go to the program unedited and unechoed (it echoes itself).
 * Cooked: the line editor below (or, with DUMBTERM, the client's own line
 * mode, which needs ECHO and SGA switched back off). */
static void set_mode(BOOL raw)
{
    static const int opts[2] = { OPT_ECHO, OPT_SGA };
    int i;
    gRaw = raw;
    if (gEdit) return;                        /* character mode either way */
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
            return (e == SOCK_EWOULDBLOCK || e == SOCK_ENXIO || e == SOCK_EINTR) ? -1 : 0;
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
        case 3:                                /* SB: option byte */
            gSbOpt = ch;
            gSbLen = 0;
            gTelState = 5;
            continue;
        case 5:                                /* SB: data */
            if (ch == TEL_IAC) gTelState = 4;
            else if (gSbLen < (int)sizeof gSbBuf) gSbBuf[gSbLen++] = ch;
            continue;
        default:                               /* SB IAC */
            if (ch == TEL_SE) { gTelState = 0; sb_done(); continue; }
            if (ch == TEL_IAC && gSbLen < (int)sizeof gSbBuf) gSbBuf[gSbLen++] = ch;  /* escaped 255 */
            gTelState = 5;
            continue;
        }
    }
}


/* ---------------- ready queue ------------------------------------------ */

static LONG ready_room(void)
{
    return READYSZ - gRCount;
}

static void ready_put(unsigned char c)
{
    if (gRCount < READYSZ) {
        gReady[(gRHead + gRCount) % READYSZ] = c;
        gRCount++;
    }
}

static unsigned char ready_get(void)
{
    unsigned char c = gReady[gRHead];
    gRHead = (gRHead + 1) % READYSZ;
    gRCount--;
    return c;
}


/* ---------------- client cursor tracking ------------------------------- */

/* The column the client's cursor is in, followed through every byte we
 * send (program output and the editor's own), so the editor knows where
 * the prompt ended. gTermCol == gCols means "wrap pending": the last
 * column was just written and the next printable character lands at the
 * start of the next row (VT100/xterm behaviour). Cursor movement in
 * program output is followed for the usual CSI sequences; anything
 * exotic can leave this off, which only affects the editor's drawing. */
static void term_track(unsigned char c)
{
    LONG n;
    if (gTrkEsc == 1) {                       /* after ESC */
        gTrkEsc = (c == '[') ? 2 : 0;
        gTrkN = 0;
        gTrkN1 = -1;
        return;
    }
    if (gTrkEsc == 2) {                       /* inside ESC [ */
        if (c >= '0' && c <= '9') { if (gTrkN < 10000) gTrkN = gTrkN * 10 + (c - '0'); return; }
        if (c == ';') { gTrkN1 = gTrkN; gTrkN = 0; return; }
        if (c < 0x40) return;                 /* other parameter/intermediate bytes */
        gTrkEsc = 0;
        if (c != 'C' && c != 'D' && c != 'G' && c != 'H' && c != 'f') return;
        n = gTrkN ? gTrkN : 1;
        if (gTermCol >= gCols) gTermCol = gCols - 1;   /* movement ends a pending wrap */
        if (c == 'C')      gTermCol += n;
        else if (c == 'D') gTermCol -= n;
        else if (c == 'G') gTermCol = n - 1;
        else               gTermCol = (gTrkN1 >= 0 && gTrkN > 0) ? gTrkN - 1 : 0;
        if (gTermCol < 0) gTermCol = 0;
        if (gTermCol > gCols - 1) gTermCol = gCols - 1;
        return;
    }
    if (c == 27) { gTrkEsc = 1; return; }
    if (c == 13) { gTermCol = 0; return; }
    if (c == 10) { if (gTermCol >= gCols) gTermCol = gCols - 1; return; }
    if (c == 8) {
        if (gTermCol >= gCols) gTermCol = gCols - 1;
        if (gTermCol > 0) gTermCol--;
        return;
    }
    if (c == 9) {
        if (gTermCol >= gCols) gTermCol = gCols - 1;
        gTermCol = (gTermCol / 8 + 1) * 8;
        if (gTermCol > gCols - 1) gTermCol = gCols - 1;
        return;
    }
    if (c < 32 || (c >= 127 && c < 160)) return;   /* other controls: no movement */
    gTermCol = (gTermCol >= gCols) ? 1 : gTermCol + 1;
}


/* ---------------- cooked-mode line editor ------------------------------ */

/* Editor output goes to the client only while it lets us echo. */
static void ed_out(const unsigned char *p, LONG n)
{
    if (n > 0 && gWill[OPT_ECHO]) sock_write(p, n);
}

static void ed_outs(const char *s)
{
    ed_out((const unsigned char *)s, (LONG)strlen(s));
}

/* ESC [ n <c> */
static void ed_csi(LONG n, unsigned char c)
{
    unsigned char seq[8];
    int i = 0;
    seq[i++] = 27; seq[i++] = '[';
    if (n >= 1000) seq[i++] = (unsigned char)('0' + (n / 1000) % 10);
    if (n >= 100)  seq[i++] = (unsigned char)('0' + (n / 100) % 10);
    if (n >= 10)   seq[i++] = (unsigned char)('0' + (n / 10) % 10);
    seq[i++] = (unsigned char)('0' + n % 10);
    seq[i++] = c;
    ed_out(seq, i);
}

/* The line is laid out on a grid gCols wide, starting at column
 * gStartCol of its first row: line offset k sits at row
 * (gStartCol + k) / gCols, column (gStartCol + k) % gCols. gPos is the
 * offset the client's cursor is at. Invariant between keys: gPos == gCur,
 * and the row holding offset gLen exists on screen. */

/* Move the client's cursor from offset gPos to offset `to`. */
static void ed_goto(LONG to)
{
    LONG a = gStartCol + gPos, b = gStartCol + to;
    LONG ra = a / gCols, ca = a % gCols, rb = b / gCols, cb = b % gCols;
    if (rb != ra) {
        ed_csi(ra > rb ? ra - rb : rb - ra, (unsigned char)(ra > rb ? 'A' : 'B'));
        ed_outs("\r");
        if (cb > 0) ed_csi(cb, 'C');
    } else if (cb > ca) {
        ed_csi(cb - ca, 'C');
    } else if (cb < ca) {
        ed_csi(ca - cb, 'D');
    }
    gPos = to;
}

/* Write gLine[from..gLen) with the cursor at `from`. Text that ends exactly
 * at the right margin leaves the client in "wrap pending"; CR LF settles
 * the cursor on the next row, where the offset arithmetic expects it. */
static void ed_write_tail(LONG from)
{
    ed_out(gLine + from, gLen - from);
    gPos = gLen;
    if (gLen > from && (gStartCol + gLen) % gCols == 0) ed_outs("\r\n");
}

/* Redraw from offset `from` to the end, clear whatever the old line left
 * behind (possibly on rows below), and put the cursor back at gCur. */
static void ed_redraw(LONG from)
{
    ed_goto(from);
    ed_write_tail(from);
    ed_outs("\033[J");
    ed_goto(gCur);
}

/* A new line starts where the cursor is now: right after the prompt. */
static void ed_anchor(void)
{
    if (gTermCol >= gCols) ed_outs("\r\n");   /* the prompt filled its row exactly */
    gStartCol = (gTermCol >= gCols) ? 0 : gTermCol;
    gPos = 0;
}

/* Program output while a line is being edited (typeahead during a
 * command): take the line off the screen, let the output through, then
 * draw the line again after it, so the two never mix. */
static void ed_output_begin(void)
{
    if (gEdit && !gRaw && gLen > 0) {
        ed_goto(0);
        ed_outs("\033[J");
    }
}

static void ed_output_end(void)
{
    if (gEdit && !gRaw && gLen > 0) {
        ed_anchor();
        ed_write_tail(0);
        ed_goto(gCur);
    }
}

/* Replace the whole line (history recall). */
static void ed_set(const unsigned char *src, LONG n)
{
    memcpy(gLine, src, n);
    gLen = gCur = n;
    ed_redraw(0);
}

static const unsigned char *hist_entry(LONG k, LONG *len)   /* k = 1: newest */
{
    LONG slot = (gHistNext - k) & (HISTN - 1);
    *len = gHistLen[slot];
    return gHist[slot];
}

static void hist_add(void)
{
    LONG plen;
    const unsigned char *prev;
    if (gLen == 0) return;
    if (gHistCount > 0) {                   /* no consecutive duplicates */
        prev = hist_entry(1, &plen);
        if (plen == gLen && memcmp(prev, gLine, gLen) == 0) return;
    }
    memcpy(gHist[gHistNext], gLine, gLen);
    gHistLen[gHistNext] = gLen;
    gHistNext = (gHistNext + 1) & (HISTN - 1);
    if (gHistCount < HISTN) gHistCount++;
}

static void hist_move(int dir)               /* +1 = older (Up), -1 = newer (Down) */
{
    const unsigned char *h;
    LONG len;
    if (dir > 0) {
        if (gHistPos >= gHistCount) return;
        if (gHistPos == 0) { memcpy(gSaved, gLine, gLen); gSavedLen = gLen; }
        gHistPos++;
    } else {
        if (gHistPos == 0) return;
        gHistPos--;
    }
    if (gHistPos == 0) ed_set(gSaved, gSavedLen);
    else { h = hist_entry(gHistPos, &len); ed_set(h, len); }
}

static void ed_insert(unsigned char ch)
{
    if (gLen >= LINEMAX) { ed_outs("\007"); return; }
    memmove(gLine + gCur + 1, gLine + gCur, gLen - gCur);
    gLine[gCur] = ch;
    gLen++;
    gCur++;
    if (gCur == gLen) ed_write_tail(gCur - 1);   /* typing at the end: just echo */
    else ed_redraw(gCur - 1);
}

static void ed_delete(LONG at)               /* remove gLine[at], cursor to `at` */
{
    memmove(gLine + at, gLine + at + 1, gLen - at - 1);
    gLen--;
    gCur = at;
    ed_redraw(at);
}

static void ed_enter(void)
{
    LONG i;
    ed_goto(gLen);                           /* output continues below the whole line */
    if (gLen == 0 || (gStartCol + gLen) % gCols != 0)
        ed_outs("\r\n");                     /* else already at the start of a new row */
    for (i = 0; i < gLen; i++) ready_put(gLine[i]);
    ready_put('\n');
    hist_add();
    gLen = gCur = gPos = 0;
    gHistPos = 0;
}

static void ed_escape(unsigned char ch)      /* final byte of ESC [ ... / ESC O ... */
{
    switch (ch) {
    case 'A': hist_move(+1); break;
    case 'B': hist_move(-1); break;
    case 'C': if (gCur < gLen) { gCur++; ed_goto(gCur); } break;
    case 'D': if (gCur > 0) { gCur--; ed_goto(gCur); } break;
    case 'H': gCur = 0; ed_goto(gCur); break;
    case 'F': gCur = gLen; ed_goto(gCur); break;
    case '~':
        if (gEscParam == 1 || gEscParam == 7)      { gCur = 0; ed_goto(gCur); }
        else if (gEscParam == 4 || gEscParam == 8) { gCur = gLen; ed_goto(gCur); }
        else if (gEscParam == 3 && gCur < gLen)    ed_delete(gCur);
        break;
    default:                                  /* unknown sequence: ignore */
        break;
    }
}

/* One input character in cooked mode. Keys follow CON: where it has them
 * (Ctrl-X kills the line, Ctrl-\ is EOF, Ctrl-D/E/F send break signals)
 * plus the usual ANSI cursor, Home/End and Delete sequences. */
static void ed_key(unsigned char ch)
{
    if (gLen == 0 && gEsc == 0) ed_anchor();

    if (gEsc == 1) {
        if (ch == '[' || ch == 'O') { gEsc = 2; gEscParam = 0; return; }
        gEsc = 0;                             /* lone ESC: dropped */
    } else if (gEsc == 2) {
        if (ch >= '0' && ch <= '9') {
            if (gEscParam < 1000) gEscParam = gEscParam * 10 + (ch - '0');
            return;
        }
        if (ch == ';') { gEscParam += 10000; return; }   /* modifiers: not handled */
        gEsc = 0;
        if (gEscParam < 10000) ed_escape(ch);
        return;
    }

    switch (ch) {
    case 27:   gEsc = 1; return;
    case 0x9b: gEsc = 2; gEscParam = 0; return;       /* 8-bit CSI */
    case '\n': ed_enter(); return;
    case 8:
    case 127:
        if (gCur > 0) ed_delete(gCur - 1);
        return;
    case 0x18:                                /* Ctrl-X: kill line (CON:) */
    case 0x15:                                /* Ctrl-U */
        gLen = gCur = 0;
        ed_redraw(0);
        return;
    case 0x0b:                                /* Ctrl-K: kill to end of line */
        gLen = gCur;
        ed_redraw(gCur);
        return;
    case 0x1c:                                /* Ctrl-\: EOF on an empty line */
        if (gLen == 0) gEof = TRUE;
        return;
    case 4: send_signal(SIGBREAKF_CTRL_D); return;
    case 5: send_signal(SIGBREAKF_CTRL_E); return;
    case 6: send_signal(SIGBREAKF_CTRL_F); return;
    default:
        if (ch >= 32 && !(ch >= 127 && ch < 160)) ed_insert(ch);
        return;                               /* other controls: ignored */
    }
}

/* DUMBTERM: the client edits the line itself (line mode, local echo) and
 * sends it whole; collect it up to the line end, without echo. BS/DEL
 * and Ctrl-X are honoured for clients that send keys one by one anyway. */
static void plain_key(unsigned char ch)
{
    LONG i;
    switch (ch) {
    case '\n':
        for (i = 0; i < gLen; i++) ready_put(gLine[i]);
        ready_put('\n');
        gLen = 0;
        return;
    case 8:
    case 127:
        if (gLen > 0) gLen--;
        return;
    case 0x18:
    case 0x15:
        gLen = 0;
        return;
    case 0x1c:
        if (gLen == 0) gEof = TRUE;
        return;
    default:
        if (gLen < LINEMAX) gLine[gLen++] = ch;
        return;
    }
}

static void do_hangup(void);

/* Move socket input into the ready queue (through the line editor in
 * cooked mode). Stops while the queue could not take a whole line, so
 * input is never dropped; the socket is then not selected until a READ
 * makes room. */
static void pump_input(void)
{
    unsigned char ch;
    LONG r;
    while (!gHangup && ready_room() > LINEMAX + 1) {
        r = next_char(&ch);
        if (r == 0) { logmsg("telnetd: hangup eof\n", 0); do_hangup(); return; }
        if (r < 0) return;
        if (gRaw)       ready_put(ch);
        else if (gEdit) ed_key(ch);
        else            plain_key(ch);
    }
}


/* === END portable input/editor section ================================= */


/* ---------------- handler ports ---------------------------------------- */

/* A handler port plus the handles still open on it after its session
 * ended (a shell that outlived its client, a "run" job opening "*"): the
 * port is not reused while there are any, so their END can never be
 * taken off a later session's count. */
struct HPort {
    struct MsgPort  mp;               /* first: used as a plain MsgPort */
    LONG            deadOpens;
};

/* A port on the shared signal: a CreateMsgPort per session would use up
 * the 16 user signals once a few shells outlive their clients. */
static struct MsgPort *port_new(void)
{
    struct MsgPort *p;
    p = (struct MsgPort *)AllocMem(sizeof(struct HPort), MEMF_PUBLIC | MEMF_CLEAR);
    if (!p) return NULL;
    p->mp_Node.ln_Type = NT_MSGPORT;
    p->mp_Flags = PA_SIGNAL;
    p->mp_SigBit = (UBYTE)gPortSig;
    p->mp_SigTask = FindTask(NULL);
    NewList(&p->mp_MsgList);
    return p;
}

/* The longest-retired port with no handles left on it, or a new one. */
static struct MsgPort *port_get(void)
{
    struct Node *n;
    for (n = gRetired.lh_Head; n->ln_Succ; n = n->ln_Succ) {
        if (((struct HPort *)n)->deadOpens <= 0) {
            Remove(n);
            return (struct MsgPort *)n;
        }
    }
    return port_new();
}

static LONG dead_opens(void)
{
    struct Node *n;
    LONG sum = 0;
    for (n = gRetired.lh_Head; n->ln_Succ; n = n->ln_Succ)
        sum += ((struct HPort *)n)->deadOpens;
    return sum;
}

/* Exit with processes possibly still holding a port as console task:
 * leave it allocated but inert - PA_IGNORE queues without signalling, so a
 * late packet waits forever instead of landing in freed memory. */
static void port_abandon(struct MsgPort *p)
{
    Forbid();
    p->mp_Flags = PA_IGNORE;
    p->mp_SigTask = NULL;
    Permit();
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

/* ---------------- WAIT_CHAR timing ------------------------------------- */

static BOOL tv_before(const struct timeval *a, const struct timeval *b)
{
    return a->tv_secs < b->tv_secs ||
           (a->tv_secs == b->tv_secs && a->tv_micro < b->tv_micro);
}

static void timer_stop(void)
{
    if (gTimerBusy) {
        AbortIO((struct IORequest *)gTimer);
        WaitIO((struct IORequest *)gTimer);
        gTimerBusy = FALSE;
    }
}

static void charwait_reply(int i, LONG res)
{
    struct DosPacket *pkt = gCW[i].pkt;
    gCW[i] = gCW[--gNCW];
    reply(pkt, res, 0);
}

/* Answer what can be answered, then re-arm the timer for the earliest
 * deadline left. Called when a waiter is added, when the timer fires and
 * when input becomes ready. */
static void charwait_update(void)
{
    struct timeval now, d;
    int i, e;

    timer_stop();
    if (gNCW == 0) return;
    if (gRCount > 0 || gEof) {
        while (gNCW > 0) charwait_reply(gNCW - 1, DOSTRUE);
        return;
    }
    GetSysTime(&now);
    for (i = gNCW - 1; i >= 0; i--)            /* descending: the swapped-in */
        if (!tv_before(&now, &gCW[i].due))     /* entry was already checked  */
            charwait_reply(i, DOSFALSE);
    if (gNCW == 0) return;

    for (e = 0, i = 1; i < gNCW; i++)
        if (tv_before(&gCW[i].due, &gCW[e].due)) e = i;
    d = gCW[e].due;                            /* d = due - now, due > now */
    if (d.tv_micro < now.tv_micro) { d.tv_micro += 1000000; d.tv_secs--; }
    d.tv_micro -= now.tv_micro;
    d.tv_secs  -= now.tv_secs;
    gTimer->tr_node.io_Command = TR_ADDREQUEST;
    gTimer->tr_time = d;
    SendIO((struct IORequest *)gTimer);
    gTimerBusy = TRUE;
}

/* Answer every queued READ and WAIT_CHAR: READ gets break (see below),
 * WAIT_CHAR gets "no character". */
static void flush_waiters(void)
{
    struct Message *msg;
    timer_stop();
    while (gNCW > 0) charwait_reply(gNCW - 1, DOSFALSE);
    while ((msg = (struct Message *)RemHead(&gReadWait)) != NULL)
        reply(pkt_of(msg), DOSFALSE, ERROR_BREAK);  /* READ: break semantics - a plain 0-byte EOF makes the shell re-read forever */
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

/* Answer WAIT_CHAR waiters once input is ready, then complete queued
 * READs from the ready queue. A cooked READ gets at most one line (the
 * queue only ever holds whole lines in cooked mode); a raw READ gets
 * whatever is there. */
static void service_reads(void)
{
    struct Message *msg;
    if (gHangup) return;
    if (gNCW > 0 && (gRCount > 0 || gEof)) charwait_update();
    while ((msg = (struct Message *)gReadWait.lh_Head)->mn_Node.ln_Succ) {
        struct DosPacket *pkt = pkt_of(msg);

        /* ACTION_READ: dp_Arg2 = buffer, dp_Arg3 = length */
        if (gRCount == 0) {
            if (!gEof) return;                 /* wait for input */
            gEof = FALSE;
            Remove(&msg->mn_Node);
            reply(pkt, 0, 0);                  /* Ctrl-\: EOF, as on CON: */
            continue;
        }
        pkt->dp_Res1 = 0;
        while (pkt->dp_Res1 < pkt->dp_Arg3 && gRCount > 0) {
            unsigned char c = ready_get();
            ((unsigned char *)pkt->dp_Arg2)[pkt->dp_Res1++] = c;
            if (!gRaw && c == '\n') break;
        }
        Remove(&msg->mn_Node);
        reply(pkt, pkt->dp_Res1, 0);
    }
}

/* Serves one packet that arrived on `port`: the current session's port
 * (gPort), or a retired one. Packets on a retired port belong to no
 * session and are answered harmlessly; its open handles are counted on
 * the port. */
static void handle_packet(struct MsgPort *port, struct Message *msg)
{
    struct DosPacket *pkt = pkt_of(msg);
    BOOL cur  = (port == gPort && gCookie != 0);
    BOOL live = (cur && !gHangup);

    switch (pkt->dp_Type) {

    case ACTION_FINDINPUT:
    case ACTION_FINDOUTPUT:
    case ACTION_FINDUPDATE: {
        /* Open("*") by a process whose console task is our port. */
        struct FileHandle *fh = (struct FileHandle *)BADDR((BPTR)pkt->dp_Arg1);
        fh->fh_Type = port;
        fh->fh_Port = port;                    /* non-zero = interactive */
        fh->fh_Arg1 = cur ? gCookie : 0;       /* informational */
        if (cur) {
            gOpens++;
            gSawOpen = TRUE;
            logmsg("telnetd: find, opens %ld\n", gOpens);
        } else {
            ((struct HPort *)port)->deadOpens++;
        }
        reply(pkt, DOSTRUE, 0);
        break;
    }

    case ACTION_READ:
        if (!live) { reply(pkt, DOSFALSE, ERROR_BREAK); break; }   /* break, not bare EOF: the shell exits on it */
        note_reader(pkt);
        pkt->dp_Res1 = 0;
        logmsg("telnetd: read queued\n", 0);
        AddTail(&gReadWait, &msg->mn_Node);
        break;

    case ACTION_WRITE:
        logmsg("telnetd: write %ld\n", pkt->dp_Arg3);
        if (live) {
            ed_output_begin();                /* typeahead line out of the way */
            if (sock_write((const unsigned char *)pkt->dp_Arg2, pkt->dp_Arg3) < 0) {
                logmsg("telnetd: hangup write fail\n", 0);
                do_hangup();
            } else {
                ed_output_end();
            }
        }
        reply(pkt, pkt->dp_Arg3, 0);          /* output nobody can see is discarded */
        break;

    case ACTION_WAIT_CHAR: {                   /* dp_Arg1 = timeout in us, no cookie */
        ULONG us = (ULONG)pkt->dp_Arg1;
        struct timeval due;
        if (!live) { reply(pkt, DOSFALSE, 0); break; }
        note_reader(pkt);
        if (gRCount > 0 || gEof) { reply(pkt, DOSTRUE, 0); break; }
        if (us == 0 || gNCW >= MAXCW) { reply(pkt, DOSFALSE, 0); break; }  /* poll, or table full */
        GetSysTime(&due);
        due.tv_secs  += us / 1000000;
        due.tv_micro += us % 1000000;
        if (due.tv_micro >= 1000000) { due.tv_micro -= 1000000; due.tv_secs++; }
        gCW[gNCW].pkt = pkt;
        gCW[gNCW].due = due;
        gNCW++;
        charwait_update();
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
        if (cur) {
            gOpens--;
            logmsg("telnetd: END, %ld handles left\n", gOpens);
        } else if (((struct HPort *)port)->deadOpens > 0) {
            if (--((struct HPort *)port)->deadOpens == 0)
                logmsg("telnetd: a hung-up shell has ended\n", 0);
        }
        break;

    case ACTION_SEEK:
        reply(pkt, -1, ERROR_OBJECT_WRONG_TYPE);
        break;

    case ACTION_DISK_INFO: {                /* more and friends probe the console */
        /* dp_Arg1 is a BPTR (dos Packets doc: "ARG1: BPTR to InfoData"),
         * like every DOS struct argument: used as a C pointer it would
         * write 36 bytes at a quarter of the real address. */
        struct td_InfoData *id = (struct td_InfoData *)BADDR((BPTR)pkt->dp_Arg1);
        if (id) {
            memset(id, 0, sizeof *id);
            id->id_DiskType = ID_NO_DISK_PRESENT;
            id->id_InUse = DOSTRUE;
        }
        reply(pkt, DOSTRUE, 0);
        break;
    }

    case ACTION_IS_FILESYSTEM:
        reply(pkt, DOSFALSE, 0);
        break;

    default:
        logmsg("telnetd: unknown packet %ld\n", pkt->dp_Type);
        reply(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
        break;
    }
}

static void drain_ports(void)
{
    struct Message *msg;
    struct Node *n;
    if (gPort)
        while ((msg = GetMsg(gPort)) != NULL) handle_packet(gPort, msg);
    for (n = gRetired.lh_Head; n->ln_Succ; n = n->ln_Succ) {
        struct MsgPort *p = (struct MsgPort *)n;
        while ((msg = GetMsg(p)) != NULL) handle_packet(p, msg);
    }
}


/* ---------------- one session ------------------------------------------ */

/* The spawn runs in this short-lived helper, never in the daemon: the
 * daemon is the handler for the handles System() is given, and a direct
 * SystemTags(SYS_Asynch) from it hangs inside the call on real hardware
 * (most likely System() waiting on a packet to those handles that only
 * the blocked daemon could answer). */
static BPTR g_spawnIn = 0, g_spawnOut = 0;
static struct MsgPort *g_spawnPort = NULL;
static struct Task    *g_daemonTask = NULL;
static ULONG           g_spawnMask = 0;    /* helper -> daemon: "I have my copy" */

/* Uses the daemon's DOSBase, which stays open until the daemon has seen
 * every helper finish (gHelpers), so there is no failure path that could
 * skip closing the handles or the gHelpers count. */
static int spawner_entry(void)
{
    BPTR in = g_spawnIn, out = g_spawnOut;
    struct MsgPort *port = g_spawnPort;
    LONG rc;

    Signal(g_daemonTask, g_spawnMask);         /* g_spawn* may be reused now */

    rc = SystemTags((STRPTR)"NewShell *",
                    SYS_Input,      in,
                    SYS_Output,     out,
                    NP_ConsoleTask, (LONG)port,
                    NP_Cli,         TRUE,
                    TAG_DONE);
    /* NewShell starts the interactive shell as a NEW process and returns
     * at once, so this returns long before the session ends. A synchronous
     * System() does not close SYS_Input/SYS_Output (only SYS_Asynch does);
     * the V36 SystemTagList autodoc says the caller must close them after
     * System returns, and AROS's systemtaglist.c only ever closes handles
     * it opened itself. Without this the two handles stay counted in
     * gOpens forever and "endcli" never ends the session. telnetd 2.0's
     * SubSubProc does
     * exactly this, Forbid() first: this code lives in the daemon's seglist, and once the last
     * END is answered the daemon may exit and unload it. Close() waits for
     * our reply (Wait breaks the Forbid); after it returns we are
     * Forbid()den again until the process is gone. */
    logmsg("telnetd: helper rc %ld\n", rc);
    Forbid();
    Close(in);
    Close(out);
    gHelpers--;                                /* still Forbid()den: we are gone */
    return (int)rc;
}

/* AllocDosObject() returns a plain C pointer, NOT a BPTR (dos_protos.h:
 * APTR). Cast to BPTR and BADDR()ed, every field below would be written
 * to 4x the real address - on a 24-bit 68000 bus anywhere from chip RAM
 * to the CIAs and custom chips. Convert with MKBADDR. */
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
    LONG start = now_secs();
    ULONG one = 1;

    gPort = port_get();
    if (!gPort) {
        PutStr((STRPTR)"telnetd: out of memory for a session\n");
        return;
    }
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
    gRHead = gRCount = 0;
    gEof = FALSE;
    gLen = gCur = 0;
    gHistCount = gHistNext = gHistPos = 0;
    gEsc = 0;
    gCols = 80;
    gDoNaws = FALSE;
    gTermCol = 0;
    gTrkEsc = 0;
    gStartCol = gPos = 0;

    IoctlSocket(gSock, FIONBIO, (APTR)&one);

    /* Line editor: character mode with server echo (it needs every key)
     * and the window width. DUMBTERM: nothing - the client stays in its own
     * line mode until a program asks for raw mode. */
    if (gEdit) {
        gWill[OPT_ECHO] = gWill[OPT_SGA] = TRUE;
        gDoNaws = TRUE;
        send_opt(TEL_WILL, OPT_ECHO);
        send_opt(TEL_WILL, OPT_SGA);
        send_opt(TEL_DO, OPT_NAWS);
    }

    in  = make_handle();
    out = make_handle();
    if (!in || !out) {
        if (in)  FreeDosObject(DOS_FILEHANDLE, (APTR)BADDR(in));
        if (out) FreeDosObject(DOS_FILEHANDLE, (APTR)BADDR(out));
        goto done;
    }

    /* Two distinct handles, closed by the helper once NewShell has
     * returned (one ACTION_END each). NewShell opens "*" on
     * NP_ConsoleTask = gPort for the interactive shell it starts. */
    gOpens = 2;
    logmsg("telnetd: session %ld, spawning shell\n", gCookie);
    g_spawnIn   = in;
    g_spawnOut  = out;
    g_spawnPort = gPort;
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
        /* A session can end within milliseconds (a client that connects
         * and leaves at once) and the next one would overwrite g_spawn*
         * before this helper has read them: wait for its copy. */
        SetSignal(0, g_spawnMask);
        if (CreateNewProc(ptags) == NULL) {
            logmsg("telnetd: CreateNewProc failed\n", 0);
            FreeDosObject(DOS_FILEHANDLE, (APTR)BADDR(in));
            FreeDosObject(DOS_FILEHANDLE, (APTR)BADDR(out));
            gOpens = 0;
            goto done;
        }
        Wait(g_spawnMask);
        gHelpers++;
    }
    logmsg("telnetd: shell started\n", 0);

    for (;;) {
        fd_set rd;
        struct timeval tv;
        ULONG mask = (1UL << gPortSig)
                   | (1UL << gTimePort->mp_SigBit)
                   | SIGBREAKF_CTRL_C;

        /* Over when every handle is closed - but not before NewShell has
         * opened "*", or its late FINDINPUT would find no session. */
        if (gOpens <= 0 && (gSawOpen || now_secs() - start > SPAWN_SECS)) break;
        /* Client gone: nothing left to wait for here - the shell got
         * Ctrl-C and break/EOF, and if it has not ended yet its handles
         * are carried by the retired port below. */
        if (gHangup) break;

        /* Watch the socket whenever its input can be taken: keys are
         * echoed and a disconnect is seen even while a command runs. Not
         * when the ready queue is full - readable data nobody consumes
         * would make WaitSelect return at once, forever, spinning the
         * CPU on typeahead while a command runs. */
        FD_ZERO(&rd);
        if (!gHangup && ready_room() > LINEMAX + 1) FD_SET(gSock, &rd);
        tv.tv_secs = 1;
        tv.tv_micro = 0;
        WaitSelect(gSock + 1, &rd, NULL, NULL, &tv, &mask);

        if ((mask & SIGBREAKF_CTRL_C) && !gHangup) {
            PutStr((STRPTR)"telnetd: break - ending session\n");
            gBreak = TRUE;
            logmsg("telnetd: hangup break\n", 0);
            do_hangup();
        }

        /* WAIT_CHAR deadline reached */
        if (GetMsg(gTimePort) != NULL) {
            gTimerBusy = FALSE;
            charwait_update();
        }

        drain_ports();
        pump_input();
        service_reads();
    }

done:
    gHangup = TRUE;
    flush_waiters();
    if (gOpens > 0)
        logmsg("telnetd: hung up, shell still has %ld handles\n", gOpens);
    /* Retire the port with the handles still open on it: the shell's
     * remaining packets are answered there, and the port is reused only
     * once they are closed. */
    ((struct HPort *)gPort)->deadOpens = gOpens > 0 ? gOpens : 0;
    AddTail(&gRetired, &gPort->mp_Node);
    gPort = NULL;
    gOpens = 0;
    gCookie = 0;
}


/* ------------------------------- main ---------------------------------- */

int main(int argc, char **argv)
{
    LONG args[3] = { 0, 0, 0 };
    struct RDArgs *rd;
    struct sockaddr_in sa;
    struct Process *self = (struct Process *)FindTask(NULL);
    ULONG port = 23;
    ULONG stack = (ULONG)self->pr_Task.tc_SPUpper - (ULONG)self->pr_Task.tc_SPLower;
    int one = 1;
    BYTE spawnSig = -1;
    APTR oldwinptr = self->pr_WindowPtr;

    (void)argc; (void)argv;

    if (stack < MIN_STACK) {
        Printf((STRPTR)"telnetd: stack is %lu bytes, needs %ld - run 'stack 20000' first\n",
               stack, (LONG)MIN_STACK);
        return RETURN_FAIL;
    }

    rd = ReadArgs((STRPTR)"PORT/N,LOG/K,DUMBTERM/S", args, NULL);
    if (rd == NULL) {
        PutStr((STRPTR)"usage: telnetd [PORT <n>] [LOG <file>] [DUMBTERM]\n");
        return RETURN_FAIL;
    }
    if (args[0]) port = *(ULONG *)args[0];
    gLogName = (STRPTR)args[1];
    gEdit = !args[2];
    if (gLogName) {                          /* fresh trace per run */
        BPTR f = Open(gLogName, MODE_NEWFILE);
        if (f) Close(f);
    }
    logmsg("telnetd: started, stack %ld\n", (LONG)stack);

    NewList(&gRetired);
    g_daemonTask = FindTask(NULL);
    gPortSig = AllocSignal(-1);
    spawnSig = AllocSignal(-1);
    gTimePort = CreateMsgPort();
    if (gTimePort)
        gTimer = (struct timerequest *)CreateIORequest(gTimePort, sizeof(struct timerequest));
    if (gPortSig < 0 || spawnSig < 0 || !gTimer ||
        OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)gTimer, 0)) {
        PutStr((STRPTR)"telnetd: out of signals/memory\n");
        if (gTimer) DeleteIORequest((struct IORequest *)gTimer);
        if (gTimePort) DeleteMsgPort(gTimePort);
        if (gPortSig >= 0) FreeSignal(gPortSig);
        if (spawnSig >= 0) FreeSignal(spawnSig);
        FreeArgs(rd);
        return RETURN_FAIL;
    }
    g_spawnMask = 1UL << spawnSig;
    TimerBase = gTimer->tr_node.io_Device;

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
        ULONG mask = SIGBREAKF_CTRL_C | (1UL << gPortSig);
        struct timeval tv;
        struct sockaddr_in ca;
        socklen_t calen = sizeof ca;
        LONG selr;

        FD_ZERO(&rdset);
        FD_SET(gListen, &rdset);
        tv.tv_secs = 2;
        tv.tv_micro = 0;
        selr = WaitSelect(gListen + 1, &rdset, NULL, NULL, &tv, &mask);
        drain_ports();                       /* shells that outlived their session */
        if (mask & SIGBREAKF_CTRL_C) break;
        if (selr <= 0 || !FD_ISSET(gListen, &rdset)) continue;

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

    /* Give shells of hung-up sessions EXIT_SECS to end, and wait - with no
     * time limit - for every spawn helper: helpers run code in this
     * program's seglist, which must not be unloaded under them. */
    if (SocketBase) {
        LONG until = now_secs() + EXIT_SECS;
        BOOL note = FALSE;
        while (gHelpers > 0 || (dead_opens() > 0 && now_secs() < until)) {
            struct timeval tv;
            ULONG m = 1UL << gPortSig;
            tv.tv_secs = 1;
            tv.tv_micro = 0;
            WaitSelect(0, NULL, NULL, NULL, &tv, &m);
            drain_ports();
            if (gHelpers > 0 && !note && now_secs() >= until) {
                PutStr((STRPTR)"telnetd: waiting for a shell to finish starting\n");
                note = TRUE;
            }
        }
        CloseLibrary(SocketBase);
    }
    self->pr_WindowPtr = oldwinptr;

    /* Processes started from a session (a "run" job, a shell that ignored
     * the hangup) may keep a port as their console task and PutMsg to it
     * after we are gone: every port is left allocated but inert. */
    drain_ports();
    while (!IsListEmpty(&gRetired))
        port_abandon((struct MsgPort *)RemHead(&gRetired));
    timer_stop();                            /* idle outside a session anyway */
    CloseDevice((struct IORequest *)gTimer);
    DeleteIORequest((struct IORequest *)gTimer);
    DeleteMsgPort(gTimePort);
    FreeSignal(gPortSig);
    FreeSignal(spawnSig);
    FreeArgs(rd);
    return RETURN_OK;
}
