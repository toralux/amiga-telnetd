/*
 * edtest.c — host-side test of telnetd's input decoder and line editor.
 *
 * `make test` extracts the "portable input/editor section" of telnetd.c
 * into editor_part.c and compiles it here against stubs: socket input
 * comes from a script, and everything sent to the client goes into a
 * small VT100/xterm-like terminal model (right-margin "wrap pending",
 * CUU/CUD/CUF/CUB/CHA, ED, EL). After every key the test checks that the
 * terminal shows exactly the edited line where the editor thinks it is,
 * with the cursor at the edit position, and that each line handed to the
 * shell equals what was on screen.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef long LONG;
typedef unsigned long ULONG;
typedef short BOOL;
typedef void *APTR;
#define TRUE  1
#define FALSE 0

#define TEL_IAC  255
#define TEL_DONT 254
#define TEL_DO   253
#define TEL_WONT 252
#define TEL_WILL 251
#define TEL_SB   250
#define TEL_IP   244
#define TEL_BRK  243
#define TEL_SE   240
#define OPT_ECHO   1
#define OPT_SGA    3
#define OPT_NAWS  31
#define SOCK_EINTR        4
#define SOCK_EWOULDBLOCK 35
#define SOCK_ENXIO        6
#define SIGBREAKF_CTRL_C 0x1000
#define SIGBREAKF_CTRL_D 0x2000
#define SIGBREAKF_CTRL_E 0x4000
#define SIGBREAKF_CTRL_F 0x8000

/* ---- the globals the section uses (as declared in telnetd.c) ---- */
static int gSock = 1;
static BOOL gHangup = FALSE, gRaw = FALSE;
static BOOL gWill[256];
static unsigned char gIn[256];
static LONG gInHead = 0, gInTail = 0;
static int gTelState = 0, gTelCmd = 0;
static BOOL gLastCR = FALSE;
#define READYSZ 1024
static unsigned char gReady[READYSZ];
static LONG gRHead = 0, gRCount = 0;
static BOOL gEof = FALSE;
#define LINEMAX 255
#define HISTN   16
static unsigned char gLine[LINEMAX];
static LONG gLen = 0, gCur = 0;
static unsigned char gSaved[LINEMAX];
static LONG gSavedLen = 0;
static unsigned char gHist[HISTN][LINEMAX];
static LONG gHistLen[HISTN];
static LONG gHistCount = 0, gHistNext = 0, gHistPos = 0;
static int gEsc = 0;
static LONG gEscParam = 0;
static BOOL gEdit = TRUE;
static LONG gCols = 80;
static BOOL gDoNaws = FALSE;
static LONG gTermCol = 0;
static int gTrkEsc = 0;
static LONG gTrkN = 0, gTrkN1 = -1;
static LONG gStartCol = 0;
static LONG gPos = 0;
static int gSbOpt = 0;
static unsigned char gSbBuf[8];
static int gSbLen = 0;

/* ---- stubs ---- */
static unsigned char feed[1 << 16];
static int feedLen, feedPos, chunk;
static int hung;
static ULONG sigs;
static int negLen;
static unsigned char negot[64];

static void logmsg(const char *f, LONG a) { (void)f; (void)a; }
static LONG Errno(void) { return SOCK_EWOULDBLOCK; }
static LONG recv(int s, APTR buf, LONG n, int fl)
{
    LONG k;
    (void)s; (void)fl;
    if (hung) return 0;
    if (feedPos >= feedLen) return -1;
    k = feedLen - feedPos;
    if (k > n) k = n;
    if (k > chunk) k = chunk;
    memcpy(buf, feed + feedPos, (size_t)k);
    feedPos += (int)k;
    return k;
}
static void send_opt(int cmd, int opt)
{
    if (negLen < 60) { negot[negLen++] = 255; negot[negLen++] = (unsigned char)cmd; negot[negLen++] = (unsigned char)opt; }
}
static void send_signal(ULONG s) { sigs |= s; }
static void send_break(void) { send_signal(SIGBREAKF_CTRL_C); }

/* ---- terminal model ---- */
#define ROWS 4000
#define MAXW 200
static char scr[ROWS][MAXW];
static int W, crow, ccol, pend;
static int tEsc, tN;

static void fail(const char *what)
{
    printf("FAIL: %s (W=%d len=%ld cur=%ld start=%ld)\n", what, W, gLen, gCur, gStartCol);
    exit(1);
}

static void term_clear(void) { memset(scr, ' ', sizeof scr); crow = ccol = pend = 0; tEsc = 0; }

static void term_byte(unsigned char c)
{
    if (tEsc == 1) { if (c == '[') { tEsc = 2; tN = 0; return; } tEsc = 0; fail("ESC without ["); }
    if (tEsc == 2) {
        int n, r, k;
        if (c >= '0' && c <= '9') { tN = tN * 10 + (c - '0'); return; }
        tEsc = 0;
        n = tN ? tN : 1;
        switch (c) {
        case 'A': pend = 0; if (crow - n < 0) fail("cursor above row 0"); crow -= n; break;
        case 'B': pend = 0; if (crow + n >= ROWS) fail("cursor below screen"); crow += n; break;
        case 'C': pend = 0; if (ccol + n > W - 1) fail("CUF past margin"); ccol += n; break;
        case 'D': pend = 0; if (ccol - n < 0) fail("CUB past column 0"); ccol -= n; break;
        case 'K': for (k = ccol; k < W; k++) scr[crow][k] = ' '; break;
        case 'J':
            for (k = ccol; k < W; k++) scr[crow][k] = ' ';
            for (r = crow + 1; r < ROWS; r++) memset(scr[r], ' ', MAXW);
            break;
        default: fail("unexpected CSI final byte");
        }
        return;
    }
    switch (c) {
    case 27: tEsc = 1; return;
    case 13: ccol = 0; pend = 0; return;
    case 10: pend = 0; if (++crow >= ROWS) fail("ran off the screen"); return;
    case 7:  return;
    case 8:  pend = 0; if (ccol > 0) ccol--; return;
    }
    if (c < 32) fail("control byte sent to terminal");
    if (pend) { pend = 0; ccol = 0; if (++crow >= ROWS) fail("ran off the screen"); }
    scr[crow][ccol] = (char)c;
    if (ccol == W - 1) pend = 1; else ccol++;
}

/* the editor's view of the world calls this (forward-declared there) */
static void term_track(unsigned char c);

/* as telnetd.c's sock_write: CSI -> ESC [, bare LF -> CR LF, IAC doubled
 * (the terminal sees one 255) - every byte through term_track */
static LONG sock_write(const unsigned char *p, LONG n)
{
    LONG i;
    for (i = 0; i < n; i++) {
        unsigned char c = p[i];
        if (c == 0x9b)      { term_track(27); term_track('['); term_byte(27); term_byte('['); }
        else if (c == 10 && (i == 0 || p[i - 1] != 13))
                            { term_track(13); term_track(10); term_byte(13); term_byte(10); }
        else                { term_track(c); term_byte(c); }
    }
    return 0;
}

#include "editor_part.c"

static void do_hangup(void) { gHangup = TRUE; }

/* ---- test plumbing ---- */
static char prompt[300];
static int promptKnown;          /* prompt sits right before the line */

static char cell(long lin) { return scr[lin / W][lin % W]; }

static void check(const char *ctx)
{
    long off, startLin, i, endLin;
    if (gEsc != 0 || !gWill[OPT_ECHO] || !gEdit || gRaw) return;
    if (gLen == 0) return;          /* empty line: anchored at the next key, after the prompt */
    if (pend) { printf("[%s] ", ctx); fail("left in wrap-pending state"); }
    off = gStartCol + gCur;
    if (ccol != off % W) { printf("[%s] col %d want %ld ", ctx, ccol, off % W); fail("cursor column"); }
    startLin = (long)(crow - off / W) * W + gStartCol;
    if (startLin < 0) fail("line start above screen");
    for (i = 0; i < gLen; i++)
        if ((unsigned char)cell(startLin + i) != gLine[i]) { printf("[%s] at %ld ", ctx, i); fail("screen != line"); }
    endLin = startLin + gLen;
    for (i = endLin; i < endLin + 3 * W; i++)
        if (cell(i) != ' ') { printf("[%s] junk at +%ld '%c' ", ctx, i - endLin, cell(i)); fail("junk after line"); }
    if (promptKnown) {
        long pl = (long)strlen(prompt);
        for (i = 0; i < pl; i++)
            if (cell(startLin - pl + i) != prompt[i]) { printf("[%s] ", ctx); fail("prompt damaged"); }
    }
}

/* Where the edited line starts on screen (linear cell index). */
static long line_start(void)
{
    long off = gStartCol + gCur;
    return (long)(crow - off / W) * W + gStartCol;
}

/* Row `r` must read exactly `want` from column 0, blanks after it. */
static void expect_row(int r, const char *want, const char *ctx)
{
    int i, n = (int)strlen(want);
    for (i = 0; i < W; i++) {
        char c = i < n ? want[i] : ' ';
        if (r < 0 || scr[r][i] != c) { printf("[%s] row %d col %d has '%c' ", ctx, r, i, r < 0 ? '?' : scr[r][i]); fail("row content"); }
    }
}

static void program_output(const char *s)    /* an ACTION_WRITE from the shell */
{
    ed_output_begin();
    sock_write((const unsigned char *)s, (LONG)strlen(s));
    ed_output_end();
}

static void show_prompt(void) { program_output(prompt); promptKnown = 1; }

static void keys(const char *s, int n)
{
    memcpy(feed + feedLen, s, (size_t)n);
    feedLen += n;
    while (feedPos < feedLen || gInHead < gInTail) {
        pump_input();
        check(s);
        if (ready_room() <= LINEMAX + 1) break;
    }
}
#define KEYS(s) keys(s, (int)sizeof(s) - 1)

static int take_line(char *out)               /* what the shell's READ gets */
{
    int n = 0;
    while (gRCount > 0) { unsigned char c = ready_get(); out[n++] = (char)c; if (c == '\n') break; }
    out[n] = 0;
    return n;
}

static void expect(const char *want)
{
    char got[600];
    take_line(got);
    if (strcmp(got, want) != 0) { printf("READ got '%s' want '%s' ", got, want); fail("delivered line"); }
    show_prompt();                            /* the shell prompts again */
}

static void fresh(int width, const char *p)
{
    gLen = gCur = gPos = 0; gHistCount = gHistNext = gHistPos = 0; gEsc = 0;
    gRHead = gRCount = 0; gEof = FALSE; gRaw = FALSE; gEdit = TRUE; gHangup = FALSE;
    gTelState = 0; gLastCR = FALSE; gInHead = gInTail = 0; feedLen = feedPos = 0;
    memset(gWill, 0, sizeof gWill); gWill[OPT_ECHO] = gWill[OPT_SGA] = TRUE;
    gCols = W = width; gTermCol = 0; gTrkEsc = 0; gStartCol = 0;
    chunk = 1 << 30; sigs = 0; hung = 0;
    term_clear();
    snprintf(prompt, sizeof prompt, "%s", p);
    show_prompt();
}

#define UP    "\033[A"
#define DOWN  "\033[B"
#define RIGHT "\033[C"
#define LEFT  "\033[D"

static void type_n(char c, int n) { int i; for (i = 0; i < n; i++) keys(&c, 1); }

int main(void)
{
    static const int widths[] = { 80, 20, 37 };
    static const char *prompts[] = { "1.SYS:> ", "", "1.Work:Projects/amiga> ", "x" };
    int wi, pi, i, t;
    char longp[200];

    for (wi = 0; wi < 3; wi++) for (pi = 0; pi < 4; pi++) {
        int w = widths[wi];
        const char *p = prompts[pi];

        /* basic lines, all NVT line ends */
        fresh(w, p); KEYS("dir\r\0"); expect("dir\n");
        KEYS("list s:\r\n"); expect("list s:\n");
        KEYS("x\r"); expect("x\n");

        /* history with a saved half-typed line */
        fresh(w, p); KEYS("echo one\r"); expect("echo one\n"); KEYS("echo two\r"); expect("echo two\n");
        KEYS("par"); KEYS(UP); KEYS(UP); KEYS(UP); KEYS(DOWN); KEYS(DOWN);
        if (gLen != 3 || memcmp(gLine, "par", 3)) fail("saved line");
        KEYS("\r"); expect("par\n");

        /* lines much longer than the width: type, recall, edit in the middle */
        fresh(w, p);
        for (i = 0; i < 3 * w + 5; i++) { char c = (char)('a' + i % 26); keys(&c, 1); }
        KEYS("\r"); { char g[600]; take_line(g); if ((int)strlen(g) != 3 * w + 6) fail("long line"); show_prompt(); }
        KEYS("short\r"); expect("short\n");
        KEYS(UP); KEYS(UP);                   /* long line back, then shorter, then long */
        KEYS(DOWN); KEYS(UP);
        for (i = 0; i < w + 3; i++) KEYS(LEFT);
        KEYS("XY"); KEYS("\177"); KEYS("\033[3~");
        KEYS("\033[H"); KEYS("\033[F"); KEYS("\033[1~"); KEYS("\033[4~");
        { char g[600]; char want[600]; int n = (int)gLen; memcpy(want, gLine, (size_t)n); want[n] = '\n'; want[n + 1] = 0;
          KEYS("\r"); take_line(g); if (strcmp(g, want)) fail("edited long line"); show_prompt(); }

        /* Enter with the cursor at the start of a wrapped line: the whole
         * line must stay on screen above the next prompt */
        fresh(w, p);
        for (i = 0; i < 2 * w + 7; i++) { char c = (char)('A' + i % 26); keys(&c, 1); }
        KEYS("\033[H");
        {
            long st = line_start(), n = gLen;
            char saved[600];
            memcpy(saved, gLine, (size_t)n);
            KEYS("\r");
            for (i = 0; i < n; i++) if (cell(st + i) != saved[i]) fail("line damaged by Enter mid-line");
            { char g[600]; take_line(g); show_prompt(); }
            KEYS("k");
        }
        KEYS("\030");

        /* a line that ends exactly at the right margin, then grows/shrinks */
        fresh(w, p);
        type_n('m', w - (int)(strlen(p) % w));
        type_n('n', 1); KEYS("\177"); KEYS("\177"); type_n('o', w);
        KEYS("\030"); KEYS("k\r"); expect("k\n");

        /* kill to end on a wrapped line */
        fresh(w, p); type_n('q', 2 * w); for (i = 0; i < w + 1; i++) KEYS(LEFT); KEYS("\013");
        if (gLen != w - 1) fail("kill to end");
        KEYS("\r"); { char g[600]; take_line(g); show_prompt(); }

        /* typeahead while a command prints: the line moves below the output,
         * and the output row holds the output only (the typed text, longer
         * than it, must not show through) */
        fresh(w, "");
        KEYS("typeahead"); program_output("x\n");
        promptKnown = 0; check("after output");
        expect_row(crow - 1, "x", "output row");
        KEYS("\030");
        fresh(w, p); KEYS("type"); program_output("some output\n");
        promptKnown = 0; check("after output");
        program_output("more output without newline"); check("after partial output");
        program_output("\n"); program_output(prompt); promptKnown = 1; check("after prompt");
        KEYS(" ahead\r"); expect("type ahead\n");
    }

    /* prompt that ends exactly at the margin, and one longer than a row */
    for (wi = 0; wi < 3; wi++) {
        int w = widths[wi];
        memset(longp, '>', (size_t)w); longp[w] = 0;
        fresh(w, longp); KEYS("abc"); KEYS(LEFT LEFT); KEYS("Z\r"); expect("aZbc\n");
        memset(longp, '#', (size_t)(w + 5)); longp[w + 5] = 0;
        fresh(w, longp); type_n('z', w + 2); KEYS(UP); KEYS("\r"); { char g[600]; take_line(g); show_prompt(); }
    }

    /* escape sequence split across reads, telnet command inside it */
    fresh(80, "> "); KEYS("ls\r"); expect("ls\n");
    chunk = 1; KEYS("\033"); KEYS("["); KEYS("A"); KEYS("\r"); expect("ls\n");
    KEYS("\033\377\373\001[A\r"); expect("ls\n");

    /* NAWS: width from the client, escaped 255 inside the subnegotiation */
    fresh(80, "> "); KEYS("\377\372\037\000\144\000\050\377\360"); if (gCols != 100) fail("NAWS 100");
    KEYS("\377\372\037\000\377\377\000\050\377\360"); if (gCols != 255) fail("NAWS 255 (escaped)");

    /* 255-char limit, signals, EOF */
    fresh(80, "> "); type_n('a', 260); if (gLen != LINEMAX) fail("limit");
    KEYS("\030"); KEYS("\003"); KEYS("\004"); if (!(sigs & SIGBREAKF_CTRL_C) || !(sigs & SIGBREAKF_CTRL_D)) fail("signals");
    KEYS("\034"); if (!gEof) fail("Ctrl-\\ EOF");

    /* DONT ECHO: nothing drawn, line still delivered */
    fresh(80, "> "); gWill[OPT_ECHO] = FALSE; KEYS("silent\r"); { char g[64]; take_line(g); if (strcmp(g, "silent\n")) fail("no-echo line"); }

    /* raw mode: bytes through, CR stays CR, no echo */
    fresh(80, "> "); gRaw = TRUE; KEYS("q\r\033[A");
    { unsigned char b[16]; int n = 0; while (gRCount) b[n++] = ready_get();
      if (n != 5 || b[0] != 'q' || b[1] != 13 || b[2] != 27) fail("raw bytes"); }

    /* DUMBTERM: client-edited lines collected, no echo, BS honoured */
    fresh(80, "> "); gEdit = FALSE; gWill[OPT_ECHO] = FALSE;
    KEYS("ab\bc\r"); { char g[64]; take_line(g); if (strcmp(g, "ac\n")) fail("DUMBTERM line"); }

    /* hangup */
    fresh(80, "> "); hung = 1; pump_input(); if (!gHangup) fail("hangup");

    /* fuzz: random keys and program output at random widths and prompts */
    srand(12345);
    for (t = 0; t < 60000; t++) {
        static const char *k[] = { "a", "b", "Z", " ", "w", "x", "y", "\177", "\b", UP, DOWN, LEFT, RIGHT,
            "\033[H", "\033[F", "\033[3~", "\030", "\013", "\033OA", "\033OB", "\033[1;2C", "\r", "!" };
        const char *key;
        if (t % 500 == 0) {
            int plen = rand() % 45;
            memset(longp, 'p', (size_t)plen); longp[plen] = 0;
            fresh(20 + rand() % 70, longp);
        }
        key = k[rand() % (int)(sizeof k / sizeof *k)];
        if (key[0] == '!') {                   /* the shell writes something */
            static const char *outs[] = { "out\n", "partial", "\n", "a much longer line of output text\n" };
            program_output(outs[rand() % 4]);
            promptKnown = 0;
            check("fuzz output");
        } else if (key[0] == '\r') {
            char shown[600], got[600];
            int n = (int)gLen;
            memcpy(shown, gLine, (size_t)n); shown[n] = '\n'; shown[n + 1] = 0;
            keys(key, 1);
            take_line(got);
            if (strcmp(got, shown)) fail("fuzz delivered line");
            show_prompt();
        } else {
            keys(key, (int)strlen(key));
        }
    }

    printf("edtest: all tests passed (3 widths x 4 prompts, margin cases, 60000 fuzz steps)\n");
    return 0;
}
