/*
 * telnetd.c — a standalone telnet daemon for AmigaOS 2.04+
 *
 * The session architecture follows the approach of telnetd 2.0 (Peter
 * Simons & Steve Holland, 1995), reimplemented for AmiTCP_NG 4.x: no
 * inetd, no usergroup.library, several sessions served by one process,
 * LAN-only. No code from the GPLv2 original is used.
 *
 * Copyright (c) 2026 Tor Anders Johansen. MIT License — see LICENSE.
 *
 * Mechanism:
 *   One process serves every connection from a single WaitSelect loop.
 *   Each session has its own PRIVATE handler port; all handler ports share
 *   one signal bit. For a connection the daemon makes two DOS filehandles
 *   whose fh_Type is the session's port, then a short-lived helper process
 *   runs SystemTags("NewShell *") synchronously with them as stdio and
 *   NP_ConsoleTask = that port, and closes them when NewShell returns. The
 *   interactive shell NewShell starts opens "*" on the port, so its
 *   console traffic arrives as DosPackets on the session's port:
 *     ACTION_FIND*         — "*" opened again: opencount++
 *     ACTION_READ          — queued; cooked: one line from the line
 *                            editor (echo, cursor keys, history, as
 *                            CON: does), raw: what is available
 *     ACTION_WRITE         — copied into the session's output buffer
 *                            (LF -> CRLF, CSI -> ESC [, IAC escaped) and
 *                            answered once all of it is queued, so a slow
 *                            client only slows its own shell
 *     ACTION_WAIT_CHAR     — one timer.device request serves all waiters
 *     ACTION_SCREEN_MODE   — raw/cooked
 *     ACTION_CHANGE_SIGNAL — who gets Ctrl-C
 *     ACTION_END           — opencount--; zero ends the session
 *   Which session a packet belongs to is the port it arrives on. Ports of
 *   ended sessions are never freed while the daemon runs: they are kept,
 *   answered harmlessly (READ gets break/EOF, WRITE is discarded) and
 *   reused, because a process started from a session (a "run" job) keeps
 *   the port as its console task and may still send to it.
 *
 * Why private ports: pr_MsgPort is where dos.library waits for the
 * replies to the process's own packets, and WaitPkt() takes whatever
 * arrives first. Serving console packets on pr_MsgPort (as telnetd 2.0
 * does) is only safe for a process that makes no DOS calls of its own
 * once the shell runs; otherwise any Open/Write/PutStr can swallow a
 * shell packet as its reply, and the real filesystem reply lands in the
 * packet loop and gets bounced back to the filesystem handler. With
 * private ports the daemon's own DOS I/O and the shells' console traffic
 * never meet. docs/DESIGN.md has the full account.
 *
 * All socket calls stay in this one process: AmiTCP_NG only accepts them
 * from the task that opened bsdsocket.library.
 *
 * Build (cross toolchain):
 *   m68k-amigaos-gcc -Os -m68000 -Wall -Wextra -o telnetd telnetd.c -s -lamiga
 *
 * Run:  1> stack 20000
 *       1> telnetd                         ; port 23, up to 4 sessions
 *       1> telnetd 2323                    ; custom port
 *       1> telnetd MAXSESSIONS=2           ; fewer (or more, up to 8)
 *       1> telnetd LOG=T:tdbg.log          ; crash-surviving trace (any path)
 *       1> telnetd DUMBTERM                ; clients without ANSI support
 *       1> telnetd SHELLSTACK=40000        ; stack for commands in sessions
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
#include <devices/conunit.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <intuition/intuition.h>
#include <proto/intuition.h>
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

static const char __attribute__((used)) verstag[] =
    "$VER: telnetd 0.5.1 (8.10.2026)";

#define MIN_STACK    16000            /* refuse to run on a smaller stack */
#define EXIT_SECS    5                /* on Ctrl-C: time for hung-up shells to end */
#define SPAWN_SECS   10               /* NewShell must open "*" by then */
#define CLOSE_SECS   5                /* after the shell ends: time to send its last output */
#define STALL_SECS   60               /* client not taking output for this long: hang up */
#define MAXSLOTS     16               /* live and closing sessions */
#define DEFSESSIONS  4                /* MAXSESSIONS default */
#define MAXSESSIONS_LIMIT 8

struct Library *SocketBase = NULL;    /* extern in proto/bsdsocket.h */
struct IntuitionBase *IntuitionBase = NULL;   /* extern in proto/intuition.h; only with CONWINDOW */

static STRPTR           gLogName  = NULL;


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


/* === BEGIN portable input/editor section ===============================
 * Everything from here to the END marker is plain C over struct Console
 * plus recv/Errno/logmsg and the two hooks declared below, so
 * tests/edtest.c compiles it on the host (make test). Keep it that way. */

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

#define READYSZ      1024             /* power of two */
#define OUTSZ        4096             /* power of two */
#define ED_RESERVE   1024             /* output room kept for the editor and replies */
#define HELDSZ       256              /* keys read while they cannot be edited yet */
#define HELD_ROOM    64               /* output room needed to read at all (option replies) */
#define LINEMAX      255
#define HISTN        16               /* power of two */

/* One telnet connection's console: everything between the socket and the
 * shell's READ/WRITE packets that does not touch AmigaOS. */
struct Console {
    int             sock;
    BOOL            hangup;          /* client gone or daemon stopping */
    BOOL            raw;             /* SetMode(fh, 1) in effect */
    BOOL            edit;            /* FALSE with DUMBTERM: client edits lines */
    BOOL            writing;         /* a program WRITE is half queued */
    UBYTE           will[256];       /* options we currently have WILL'd */

    /* input decoding */
    unsigned char   in[256];
    LONG            inHead, inTail;
    int             telState;        /* 0 data, 1 IAC, 2 option, 3 SB option, 4 SB IAC, 5 SB data */
    int             telCmd;
    BOOL            lastCR;
    int             sbOpt;           /* subnegotiation being received */
    unsigned char   sbBuf[8];
    int             sbLen;

    /* Input that is ready for READ: whole edited lines in cooked mode, raw
     * bytes in raw mode. The socket is drained into it whether or not a
     * READ is waiting, so typeahead is echoed and a disconnect seen. */
    unsigned char   held[HELDSZ];    /* decoded keys waiting for the editor */
    LONG            hHead, hCount;
    unsigned char   ready[READYSZ];
    LONG            rHead, rCount;
    BOOL            eof;             /* Ctrl-\ on an empty line */

    /* Bytes for the client, exactly as they go on the wire. */
    unsigned char   out[OUTSZ];
    LONG            oHead, oCount;
    LONG            dropped;         /* bytes lost to a full buffer (must stay 0) */
    unsigned char   prevOut;         /* last program byte, for LF -> CRLF */

    /* Cooked-mode line editor with history. On a real Amiga this is CON:'s
     * job, not the shell's: the shell only reads finished lines, so cursor
     * keys and history must be done by whoever plays the console - us. */
    unsigned char   line[LINEMAX];
    LONG            len, cur;
    unsigned char   saved[LINEMAX];  /* the line being typed before Up */
    LONG            savedLen;
    unsigned char   hist[HISTN][LINEMAX];
    LONG            histLen[HISTN];
    LONG            histCount, histNext, histPos;
    int             esc;             /* 0 none, 1 ESC, 2 ESC [ / ESC O / CSI */
    LONG            escParam;

    /* client screen geometry, for drawing lines that wrap */
    LONG            cols;            /* from NAWS; 80 if the client never says */
    LONG            rows;            /* from NAWS; 24 if the client never says */
    BOOL            nawsSeen;        /* the client has reported a window size */
    BOOL            doNaws;          /* we have sent DO NAWS */
    LONG            termCol;         /* client cursor column (see term_track) */
    LONG            termRow;         /* client cursor row, 0-based (see term_track) */
    BOOL            trkSpace;        /* CSI sequence had a SPACE intermediate */
    int             trkEsc;
    LONG            trkN, trkN1;
    LONG            startCol;        /* column where the edited line begins */
    LONG            pos;             /* line offset of the client's cursor */
};

/* Provided by the daemon (or the test harness). */
static void send_signal(struct Console *c, ULONG sig);
static void do_hangup(struct Console *c);

static void con_init(struct Console *c, int sock, BOOL edit)
{
    memset(c, 0, sizeof *c);
    c->sock = sock;
    c->edit = edit;
    c->cols = 80;
    c->rows = 24;
    c->trkN1 = -1;
}

static void send_break(struct Console *c)
{
    send_signal(c, SIGBREAKF_CTRL_C);
}


/* ---------------- output buffer ---------------------------------------- */

static LONG out_room(struct Console *c)
{
    return OUTSZ - c->oCount;
}

/* Never full in practice: program output stops ED_RESERVE short of the
 * end, the editor only runs with ED_RESERVE free and writes far less per
 * key, and option replies never touch the reserve (send_opt). */
static void out_put(struct Console *c, unsigned char b)
{
    if (c->oCount < OUTSZ) {
        c->out[(c->oHead + c->oCount) & (OUTSZ - 1)] = b;
        c->oCount++;
    } else {
        c->dropped++;
    }
}

/* Option replies are skipped, whole, rather than eat into the editor's
 * reserve - only a client flooding requests while output is backed up
 * can get here, and it loses replies, never parts of other output. */
static void send_opt(struct Console *c, int cmd, int opt)
{
    if (out_room(c) < ED_RESERVE + 3) return;
    out_put(c, TEL_IAC);
    out_put(c, (unsigned char)cmd);
    out_put(c, (unsigned char)opt);
}

static void term_track(struct Console *c, unsigned char ch);

/* Text for the client's terminal: bare LF -> CRLF, CSI -> ESC [, IAC
 * escaped. Every byte the terminal will see goes through term_track(), so
 * the line editor knows the cursor column. */
static void con_write(struct Console *c, const unsigned char *p, LONG n)
{
    LONG i;
    if (c->hangup) return;
    for (i = 0; i < n; i++) {
        unsigned char ch = p[i];
        if (ch == 255) {                       /* IAC escape */
            out_put(c, 255); out_put(c, 255);
            term_track(c, 255);
        } else if (ch == 0x9b) {               /* Amiga CSI -> ANSI ESC [ */
            out_put(c, 27); out_put(c, '[');
            term_track(c, 27); term_track(c, '[');
        } else if (ch == 12) {               /* FF: Amiga console clears and homes */
            out_put(c, 27); out_put(c, '['); out_put(c, '2'); out_put(c, 'J');
            out_put(c, 27); out_put(c, '['); out_put(c, 'H');
            term_track(c, 27); term_track(c, '['); term_track(c, '2'); term_track(c, 'J');
            term_track(c, 27); term_track(c, '['); term_track(c, 'H');
        } else if (ch == 10 && c->prevOut != 13) {
            out_put(c, 13); out_put(c, 10);    /* bare LF -> CRLF */
            term_track(c, 13); term_track(c, 10);
        } else {
            out_put(c, ch);
            term_track(c, ch);
        }
        c->prevOut = ch;
    }
}

/* How many of n program bytes con_write can take without producing more
 * than room bytes: FF becomes seven, IAC, CSI and a bare LF become two. */
static LONG con_fit(const struct Console *c, const unsigned char *p, LONG n, LONG room)
{
    unsigned char prev = c->prevOut;
    LONG i;
    for (i = 0; i < n; i++) {
        unsigned char ch = p[i];
        LONG w = ch == 12 ? 7
               : (ch == 255 || ch == 0x9b || (ch == 10 && prev != 13)) ? 2 : 1;
        if (w > room) break;
        room -= w;
        prev = ch;
    }
    return i;
}


/* ---------------- telnet option handling ------------------------------- */

/* With the line editor (the default) we always agree to ECHO and SGA:
 * the client runs in character mode and the daemon echoes and edits lines
 * itself (like CON:), which is the only way to give the shell cursor keys
 * and history; we also ask for the window size (NAWS) to draw lines that
 * wrap. A client that refuses ECHO (DONT ECHO) echoes locally and gets no
 * editor output from us. With DUMBTERM, ECHO and SGA are only agreed in raw
 * mode and the client edits cooked lines itself, as telnetd 2.0 does.
 * will[] tracks what we have announced, so acknowledgements are never
 * answered (RFC 854 loop rule). */
static void answer_option(struct Console *c, int cmd, int opt)
{
    BOOL want = (opt == OPT_ECHO || opt == OPT_SGA) && (c->edit || c->raw);
    switch (cmd) {
    case TEL_DO:
        if (want) { if (!c->will[opt]) { c->will[opt] = TRUE; send_opt(c, TEL_WILL, opt); } }
        else      { c->will[opt] = FALSE; send_opt(c, TEL_WONT, opt); }
        break;
    case TEL_DONT:
        if (c->will[opt]) { c->will[opt] = FALSE; send_opt(c, TEL_WONT, opt); }
        break;
    case TEL_WILL:
        if (opt == OPT_NAWS && c->edit) {     /* usually the answer to our DO */
            if (!c->doNaws) { c->doNaws = TRUE; send_opt(c, TEL_DO, OPT_NAWS); }
        } else {
            send_opt(c, TEL_DONT, opt);       /* nothing else wanted from the client */
        }
        break;
    default:                                  /* WONT */
        if (opt == OPT_NAWS) c->doNaws = FALSE;
        break;
    }
}

/* What we ask for when a connection opens. Line editor: character mode
 * with server echo (it needs every key) and the window width. DUMBTERM:
 * nothing - the client stays in its own line mode until raw mode. */
static void con_negotiate(struct Console *c)
{
    if (!c->edit) return;
    c->will[OPT_ECHO] = c->will[OPT_SGA] = TRUE;
    c->doNaws = TRUE;
    send_opt(c, TEL_WILL, OPT_ECHO);
    send_opt(c, TEL_WILL, OPT_SGA);
    send_opt(c, TEL_DO, OPT_NAWS);
}

/* IAC SB <opt> ... IAC SE received. NAWS: width (16 bit), height. */
static void sb_done(struct Console *c)
{
    if (c->sbOpt == OPT_NAWS && c->sbLen >= 4) {
        LONG cols = ((LONG)c->sbBuf[0] << 8) | c->sbBuf[1];
        LONG rows = ((LONG)c->sbBuf[2] << 8) | c->sbBuf[3];
        if (cols >= 20 && cols <= 1000) c->cols = cols;
        if (rows >= 10 && rows <= 300)  c->rows = rows;
        if ((cols >= 20 && cols <= 1000) && (rows >= 10 && rows <= 300))
            c->nawsSeen = TRUE;
        logmsg("telnetd: window %ld cols\n", c->cols);
        logmsg("telnetd: window %ld rows\n", c->rows);
    }
}

/* Raw: bytes go to the program unedited and unechoed (it echoes itself).
 * Cooked: the line editor below (or, with DUMBTERM, the client's own line
 * mode, which needs ECHO and SGA switched back off). */
static void set_mode(struct Console *c, BOOL raw)
{
    static const int opts[2] = { OPT_ECHO, OPT_SGA };
    int i;
    c->raw = raw;
    if (c->edit) return;                      /* character mode either way */
    for (i = 0; i < 2; i++) {
        if (raw && !c->will[opts[i]])      { c->will[opts[i]] = TRUE;  send_opt(c, TEL_WILL, opts[i]); }
        else if (!raw && c->will[opts[i]]) { c->will[opts[i]] = FALSE; send_opt(c, TEL_WONT, opts[i]); }
    }
}


/* ---------------- socket input ----------------------------------------- */

/* One raw byte from the socket. 1 = byte, 0 = hangup, -1 = none right now. */
static LONG in_byte(struct Console *c, unsigned char *b)
{
    if (c->inHead == c->inTail) {
        LONG n = recv(c->sock, (APTR)c->in, sizeof c->in, 0);
        if (n == 0) { logmsg("telnetd: recv eof\n", 0); return 0; }
        if (n < 0) {
            LONG e = Errno();
            if (e == SOCK_EWOULDBLOCK || e == SOCK_ENXIO || e == SOCK_EINTR) return -1;
            logmsg("telnetd: recv err %ld\n", e);
            return 0;
        }
        c->inHead = 0;
        c->inTail = n;
    }
    *b = c->in[c->inHead++];
    return 1;
}

/* One data character with telnet commands removed and NVT line ends
 * folded (CR LF / CR NUL / bare CR -> '\n' cooked, CR raw).
 * 1 = char in *loc, 0 = hangup, -1 = none right now. */
static LONG next_char(struct Console *c, unsigned char *loc)
{
    unsigned char ch;
    LONG r;

    for (;;) {
        r = in_byte(c, &ch);
        if (r <= 0) return r;

        switch (c->telState) {
        case 0:
            if (ch == TEL_IAC) { c->telState = 1; continue; }
            if (c->lastCR) {
                c->lastCR = FALSE;
                if (ch == 10 || ch == 0) continue;
            }
            if (ch == 13) { c->lastCR = TRUE; *loc = c->raw ? 13 : 10; return 1; }
            if (ch == 3) {                     /* Ctrl-C: break the foreground */
                send_break(c);
                if (!c->raw) continue;
            }
            *loc = ch;
            return 1;
        case 1:                                /* IAC command byte */
            c->telState = 0;
            if (ch == TEL_IAC) { c->lastCR = FALSE; *loc = 255; return 1; }
            if (ch == TEL_SB) { c->telState = 3; continue; }
            if (ch >= TEL_WILL) { c->telCmd = ch; c->telState = 2; continue; }
            if (ch == TEL_IP || ch == TEL_BRK) send_break(c);
            continue;
        case 2:                                /* option byte */
            c->telState = 0;
            answer_option(c, c->telCmd, ch);
            continue;
        case 3:                                /* SB: option byte */
            c->sbOpt = ch;
            c->sbLen = 0;
            c->telState = 5;
            continue;
        case 5:                                /* SB: data */
            if (ch == TEL_IAC) c->telState = 4;
            else if (c->sbLen < (int)sizeof c->sbBuf) c->sbBuf[c->sbLen++] = ch;
            continue;
        default:                               /* SB IAC */
            if (ch == TEL_SE) { c->telState = 0; sb_done(c); continue; }
            if (ch == TEL_IAC && c->sbLen < (int)sizeof c->sbBuf) c->sbBuf[c->sbLen++] = ch;  /* escaped 255 */
            c->telState = 5;
            continue;
        }
    }
}


/* ---------------- ready queue ------------------------------------------ */

static LONG ready_room(struct Console *c)
{
    return READYSZ - c->rCount;
}

static void ready_put(struct Console *c, unsigned char b)
{
    if (c->rCount < READYSZ) {
        c->ready[(c->rHead + c->rCount) & (READYSZ - 1)] = b;
        c->rCount++;
    }
}

static unsigned char ready_get(struct Console *c)
{
    unsigned char b = c->ready[c->rHead];
    c->rHead = (c->rHead + 1) & (READYSZ - 1);
    c->rCount--;
    return b;
}


/* ---------------- client cursor tracking ------------------------------- */

static void ready_put_dec(struct Console *c, LONG n)
{
    char d[8];
    int i = 0;
    if (n <= 0) { ready_put(c, '0'); return; }
    while (n > 0 && i < (int)sizeof d) { d[i++] = (char)('0' + n % 10); n /= 10; }
    while (i > 0) ready_put(c, (unsigned char)d[--i]);
}

/* A program wrote "window status request" (CSI 0 SPACE q): answer in its
 * input with "window bounds report" CSI 1;1;<rows>;<cols> SPACE r, as
 * CON: does. CSI is the Amiga byte 0x9B. */
static void con_report_bounds(struct Console *c)
{
    ready_put(c, 0x9b);
    ready_put(c, '1'); ready_put(c, ';');
    ready_put(c, '1'); ready_put(c, ';');
    ready_put_dec(c, c->rows); ready_put(c, ';');
    ready_put_dec(c, c->cols);
    ready_put(c, ' '); ready_put(c, 'r');
}

/* The cursor row/column the client terminal is in, followed through every
 * byte we send (program output and the editor's own). termCol == cols means
 * "wrap pending": the last column was just written and the next printable
 * character lands at the start of the next row (VT100/xterm behaviour);
 * then the row moves down too, clamped at the bottom (the screen scrolls).
 * Cursor movement in program output is followed for the usual CSI
 * sequences; anything exotic can leave this off, which only affects the
 * editor's drawing and More's page counting. */
static void row_down(struct Console *c, LONG n)
{
    c->termRow += n;
    if (c->termRow > c->rows - 1) c->termRow = c->rows - 1;   /* the screen scrolls */
}

static void term_track(struct Console *c, unsigned char ch)
{
    LONG n;
    if (c->trkEsc == 1) {                     /* after ESC */
        c->trkEsc = (ch == '[') ? 2 : 0;
        c->trkN = 0;
        c->trkN1 = -1;
        c->trkSpace = FALSE;
        if (ch == 'c') { c->termCol = 0; c->termRow = 0; }   /* ESC c: full reset */
        return;
    }
    if (c->trkEsc == 2) {                     /* inside ESC [ */
        if (ch >= '0' && ch <= '9') { if (c->trkN < 10000) c->trkN = c->trkN * 10 + (ch - '0'); return; }
        if (ch == ';') { c->trkN1 = c->trkN; c->trkN = 0; return; }
        if (ch == ' ') { c->trkSpace = TRUE; return; }
        if (ch < 0x40) return;                /* other parameter/intermediate bytes */
        c->trkEsc = 0;
        if (ch == 'q' && c->trkSpace && c->trkN == 0 && c->trkN1 < 0) {
            con_report_bounds(c);             /* CSI 0 SPACE q / CSI SPACE q */
            return;
        }
        if (c->trkSpace) return;              /* other SPACE sequences: no movement */
        n = c->trkN ? c->trkN : 1;
        if (c->termCol >= c->cols) c->termCol = c->cols - 1;   /* movement ends a pending wrap */
        switch (ch) {
        case 'C': c->termCol += n; break;
        case 'D': c->termCol -= n; break;
        case 'G': c->termCol = n - 1; break;
        case 'A': c->termRow -= n; break;
        case 'B': c->termRow += n; break;
        case 'E': c->termRow += n; c->termCol = 0; break;
        case 'F': c->termRow -= n; c->termCol = 0; break;
        case 'd': c->termRow = n - 1; break;
        case 'H':
        case 'f':
            if (c->trkN1 >= 0) {              /* ESC [ row ; col H */
                c->termRow = c->trkN1 > 0 ? c->trkN1 - 1 : 0;
                c->termCol = c->trkN > 0 ? c->trkN - 1 : 0;
            } else {                          /* ESC [ row H, ESC [ H */
                c->termRow = c->trkN > 0 ? c->trkN - 1 : 0;
                c->termCol = 0;
            }
            break;
        default:
            return;
        }
        if (c->termCol < 0) c->termCol = 0;
        if (c->termCol > c->cols - 1) c->termCol = c->cols - 1;
        if (c->termRow < 0) c->termRow = 0;
        if (c->termRow > c->rows - 1) c->termRow = c->rows - 1;
        return;
    }
    if (ch == 27) { c->trkEsc = 1; return; }
    if (ch == 13) { c->termCol = 0; return; }
    if (ch == 10 || ch == 11 || ch == 12) {   /* LF, VT, FF: xterm moves down a line */
        if (c->termCol >= c->cols) c->termCol = c->cols - 1;
        row_down(c, 1);
        return;
    }
    if (ch == 8) {
        if (c->termCol >= c->cols) c->termCol = c->cols - 1;
        if (c->termCol > 0) c->termCol--;
        return;
    }
    if (ch == 9) {
        if (c->termCol >= c->cols) c->termCol = c->cols - 1;
        c->termCol = (c->termCol / 8 + 1) * 8;
        if (c->termCol > c->cols - 1) c->termCol = c->cols - 1;
        return;
    }
    if (ch < 32 || (ch >= 127 && ch < 160)) return;   /* other controls: no movement */
    if (c->termCol >= c->cols) {              /* wrap pending: this char starts the next row */
        c->termCol = 1;
        row_down(c, 1);
    } else {
        c->termCol++;
    }
}


/* ---------------- cooked-mode line editor ------------------------------ */

/* Editor output goes to the client only while it lets us echo. */
static void ed_out(struct Console *c, const unsigned char *p, LONG n)
{
    if (n > 0 && c->will[OPT_ECHO]) con_write(c, p, n);
}

static void ed_outs(struct Console *c, const char *s)
{
    ed_out(c, (const unsigned char *)s, (LONG)strlen(s));
}

/* ESC [ n <ch> */
static void ed_csi(struct Console *c, LONG n, unsigned char ch)
{
    unsigned char seq[8];
    int i = 0;
    seq[i++] = 27; seq[i++] = '[';
    if (n >= 1000) seq[i++] = (unsigned char)('0' + (n / 1000) % 10);
    if (n >= 100)  seq[i++] = (unsigned char)('0' + (n / 100) % 10);
    if (n >= 10)   seq[i++] = (unsigned char)('0' + (n / 10) % 10);
    seq[i++] = (unsigned char)('0' + n % 10);
    seq[i++] = ch;
    ed_out(c, seq, i);
}

/* The line is laid out on a grid c->cols wide, starting at column
 * c->startCol of its first row: line offset k sits at row
 * (startCol + k) / cols, column (startCol + k) % cols. c->pos is the
 * offset the client's cursor is at. Invariant between keys: pos == cur,
 * and the row holding offset len exists on screen. */

/* Move the client's cursor from offset c->pos to offset `to`. */
static void ed_goto(struct Console *c, LONG to)
{
    LONG a = c->startCol + c->pos, b = c->startCol + to;
    LONG ra = a / c->cols, ca = a % c->cols, rb = b / c->cols, cb = b % c->cols;
    if (rb != ra) {
        ed_csi(c, ra > rb ? ra - rb : rb - ra, (unsigned char)(ra > rb ? 'A' : 'B'));
        ed_outs(c, "\r");
        if (cb > 0) ed_csi(c, cb, 'C');
    } else if (cb > ca) {
        ed_csi(c, cb - ca, 'C');
    } else if (cb < ca) {
        ed_csi(c, ca - cb, 'D');
    }
    c->pos = to;
}

/* Write line[from..len) with the cursor at `from`. Text that ends exactly
 * at the right margin leaves the client in "wrap pending"; CR LF settles
 * the cursor on the next row, where the offset arithmetic expects it. */
static void ed_write_tail(struct Console *c, LONG from)
{
    ed_out(c, c->line + from, c->len - from);
    c->pos = c->len;
    if (c->len > from && (c->startCol + c->len) % c->cols == 0) ed_outs(c, "\r\n");
}

/* Redraw from offset `from` to the end, clear whatever the old line left
 * behind (possibly on rows below), and put the cursor back at cur. */
static void ed_redraw(struct Console *c, LONG from)
{
    ed_goto(c, from);
    ed_write_tail(c, from);
    ed_outs(c, "\033[J");
    ed_goto(c, c->cur);
}

/* A new line starts where the cursor is now: right after the prompt. */
static void ed_anchor(struct Console *c)
{
    if (c->termCol >= c->cols) ed_outs(c, "\r\n");   /* the prompt filled its row exactly */
    c->startCol = (c->termCol >= c->cols) ? 0 : c->termCol;
    c->pos = 0;
}

/* Program output while a line is being edited (typeahead during a
 * command): take the line off the screen, let the output through, then
 * draw the line again after it, so the two never mix. While a WRITE is
 * in progress (c->writing) no input is pumped, so no key is echoed into
 * the middle of the output. */
static void ed_output_begin(struct Console *c)
{
    c->writing = TRUE;
    if (c->edit && !c->raw && c->len > 0) {
        ed_goto(c, 0);
        ed_outs(c, "\033[J");
    }
}

static void ed_output_end(struct Console *c)
{
    c->writing = FALSE;
    if (c->edit && !c->raw && c->len > 0) {
        ed_anchor(c);
        ed_write_tail(c, 0);
        ed_goto(c, c->cur);
    }
}

/* Replace the whole line (history recall). */
static void ed_set(struct Console *c, const unsigned char *src, LONG n)
{
    memcpy(c->line, src, n);
    c->len = c->cur = n;
    ed_redraw(c, 0);
}

static const unsigned char *hist_entry(struct Console *c, LONG k, LONG *len)   /* k = 1: newest */
{
    LONG slot = (c->histNext - k) & (HISTN - 1);
    *len = c->histLen[slot];
    return c->hist[slot];
}

static void hist_add(struct Console *c)
{
    LONG plen;
    const unsigned char *prev;
    if (c->len == 0) return;
    if (c->histCount > 0) {                 /* no consecutive duplicates */
        prev = hist_entry(c, 1, &plen);
        if (plen == c->len && memcmp(prev, c->line, c->len) == 0) return;
    }
    memcpy(c->hist[c->histNext], c->line, c->len);
    c->histLen[c->histNext] = c->len;
    c->histNext = (c->histNext + 1) & (HISTN - 1);
    if (c->histCount < HISTN) c->histCount++;
}

static void hist_move(struct Console *c, int dir)   /* +1 = older (Up), -1 = newer (Down) */
{
    const unsigned char *h;
    LONG len;
    if (dir > 0) {
        if (c->histPos >= c->histCount) return;
        if (c->histPos == 0) { memcpy(c->saved, c->line, c->len); c->savedLen = c->len; }
        c->histPos++;
    } else {
        if (c->histPos == 0) return;
        c->histPos--;
    }
    if (c->histPos == 0) ed_set(c, c->saved, c->savedLen);
    else { h = hist_entry(c, c->histPos, &len); ed_set(c, h, len); }
}

static void ed_insert(struct Console *c, unsigned char ch)
{
    if (c->len >= LINEMAX) { ed_outs(c, "\007"); return; }
    memmove(c->line + c->cur + 1, c->line + c->cur, c->len - c->cur);
    c->line[c->cur] = ch;
    c->len++;
    c->cur++;
    if (c->cur == c->len) ed_write_tail(c, c->cur - 1);   /* typing at the end: just echo */
    else ed_redraw(c, c->cur - 1);
}

static void ed_delete(struct Console *c, LONG at)   /* remove line[at], cursor to `at` */
{
    memmove(c->line + at, c->line + at + 1, c->len - at - 1);
    c->len--;
    c->cur = at;
    ed_redraw(c, at);
}

static void ed_enter(struct Console *c)
{
    LONG i;
    ed_goto(c, c->len);                      /* output continues below the whole line */
    if (c->len == 0 || (c->startCol + c->len) % c->cols != 0)
        ed_outs(c, "\r\n");                  /* else already at the start of a new row */
    for (i = 0; i < c->len; i++) ready_put(c, c->line[i]);
    ready_put(c, '\n');
    hist_add(c);
    c->len = c->cur = c->pos = 0;
    c->histPos = 0;
}

static void ed_escape(struct Console *c, unsigned char ch)   /* final byte of ESC [ ... / ESC O ... */
{
    switch (ch) {
    case 'A': hist_move(c, +1); break;
    case 'B': hist_move(c, -1); break;
    case 'C': if (c->cur < c->len) { c->cur++; ed_goto(c, c->cur); } break;
    case 'D': if (c->cur > 0) { c->cur--; ed_goto(c, c->cur); } break;
    case 'H': c->cur = 0; ed_goto(c, c->cur); break;
    case 'F': c->cur = c->len; ed_goto(c, c->cur); break;
    case '~':
        if (c->escParam == 1 || c->escParam == 7)      { c->cur = 0; ed_goto(c, c->cur); }
        else if (c->escParam == 4 || c->escParam == 8) { c->cur = c->len; ed_goto(c, c->cur); }
        else if (c->escParam == 3 && c->cur < c->len)  ed_delete(c, c->cur);
        break;
    default:                                  /* unknown sequence: ignore */
        break;
    }
}

/* One input character in cooked mode. Keys follow CON: where it has them
 * (Ctrl-X kills the line, Ctrl-\ is EOF, Ctrl-D/E/F send break signals)
 * plus the usual ANSI cursor, Home/End and Delete sequences. */
static void ed_key(struct Console *c, unsigned char ch)
{
    if (c->len == 0 && c->esc == 0) ed_anchor(c);

    if (c->esc == 1) {
        if (ch == '[' || ch == 'O') { c->esc = 2; c->escParam = 0; return; }
        c->esc = 0;                           /* lone ESC: dropped */
    } else if (c->esc == 2) {
        if (ch >= '0' && ch <= '9') {
            if (c->escParam < 1000) c->escParam = c->escParam * 10 + (ch - '0');
            return;
        }
        if (ch == ';') { c->escParam += 10000; return; }   /* modifiers: not handled */
        c->esc = 0;
        if (c->escParam < 10000) ed_escape(c, ch);
        return;
    }

    switch (ch) {
    case 27:   c->esc = 1; return;
    case 0x9b: c->esc = 2; c->escParam = 0; return;   /* 8-bit CSI */
    case '\n': ed_enter(c); return;
    case 8:
    case 127:
        if (c->cur > 0) ed_delete(c, c->cur - 1);
        return;
    case 0x18:                                /* Ctrl-X: kill line (CON:) */
    case 0x15:                                /* Ctrl-U */
        c->len = c->cur = 0;
        ed_redraw(c, 0);
        return;
    case 0x0b:                                /* Ctrl-K: kill to end of line */
        c->len = c->cur;
        ed_redraw(c, c->cur);
        return;
    case 0x1c:                                /* Ctrl-\: EOF on an empty line */
        if (c->len == 0) c->eof = TRUE;
        return;
    case 4: send_signal(c, SIGBREAKF_CTRL_D); return;
    case 5: send_signal(c, SIGBREAKF_CTRL_E); return;
    case 6: send_signal(c, SIGBREAKF_CTRL_F); return;
    default:
        if (ch >= 32 && !(ch >= 127 && ch < 160)) ed_insert(c, ch);
        return;                               /* other controls: ignored */
    }
}

/* DUMBTERM: the client edits the line itself (line mode, local echo) and
 * sends it whole; collect it up to the line end, without echo. BS/DEL
 * and Ctrl-X are honoured for clients that send keys one by one anyway. */
static void plain_key(struct Console *c, unsigned char ch)
{
    LONG i;
    switch (ch) {
    case '\n':
        for (i = 0; i < c->len; i++) ready_put(c, c->line[i]);
        ready_put(c, '\n');
        c->len = 0;
        return;
    case 8:
    case 127:
        if (c->len > 0) c->len--;
        return;
    case 0x18:
    case 0x15:
        c->len = 0;
        return;
    case 0x1c:
        if (c->len == 0) c->eof = TRUE;
        return;
    default:
        if (c->len < LINEMAX) c->line[c->len++] = ch;
        return;
    }
}

/* Whether socket input can be taken now: room for a whole line in the
 * ready queue, room for the editor's output, and no program output half
 * written. Otherwise the socket is not selected (readable data nobody
 * consumes would make WaitSelect return at once, forever). */
/* Whether the editor (or raw/DUMBTERM input) can take keys now: room for
 * a whole line in the ready queue, room for the editor's output, and no
 * program output half written (no echo into the middle of output). */
static BOOL con_can_edit(struct Console *c)
{
    return !c->writing && ready_room(c) > LINEMAX + 1 && out_room(c) >= ED_RESERVE;
}

/* Whether the socket should be read now. Even when the editor cannot take
 * keys, the socket is read into the held buffer, so Ctrl-C, telnet
 * "interrupt process" and a disconnect are seen at once - during long
 * output, or while a command runs without reading. Only when that buffer
 * is full is the socket left alone (readable data nobody consumes would
 * make WaitSelect return at once, forever). */
static BOOL con_wants_input(struct Console *c)
{
    if (c->hangup) return FALSE;
    if (c->hCount == 0 && con_can_edit(c)) return TRUE;
    return c->hCount < HELDSZ && out_room(c) >= HELD_ROOM;
}

static void con_key(struct Console *c, unsigned char ch)
{
    if (c->raw)       ready_put(c, ch);
    else if (c->edit) ed_key(c, ch);
    else              plain_key(c, ch);
}

/* Move input on: held keys first, in order, then the socket - straight to
 * the editor when it can take keys, into the held buffer otherwise. Break
 * keys act inside next_char(), whichever way the key then goes. */
static void pump_input(struct Console *c)
{
    unsigned char ch;
    LONG r;
    while (c->hCount > 0 && con_can_edit(c)) {
        ch = c->held[c->hHead];
        c->hHead = (c->hHead + 1) & (HELDSZ - 1);
        c->hCount--;
        con_key(c, ch);
    }
    while (con_wants_input(c)) {
        r = next_char(c, &ch);
        if (r == 0) { do_hangup(c); return; }
        if (r < 0) return;
        if (c->hCount == 0 && con_can_edit(c)) {
            con_key(c, ch);
        } else {
            c->held[(c->hHead + c->hCount) & (HELDSZ - 1)] = ch;
            c->hCount++;
        }
    }
}


/* === END portable input/editor section ================================= */


/* ---------------- sessions ---------------------------------------------- */

struct Session {
    struct Console  con;              /* first: the portable code's hooks get &s->con */
    LONG            id;               /* session number, for the log */
    struct MsgPort *port;             /* handler port: fh_Type of this session's handles */
    struct List     readWait;         /* queued READ messages */
    struct List     writeWait;        /* WRITE messages not yet fully queued */
    struct Task    *breakTask;        /* receives Ctrl-C from the client */
    LONG            opens;            /* handles on port not yet ENDed */
    BOOL            sawOpen;          /* NewShell has opened "*" */
    LONG            started, closingAt;
    LONG            stalledAt;        /* output waiting, socket full since (0: not) */
};

static struct Session  *gSess[MAXSLOTS];
static BOOL             gConWinWanted = TRUE;   /* console window is the default since 0.5.1 */
static struct Window   *gConWin = NULL;         /* shared 1x1 window for More & co. */
static BOOL             gConWinUsed = FALSE;    /* handed out at least once */
static struct List      gRetired;         /* ports of ended sessions (mp_Node) */
static LONG             gSessions = 0;    /* sessions started so far */
static LONG             gMaxSessions = DEFSESSIONS;
static BOOL             gEditDefault = TRUE;
static BOOL             gBreak    = FALSE;
static int              gListen   = -1;

/* handler ports share one signal bit */
static BYTE             gPortSig  = -1;

/* one timer for every WaitForChar */
static struct MsgPort  *gTimePort = NULL;
static struct timerequest *gTimer = NULL;
static BOOL             gTimerBusy = FALSE;
struct Device          *TimerBase = NULL;   /* for GetSysTime() */

#define MAXCW        16
static struct {
    struct Session   *s;
    struct DosPacket *pkt;
    struct timeval    due;
}                       gCW[MAXCW];
static int              gNCW = 0;

static LONG now_secs(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return ds.ds_Days * 86400 + ds.ds_Minute * 60 + ds.ds_Tick / TICKS_PER_SECOND;
}

/* logmsg with the session number in front */
static void slog(struct Session *s, const char *fmt, LONG arg)
{
    BPTR f;
    LONG id = s->id;
    if (!gLogName) return;
    f = Open(gLogName, MODE_READWRITE);
    if (!f) return;
    Seek(f, 0, OFFSET_END);
    VFPrintf(f, (STRPTR)"[%ld] ", (APTR)&id);
    VFPrintf(f, (STRPTR)fmt, (APTR)&arg);
    Close(f);
}


/* ---------------- handler ports ---------------------------------------- */

/* A handler port plus the handles opened on it while it was retired
 * (FIND with no session): those are never counted by a session, so the
 * port is not reused until they are closed again - otherwise their END
 * would be taken off the next session's count. */
struct HPort {
    struct MsgPort  mp;               /* first: used as a plain MsgPort */
    LONG            deadOpens;
    struct IOStdReq io;               /* CONWINDOW: id_InUse; only io_Unit is read */
    struct ConUnit  cu;               /* CONWINDOW: io.io_Unit points here */
};

/* A port on the shared signal: AllocSignal for every session would run
 * out of the 16 user signals. */
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

/* The longest-retired port with no dead handles, or a new one. */
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

/* Exit with processes possibly still holding a port as console task:
 * leave it allocated but inert - PA_IGNORE queues without signalling, so a
 * late packet waits forever instead of landing in freed memory. */
static LONG dead_opens(void)
{
    struct Node *n;
    LONG sum = 0;
    for (n = gRetired.lh_Head; n->ln_Succ; n = n->ln_Succ)
        sum += ((struct HPort *)n)->deadOpens;
    return sum;
}

/* CONWINDOW: the real window behind id_VolumeNode. More (v3.27) only calls
 * SetWindowTitles() on it; with no title bar nothing is drawn. A fake
 * struct Window must never be used - Intuition would work on garbage. */
static struct Window *conwin_get(void)
{
    struct NewWindow nw;
    if (gConWin || !IntuitionBase) return gConWin;
    memset(&nw, 0, sizeof nw);
    nw.Width = 1;
    nw.Height = 1;
    nw.DetailPen = (UBYTE)-1;
    nw.BlockPen = (UBYTE)-1;
    nw.Flags = WFLG_BORDERLESS | WFLG_BACKDROP | WFLG_SIMPLE_REFRESH |
               WFLG_NOCAREREFRESH | WFLG_RMBTRAP;
    nw.Type = WBENCHSCREEN;
    gConWin = OpenWindow(&nw);
    logmsg(gConWin ? "telnetd: console window opened\n"
                   : "telnetd: could not open the console window\n", 0);
    return gConWin;
}

/* Keep the session's ConUnit in step with the client screen. More reads
 * cu_YCP right after each Write() returns, so this must run before a WRITE
 * is answered. */
static void conunit_sync(struct Session *s)
{
    struct HPort *hp = (struct HPort *)s->port;
    struct Console *c = &s->con;
    hp->cu.cu_XMax = (WORD)(c->cols - 1);
    hp->cu.cu_YMax = (WORD)(c->rows - 1);
    hp->cu.cu_XCP  = (WORD)(c->termCol >= c->cols ? c->cols - 1 : c->termCol);
    hp->cu.cu_YCP  = (WORD)c->termRow;
}

static void port_abandon(struct MsgPort *p)
{
    Forbid();
    p->mp_Flags = PA_IGNORE;
    p->mp_SigTask = NULL;
    Permit();
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

static void send_signal(struct Console *c, ULONG sig)
{
    struct Session *s = (struct Session *)c;
    if (task_alive(s->breakTask)) Signal(s->breakTask, sig);
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
static void note_reader(struct Session *s, struct DosPacket *pkt)
{
    struct MsgPort *p = pkt->dp_Port;
    if (p && (p->mp_Flags & PF_ACTION) == PA_SIGNAL && p->mp_SigTask)
        s->breakTask = (struct Task *)p->mp_SigTask;
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

static BOOL has_input(struct Session *s)
{
    return s->con.rCount > 0 || s->con.eof;
}

/* Answer what can be answered (input ready: TRUE, deadline passed:
 * FALSE), then re-arm the timer for the earliest deadline left. Called
 * when a waiter is added, when the timer fires and when input arrives. */
static void charwait_update(void)
{
    struct timeval now, d;
    int i, e;

    timer_stop();
    if (gNCW == 0) return;
    GetSysTime(&now);
    for (i = gNCW - 1; i >= 0; i--) {          /* descending: the swapped-in */
        if (has_input(gCW[i].s))                /* entry was already checked  */
            charwait_reply(i, DOSTRUE);
        else if (!tv_before(&now, &gCW[i].due))
            charwait_reply(i, DOSFALSE);
    }
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

static BOOL charwait_pending(struct Session *s)
{
    int i;
    for (i = 0; i < gNCW; i++) if (gCW[i].s == s) return TRUE;
    return FALSE;
}

/* Answer every queued READ, WRITE and WAIT_CHAR of a session: READ gets
 * break (a plain 0-byte EOF makes the shell re-read forever), WRITE is
 * discarded, WAIT_CHAR gets "no character". */
static void flush_waiters(struct Session *s)
{
    struct Message *msg;
    int i;
    BOOL any = FALSE;
    for (i = gNCW - 1; i >= 0; i--)
        if (gCW[i].s == s) { charwait_reply(i, DOSFALSE); any = TRUE; }
    if (any) charwait_update();
    while ((msg = (struct Message *)RemHead(&s->readWait)) != NULL)
        reply(pkt_of(msg), DOSFALSE, ERROR_BREAK);
    while ((msg = (struct Message *)RemHead(&s->writeWait)) != NULL)
        reply(pkt_of(msg), pkt_of(msg)->dp_Arg3, 0);
    s->con.writing = FALSE;
}

/* Client gone (or daemon stopping): the foreground command gets Ctrl-C,
 * every pending and future READ gets break, so the shell ends by itself
 * and closes its handles; the socket is closed at once. */
static void do_hangup(struct Console *c)
{
    struct Session *s = (struct Session *)c;
    if (c->hangup) return;
    slog(s, "hangup, %ld handles open\n", s->opens);
    send_break(c);
    c->hangup = TRUE;
    flush_waiters(s);
    c->oCount = 0;
    if (c->sock >= 0) { CloseSocket(c->sock); c->sock = -1; }
}

/* Send what the output buffer holds, as far as the socket takes it. */
static void flush_out(struct Session *s)
{
    struct Console *c = &s->con;
    while (c->oCount > 0 && c->sock >= 0) {
        LONG n = OUTSZ - c->oHead;
        LONG k;
        if (n > c->oCount) n = c->oCount;
        k = send(c->sock, (APTR)(c->out + c->oHead), (int)n, 0);
        if (k > 0) {
            c->oHead = (c->oHead + k) & (OUTSZ - 1);
            c->oCount -= k;
            s->stalledAt = 0;
            continue;
        }
        if (k < 0) {
            LONG e = Errno();
            if (e != SOCK_EWOULDBLOCK && e != SOCK_ENXIO && e != SOCK_EINTR) {
                slog(s, "send error %ld\n", e);
                do_hangup(c);
                return;
            }
        }
        if (!s->stalledAt) s->stalledAt = now_secs();
        return;                                /* full: WaitSelect says when */
    }
    s->stalledAt = 0;
}

/* Feed queued WRITEs into the output buffer, keeping ED_RESERVE free for
 * the editor. A WRITE's progress lives in dp_Res1 while it is queued; it
 * is answered when all of it is in the buffer. */
static void service_writes(struct Session *s)
{
    struct Console *c = &s->con;
    struct Message *msg;
    while ((msg = (struct Message *)s->writeWait.lh_Head)->mn_Node.ln_Succ) {
        struct DosPacket *pkt = pkt_of(msg);
        const unsigned char *src = (const unsigned char *)pkt->dp_Arg2 + pkt->dp_Res1;
        LONG left;
        if (!c->writing) ed_output_begin(c);
        left = con_fit(c, src, pkt->dp_Arg3 - pkt->dp_Res1, out_room(c) - ED_RESERVE);
        if (left > 0) {
            con_write(c, src, left);
            pkt->dp_Res1 += left;
        }
        if (pkt->dp_Res1 < pkt->dp_Arg3) return;      /* wait for the socket */
        ed_output_end(c);
        Remove(&msg->mn_Node);
        conunit_sync(s);
        reply(pkt, pkt->dp_Arg3, 0);
    }
}

/* Answer WAIT_CHAR waiters once input is ready, then complete queued
 * READs from the ready queue. A cooked READ gets at most one line (the
 * queue only ever holds whole lines in cooked mode); a raw READ gets
 * whatever is there. */
static void service_reads(struct Session *s)
{
    struct Console *c = &s->con;
    struct Message *msg;
    if (c->hangup) return;
    if (has_input(s) && charwait_pending(s)) charwait_update();
    while ((msg = (struct Message *)s->readWait.lh_Head)->mn_Node.ln_Succ) {
        struct DosPacket *pkt = pkt_of(msg);

        /* ACTION_READ: dp_Arg2 = buffer, dp_Arg3 = length */
        if (c->rCount == 0) {
            if (!c->eof) return;               /* wait for input */
            c->eof = FALSE;
            Remove(&msg->mn_Node);
            reply(pkt, 0, 0);                  /* Ctrl-\: EOF, as on CON: */
            continue;
        }
        pkt->dp_Res1 = 0;
        while (pkt->dp_Res1 < pkt->dp_Arg3 && c->rCount > 0) {
            unsigned char b = ready_get(c);
            ((unsigned char *)pkt->dp_Arg2)[pkt->dp_Res1++] = b;
            if (!c->raw && b == '\n') break;
        }
        Remove(&msg->mn_Node);
        reply(pkt, pkt->dp_Res1, 0);
    }
}

/* Serves one packet that arrived on `port`, which belongs to session s,
 * or to no session (s == NULL: a retired port, still used by a process
 * that outlived its session). A port's packets are always its session's,
 * so a "run" job left over from an earlier session can only meet a new
 * session after the port has been retired and reused. */
static void handle_packet(struct Session *s, struct MsgPort *port, struct Message *msg)
{
    struct DosPacket *pkt = pkt_of(msg);
    BOOL live = (s != NULL && !s->con.hangup);

    switch (pkt->dp_Type) {

    case ACTION_FINDINPUT:
    case ACTION_FINDOUTPUT:
    case ACTION_FINDUPDATE: {
        /* Open("*") by a process whose console task is this port. */
        struct FileHandle *fh = (struct FileHandle *)BADDR((BPTR)pkt->dp_Arg1);
        fh->fh_Type = port;
        fh->fh_Port = port;                    /* non-zero = interactive */
        fh->fh_Arg1 = s ? s->id : 0;           /* informational */
        if (s) {
            s->opens++;
            s->sawOpen = TRUE;
            s->closingAt = 0;                  /* the session is in use again */
            slog(s, "find, opens %ld\n", s->opens);
        } else {
            ((struct HPort *)port)->deadOpens++;
        }
        reply(pkt, DOSTRUE, 0);
        break;
    }

    case ACTION_READ:
        if (!live) { reply(pkt, DOSFALSE, ERROR_BREAK); break; }   /* break, not bare EOF: the shell exits on it */
        note_reader(s, pkt);
        pkt->dp_Res1 = 0;
        AddTail(&s->readWait, &msg->mn_Node);
        break;

    case ACTION_WRITE:
        if (!live) { reply(pkt, pkt->dp_Arg3, 0); break; }        /* nobody to see it */
        pkt->dp_Res1 = 0;                      /* progress while queued */
        AddTail(&s->writeWait, &msg->mn_Node);
        break;

    case ACTION_WAIT_CHAR: {                   /* dp_Arg1 = timeout in us */
        ULONG us = (ULONG)pkt->dp_Arg1;
        struct timeval due;
        if (!live) { reply(pkt, DOSFALSE, 0); break; }
        note_reader(s, pkt);
        if (has_input(s)) { reply(pkt, DOSTRUE, 0); break; }
        if (us == 0 || gNCW >= MAXCW) { reply(pkt, DOSFALSE, 0); break; }  /* poll, or table full */
        GetSysTime(&due);
        due.tv_secs  += us / 1000000;
        due.tv_micro += us % 1000000;
        if (due.tv_micro >= 1000000) { due.tv_micro -= 1000000; due.tv_secs++; }
        gCW[gNCW].s = s;
        gCW[gNCW].pkt = pkt;
        gCW[gNCW].due = due;
        gNCW++;
        charwait_update();
        break;
    }

    case ACTION_SCREEN_MODE:                   /* dp_Arg1 = mode */
        if (live) set_mode(&s->con, pkt->dp_Arg1 != 0);
        reply(pkt, DOSTRUE, 0);
        break;

    case ACTION_CHANGE_SIGNAL: {
        struct MsgPort *np = (struct MsgPort *)pkt->dp_Arg2;
        if (live && np && np->mp_SigTask) s->breakTask = (struct Task *)np->mp_SigTask;
        reply(pkt, DOSTRUE, 0);
        break;
    }

    case ACTION_END:
        reply(pkt, DOSTRUE, 0);
        if (s) {
            s->opens--;
            slog(s, "END, %ld handles left\n", s->opens);
        } else if (((struct HPort *)port)->deadOpens > 0) {
            if (--((struct HPort *)port)->deadOpens == 0)
                logmsg("telnetd: a hung-up shell has ended\n", 0);
        }
        break;

    case ACTION_SEEK:
        reply(pkt, -1, ERROR_OBJECT_WRONG_TYPE);
        break;

    case ACTION_DISK_INFO: {                /* size-aware tools such as More probe the console */
        /* dp_Arg1 is a BPTR (dos Packets doc: "ARG1: BPTR to InfoData"),
         * like every DOS struct argument: used as a C pointer it would
         * write 36 bytes at a quarter of the real address. */
        struct InfoData *id = (struct InfoData *)BADDR((BPTR)pkt->dp_Arg1);
        if (id) {
            /* Default: all NULL, the documented answer for a console
             * without a window (AUX:); programs then use defaults (More:
             * 80x24). With CONWINDOW a live session answers like CON::
             * id_VolumeNode = a real window, id_InUse = an IOStdReq whose
             * io_Unit is a ConUnit kept in step with the client screen
             * (cu_XMax/cu_YMax from NAWS, cu_XCP/cu_YCP from the tracked
             * cursor). More v3.27 reads only those four ConUnit fields and
             * calls SetWindowTitles() on the window - nothing else. Both
             * are plain C pointers, as CON: stores them. */
            memset(id, 0, sizeof *id);
            if (s && !s->con.hangup && gConWinWanted && conwin_get()) {
                struct HPort *hp = (struct HPort *)port;
                SetWindowTitles(gConWin, NULL, (UBYTE *)~0);   /* drop stale titles */
                conunit_sync(s);
                hp->cu.cu_Window = gConWin;
                hp->io.io_Unit = (struct Unit *)&hp->cu;
                id->id_VolumeNode = (BPTR)(ULONG)gConWin;
                id->id_InUse = (LONG)&hp->io;
                gConWinUsed = TRUE;
                slog(s, "disk_info: console window, %ld rows\n", s->con.rows);
            }
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
    int i;
    for (i = 0; i < MAXSLOTS; i++) {
        struct Session *s = gSess[i];
        if (!s) continue;
        while ((msg = GetMsg(s->port)) != NULL) handle_packet(s, s->port, msg);
    }
    for (n = gRetired.lh_Head; n->ln_Succ; n = n->ln_Succ) {
        struct MsgPort *p = (struct MsgPort *)n;
        while ((msg = GetMsg(p)) != NULL) handle_packet(NULL, p, msg);
    }
}


/* ---------------- shell spawning --------------------------------------- */

/* The spawn runs in this short-lived helper, never in the daemon: the
 * daemon is the handler for the handles System() is given, and a direct
 * SystemTags(SYS_Asynch) from it hangs inside the call on real hardware
 * (most likely System() waiting on a packet to those handles that only
 * the blocked daemon could answer). The helper copies its arguments and
 * signals gSpawnMask; only then may the daemon start the next one. */
static struct {
    BPTR            in, out;
    struct MsgPort *port;
}                       gSpawn;
static struct Task     *gDaemonTask = NULL;
static ULONG            gSpawnMask = 0;
static volatile LONG    gHelpers = 0;      /* helpers that may still run our code */

static LONG             gShellStack = 20000;   /* SHELLSTACK */

/* Uses the daemon's DOSBase, which stays open until the daemon has seen
 * every helper finish (gHelpers), so there is no failure path that could
 * skip closing the handles or the gHelpers count. */
static int spawner_entry(void)
{
    BPTR in = gSpawn.in, out = gSpawn.out;
    struct MsgPort *port = gSpawn.port;
    LONG rc;

    Signal(gDaemonTask, gSpawnMask);           /* gSpawn may be reused now */

    /* NP_StackSize: a CLI made here would otherwise get the DOS default
     * (about 4 KB) as the stack for every command typed in the session -
     * this helper has no CLI to inherit `stack` from. */
    rc = SystemTags((STRPTR)"NewShell *",
                    SYS_Input,      in,
                    SYS_Output,     out,
                    NP_ConsoleTask, (LONG)port,
                    NP_Cli,         TRUE,
                    NP_StackSize,   gShellStack,
                    TAG_DONE);
    /* NewShell starts the interactive shell as a NEW process and returns
     * at once, so this returns long before the session ends. A synchronous
     * System() does not close SYS_Input/SYS_Output (only SYS_Asynch does);
     * the V36 SystemTagList autodoc says the caller must close them after
     * System returns, and AROS's systemtaglist.c only ever closes handles
     * it opened itself. Without this the two handles stay counted in
     * opens forever and "endcli" never ends the session. telnetd 2.0's
     * SubSubProc does exactly this, Forbid() first: this code lives in the
     * daemon's seglist, and once the last END is answered the daemon may
     * exit and unload it. Close() waits for our reply (Wait breaks the
     * Forbid); after it returns we are Forbid()den again until the process
     * is gone. */
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
static BPTR make_handle(struct Session *s)
{
    struct FileHandle *fh = (struct FileHandle *)AllocDosObject(DOS_FILEHANDLE, NULL);
    if (!fh) return 0;
    fh->fh_Type = s->port;
    fh->fh_Port = s->port;                     /* non-zero = interactive */
    fh->fh_Arg1 = s->id;
    fh->fh_Pos  = -1;                          /* unbuffered */
    fh->fh_End  = -1;
    return MKBADDR(fh);
}

/* Two distinct handles, closed by the helper once NewShell has returned
 * (one ACTION_END each). NewShell opens "*" on NP_ConsoleTask = the
 * session's port for the interactive shell it starts. */
static BOOL spawn_shell(struct Session *s)
{
    struct TagItem ptags[4];
    BPTR in = make_handle(s), out = make_handle(s);

    if (!in || !out) {
        if (in)  FreeDosObject(DOS_FILEHANDLE, BADDR(in));
        if (out) FreeDosObject(DOS_FILEHANDLE, BADDR(out));
        return FALSE;
    }
    gSpawn.in = in;
    gSpawn.out = out;
    gSpawn.port = s->port;
    ptags[0].ti_Tag  = NP_Entry;
    ptags[0].ti_Data = (LONG)spawner_entry;
    ptags[1].ti_Tag  = NP_StackSize;
    ptags[1].ti_Data = 20000;
    ptags[2].ti_Tag  = NP_Name;
    ptags[2].ti_Data = (LONG)"telnetd shell";
    ptags[3].ti_Tag  = TAG_END;
    ptags[3].ti_Data = 0;
    SetSignal(0, gSpawnMask);
    if (CreateNewProc(ptags) == NULL) {
        FreeDosObject(DOS_FILEHANDLE, BADDR(in));
        FreeDosObject(DOS_FILEHANDLE, BADDR(out));
        return FALSE;
    }
    Wait(gSpawnMask);                          /* the helper has its copy */
    gHelpers++;
    s->opens = 2;
    return TRUE;
}


/* ---------------- session lifecycle ------------------------------------ */

static int live_sessions(void)
{
    int i, n = 0;
    for (i = 0; i < MAXSLOTS; i++)
        if (gSess[i] && !gSess[i]->con.hangup && !gSess[i]->closingAt) n++;
    return n;
}

static int free_slot(void)
{
    int i;
    for (i = 0; i < MAXSLOTS; i++) if (!gSess[i]) return i;
    return -1;
}

/* End a session: its port is retired with the handles still open on it
 * (a hung-up shell that has not ended yet). A retired port keeps
 * answering - READ break/EOF, WRITE discarded, FIND/END counted on the
 * port - and is reused only once no handle is left on it, so such a shell
 * costs one port, no session slot, and delays nobody. */
static void session_free(int i)
{
    struct Session *s = gSess[i];
    struct Message *msg;
    flush_waiters(s);
    if (s->con.sock >= 0) CloseSocket(s->con.sock);
    if (s->opens > 0) {
        slog(s, "hung up, shell still has %ld handles\n", s->opens);
        ((struct HPort *)s->port)->deadOpens = s->opens;
    }
    while ((msg = GetMsg(s->port)) != NULL) handle_packet(NULL, s->port, msg);
    AddTail(&gRetired, &s->port->mp_Node);    /* never freed while we run */
    slog(s, "session closed\n", 0);
    FreeVec(s);
    gSess[i] = NULL;
}

static void session_start(int sock)
{
    struct Session *s;
    int slot = free_slot();
    ULONG one = 1;

    if (slot < 0 || live_sessions() >= gMaxSessions) {
        static const char busy[] = "telnetd: too many sessions - try again later\r\n";
        send(sock, (APTR)busy, sizeof busy - 1, 0);
        CloseSocket(sock);
        logmsg("telnetd: refused, too many sessions\n", 0);
        return;
    }
    s = (struct Session *)AllocVec(sizeof(struct Session), MEMF_ANY | MEMF_CLEAR);
    if (s) s->port = port_get();
    if (!s || !s->port) {
        if (s) FreeVec(s);
        CloseSocket(sock);
        PutStr((STRPTR)"telnetd: out of memory for a session\n");
        return;
    }
    con_init(&s->con, sock, gEditDefault);
    s->id = ++gSessions;
    NewList(&s->readWait);
    NewList(&s->writeWait);
    s->started = now_secs();
    gSess[slot] = s;

    IoctlSocket(sock, FIONBIO, (APTR)&one);
    con_negotiate(&s->con);
    slog(s, "spawning shell\n", 0);
    if (!spawn_shell(s)) {
        static const char nosh[] = "telnetd: could not start a shell\n";
        slog(s, "could not start the shell\n", 0);
        PutStr((STRPTR)nosh);
        con_write(&s->con, (const unsigned char *)nosh, sizeof nosh - 1);
        s->sawOpen = TRUE;                     /* nothing to wait for */
        s->closingAt = now_secs();
    }
    flush_out(s);
}

/* One pass over a session after WaitSelect: output, input, reads, and the
 * lifecycle checks; may free the session (gSess[i] becomes NULL). */
static void session_step(int i)
{
    struct Session *s = gSess[i];
    struct Console *c = &s->con;
    LONG now = now_secs();

    service_writes(s);
    flush_out(s);
    if (!s->closingAt) pump_input(c);
    conunit_sync(s);
    service_reads(s);
    flush_out(s);

    if (!c->hangup && s->stalledAt && now - s->stalledAt > STALL_SECS) {
        slog(s, "client stopped reading\n", 0);
        do_hangup(c);
    }
    /* Shell gone: every handle closed - but not before NewShell has
     * opened "*", or its late FINDINPUT would find no session. */
    if (!s->closingAt && s->opens <= 0 && (s->sawOpen || now - s->started > SPAWN_SECS))
        s->closingAt = now;
    if (s->closingAt && s->opens <= 0 &&
        (c->sock < 0 || c->oCount == 0 || now - s->closingAt > CLOSE_SECS)) {
        session_free(i);
        return;
    }
    /* Client gone: nothing left to wait for - the shell got Ctrl-C and
     * break/EOF, and if it has not ended yet its handles are carried by
     * the retired port. */
    if (c->hangup) session_free(i);
}


/* ------------------------------- main ---------------------------------- */

int main(int argc, char **argv)
{
    LONG args[7] = { 0, 0, 0, 0, 0, 0, 0 };
    struct RDArgs *rd;
    struct sockaddr_in sa;
    struct Process *self = (struct Process *)FindTask(NULL);
    ULONG port = 23;
    ULONG stack = (ULONG)self->pr_Task.tc_SPUpper - (ULONG)self->pr_Task.tc_SPLower;
    int one = 1, i;
    BYTE spawnSig = -1;
    LONG breakAt = 0;
    BOOL helperNote = FALSE;
    APTR oldwinptr = self->pr_WindowPtr;

    (void)argc; (void)argv;

    if (stack < MIN_STACK) {
        Printf((STRPTR)"telnetd: stack is %lu bytes, needs %ld - run 'stack 20000' first\n",
               stack, (LONG)MIN_STACK);
        return RETURN_FAIL;
    }

    rd = ReadArgs((STRPTR)"PORT/N,LOG/K,DUMBTERM/S,MAXSESSIONS/K/N,SHELLSTACK/K/N,CONWINDOW/S,NOCONWINDOW/S", args, NULL);
    if (rd == NULL) {
        PutStr((STRPTR)"usage: telnetd [PORT <n>] [LOG <file>] [DUMBTERM] [MAXSESSIONS <n>] [SHELLSTACK <bytes>] [NOCONWINDOW]\n");
        return RETURN_FAIL;
    }
    if (args[0]) port = *(ULONG *)args[0];
    gLogName = (STRPTR)args[1];
    gEditDefault = !args[2];
    if (args[3]) {
        gMaxSessions = *(LONG *)args[3];
        if (gMaxSessions < 1) gMaxSessions = 1;
        if (gMaxSessions > MAXSESSIONS_LIMIT) gMaxSessions = MAXSESSIONS_LIMIT;
    }
    if (args[4]) {
        gShellStack = *(LONG *)args[4];
        if (gShellStack < 4000) gShellStack = 4000;
    }
    /* args[5] = CONWINDOW: accepted as a no-op; the console window is
     * the default since 0.5.1 (it was the opt-in switch before). */
    if (args[6]) gConWinWanted = FALSE;       /* NOCONWINDOW */
    if (gConWinWanted) {
        IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 36);
        if (!IntuitionBase) {
            PutStr((STRPTR)"telnetd: no intuition.library - console window off\n");
            gConWinWanted = FALSE;
        }
    }
    if (gLogName) {                          /* fresh trace per run */
        BPTR f = Open(gLogName, MODE_NEWFILE);
        if (f) Close(f);
    }
    logmsg("telnetd: started, stack %ld\n", (LONG)stack);

    NewList(&gRetired);
    gDaemonTask = FindTask(NULL);
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
        if (spawnSig >= 0) FreeSignal(spawnSig);
        if (gPortSig >= 0) FreeSignal(gPortSig);
        FreeArgs(rd);
        return RETURN_FAIL;
    }
    gSpawnMask = 1UL << spawnSig;
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
    if (listen(gListen, 4) < 0) { PutStr((STRPTR)"telnetd: listen failed\n"); goto out; }
    IoctlSocket(gListen, FIONBIO, (APTR)&one);   /* accept() must never block the loop */

    Printf((STRPTR)"telnetd: listening on port %ld, up to %ld sessions - Ctrl-C stops\n",
           port, gMaxSessions);
    logmsg("telnetd: listening on port %ld\n", (LONG)port);

    for (;;) {
        fd_set rd, wr;
        struct timeval tv;
        ULONG mask = SIGBREAKF_CTRL_C | (1UL << gPortSig) | (1UL << gTimePort->mp_SigBit);
        int maxfd = -1;
        LONG selr;
        BOOL listening = !gBreak;
        BOOL any = FALSE;

        FD_ZERO(&rd);
        FD_ZERO(&wr);
        if (listening) { FD_SET(gListen, &rd); maxfd = gListen; }
        for (i = 0; i < MAXSLOTS; i++) {
            struct Session *s = gSess[i];
            if (!s) continue;
            any = TRUE;
            if (s->con.sock < 0) continue;
            if (!s->closingAt && con_wants_input(&s->con)) FD_SET(s->con.sock, &rd);
            if (s->con.oCount > 0) FD_SET(s->con.sock, &wr);
            if (s->con.sock > maxfd) maxfd = s->con.sock;
        }
        /* Stopping: wait (serving packets) up to EXIT_SECS for sessions
         * and hung-up shells to end - and, without any time limit, until no
         * spawn helper can still return into this program's code. */
        if (gBreak && gHelpers == 0 &&
            ((!any && dead_opens() == 0) || now_secs() - breakAt > EXIT_SECS)) break;
        if (gBreak && gHelpers > 0 && !helperNote && now_secs() - breakAt > EXIT_SECS) {
            PutStr((STRPTR)"telnetd: waiting for a shell to finish starting\n");
            helperNote = TRUE;
        }

        tv.tv_secs = 1;
        tv.tv_micro = 0;
        selr = WaitSelect(maxfd + 1, &rd, &wr, NULL, &tv, &mask);

        if ((mask & SIGBREAKF_CTRL_C) && !gBreak) {
            PutStr((STRPTR)"telnetd: break - ending all sessions\n");
            logmsg("telnetd: break\n", 0);
            gBreak = TRUE;
            breakAt = now_secs();
            for (i = 0; i < MAXSLOTS; i++)
                if (gSess[i]) do_hangup(&gSess[i]->con);
        }

        if (GetMsg(gTimePort) != NULL) {       /* a WAIT_CHAR deadline */
            gTimerBusy = FALSE;
            charwait_update();
        }

        drain_ports();
        for (i = 0; i < MAXSLOTS; i++)
            if (gSess[i]) session_step(i);

        /* The fd sets mean something only when sockets were reported. */
        if (listening && selr > 0 && FD_ISSET(gListen, &rd)) {
            struct sockaddr_in ca;
            socklen_t calen = sizeof ca;       /* real buffers, never NULL */
            int sock = accept(gListen, (struct sockaddr *)&ca, &calen);
            if (sock >= 0) {
                logmsg("telnetd: accepted from %lx\n", (LONG)ca.sin_addr.s_addr);
                PutStr((STRPTR)"telnetd: connection - starting shell\n");
                session_start(sock);
            } else {
                logmsg("telnetd: accept failed, errno %ld\n", Errno());
            }
        }
    }
    PutStr((STRPTR)"telnetd: stopped\n");

out:
    if (gListen >= 0) CloseSocket(gListen);

    /* Sessions still here hold handles of shells that did not exit: their
     * ports are left inert, like the retired ones. */
    drain_ports();
    for (i = 0; i < MAXSLOTS; i++) {
        struct Session *s = gSess[i];
        if (!s) continue;
        flush_waiters(s);
        if (s->con.sock >= 0) CloseSocket(s->con.sock);
        port_abandon(s->port);
        FreeVec(s);
        gSess[i] = NULL;
    }
    {
        /* The shared window may still be in use by a More whose shell
         * ignored the hangup: its handles are counted on the retired ports
         * (dead_opens). Close it only when nothing can still hold it;
         * otherwise leave it (and intuition.library) open - More calls
         * SetWindowTitles() on it when it exits. */
        BOOL conWinFree = !gConWinUsed || dead_opens() == 0;
        while (!IsListEmpty(&gRetired))
            port_abandon((struct MsgPort *)RemHead(&gRetired));
        if (gConWin) {
            SetWindowTitles(gConWin, NULL, (UBYTE *)~0);
            if (conWinFree) {
                CloseWindow(gConWin);
                gConWin = NULL;
            }
        }
        if (IntuitionBase && !gConWin) CloseLibrary((struct Library *)IntuitionBase);
    }

    if (SocketBase) CloseLibrary(SocketBase);
    self->pr_WindowPtr = oldwinptr;

    timer_stop();
    CloseDevice((struct IORequest *)gTimer);
    DeleteIORequest((struct IORequest *)gTimer);
    DeleteMsgPort(gTimePort);
    FreeSignal(spawnSig);
    FreeSignal(gPortSig);
    FreeArgs(rd);
    return RETURN_OK;
}
