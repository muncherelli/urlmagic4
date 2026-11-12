/*
 * urlmagic<version>.exe (e.g. urlmagic4.0.0.exe) - console installer for urlmagic4 on IIS 4.0.
 *
 *   urlmagic4.0.0                       ask, then install / upgrade / uninstall (double-click)
 *   urlmagic4.0.0 install [dir]         install to dir (default %SystemDrive%\URLMagic4)
 *   urlmagic4.0.0 uninstall          remove (keeps each site's settings)
 *   urlmagic4.0.0 uninstall /purge   remove, including each site's settings
 *
 * Everything it installs is inside it (RCDATA resources, see payload.h), so this one file is
 * all you need. It also copies itself into the install folder, to uninstall later.
 *
 * Installing: stops the WWW service, copies urlmagic4.dll and urlmagic4-mmc-snap-in.dll, registers
 * the URL Magic tab, adds urlmagic4 to IIS's global ISAPI filters, starts the WWW service.
 */
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#include <windows.h>   /* NOSONAR: MinGW's headers are lower case, and it's cross-built on Linux */
#include <ole2.h>
#include <shellapi.h>
#include <iadmw.h>     /* NOSONAR: as above */
#include <iiscnfg.h>   /* NOSONAR: as above */

#include "version.h"
#include "payload.h"
#include "../common/settings.h"

static const GUID CLSID_UmMSAdminBase =
    {0xa9e69610, 0xb80d, 0x11d0, {0xb9, 0xb9, 0x00, 0xa0, 0xc9, 0x22, 0xe7, 0x50}};
static const GUID IID_UmIMSAdminBase =
    {0x70b51430, 0xb6ca, 0x11d0, {0xb9, 0xb9, 0x00, 0xa0, 0xc9, 0x22, 0xe7, 0x50}};

static const WCHAR FILTER_NAME[] = L"urlmagic4";
static const WCHAR FILTERS_KEY[] = L"/LM/W3SVC/Filters";
/* installed files, in the order of their resource IDs (payload.h) */
static const WCHAR *FILES[] = { L"urlmagic4.dll", L"urlmagic4-mmc-snap-in.dll", L"README.txt", L"LICENSE.txt",
                                L"CHANGELOG.txt", NULL };
static const int FILE_IDS[] = { ID_FILTER_DLL, ID_TAB_DLL, ID_README, ID_LICENSE, ID_CHANGELOG };

static HANDLE g_out;
static WCHAR g_self[MAX_PATH];         /* this program's file name, e.g. urlmagic4.0.0.exe */
static BOOL g_interactive;             /* double-clicked: ask, and wait before closing */
static int g_errors;

/* ---------------------------------------------------------------- console output
 *
 * NT 4.0's console has no ANSI escapes, but it has 16 colours (SetConsoleTextAttribute) and
 * the IBM PC character set (code page 437): box drawing, blocks, bullets. Output that isn't a
 * console (redirected to a file) gets no colours.
 */

enum {
    C_DIM = 8, C_GREY = 7, C_WHITE = 15, C_BLUE = 9, C_GREEN = 10, C_CYAN = 11, C_RED = 12,
    C_MAGENTA = 13, C_YELLOW = 14, C_DCYAN = 3
};

static BOOL g_color;
static WORD g_attr0 = C_GREY;
static int g_spin = -1;                 /* spinner position, -1 = not spinning */
static int g_col;                       /* output column, for lining up the step results */

static void color(WORD a)
{
    if (g_color) SetConsoleTextAttribute(g_out, a);
}

static void out(const char *s)
{
    DWORD w;
    WriteFile(g_out, s, (DWORD)lstrlenA(s), &w, NULL);
    for (const char *p = s; *p; p++) {
        if (*p == '\n' || *p == '\r') g_col = 0;
        else if (*p == '\b') { if (g_col) g_col--; }
        else g_col++;
    }
}

static void outc(char c, int n)
{
    char buf[2] = { c, 0 };
    while (n-- > 0) out(buf);
}

static void outw(const WCHAR *s)
{
    char buf[1024];
    WideCharToMultiByte(CP_OEMCP, 0, s, -1, buf, sizeof buf, NULL, NULL);
    out(buf);
}

static void ui_init(void)
{
    CONSOLE_SCREEN_BUFFER_INFO ci;
    DWORD mode;
    g_color = GetConsoleMode(g_out, &mode) && GetConsoleScreenBufferInfo(g_out, &ci);
    if (g_color) g_attr0 = ci.wAttributes;
    SetConsoleTitleA("URL Magic v" UM_VERSION_STR " Setup");
    {
        /* the console window's own icon: undocumented, so only if kernel32 has it */
        typedef BOOL (WINAPI *seticon_fn)(HICON);
        seticon_fn seticon = (seticon_fn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetConsoleIcon");
        HICON icon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
        if (seticon && icon) seticon(icon);
    }
}

static void ui_done(void) { color(g_attr0); }

/* the big logo: 5-row block letters (X = full block) */
static const char *LOGO_FONT[][5] = {
    {"X...X", "X...X", "X...X", "X...X", ".XXX."},   /* U */
    {"XXXX.", "X...X", "XXXX.", "X..X.", "X...X"},   /* R */
    {"X....", "X....", "X....", "X....", "XXXXX"},   /* L */
    {"X...X", "XX.XX", "X.X.X", "X...X", "X...X"},   /* M */
    {".XXX.", "X...X", "XXXXX", "X...X", "X...X"},   /* A */
    {".XXXX", "X....", "X..XX", "X...X", ".XXX."},   /* G */
    {"XXX", ".X.", ".X.", ".X.", "XXX"},             /* I */
    {".XXXX", "X....", "X....", "X....", ".XXXX"},   /* C */
    {"X..X.", "X..X.", "XXXXX", "...X.", "...X."},   /* 4 */
};
/* glyph index, or -1 for a word gap */
static const int LOGO_TEXT[] = { 0, 1, 2, -1, 3, 4, 5, 6, 7 };   /* URL MAGIC; the 4 is in the icon */
#define LOGO_W 46
#define BOX_W 69                         /* inside: 2 + icon 16 + 2 + logo 46 + 3 (the icon's
                                            first pixel column is empty, so 3 balances it) */

/* the wand icon (same pixel art as res/urlmagic4.ico, tools/make_art.py), drawn with half
   blocks: each character cell shows two pixels, one above the other */
static const char *ICON_ART[16] = {
    "..........Y.....",
    "..........Y.....",
    "........YYWYY...",
    "..........Y..M..",
    "....C.....Y.....",
    "...CWC..WW......",
    "....C..WWWK...Y.",
    ".......WWKK..YWY",
    "......KKKK....Y.",
    ".....KKKK....R..",
    "....KKKK....RR..",
    "...KKKK....R.R..",
    "..KKKK....R..R..",
    ".KKKK.....RRRRR.",
    ".KKK.........R..",
    "..K..........R..",
};

/* console colour of an icon pixel; the black wand becomes dark grey on the black console */
static WORD icon_color(char c)
{
    switch (c) {
    case 'Y': return C_YELLOW;
    case 'W': return C_WHITE;
    case 'M': return C_MAGENTA;
    case 'C': return C_CYAN;
    case 'R': return C_RED;
    case 'K': return C_DIM;
    default:  return 0;
    }
}

/* one row of the icon: pixel rows 2*row and 2*row+1 */
static void icon_row(int row)
{
    for (int x = 0; x < 16; x++) {
        char top = ICON_ART[row * 2][x];
        char bot = ICON_ART[row * 2 + 1][x];
        if (top == '.' && bot == '.') { color(C_GREY); out(" "); }
        else if (bot == '.') { color(icon_color(top)); outc((char)0xDF, 1); }          /* upper half */
        else if (top == '.') { color(icon_color(bot)); outc((char)0xDC, 1); }          /* lower half */
        else { color((WORD)(icon_color(top) | (icon_color(bot) << 4))); outc((char)0xDF, 1); }
    }
}

static void box_edge(char left, char right)
{
    color(C_DCYAN);
    out("  ");
    outc(left, 1);
    outc((char)0xCD, BOX_W);
    outc(right, 1);
    out("\r\n");
}

/* one line inside the box: centred text in colour c */
static void box_line(const char *text, WORD c)
{
    int n = lstrlenA(text);
    int pad = (BOX_W - n) / 2;
    color(C_DCYAN);
    out("  \xBA");
    color(c);
    outc(' ', pad);
    out(text);
    outc(' ', BOX_W - n - pad);
    color(C_DCYAN);
    out("\xBA\r\n");
}

#define LOGO_N ((int)(sizeof LOGO_TEXT / sizeof LOGO_TEXT[0]))

/* console colour of logo glyph g */
static WORD logo_color(int g)
{
    if (g <= 2) return C_CYAN;
    if (g <= 7) return C_MAGENTA;
    return C_RED;
}

/* one row (0-4) of the logo letters */
static void logo_row(int lr)
{
    for (int i = 0; i < LOGO_N; i++) {
        int g = LOGO_TEXT[i];
        const char *bits;
        if (g < 0) { out("  "); continue; }
        color(logo_color(g));
        bits = LOGO_FONT[g][lr];
        for (int j = 0; bits[j]; j++) outc(bits[j] == 'X' ? (char)0xDB : ' ', 1);
        if (i + 1 < LOGO_N && LOGO_TEXT[i + 1] >= 0) out(" ");
    }
}

static void banner(void)
{
    out("\r\n");
    box_edge((char)0xC9, (char)0xBB);
    /* icon on the left (8 rows), logo beside it (5 rows, rows 2-6) */
    for (int row = 0; row < 8; row++) {
        int lr = row - 2;
        color(C_DCYAN);
        out("  \xBA  ");
        icon_row(row);
        color(C_GREY);
        out("  ");
        if (lr >= 0 && lr < 5) {
            logo_row(lr);
        } else {
            outc(' ', LOGO_W);
        }
        outc(' ', BOX_W - 2 - 16 - 2 - LOGO_W);
        color(C_DCYAN);
        out("\xBA\r\n");
    }
    box_line("", C_GREY);
    box_line("The missing IIS tab: URL rewriting for IIS 4.0 on NT 4.0", C_WHITE);
    box_line("version " UM_VERSION_STR "  \xFA  github.com/muncherelli/urlmagic4", C_DIM);
    box_edge((char)0xC8, (char)0xBC);
    color(C_GREY);
    out("\r\n");
}

/* ---------------------------------------------------------------- full-screen mode
 *
 * Double-clicked, setup has a fresh console of its own: it turns it into an 80 x 25 screen in
 * the style of NT's own text-mode Setup (blue, with a grey key bar at the bottom) and draws
 * with WriteConsoleOutput*, which never scrolls.
 */
#define SCR_W 80
#define SCR_H 25
#define ON_BLUE(fg) ((WORD)(0x10 | (fg)))
#define A_TEXT   ON_BLUE(C_GREY)
#define A_BRIGHT ON_BLUE(C_WHITE)
#define A_KEYBAR ((WORD)0x70)
#define STEP_X   4                    /* one column, 4 in from the left ... */
#define TAG_X    70                   /* ... and "[ OK ]" ends 4 in from the right */
#define BAR_ROW  11
#define RES_ROW  13
#define MAX_MSGS 6

static BOOL g_tui;
static HANDLE g_in;
static char g_rawch;                   /* the last character typed, not lowercased */
static int g_row;
static int g_cur_row = -1;
static int g_total = 1;
static int g_done;
static char g_msgs[MAX_MSGS][100];
static WORD g_msg_fg[MAX_MSGS];
static int g_nmsgs;

static void put(int x, int y, WORD attr, const char *t)
{
    COORD c;
    DWORD n = (DWORD)lstrlenA(t);
    DWORD w;
    if (!n || y < 0 || y >= SCR_H) return;
    if (x + (int)n > SCR_W) n = (DWORD)(SCR_W - x);
    c.X = (SHORT)x;
    c.Y = (SHORT)y;
    WriteConsoleOutputCharacterA(g_out, t, n, c, &w);
    FillConsoleOutputAttribute(g_out, attr, n, c, &w);
}

static void fill(int x, int y, WORD attr, char ch, int n)
{
    COORD c;
    DWORD w;
    if (n <= 0) return;
    c.X = (SHORT)x;
    c.Y = (SHORT)y;
    FillConsoleOutputCharacterA(g_out, ch, (DWORD)n, c, &w);
    FillConsoleOutputAttribute(g_out, attr, (DWORD)n, c, &w);
}


static void clear_rows(int y0, int y1)
{
    for (int y = y0; y <= y1; y++) fill(0, y, A_TEXT, ' ', SCR_W);
}

static void keybar(const char *keys)
{
    static const char ver[] = "v" UM_VERSION_STR;
    fill(0, SCR_H - 1, A_KEYBAR, ' ', SCR_W);
    put(1, SCR_H - 1, A_KEYBAR, keys);                         /* keys on the left ... */
    put(SCR_W - 1 - lstrlenA(ver), SCR_H - 1, A_KEYBAR, ver);  /* ... version on the right */
}

/* "[ OK ]" etc. */
static void tui_tag(int x, int y, const char *t, WORD fg)
{
    put(x, y, A_TEXT, "[");
    put(x + 1, y, ON_BLUE(fg), t);
    put(x + 5, y, A_TEXT, "]");
}

/* a blank blue screen */
static void tui_frame(void)
{
    clear_rows(0, SCR_H - 1);          /* the window title names the program */
}

static BOOL tui_begin(void)
{
    CONSOLE_SCREEN_BUFFER_INFO ci;
    CONSOLE_CURSOR_INFO cur;
    SMALL_RECT r;
    COORD size;

    if (!g_color || !GetConsoleScreenBufferInfo(g_out, &ci)) return FALSE;
    /* only a console of our own (started by double-click) is still at the top-left */
    if (ci.dwCursorPosition.X != 0 || ci.dwCursorPosition.Y != 0) return FALSE;
    r.Left = 0; r.Top = 0; r.Right = SCR_W - 1; r.Bottom = SCR_H - 1;
    size.X = SCR_W; size.Y = SCR_H;
    SetConsoleWindowInfo(g_out, TRUE, &r);
    SetConsoleScreenBufferSize(g_out, size);
    SetConsoleWindowInfo(g_out, TRUE, &r);
    if (!GetConsoleScreenBufferInfo(g_out, &ci) || ci.dwSize.X < SCR_W || ci.dwSize.Y < SCR_H ||
        ci.srWindow.Left != 0 || ci.srWindow.Top != 0 ||
        ci.srWindow.Right < SCR_W - 1 || ci.srWindow.Bottom < SCR_H - 1)
        return FALSE;
    cur.dwSize = 25;
    cur.bVisible = FALSE;
    SetConsoleCursorInfo(g_out, &cur);
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    SetConsoleMode(g_in, 0);
    g_tui = TRUE;
    return TRUE;
}

/* wait for a key: a character (letters lowercased), 13 = Enter, 8 = Backspace, 27 = Esc or F3,
   3 = Ctrl+C */
/* what key() makes of a key press; 0 for one it ignores */
static int key_code(const KEY_EVENT_RECORD *ke)
{
    WORD vk = ke->wVirtualKeyCode;
    char ch = ke->uChar.AsciiChar;
    if (vk == VK_RETURN) return 13;
    if (vk == VK_BACK) return 8;
    if (ch == 3) return 3;                                        /* Ctrl+C */
    if (vk == VK_ESCAPE || vk == VK_F3) return 27;
    g_rawch = ch;
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
    if (ch >= ' ') return ch;
    return 0;                                                     /* not a key we use */
}

static int key(void)
{
    INPUT_RECORD ir;
    DWORD n;
    for (;;) {
        if (!ReadConsoleInputA(g_in, &ir, 1, &n)) return 27;
        if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown) {
            int k = key_code(&ir.Event.KeyEvent);
            if (k) return k;
        }
    }
}

/* the wand, 16 x 8 cells; on blue the wand can be its real black */
static void tui_icon(int x0, int y0)
{
    for (int row = 0; row < 8; row++) {
        for (int x = 0; x < 16; x++) {
            char top = ICON_ART[row * 2][x];
            char bot = ICON_ART[row * 2 + 1][x];
            WORD ft = top == 'K' ? 0 : icon_color(top);
            WORD fb = bot == 'K' ? 0 : icon_color(bot);
            char ch[2] = { ' ', 0 };
            WORD a = A_TEXT;
            if (top == '.' && bot == '.') continue;
            if (bot == '.') { ch[0] = (char)0xDF; a = ON_BLUE(ft); }
            else if (top == '.') { ch[0] = (char)0xDC; a = ON_BLUE(fb); }
            else { ch[0] = (char)0xDF; a = (WORD)(ft | (fb << 4)); }
            put(x0 + x, y0 + row, a, ch);
        }
    }
}

/* one row of logo glyph g at x, y; returns its width */
static int tui_glyph_row(int g, int row, int x, int y)
{
    const char *bits = LOGO_FONT[g][row];
    int w = lstrlenA(bits);
    for (int j = 0; j < w; j++)
        if (bits[j] == 'X') fill(x + j, y, ON_BLUE(g <= 2 ? C_CYAN : C_MAGENTA), (char)0xDB, 1);
    return w;
}

static void tui_logo(int x0, int y0)
{
    for (int row = 0; row < 5; row++) {
        int x = x0;
        for (int i = 0; i < LOGO_N; i++) {
            int g = LOGO_TEXT[i];
            if (g < 0) { x += 2; continue; }
            x += tui_glyph_row(g, row, x, y0 + row);
            if (i + 1 < LOGO_N && LOGO_TEXT[i + 1] >= 0) x++;
        }
    }
}

static void tui_bar(void)
{
    const int x0 = STEP_X;
    const int width = 65;                /* [####] at 4..70, "100%" at 72..75 */
    int filled = g_total ? g_done * width / g_total : 0;
    int percent = 0;
    char pct[8];
    if (filled > width) filled = width;
    put(x0, BAR_ROW, A_TEXT, "[");
    fill(x0 + 1, BAR_ROW, ON_BLUE(C_CYAN), (char)0xDB, filled);
    fill(x0 + 1 + filled, BAR_ROW, ON_BLUE(C_DCYAN), (char)0xB0, width - filled);
    put(x0 + 1 + width, BAR_ROW, A_TEXT, "]");
    if (g_total) percent = g_done * 100 / g_total;
    if (percent > 100) percent = 100;
    wsprintfA(pct, "%3d%%", percent);
    put(x0 + width + 3, BAR_ROW, A_BRIGHT, pct);
}

/* a message for the result area (full screen) or an indented line (console) */
static void msg(const char *text, WORD fg)
{
    if (g_tui) {
        if (g_nmsgs < MAX_MSGS) {
            lstrcpynA(g_msgs[g_nmsgs], text, sizeof g_msgs[0]);
            g_msg_fg[g_nmsgs++] = fg;
        }
        return;
    }
    color(fg);
    out("      ");
    out(text);
    color(C_GREY);
    out("\r\n");
}

/* "  -- Title ------" */
static void section(const char *title)
{
    if (g_tui) {                         /* start the progress screen */
        tui_frame();
        put(STEP_X, 2, A_BRIGHT, title);
        g_row = 4;
        g_done = 0;
        tui_bar();
        keybar("Please wait...");
        return;
    }
    color(C_DCYAN);
    out("  \xC4\xC4 ");
    color(C_WHITE);
    out(title);
    out(" ");
    color(C_DCYAN);
    outc((char)0xC4, BOX_W - lstrlenA(title) - 2);
    color(C_GREY);
    out("\r\n");
}

#define RESULT_COL 56

/* "  >> What it's doing ........." then a result */
static void step(const char *what)
{
    if (g_tui) {
        int n = lstrlenA(what);
        g_cur_row = g_row++;
        put(STEP_X, g_cur_row, ON_BLUE(C_CYAN), "\xAF");
        put(STEP_X + 2, g_cur_row, A_TEXT, what);
        fill(STEP_X + 3 + n, g_cur_row, ON_BLUE(C_DCYAN), (char)0xFA, TAG_X - 1 - (STEP_X + 3 + n));
        return;
    }
    color(C_CYAN);
    out("  \xAF ");
    color(C_GREY);
    out(what);
    out(" ");
    color(C_DIM);
    outc((char)0xFA, RESULT_COL - g_col - 1);
    out(" ");
}

static void spin(void)
{
    static const char chars[] = "|/-\\";
    if (g_tui) {
        char c[2] = { 0, 0 };
        g_spin = (g_spin + 1) % 4;
        c[0] = chars[g_spin];
        put(TAG_X + 2, g_cur_row, ON_BLUE(C_YELLOW), c);
        return;
    }
    if (g_spin < 0) { g_spin = 0; color(C_YELLOW); outc(chars[0], 1); return; }
    g_spin = (g_spin + 1) % 4;
    color(C_YELLOW);
    out("\b");
    outc(chars[g_spin], 1);
}

static void spin_end(void)
{
    if (g_tui) { g_spin = -1; return; }
    if (g_spin >= 0) { out("\b"); g_spin = -1; }
}

static void tag(const char *t, WORD c)
{
    spin_end();
    color(C_DIM); out("[");
    color(c); out(t);
    color(C_DIM); out("]");
    color(C_GREY);
}

static void ok(void)
{
    if (g_tui) {
        spin_end();
        tui_tag(TAG_X, g_cur_row, " OK ", C_GREEN);
        g_done++;
        tui_bar();
        return;
    }
    tag(" OK ", C_GREEN);
    out("\r\n");
}

/* a result that's a value rather than OK, e.g. a version number */
static void value(const char *v, BOOL good)
{
    spin_end();
    color(good ? C_GREEN : C_YELLOW);
    out(v);
    color(C_GREY);
    out("\r\n");
}

static void note(const char *text) { msg(text, C_YELLOW); }

static void fail(const char *why, DWORD code)
{
    char buf[300];
    if (g_tui) {
        if (g_cur_row >= 0) { spin_end(); tui_tag(TAG_X, g_cur_row, "FAIL", C_RED); }
    } else if (g_col > 2) {
        tag("FAIL", C_RED);
        out("\r\n");
    }
    if (code) wsprintfA(buf, "%s (error %lu)", why, (unsigned long)code);
    else lstrcpynA(buf, why, sizeof buf);
    msg(buf, C_RED);
    g_errors++;
}

/* ---------------------------------------------------------------- WWW service */

/* half a second, with the spinner going */
static void spin_wait(void)
{
    spin();
    Sleep(250);
    spin();
    Sleep(250);
}

/* stop svc, whose status is st, and wait up to a minute for it */
static BOOL www_stop(SC_HANDLE svc, SERVICE_STATUS *st)
{
    if (st->dwCurrentState != SERVICE_STOPPED && st->dwCurrentState != SERVICE_STOP_PENDING &&
        !ControlService(svc, SERVICE_CONTROL_STOP, st)) {
        fail("can't stop the WWW service", GetLastError());
        return FALSE;
    }
    for (int i = 0; i < 120 && st->dwCurrentState != SERVICE_STOPPED; i++) {
        spin_wait();
        if (!QueryServiceStatus(svc, st)) break;
    }
    if (st->dwCurrentState != SERVICE_STOPPED) { fail("the WWW service didn't stop", 0); return FALSE; }
    return TRUE;
}

/* start svc, whose status is st, and wait up to a minute for it */
static BOOL www_start(SC_HANDLE svc, SERVICE_STATUS *st)
{
    if (st->dwCurrentState == SERVICE_RUNNING) return TRUE;
    if (!StartServiceW(svc, 0, NULL)) { fail("can't start the WWW service", GetLastError()); return FALSE; }
    for (int i = 0; i < 120; i++) {
        spin_wait();
        if (!QueryServiceStatus(svc, st) || st->dwCurrentState != SERVICE_START_PENDING) break;
    }
    if (st->dwCurrentState != SERVICE_RUNNING) { fail("the WWW service didn't start", 0); return FALSE; }
    return TRUE;
}

/* Stop or start W3SVC; was_running says whether it was running before a stop. */
static BOOL www(BOOL start, BOOL *was_running)
{
    SC_HANDLE scm;
    SC_HANDLE svc;
    SERVICE_STATUS st;
    BOOL result = FALSE;

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) { fail("can't open the service manager", GetLastError()); return FALSE; }
    svc = OpenServiceW(scm, L"W3SVC", SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) { fail("can't open the WWW service (is IIS installed?)", GetLastError()); CloseServiceHandle(scm); return FALSE; }

    if (!QueryServiceStatus(svc, &st)) {
        fail("can't query the WWW service", GetLastError());
    } else if (start) {
        result = www_start(svc, &st);
    } else {
        if (was_running) *was_running = st.dwCurrentState != SERVICE_STOPPED;
        result = www_stop(svc, &st);
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return result;
}

/* ---------------------------------------------------------------- metabase */

static IMSAdminBaseW *metabase(void)
{
    IMSAdminBaseW *mb = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_UmMSAdminBase, NULL, CLSCTX_ALL, &IID_UmIMSAdminBase, (void **)&mb);
    if (FAILED(hr)) { fail("can't open the IIS metabase", (DWORD)hr); return NULL; }
    return mb;
}

/* v isn't const: the record's data pointer isn't (SetData only reads it) */
static void rec_string(METADATA_RECORD *r, DWORD id, DWORD utype, WCHAR *v)
{
    memset(r, 0, sizeof *r);
    r->dwMDIdentifier = id;
    r->dwMDUserType = utype;
    r->dwMDDataType = STRING_METADATA;
    r->dwMDDataLen = (DWORD)(lstrlenW(v) + 1) * sizeof(WCHAR);
    r->pbMDData = (unsigned char *)v;
}

/* case-insensitive: is name one of the comma-separated items in list? */
static BOOL in_list(const WCHAR *list, const WCHAR *name)
{
    int n = lstrlenW(name);
    const WCHAR *p = list;
    while (*p) {
        const WCHAR *e = p;
        WCHAR item[128];
        int len;
        while (*e && *e != L',') e++;
        len = (int)(e - p);
        while (len && *p == L' ') { p++; len--; }
        while (len && p[len - 1] == L' ') len--;
        if (len == n && len < 128) {
            lstrcpynW(item, p, len + 1);
            if (lstrcmpiW(item, name) == 0) return TRUE;
        }
        p = *e ? e + 1 : e;
    }
    return FALSE;
}

/* add item (len characters) to the comma-separated outl, as far as it fits */
static void append_item(WCHAR *outl, const WCHAR *item, int len, int cap)
{
    if (outl[0] && lstrlenW(outl) + 1 < cap) lstrcatW(outl, L",");
    if (lstrlenW(outl) + len < cap) lstrcatW(outl, item);
}

/* list without name (comma-separated) */
static void remove_from_list(const WCHAR *list, const WCHAR *name, WCHAR *outl, int cap)
{
    const WCHAR *p = list;
    outl[0] = 0;
    while (*p) {
        const WCHAR *e = p;
        WCHAR item[256];
        int len;
        while (*e && *e != L',') e++;
        len = (int)(e - p);
        if (len < 256) {
            lstrcpynW(item, p, len + 1);
            if (!in_list(item, name) && item[0]) append_item(outl, item, len, cap);
        }
        p = *e ? e + 1 : e;
    }
}

static BOOL filter_register(WCHAR *dll)
{
    IMSAdminBaseW *mb = metabase();
    METADATA_HANDLE h;
    METADATA_RECORD r;
    WCHAR order[1024];
    WCHAR neworder[1100];
    DWORD need = 0;
    HRESULT hr;
    BOOL result = FALSE;

    if (!mb) return FALSE;
    hr = mb->lpVtbl->OpenKey(mb, METADATA_MASTER_ROOT_HANDLE, FILTERS_KEY,
                             METADATA_PERMISSION_READ | METADATA_PERMISSION_WRITE, 30000, &h);
    if (FAILED(hr)) { fail("can't open /LM/W3SVC/Filters in the metabase", (DWORD)hr); goto out; }

    order[0] = 0;
    memset(&r, 0, sizeof r);
    r.dwMDIdentifier = MD_FILTER_LOAD_ORDER;
    r.dwMDUserType = IIS_MD_UT_SERVER;
    r.dwMDDataType = STRING_METADATA;
    r.dwMDDataLen = sizeof order;
    r.pbMDData = (unsigned char *)order;
    mb->lpVtbl->GetData(mb, h, L"", &r, &need);       /* missing = empty list */
    order[1023] = 0;

    hr = mb->lpVtbl->AddKey(mb, h, FILTER_NAME);
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        fail("can't add the urlmagic4 filter key", (DWORD)hr);
        goto close;
    }
    rec_string(&r, MD_KEY_TYPE, IIS_MD_UT_SERVER, L"IIsFilter");
    hr = mb->lpVtbl->SetData(mb, h, FILTER_NAME, &r);
    if (SUCCEEDED(hr)) {
        rec_string(&r, MD_FILTER_IMAGE_PATH, IIS_MD_UT_SERVER, dll);
        hr = mb->lpVtbl->SetData(mb, h, FILTER_NAME, &r);
    }
    if (SUCCEEDED(hr) && !in_list(order, FILTER_NAME)) {
        lstrcpynW(neworder, order, 1024);
        if (neworder[0]) lstrcatW(neworder, L",");
        lstrcatW(neworder, FILTER_NAME);
        rec_string(&r, MD_FILTER_LOAD_ORDER, IIS_MD_UT_SERVER, neworder);
        hr = mb->lpVtbl->SetData(mb, h, L"", &r);
    }
    if (FAILED(hr)) { fail("can't write the filter settings", (DWORD)hr); goto close; }
    result = TRUE;
close:
    mb->lpVtbl->CloseKey(mb, h);
    if (result) mb->lpVtbl->SaveData(mb);
out:
    mb->lpVtbl->Release(mb);
    return result;
}

static BOOL filter_unregister(void)
{
    IMSAdminBaseW *mb = metabase();
    METADATA_HANDLE h;
    METADATA_RECORD r;
    WCHAR order[1024];
    WCHAR neworder[1024];
    DWORD need = 0;
    HRESULT hr;

    if (!mb) return FALSE;
    hr = mb->lpVtbl->OpenKey(mb, METADATA_MASTER_ROOT_HANDLE, FILTERS_KEY,
                             METADATA_PERMISSION_READ | METADATA_PERMISSION_WRITE, 30000, &h);
    if (FAILED(hr)) { fail("can't open /LM/W3SVC/Filters in the metabase", (DWORD)hr); mb->lpVtbl->Release(mb); return FALSE; }
    order[0] = 0;
    memset(&r, 0, sizeof r);
    r.dwMDIdentifier = MD_FILTER_LOAD_ORDER;
    r.dwMDUserType = IIS_MD_UT_SERVER;
    r.dwMDDataType = STRING_METADATA;
    r.dwMDDataLen = sizeof order;
    r.pbMDData = (unsigned char *)order;
    if (SUCCEEDED(mb->lpVtbl->GetData(mb, h, L"", &r, &need)) && in_list(order, FILTER_NAME)) {
        remove_from_list(order, FILTER_NAME, neworder, 1024);
        rec_string(&r, MD_FILTER_LOAD_ORDER, IIS_MD_UT_SERVER, neworder);
        mb->lpVtbl->SetData(mb, h, L"", &r);
    }
    mb->lpVtbl->DeleteKey(mb, h, FILTER_NAME);         /* fine if it's already gone */
    mb->lpVtbl->CloseKey(mb, h);
    mb->lpVtbl->SaveData(mb);
    mb->lpVtbl->Release(mb);
    return TRUE;
}

/* ---------------------------------------------------------------- the tab (COM registration) */

static BOOL tab_register(const WCHAR *dll, BOOL reg)
{
    typedef HRESULT (WINAPI *regfn)(void);
    HMODULE m = LoadLibraryW(dll);
    regfn fn;
    HRESULT hr;
    if (!m) {
        if (reg) fail("can't load urlmagic4-mmc-snap-in.dll", GetLastError());
        return !reg;
    }
    fn = (regfn)GetProcAddress(m, reg ? "DllRegisterServer" : "DllUnregisterServer");
    hr = fn ? fn() : E_FAIL;
    FreeLibrary(m);
    if (FAILED(hr)) { fail(reg ? "DllRegisterServer failed" : "DllUnregisterServer failed", (DWORD)hr); return FALSE; }
    return TRUE;
}

/* ---------------------------------------------------------------- install / uninstall */

static void dir_of_exe(WCHAR *dir)
{
    int i = (int)GetModuleFileNameW(NULL, dir, MAX_PATH);
    while (i > 0 && dir[i - 1] != L'\\') i--;
    dir[i] = 0;                                 /* keeps the trailing backslash */
}

static void default_dir(WCHAR *dir)
{
    WCHAR drive[8];
    if (!GetEnvironmentVariableW(L"SystemDrive", drive, 8)) lstrcpyW(drive, L"C:");
    wsprintfW(dir, L"%s\\URLMagic4", drive);
}

static void join(WCHAR *outp, const WCHAR *dir, const WCHAR *file)
{
    int n;
    lstrcpynW(outp, dir, MAX_PATH);
    n = lstrlenW(outp);
    if (n && outp[n - 1] != L'\\') lstrcatW(outp, L"\\");
    lstrcatW(outp, file);
}

/* The install folder recorded at install time (for uninstall). */
static BOOL installed_dir(WCHAR *dir)
{
    HKEY k;
    DWORD type;
    DWORD size = MAX_PATH * sizeof(WCHAR);
    BOOL found = FALSE;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, UM_REG_ROOT, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        found = RegQueryValueExW(k, L"InstallDir", NULL, &type, (BYTE *)dir, &size) == ERROR_SUCCESS &&
                type == REG_SZ;
        RegCloseKey(k);
    }
    return found;
}

#define ARP_KEY L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\URLMagic4"

static void set_sz(HKEY k, const WCHAR *name, const WCHAR *v)
{
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v, (lstrlenW(v) + 1) * sizeof(WCHAR));
}

/* Control Panel > Add/Remove Programs: runs the copy of this installer with /remove */
static void arp_register(const WCHAR *dir)
{
    HKEY k;
    DWORD disp;
    DWORD one = 1;
    WCHAR exe[MAX_PATH];
    WCHAR cmd[MAX_PATH + 20];
    join(exe, dir, g_self);
    wsprintfW(cmd, L"\"%s\" /remove", exe);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, ARP_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, &disp) != ERROR_SUCCESS) return;
    set_sz(k, L"DisplayName", L"URL Magic " UM_VERSION_WSTR L" (urlmagic4)");
    set_sz(k, L"DisplayVersion", UM_VERSION_WSTR);
    set_sz(k, L"Publisher", L"urlmagic4");
    set_sz(k, L"URLInfoAbout", L"https://github.com/muncherelli/urlmagic4");
    set_sz(k, L"InstallLocation", dir);
    set_sz(k, L"DisplayIcon", exe);
    set_sz(k, L"UninstallString", cmd);
    RegSetValueExW(k, L"NoModify", 0, REG_DWORD, (const BYTE *)&one, sizeof one);
    RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, (const BYTE *)&one, sizeof one);
    RegCloseKey(k);
}

/* ---------------------------------------------------------------- permissions */

/* NT 4.0 gives Everyone full control of C:\ and lets Everyone write under HKLM\SOFTWARE, and
   new folders and keys take that on. Anyone who can run code on the server (a web site's own
   account, say) could then swap urlmagic4.dll, which IIS loads as SYSTEM, or turn a site's
   rewriting on and point it at any file. So what setup makes gets its own permissions:
   Administrators and SYSTEM may change it, everyone else may only read it. */
enum { LOCK_FILE, LOCK_DIR, LOCK_KEY };

static BOOL lock_sd(int kind, DWORD *aclbuf, DWORD size, SECURITY_DESCRIPTOR *sd)
{
    SID_IDENTIFIER_AUTHORITY nt = {SECURITY_NT_AUTHORITY};
    SID_IDENTIFIER_AUTHORITY world = {SECURITY_WORLD_SID_AUTHORITY};
    PSID admins = NULL;
    PSID sys = NULL;
    PSID everyone = NULL;
    PACL acl = (PACL)aclbuf;
    DWORD full = kind == LOCK_KEY ? KEY_ALL_ACCESS : FILE_ALL_ACCESS;
    DWORD read = kind == LOCK_KEY ? KEY_READ : FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
    BYTE inherit = 0;
    BOOL ok;
    if (kind == LOCK_DIR) inherit = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
    else if (kind == LOCK_KEY) inherit = CONTAINER_INHERIT_ACE;
    ok =AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                       0, 0, 0, 0, 0, 0, &admins) &&
              AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &sys) &&
              AllocateAndInitializeSid(&world, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &everyone) &&
              InitializeAcl(acl, size, ACL_REVISION) &&
              AddAccessAllowedAce(acl, ACL_REVISION, full, admins) &&
              AddAccessAllowedAce(acl, ACL_REVISION, full, sys) &&
              AddAccessAllowedAce(acl, ACL_REVISION, read, everyone) &&
              InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION) &&
              SetSecurityDescriptorDacl(sd, TRUE, acl, FALSE);
    if (ok && inherit) {
        /* NT 4.0 has no AddAccessAllowedAceEx: set the inheritance flags afterwards */
        ACE_HEADER *h;
        for (DWORD i = 0; i < acl->AceCount; i++)
            if (GetAce(acl, i, (LPVOID *)&h)) h->AceFlags = inherit;
    }
    if (admins) FreeSid(admins);
    if (sys) FreeSid(sys);
    if (everyone) FreeSid(everyone);
    return ok;
}

/* a file or folder of ours: LOCK_FILE or LOCK_DIR */
static BOOL lock_path(const WCHAR *path, int kind)
{
    DWORD aclbuf[64];
    SECURITY_DESCRIPTOR sd;
    return lock_sd(kind, aclbuf, sizeof aclbuf, &sd) &&
           SetFileSecurityW(path, DACL_SECURITY_INFORMATION, &sd);
}

/* a registry key of ours, and every key under it */
static void lock_key(HKEY root, const WCHAR *path)
{
    DWORD aclbuf[64];
    SECURITY_DESCRIPTOR sd;
    WCHAR sub[256];
    WCHAR full[600];
    HKEY k;
    DWORD i = 0;
    DWORD n = 256;
    if (!lock_sd(LOCK_KEY, aclbuf, sizeof aclbuf, &sd)) return;
    if (RegOpenKeyExW(root, path, 0, WRITE_DAC | KEY_ENUMERATE_SUB_KEYS, &k) != ERROR_SUCCESS) return;
    RegSetKeySecurity(k, DACL_SECURITY_INFORMATION, &sd);
    while (RegEnumKeyExW(k, i, sub, &n, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        if (lstrlenW(path) + lstrlenW(sub) + 2 <= 600) {
            wsprintfW(full, L"%s\\%s", path, sub);
            lock_key(root, full);
        }
        i++;
        n = 256;
    }
    RegCloseKey(k);
}

static void record_dir(const WCHAR *dir)
{
    HKEY k;
    DWORD disp;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, UM_REG_ROOT, 0, NULL, 0, KEY_WRITE, NULL, &k, &disp) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"InstallDir", 0, REG_SZ, (const BYTE *)dir, (lstrlenW(dir) + 1) * sizeof(WCHAR));
        RegSetValueExW(k, L"Version", 0, REG_SZ, (const BYTE *)UM_VERSION_WSTR, sizeof(UM_VERSION_WSTR));
        RegSetValueExW(k, L"Setup", 0, REG_SZ, (const BYTE *)g_self, (lstrlenW(g_self) + 1) * sizeof(WCHAR));
        RegCloseKey(k);
    }
    arp_register(dir);
    /* the sites' settings live under this key too (the tab's new keys take these permissions on) */
    lock_key(HKEY_LOCAL_MACHINE, UM_REG_ROOT);
    lock_key(HKEY_LOCAL_MACHINE, ARP_KEY);
}

/* ---------------------------------------------------------------- talking to the user */

/* first non-blank character of a line typed by the user, lowercased; 0 for just Enter */
static int ask(const char *question)
{
    char line[64];
    DWORD n = 0;
    color(C_YELLOW);
    out("  ");
    out(question);
    color(C_WHITE);
    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), line, sizeof line - 1, &n, NULL)) return 0;
    for (DWORD i = 0; i < n; i++) {
        char c = line[i];
        if (c == '\r' || c == '\n') break;
        if (c != ' ' && c != '\t') return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    return 0;
}

static void nothing_changed(void)
{
    color(C_DIM);
    out("  Nothing changed.\r\n");
    color(C_GREY);
}

/* Warn that the web sites go down briefly; TRUE if the user says go ahead. */
static BOOL confirm_restart(const char *what)
{
    int c;
    color(C_YELLOW);
    out("  ! ");
    color(C_WHITE);
    out("This stops the WWW service, so every web site on this server is down for a\r\n"
        "    few seconds while urlmagic ");
    out(what);
    out(", then starts it again.\r\n\r\n");
    c = ask("Continue? [Y/n] ");
    out("\r\n");
    if (c == 0 || c == 'y') return TRUE;
    nothing_changed();
    return FALSE;
}

static void wait_to_close(void)
{
    char line[16];
    DWORD n;
    color(C_DIM);
    out("\r\n  Press Enter to close this window.");
    color(C_GREY);
    ReadFile(GetStdHandle(STD_INPUT_HANDLE), line, sizeof line, &n, NULL);
}

/* ---------------------------------------------------------------- is this NT 4.0 with IIS 4.0? */

static void os_version(OSVERSIONINFOA *v)
{
    memset(v, 0, sizeof *v);
    v->dwOSVersionInfoSize = sizeof *v;
    GetVersionExA(v);   /* NOSONAR: its replacement, VerifyVersionInfo, needs Windows 2000 */
}

static BOOL check_system(void)
{
    OSVERSIONINFOA v;
    HKEY k;
    DWORD major = 0;
    DWORD minor = 0;
    DWORD size;
    DWORD type;
    char line[200];

    section("Checking the system");
    os_version(&v);
    step("Windows");
    if (v.dwPlatformId != VER_PLATFORM_WIN32_NT) {
        fail("this is Windows 95/98/Me; urlmagic needs Windows NT 4.0 with IIS 4.0", 0);
        return FALSE;
    }
    wsprintfA(line, "NT %lu.%lu%s%s", v.dwMajorVersion, v.dwMinorVersion,
              v.szCSDVersion[0] ? " " : "", v.szCSDVersion);
    value(line, v.dwMajorVersion == 4);
    if (v.dwMajorVersion != 4) note("Not NT 4.0: untested, carrying on.");

    step("IIS");
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\InetStp", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        size = sizeof major;
        RegQueryValueExW(k, L"MajorVersion", NULL, &type, (BYTE *)&major, &size);
        size = sizeof minor;
        RegQueryValueExW(k, L"MinorVersion", NULL, &type, (BYTE *)&minor, &size);
        RegCloseKey(k);
    }
    if (!major) {
        fail("IIS isn't installed (install it from the NT 4.0 Option Pack)", 0);
        return FALSE;
    }
    wsprintfA(line, "%lu.%lu", major, minor);
    value(line, major == 4);
    if (major != 4) note("Not IIS 4.0: untested, carrying on.");
    out("\r\n");
    return TRUE;
}

/* Write one embedded file to path; returns 0 or a Win32 error code. */
static DWORD extract(int id, const WCHAR *path)
{
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
    const void *data = g ? LockResource(g) : NULL;
    DWORD size;
    DWORD w;
    DWORD err = 0;
    HANDLE f;

    if (!data) return ERROR_RESOURCE_DATA_NOT_FOUND;
    size = SizeofResource(NULL, r);
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return GetLastError();
    if (!WriteFile(f, data, size, &w, NULL) || w != size) err = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
    CloseHandle(f);
    if (err) DeleteFileW(path);
    return err;
}

/* IIS can hold the old filter DLL for a while after the service says it has stopped:
   keep trying for up to a minute */
static DWORD extract_retry(int id, const WCHAR *path)
{
    DWORD e = extract(id, path);
    for (int tries = 0; e == ERROR_SHARING_VIOLATION && tries < 120; tries++) {
        spin_wait();
        e = extract(id, path);
    }
    return e;
}

/* The install folder, made if need be. One we made (now or at an earlier install) gets our
   permissions; one that was already there for something else keeps its own. */
enum { DIR_FOREIGN, DIR_LOCKED, DIR_UNLOCKED };

static int make_dir(const WCHAR *dir)
{
    WCHAR was[MAX_PATH];
    if (!CreateDirectoryW(dir, NULL) && !(installed_dir(was) && lstrcmpiW(was, dir) == 0))
        return DIR_FOREIGN;
    return lock_path(dir, LOCK_DIR) ? DIR_LOCKED : DIR_UNLOCKED;
}

/* our files in dir: a rewritten file keeps the permissions it had, so each one gets ours */
static void lock_files(const WCHAR *dir)
{
    static const WCHAR *LOGS[] = { L"urlmagic4.log", L"urlmagic4.log.old", NULL };
    WCHAR f[MAX_PATH];
    join(f, dir, g_self);
    lock_path(f, LOCK_FILE);
    for (int i = 0; FILES[i]; i++) { join(f, dir, FILES[i]); lock_path(f, LOCK_FILE); }
    for (int i = 0; LOGS[i]; i++) { join(f, dir, LOGS[i]); lock_path(f, LOCK_FILE); }
}

static void dir_note(int state)
{
    if (state == DIR_FOREIGN)
        note("The install folder's own permissions were left as they were: make sure only administrators can change files in it.");
    else if (state == DIR_UNLOCKED)
        note("Couldn't restrict who may change the install folder (a FAT drive?): only administrators should be able to.");
}

static int install(const WCHAR *dir)
{
    WCHAR src[MAX_PATH];
    WCHAR from[MAX_PATH];
    WCHAR to[MAX_PATH];
    BOOL running = FALSE;
    int dirstate;

    dir_of_exe(src);
    {
        char title[MAX_PATH + 40];
        char d[MAX_PATH];
        WideCharToMultiByte(CP_OEMCP, 0, dir, -1, d, MAX_PATH, NULL, NULL);
        wsprintfA(title, "Installing to %s", d);
        section(title);
    }

    step("Stopping the WWW service");
    if (!www(FALSE, &running)) return 1;
    ok();

    step("Unpacking files");
    dirstate = make_dir(dir);
    for (int i = 0; FILES[i]; i++) {
        DWORD e;
        join(to, dir, FILES[i]);
        e = extract_retry(FILE_IDS[i], to);
        if (e != 0) {
            char path[MAX_PATH];
            fail("can't write a file", e);
            WideCharToMultiByte(CP_OEMCP, 0, to, -1, path, MAX_PATH, NULL, NULL);
            msg(path, C_RED);
            if (e == ERROR_SHARING_VIOLATION) note("It's still in use after a minute: close Internet Service Manager and run setup again.");
            if (running) www(TRUE, NULL);
            return 1;
        }
    }
    /* a copy of this program, for uninstalling later */
    join(from, src, g_self);
    join(to, dir, g_self);
    if (lstrcmpiW(from, to) != 0) CopyFileW(from, to, FALSE);
    lock_files(dir);
    ok();
    dir_note(dirstate);
    record_dir(dir);

    step("Registering the URL Magic tab");
    join(to, dir, L"urlmagic4-mmc-snap-in.dll");
    if (tab_register(to, TRUE)) ok();
    join(to, dir, L"urlmagic4tab.dll");          /* the tab's name in early 4.0.0 builds */
    DeleteFileW(to);

    step("Adding the urlmagic4 ISAPI filter to IIS");
    join(to, dir, L"urlmagic4.dll");
    if (filter_register(to)) ok();

    step("Starting the WWW service");
    if (www(TRUE, NULL)) ok();

    if (g_tui) return g_errors ? 1 : 0;   /* the result screen is drawn by tui_main */
    out("\r\n");
    if (g_errors) {
        color(C_RED);
        out("  \xFE Setup finished with errors (see above).\r\n");
        color(C_GREY);
        return 1;
    }
    color(C_GREEN);
    out("  \xFE urlmagic " UM_VERSION_STR " is installed. Abracadabra!\r\n\r\n");
    section("Next");
    color(C_WHITE); out("  1. ");
    color(C_GREY);
    out("Internet Service Manager > a web site's Properties > ");
    color(C_WHITE); out("URL Magic"); color(C_GREY); out(".\r\n"
        "     No tab? Enable it once: Console > Add/Remove Snap-in > Extensions,\r\n"
        "     Internet Information Server, tick URL Magic, OK, then Console > Save.\r\n");
    color(C_WHITE); out("  2. ");
    color(C_GREY); out("Put an ");
    color(C_WHITE); out("httpd.htaccess"); color(C_GREY); out(" in a web site's home directory, e.g.\r\n\r\n");
    color(C_CYAN);
    out("       RewriteEngine On\r\n"
        "       RewriteCond %{REQUEST_FILENAME} !-f\r\n"
        "       RewriteCond %{REQUEST_FILENAME} !-d\r\n"
        "       RewriteRule ^(.*)$ index.cfm?page=$1 [L,QSA]\r\n\r\n");
    color(C_DIM);
    out("  Problems with rules are logged to ");
    join(to, dir, L"urlmagic4.log");
    outw(to);
    color(C_GREY);
    out("\r\n");
    return 0;
}

static void delete_tree(HKEY root, const WCHAR *path)
{
    HKEY k;
    WCHAR sub[256];
    WCHAR full[600];
    if (RegOpenKeyExW(root, path, 0, KEY_ALL_ACCESS, &k) == ERROR_SUCCESS) {
        for (;;) {
            DWORD n = 256;
            if (RegEnumKeyExW(k, 0, sub, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            wsprintfW(full, L"%s\\%s", path, sub);
            delete_tree(root, full);
        }
        RegCloseKey(k);
    }
    RegDeleteKeyW(root, path);
}

/* delete f, or if it's in use, at the next reboot; TRUE for the latter */
static BOOL delete_or_later(const WCHAR *f)
{
    if (!DeleteFileW(f) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        MoveFileExW(f, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        return TRUE;
    }
    return FALSE;
}

/* the installer copy (maybe this very program, which can't delete itself while running);
   TRUE if it goes at the next reboot */
static BOOL delete_setup_copy(const WCHAR *dir)
{
    WCHAR setup[MAX_PATH];
    WCHAR here[MAX_PATH];
    WCHAR f[MAX_PATH];
    HKEY k;
    DWORD type;
    DWORD size = sizeof setup;
    setup[0] = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, UM_REG_ROOT, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"Setup", NULL, &type, (BYTE *)setup, &size) != ERROR_SUCCESS || type != REG_SZ)
            setup[0] = 0;
        RegCloseKey(k);
    }
    if (!setup[0]) return FALSE;
    join(f, dir, setup);
    GetModuleFileNameW(NULL, here, MAX_PATH);
    if (lstrcmpiW(f, here) == 0) {
        MoveFileExW(f, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        return TRUE;
    }
    return delete_or_later(f);
}

/* everything setup put in dir, and dir itself; TRUE if some of it goes at the next reboot */
static BOOL delete_files(const WCHAR *dir)
{
    /* files as named by early 4.0.0 builds, and the old log */
    static const WCHAR *OLD[] = { L"README.md", L"CHANGELOG.md", L"LICENSE", L"urlmagic4.log.old", NULL };
    WCHAR f[MAX_PATH];
    BOOL reboot = FALSE;
    for (int i = 0; FILES[i]; i++) {
        join(f, dir, FILES[i]);
        if (delete_or_later(f)) reboot = TRUE;
    }
    if (delete_setup_copy(dir)) reboot = TRUE;
    join(f, dir, L"urlmagic4.log");
    DeleteFileW(f);
    join(f, dir, L"urlmagic4tab.dll");
    if (delete_or_later(f)) reboot = TRUE;
    for (int i = 0; OLD[i]; i++) {
        join(f, dir, OLD[i]);
        DeleteFileW(f);
    }
    if (!RemoveDirectoryW(dir) && reboot) MoveFileExW(dir, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    return reboot;
}

/* uninstall without /purge: keep the sites' settings, forget the install */
static void forget_install(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, UM_REG_ROOT, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, L"InstallDir");
        RegDeleteValueW(k, L"Version");
        RegDeleteValueW(k, L"Setup");
        RegCloseKey(k);
    }
}

static int uninstall(BOOL purge)
{
    WCHAR dir[MAX_PATH];
    WCHAR f[MAX_PATH];
    BOOL running = FALSE;
    BOOL reboot;

    if (!installed_dir(dir)) default_dir(dir);
    {
        char title[MAX_PATH + 40];
        char d[MAX_PATH];
        WideCharToMultiByte(CP_OEMCP, 0, dir, -1, d, MAX_PATH, NULL, NULL);
        wsprintfA(title, "Removing from %s", d);
        section(title);
    }

    step("Stopping the WWW service");
    if (!www(FALSE, &running)) return 1;
    ok();

    RegDeleteKeyW(HKEY_LOCAL_MACHINE, ARP_KEY);          /* out of Add/Remove Programs */

    step("Removing the urlmagic4 ISAPI filter from IIS");
    if (filter_unregister()) ok();

    step("Unregistering the URL Magic tab");
    join(f, dir, L"urlmagic4-mmc-snap-in.dll");
    if (GetFileAttributesW(f) == 0xFFFFFFFF) join(f, dir, L"urlmagic4tab.dll");   /* early 4.0.0 builds */
    if (tab_register(f, FALSE)) ok();

    step("Deleting files");
    reboot = delete_files(dir);
    ok();
    if (reboot) note("Some files were in use; they go at the next reboot.");

    if (purge) {
        step("Deleting every site's urlmagic4 settings");
        delete_tree(HKEY_LOCAL_MACHINE, UM_REG_ROOT);
        ok();
    } else {
        forget_install();
    }

    if (running) {
        step("Starting the WWW service");
        if (www(TRUE, NULL)) ok();
    }
    if (g_tui) return g_errors ? 1 : 0;
    out("\r\n");
    color(g_errors ? C_RED : C_GREEN);
    out(g_errors ? "  \xFE Finished with errors (see above).\r\n" : "  \xFE urlmagic is removed. Poof!\r\n");
    color(C_DIM);
    if (!purge) { out("  Site settings were kept; `"); outw(g_self); out(" uninstall /purge` removes them too.\r\n"); }
    color(C_GREY);
    return g_errors ? 1 : 0;
}

static void usage(void)
{
    out("urlmagic " UM_VERSION_STR " setup\r\n\r\n  ");
    outw(g_self); out("                    ask what to do\r\n  ");
    outw(g_self); out(" install [folder]   install or upgrade (default %SystemDrive%\\URLMagic4)\r\n  ");
    outw(g_self); out(" uninstall          remove urlmagic4 (keeps site settings)\r\n  ");
    outw(g_self); out(" uninstall /purge   remove urlmagic4 and every site's settings\r\n");
}

/* ---------------------------------------------------------------- entry point (no CRT) */

/* ---------------------------------------------------------------- full-screen flow */

typedef struct {
    BOOL nt;
    DWORD major;
    DWORD minor;
    DWORD build;
    int sp;
    BOOL sp6a;
    DWORD iis_major;
    DWORD iis_minor;
    BOOL admin;
    BOOL installed;
    WCHAR dir[MAX_PATH];
    WCHAR ver[32];
} sysinfo;

/* the service pack number in e.g. "Service Pack 6", 0 for none */
static int service_pack(const char *csd)
{
    for (const char *p = csd; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            int sp = *p - '0';
            if (p[1] >= '0' && p[1] <= '9') sp = sp * 10 + (p[1] - '0');
            return sp;
        }
    }
    return 0;
}

static void get_sysinfo(sysinfo *si)
{
    OSVERSIONINFOA v;
    HKEY k;
    DWORD size;
    DWORD type;
    SC_HANDLE scm;

    memset(si, 0, sizeof *si);
    os_version(&v);
    si->nt = v.dwPlatformId == VER_PLATFORM_WIN32_NT;
    si->major = v.dwMajorVersion;
    si->minor = v.dwMinorVersion;
    si->build = v.dwBuildNumber & 0xFFFF;
    si->sp = service_pack(v.szCSDVersion);
    /* Service Pack 6a = SP6 plus hotfix Q246009 */
    if (si->sp == 6 && RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Hotfix\\Q246009", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        si->sp6a = TRUE;
        RegCloseKey(k);
    }
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\InetStp", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        size = sizeof si->iis_major;
        RegQueryValueExW(k, L"MajorVersion", NULL, &type, (BYTE *)&si->iis_major, &size);
        size = sizeof si->iis_minor;
        RegQueryValueExW(k, L"MinorVersion", NULL, &type, (BYTE *)&si->iis_minor, &size);
        RegCloseKey(k);
    }
    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);   /* only administrators can */
    si->admin = scm != NULL;
    if (scm) CloseServiceHandle(scm);
    si->installed = installed_dir(si->dir);
    if (si->installed && RegOpenKeyExW(HKEY_LOCAL_MACHINE, UM_REG_ROOT, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        size = sizeof si->ver;
        if (RegQueryValueExW(k, L"Version", NULL, &type, (BYTE *)si->ver, &size) != ERROR_SUCCESS) si->ver[0] = 0;
        RegCloseKey(k);
    }
}

enum { OK_ = 0, WARN_ = 1, FAIL_ = 2 };

/* compare "a.b.c" version strings: <0, 0, >0 */
static int vercmp(const WCHAR *a, const WCHAR *b)
{
    for (int i = 0; i < 3; i++) {
        long x = 0;
        long y = 0;
        while (*a >= L'0' && *a <= L'9') x = x * 10 + (*a++ - L'0');
        while (*b >= L'0' && *b <= L'9') y = y * 10 + (*b++ - L'0');
        if (x != y) return x < y ? -1 : 1;
        if (*a == L'.') a++;
        if (*b == L'.') b++;
    }
    return 0;
}

/* one line of the system check: label, value, result tag */
static void check_line(int y, const char *label, const char *val, int level)
{
    put(STEP_X + 2, y, A_TEXT, label);
    put(STEP_X + 18, y, A_BRIGHT, val);
    if (level == OK_) tui_tag(TAG_X, y, " OK ", C_GREEN);
    else if (level == WARN_) tui_tag(TAG_X, y, "WARN", C_YELLOW);
    else if (level == FAIL_) tui_tag(TAG_X, y, "FAIL", C_RED);
}

#define CHECK_ROW 12                         /* "System check" and its four lines */
#define MSG_ROW   18                         /* install folder / message, warning, advice */
#define FIELD_X   (STEP_X + 13)
#define FIELD_W   (TAG_X + 6 - FIELD_X)      /* the folder field ends where the tags end */
#define A_FIELD   ((WORD)0x70)               /* black on grey, like a text box */

static void cursor(BOOL on, int x, int y)
{
    CONSOLE_CURSOR_INFO cur;
    COORD c;
    cur.dwSize = 25;
    cur.bVisible = on;
    SetConsoleCursorInfo(g_out, &cur);
    if (on) { c.X = (SHORT)x; c.Y = (SHORT)y; SetConsoleCursorPosition(g_out, c); }
}

/* the install-folder text box; shows the end of the path if it's longer than the box */
static void draw_field(const char *path)
{
    int n = lstrlenA(path);
    int shown = n > FIELD_W - 1 ? FIELD_W - 1 : n;
    fill(FIELD_X, MSG_ROW, A_FIELD, ' ', FIELD_W);
    put(FIELD_X, MSG_ROW, A_FIELD, path + n - shown);
    cursor(TRUE, FIELD_X + shown, MSG_ROW);
}

/* a usable install folder: a full path on a local hard disk. NULL if fine, else why not. */
static const char *check_dir(const char *path, WCHAR *out)
{
    WCHAR w[MAX_PATH];
    WCHAR root[4];
    int n;
    if (!(((path[0] | 32) >= 'a' && (path[0] | 32) <= 'z') && path[1] == ':' && path[2] == '\\'))
        return "Type a full path, like C:\\urlmagic4.";
    MultiByteToWideChar(CP_OEMCP, 0, path, -1, w, MAX_PATH);
    if (!GetFullPathNameW(w, MAX_PATH, out, NULL)) return "That isn't a valid folder name.";
    n = lstrlenW(out);
    while (n > 3 && out[n - 1] == L'\\') out[--n] = 0;
    if (n <= 3) return "Choose a folder, not the root of a drive.";
    root[0] = out[0]; root[1] = L':'; root[2] = L'\\'; root[3] = 0;
    if (GetDriveTypeW(root) != DRIVE_FIXED)
        return "Choose a folder on a local hard disk (IIS loads the filter from there).";
    return NULL;
}

/* the icon, logo and repo link at the top of the splash screen */
static void splash_logo(void)
{
    /* repo link a line below the letters, right-aligned to them (they end at column 71) */
    static const char url[] = "github.com/muncherelli/urlmagic4";
    tui_frame();
    tui_icon(7, 2);                          /* icon 16 + 3 + logo 46, visually centred */
    tui_logo(7 + 16 + 3, 4);
    put(7 + 16 + 3 + LOGO_W - lstrlenA(url), 10, ON_BLUE(C_CYAN), url);
}

/* NT 4.0's service pack: IIS 4.0 needs SP3, SP6a is recommended */
static int check_sp(int sp, const char **problem, const char **advice)
{
    if (sp < 3) {
        *problem = "IIS 4.0 needs NT 4.0 Service Pack 3 or later (6a recommended).";
        return FAIL_;
    }
    if (sp < 6) {
        if (!*advice) *advice = "Service Pack 6a is recommended.";
        return WARN_;
    }
    return OK_;
}

/* the system check's Windows line: Windows and its service pack */
static void check_windows(const sysinfo *si, const char **problem, const char **advice)
{
    char v[MAX_PATH + 40];
    char sp[24];
    int lw;
    int ls = OK_;
    if (!si->nt) {
        check_line(CHECK_ROW + 1, "Windows", "Windows 95/98/Me", FAIL_);
        *problem = "urlmagic needs Windows NT 4.0.";
        return;
    }
    if (si->sp) wsprintfA(sp, ", Service Pack %d%s", si->sp, si->sp6a ? "a" : "");
    else lstrcpyA(sp, ", no service pack");
    wsprintfA(v, "Windows NT %lu.%lu%s (build %lu)", si->major, si->minor, sp, si->build);
    lw = si->major == 4 ? OK_ : WARN_;
    if (lw == WARN_) *advice = "Not Windows NT 4.0: urlmagic is untested here.";
    if (si->major == 4) ls = check_sp(si->sp, problem, advice);
    if (ls > lw) lw = ls;
    check_line(CHECK_ROW + 1, "Windows", v, lw);
}

/* the system check's web server line */
static void check_iis(const sysinfo *si, const char **problem, const char **advice)
{
    char v[MAX_PATH + 40];
    int li;
    if (!si->iis_major) {
        li = FAIL_;
        lstrcpyA(v, "Not installed");
        if (!*problem) *problem = "Install IIS 4.0 from the NT 4.0 Option Pack first.";
    } else {
        wsprintfA(v, "IIS %lu.%lu", si->iis_major, si->iis_minor);
        li = si->iis_major == 4 ? OK_ : WARN_;
        if (li == WARN_ && !*advice) *advice = "Not IIS 4.0: urlmagic is untested here.";
    }
    check_line(CHECK_ROW + 2, "Web server", v, li);
}

/* the system check's urlmagic line; if installed, its folder goes in dir and d (OEM) */
static void check_installed(const sysinfo *si, WCHAR *dir, char *d)
{
    char v[MAX_PATH + 40];
    if (si->installed) {
        WideCharToMultiByte(CP_OEMCP, 0, si->dir, -1, d, MAX_PATH, NULL, NULL);
        WideCharToMultiByte(CP_OEMCP, 0, si->ver[0] ? si->ver : L"?", -1, v, 32, NULL, NULL);
        lstrcatA(v, " in ");
        lstrcatA(v, d);
        lstrcpynW(dir, si->dir, MAX_PATH);
    } else {
        lstrcpyA(v, "Not installed");
    }
    check_line(CHECK_ROW + 4, "urlmagic", v, -1);
}

/* why setup can't go on; waits for Esc and returns 'q' */
static int splash_problem(const char *problem)
{
    int k;
    put(STEP_X, MSG_ROW, ON_BLUE(C_RED), "Setup can't continue:");
    put(STEP_X, MSG_ROW + 1, A_BRIGHT, problem);
    keybar("ESC/CTRL+C=Quit");
    do {
        k = key();
    } while (k != 27 && k != 3);
    return 'q';
}

/* let the user edit path in the text box; FALSE if they quit */
static BOOL edit_field(char *path)
{
    draw_field(path);
    for (;;) {
        int k = key();
        int n = lstrlenA(path);
        if (k == 27 || k == 3) return FALSE;
        if (k == 13) return TRUE;
        if (k == 8) {
            if (n) path[n - 1] = 0;
        } else if (k >= ' ' && n < MAX_PATH - 1) {
            path[n] = g_rawch;              /* as typed, capitals included */
            path[n + 1] = 0;
        }
        draw_field(path);
    }
}

/* a new install: the folder is typed in a text box. 'i' (install into dir) or 'q' */
static int ask_folder(const char *advice, const char *quit, WCHAR *dir)
{
    char path[MAX_PATH];
    char keys[100];
    WCHAR dflt[MAX_PATH];
    const char *err = NULL;
    default_dir(dflt);
    WideCharToMultiByte(CP_OEMCP, 0, dflt, -1, path, MAX_PATH, NULL, NULL);
    for (;;) {
        BOOL go;
        clear_rows(MSG_ROW, SCR_H - 2);
        put(STEP_X, MSG_ROW, A_BRIGHT, "Install to");
        put(STEP_X, MSG_ROW + 2, ON_BLUE(C_YELLOW),
            "Web sites go offline for a few seconds while the WWW service restarts.");
        if (err) put(STEP_X, MSG_ROW + 3, ON_BLUE(C_RED), err);
        else if (advice) put(STEP_X, MSG_ROW + 3, ON_BLUE(C_YELLOW), advice);
        wsprintfA(keys, "ENTER=Install   %s", quit);
        keybar(keys);
        go = edit_field(path);
        cursor(FALSE, 0, 0);
        if (!go) return 'q';
        err = check_dir(path, dir);
        if (!err) return 'i';
    }
}

/* the Enter key's label for an installed version: cmp as in splash */
static const char *cmp_word(int cmp)
{
    if (cmp == 0) return "Reinstall";
    return cmp < 0 ? "Upgrade" : "Downgrade";
}

/* "Setup will upgrade urlmagic ..." for the installed version in folder d */
static void action_text(char *v, const sysinfo *si, int cmp, const char *d)
{
    char have[32];
    WideCharToMultiByte(CP_OEMCP, 0, si->ver[0] ? si->ver : L"?", -1, have, sizeof have, NULL, NULL);
    if (cmp == 0) wsprintfA(v, "Setup will reinstall urlmagic %s in %s.", UM_VERSION_STR, d);
    else if (cmp < 0) wsprintfA(v, "Setup will upgrade urlmagic %s to %s in %s.", have, UM_VERSION_STR, d);
    else wsprintfA(v, "Setup will replace the newer urlmagic %s with %s in %s.", have, UM_VERSION_STR, d);
}

/* also delete the settings? 'u' (with *purge set), 'q', or 0 to go back */
static int ask_purge(BOOL *purge)
{
    clear_rows(MSG_ROW, SCR_H - 2);
    put(STEP_X, MSG_ROW, A_BRIGHT, "Also delete every web site's URL Magic settings?");
    put(STEP_X, MSG_ROW + 1, A_TEXT, "Keep them if you might install urlmagic again.");
    keybar("Y=Delete settings   N=Keep settings   ESC=Back   CTRL+C=Quit");
    for (;;) {
        int k = key();
        if (k == 'y' || k == 'n') { *purge = k == 'y'; return 'u'; }
        if (k == 3) return 'q';
        if (k == 27) return 0;
    }
}

/* the splash screen; returns 'i' (install into dir), 'u' (uninstall) or 'q' */
static int splash(const sysinfo *si, BOOL *purge, BOOL remove, WCHAR *dir)
{
    char v[MAX_PATH + 100];
    char d[MAX_PATH];
    char keys[100];
    const char *problem = NULL;
    const char *advice = NULL;
    const char *quit = remove ? "ESC/CTRL+C=Cancel" : "ESC/CTRL+C=Quit";
    /* installed version vs this installer: <0 upgrade, 0 reinstall, >0 downgrade */
    int cmp = 0;
    if (si->installed) cmp = vercmp(si->ver[0] ? si->ver : L"0", UM_VERSION_WSTR);

    splash_logo();
    put(STEP_X, CHECK_ROW, A_BRIGHT, "System check");
    fill(STEP_X + 13, CHECK_ROW, ON_BLUE(C_DCYAN), (char)0xC4, TAG_X + 6 - (STEP_X + 13));
    check_windows(si, &problem, &advice);
    check_iis(si, &problem, &advice);
    check_line(CHECK_ROW + 3, "Administrator", si->admin ? "Yes" : "No", si->admin ? OK_ : FAIL_);
    if (!si->admin && !problem) problem = "Log on as an Administrator to install urlmagic.";
    check_installed(si, dir, d);

    if (remove && !si->installed && !problem) problem = "urlmagic isn't installed.";
    if (problem) return splash_problem(problem);
    if (!si->installed && !remove) return ask_folder(advice, quit, dir);

    for (;;) {
        int k;
        clear_rows(MSG_ROW, SCR_H - 2);
        if (remove) wsprintfA(v, "Setup will remove urlmagic from %s.", d);
        else action_text(v, si, cmp, d);
        put(STEP_X, MSG_ROW, A_BRIGHT, v);
        put(STEP_X, MSG_ROW + 1, ON_BLUE(C_YELLOW),
            "Web sites go offline for a few seconds while the WWW service restarts.");
        if (advice && !remove) put(STEP_X, MSG_ROW + 2, ON_BLUE(C_YELLOW), advice);

        if (remove) wsprintfA(keys, "ENTER=Uninstall   %s", quit);
        else wsprintfA(keys, "ENTER=%s   U=Uninstall   %s", cmp_word(cmp), quit);
        keybar(keys);
        k = key();
        if (k == 27 || k == 3) return 'q';
        if (k == 13 && !remove) return 'i';
        if (k == 13 || k == 'u') {
            k = ask_purge(purge);
            if (k) return k;
        }
    }
}

static void tui_result(BOOL installing, BOOL purge, const WCHAR *dir)
{
    int y = RES_ROW;
    int i = 0;
    int k;
    char line[MAX_PATH + 60];
    char d[MAX_PATH];

    if (!g_errors) { g_done = g_total; tui_bar(); }
    if (g_errors) {
        put(STEP_X, y++, ON_BLUE(C_RED), installing ? "\xFE Setup ran into problems:" : "\xFE Uninstall ran into problems:");
    } else if (installing) {
        put(STEP_X, y++, ON_BLUE(C_GREEN), "\xFE urlmagic " UM_VERSION_STR " is installed. Abracadabra!");
    } else {
        put(STEP_X, y++, ON_BLUE(C_GREEN), "\xFE urlmagic is removed. Poof!");
    }
    y++;
    while (i < g_nmsgs && y < SCR_H - 3) {
        put(STEP_X + 2, y, ON_BLUE(g_msg_fg[i]), g_msgs[i]);
        y++;
        i++;
    }
    if (!g_errors && installing) {
        put(STEP_X, y, A_BRIGHT, "1.");
        put(STEP_X + 3, y++, A_TEXT, "Internet Service Manager > a web site's Properties > URL Magic.");
        put(STEP_X + 3, y++, A_TEXT, "No tab? Console > Add/Remove Snap-in > Extensions: tick URL Magic.");
        put(STEP_X, y, A_BRIGHT, "2.");
        put(STEP_X + 3, y++, A_TEXT, "Put rules in httpd.htaccess in the site's home directory, e.g.");
        put(STEP_X + 6, y++, ON_BLUE(C_CYAN), "RewriteEngine On");
        put(STEP_X + 6, y++, ON_BLUE(C_CYAN), "RewriteRule ^old-page$ /new-page [R=301,L]");
        WideCharToMultiByte(CP_OEMCP, 0, dir, -1, d, MAX_PATH, NULL, NULL);
        wsprintfA(line, "Rule problems are logged to %s\\urlmagic4.log", d);
        put(STEP_X, SCR_H - 4, A_TEXT, line);   /* two blank rows above the key bar */
    } else if (!g_errors && !purge) {
        put(STEP_X, y++, A_TEXT, "Each web site's URL Magic settings were kept, in case you reinstall.");
    }
    keybar("ENTER=Close");
    do {
        k = key();
    } while (k != 13 && k != 27 && k != 3);
}

static int tui_main(BOOL remove)
{
    sysinfo si;
    BOOL purge = FALSE;
    WCHAR dir[MAX_PATH];
    int action;
    int rc;

    get_sysinfo(&si);
    action = splash(&si, &purge, remove, dir);
    if (action == 'q') return 0;
    if (action == 'i') {
        g_total = 5;
        rc = install(dir);
    } else {
        lstrcpynW(dir, si.dir, MAX_PATH);
        g_total = purge ? 6 : 5;
        rc = uninstall(purge);
    }
    tui_result(action == 'i', purge, dir);
    return rc;
}

/* already installed, double-clicked without the full screen: ask what to do */
static int ask_installed(const WCHAR *have, BOOL remove)
{
    WCHAR ver[32];
    HKEY k;
    DWORD type;
    DWORD size = sizeof ver;
    int c = 'u';                                       /* /remove, from Add/Remove Programs */
    ver[0] = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, UM_REG_ROOT, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        RegQueryValueExW(k, L"Version", NULL, &type, (BYTE *)ver, &size);
        RegCloseKey(k);
    }
    color(C_GREY);
    out("  urlmagic ");
    color(C_WHITE);
    outw(ver[0] ? ver : L"(unknown version)");
    color(C_GREY);
    out(" is installed in ");
    color(C_WHITE);
    outw(have);
    color(C_GREY);
    out(".\r\n\r\n");
    if (!remove) {
        c = ask("[I]nstall " UM_VERSION_STR " over it, [U]ninstall, or [Q]uit? ");
        out("\r\n");
    }
    if (c == 'i') return confirm_restart("is updated") ? install(have) : 0;
    if (c == 'u') {
        c = ask("Also delete every web site's URL Magic settings (rules format and file)?\r\n"
                "  Keep them if you might install urlmagic again. [y/N] ");
        out("\r\n");
        return confirm_restart("is removed") ? uninstall(c == 'y') : 0;
    }
    nothing_changed();
    return 0;
}

/* not installed, double-clicked without the full screen: ask to install */
static int ask_new(void)
{
    WCHAR dir[MAX_PATH];
    char q[MAX_PATH + 60];
    char d[MAX_PATH];
    int c;
    default_dir(dir);
    WideCharToMultiByte(CP_OEMCP, 0, dir, -1, d, MAX_PATH, NULL, NULL);
    wsprintfA(q, "Install urlmagic " UM_VERSION_STR " to %s? [Y/n] ", d);
    c = ask(q);
    out("\r\n");
    if ((c == 0 || c == 'y') && confirm_restart("is installed")) return install(dir);
    nothing_changed();
    return 0;
}

/* "install [folder]" */
static int install_cmd(int argc, WCHAR *const *argv)
{
    WCHAR dir[MAX_PATH];
    if (argc < 3) default_dir(dir);
    else if (!GetFullPathNameW(argv[2], MAX_PATH, dir, NULL)) lstrcpynW(dir, argv[2], MAX_PATH);
    return install(dir);
}

static BOOL is_command(const WCHAR *a)
{
    return lstrcmpiW(a, L"uninstall") == 0 || lstrcmpiW(a, L"install") == 0 || lstrcmpiW(a, L"/remove") == 0;
}

static BOOL is_help(const WCHAR *a)
{
    return lstrcmpiW(a, L"/?") == 0 || lstrcmpiW(a, L"-h") == 0 || lstrcmpiW(a, L"help") == 0;
}

/* the console (not full-screen) setup; returns the exit code */
static int console_main(int argc, WCHAR *const *argv)
{
    WCHAR have[MAX_PATH];
    if (argc >= 2 && !is_command(argv[1])) {
        usage();
        return is_help(argv[1]) ? 0 : 2;
    }
    if (!check_system()) return 1;
    if (g_interactive) return installed_dir(have) ? ask_installed(have, argc == 2) : ask_new();
    if (lstrcmpiW(argv[1], L"install") == 0) return install_cmd(argc, argv);
    if (lstrcmpiW(argv[1], L"uninstall") == 0) return uninstall(argc >= 3 && lstrcmpiW(argv[2], L"/purge") == 0);
    return 0;
}

/* g_self: this program's file name, without its folder */
static void set_self(void)
{
    WCHAR path[MAX_PATH];
    int i = (int)GetModuleFileNameW(NULL, path, MAX_PATH);
    while (i > 0 && path[i - 1] != L'\\') i--;
    lstrcpynW(g_self, path + i, MAX_PATH);
}

void WINAPI start(void)
{
    int argc;
    int rc;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    set_self();
    CoInitialize(NULL);
    /* double-click (no arguments), or Add/Remove Programs (/remove): the full-screen setup */
    g_interactive = argv && (argc == 1 || (argc == 2 && lstrcmpiW(argv[1], L"/remove") == 0));

    ui_init();
    if (g_interactive && tui_begin()) {
        rc = tui_main(argc == 2);
        if (argv) LocalFree(argv);
        CoUninitialize();
        ExitProcess((UINT)rc);
    }
    banner();
    rc = argv ? console_main(argc, argv) : 1;

    if (g_interactive) wait_to_close();
    out("\r\n");
    ui_done();
    if (argv) LocalFree(argv);
    CoUninitialize();
    ExitProcess((UINT)rc);
}
