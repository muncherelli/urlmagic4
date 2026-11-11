/*
 * urlmagic<version>.exe (e.g. urlmagic4.0.0.exe) - console installer for urlmagic4 on IIS 4.0.
 *
 *   urlmagic4.0.0                       ask, then install / upgrade / uninstall (double-click)
 *   urlmagic4.0.0 install [dir]         install to dir (default %SystemDrive%\urlmagic4)
 *   urlmagic4.0.0 uninstall          remove (keeps each site's settings)
 *   urlmagic4.0.0 uninstall /purge   remove, including each site's settings
 *
 * Everything it installs is inside it (RCDATA resources, see payload.h), so this one file is
 * all you need. It also copies itself into the install folder, to uninstall later.
 *
 * Installing: stops the WWW service, copies urlmagic4.dll and urlmagic4tab.dll, registers
 * the URL Magic tab, adds urlmagic4 to IIS's global ISAPI filters, starts the WWW service.
 */
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <iadmw.h>
#include <iiscnfg.h>

#include "version.h"
#include "payload.h"

static const GUID CLSID_UmMSAdminBase =
    {0xa9e69610, 0xb80d, 0x11d0, {0xb9, 0xb9, 0x00, 0xa0, 0xc9, 0x22, 0xe7, 0x50}};
static const GUID IID_UmIMSAdminBase =
    {0x70b51430, 0xb6ca, 0x11d0, {0xb9, 0xb9, 0x00, 0xa0, 0xc9, 0x22, 0xe7, 0x50}};

static const WCHAR FILTER_NAME[] = L"urlmagic4";
static const WCHAR FILTERS_KEY[] = L"/LM/W3SVC/Filters";
/* installed files, in the order of their resource IDs (payload.h) */
static const WCHAR *FILES[] = { L"urlmagic4.dll", L"urlmagic4tab.dll", L"README.md", L"LICENSE",
                                L"CHANGELOG.md", NULL };
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
    const char *p;
    WriteFile(g_out, s, (DWORD)lstrlenA(s), &w, NULL);
    for (p = s; *p; p++) {
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
    SetConsoleTitleA("urlmagic " UM_VERSION_STR " setup");
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
static const int LOGO_TEXT[] = { 0, 1, 2, -1, 3, 4, 5, 6, 7, -1, 8 };
#define LOGO_W 53
#define BOX_W 63                         /* inside width */

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
    int n = lstrlenA(text), pad = (BOX_W - n) / 2;
    color(C_DCYAN);
    out("  \xBA");
    color(c);
    outc(' ', pad);
    out(text);
    outc(' ', BOX_W - n - pad);
    color(C_DCYAN);
    out("\xBA\r\n");
}

static void banner(void)
{
    int row, i, j;
    out("\r\n");
    box_edge((char)0xC9, (char)0xBB);
    /* sparkles over the logo */
    color(C_DCYAN); out("  \xBA");
    color(C_YELLOW); out("      *          .        +                   .     *          ");
    color(C_DCYAN); out("  \xBA\r\n");
    for (row = 0; row < 5; row++) {
        color(C_DCYAN);
        out("  \xBA     ");
        for (i = 0; i < (int)(sizeof LOGO_TEXT / sizeof LOGO_TEXT[0]); i++) {
            int g = LOGO_TEXT[i];
            const char *bits;
            if (g < 0) { out("  "); continue; }
            color(g <= 2 ? C_CYAN : g <= 7 ? C_MAGENTA : C_RED);
            bits = LOGO_FONT[g][row];
            for (j = 0; bits[j]; j++) outc(bits[j] == 'X' ? (char)0xDB : ' ', 1);
            if (i + 1 < (int)(sizeof LOGO_TEXT / sizeof LOGO_TEXT[0]) && LOGO_TEXT[i + 1] >= 0) out(" ");
        }
        outc(' ', BOX_W - 5 - LOGO_W);
        color(C_DCYAN);
        out("\xBA\r\n");
    }
    box_line("", C_GREY);
    box_line("URL rewriting for IIS 4.0 on Windows NT 4.0", C_WHITE);
    box_line("version " UM_VERSION_STR "  \xFA  github.com/muncherelli/urlmagic4", C_DIM);
    box_edge((char)0xC8, (char)0xBC);
    color(C_GREY);
    out("\r\n");
}

/* "  -- Title ------" */
static void section(const char *title)
{
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
    if (g_spin < 0) { g_spin = 0; color(C_YELLOW); outc(chars[0], 1); return; }
    g_spin = (g_spin + 1) % 4;
    color(C_YELLOW);
    out("\b");
    outc(chars[g_spin], 1);
}

static void spin_end(void)
{
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

static void ok(void) { tag(" OK ", C_GREEN); out("\r\n"); }

/* a result that's a value rather than OK, e.g. a version number */
static void value(const char *v, BOOL good)
{
    spin_end();
    color(good ? C_GREEN : C_YELLOW);
    out(v);
    color(C_GREY);
    out("\r\n");
}

static void note(const char *text)
{
    color(C_YELLOW);
    out("      ");
    out(text);
    color(C_GREY);
    out("\r\n");
}

static void fail(const char *why, DWORD code)
{
    char buf[300];
    if (g_col > 2) { tag("FAIL", C_RED); out("\r\n"); }
    if (code) wsprintfA(buf, "%s (error %lu)", why, (unsigned long)code);
    else lstrcpynA(buf, why, sizeof buf);
    color(C_RED);
    out("      ");
    out(buf);
    color(C_GREY);
    out("\r\n");
    g_errors++;
}

/* ---------------------------------------------------------------- WWW service */

/* Stop or start W3SVC; returns TRUE if it was running before a stop. */
static BOOL www(BOOL start, BOOL *was_running)
{
    SC_HANDLE scm, svc;
    SERVICE_STATUS st;
    BOOL result = FALSE;
    int i;

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) { fail("can't open the service manager", GetLastError()); return FALSE; }
    svc = OpenServiceW(scm, L"W3SVC", SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) { fail("can't open the WWW service (is IIS installed?)", GetLastError()); CloseServiceHandle(scm); return FALSE; }

    if (!QueryServiceStatus(svc, &st)) { fail("can't query the WWW service", GetLastError()); goto done; }
    if (!start) {
        if (was_running) *was_running = st.dwCurrentState != SERVICE_STOPPED;
        if (st.dwCurrentState != SERVICE_STOPPED && st.dwCurrentState != SERVICE_STOP_PENDING &&
            !ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
            fail("can't stop the WWW service", GetLastError());
            goto done;
        }
        for (i = 0; i < 120 && st.dwCurrentState != SERVICE_STOPPED; i++) {
            spin();
            Sleep(250);
            spin();
            Sleep(250);
            if (!QueryServiceStatus(svc, &st)) break;
        }
        if (st.dwCurrentState != SERVICE_STOPPED) { fail("the WWW service didn't stop", 0); goto done; }
    } else {
        if (st.dwCurrentState == SERVICE_RUNNING) { result = TRUE; goto done; }
        if (!StartServiceW(svc, 0, NULL)) { fail("can't start the WWW service", GetLastError()); goto done; }
        for (i = 0; i < 120; i++) {
            spin();
            Sleep(250);
            spin();
            Sleep(250);
            if (!QueryServiceStatus(svc, &st) || st.dwCurrentState != SERVICE_START_PENDING) break;
        }
        if (st.dwCurrentState != SERVICE_RUNNING) { fail("the WWW service didn't start", 0); goto done; }
    }
    result = TRUE;
done:
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

static void rec_string(METADATA_RECORD *r, DWORD id, DWORD utype, const WCHAR *v)
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
            if (!in_list(item, name) && item[0]) {
                if (outl[0] && lstrlenW(outl) + 1 < cap) lstrcatW(outl, L",");
                if (lstrlenW(outl) + len < cap) lstrcatW(outl, item);
            }
        }
        p = *e ? e + 1 : e;
    }
}

static BOOL filter_register(const WCHAR *dll)
{
    IMSAdminBaseW *mb = metabase();
    METADATA_HANDLE h;
    METADATA_RECORD r;
    WCHAR order[1024], neworder[1100];
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
    WCHAR order[1024], neworder[1024];
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
    if (!m) { if (reg) fail("can't load urlmagic4tab.dll", GetLastError()); return !reg; }
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
    wsprintfW(dir, L"%s\\urlmagic4", drive);
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
    DWORD type, size = MAX_PATH * sizeof(WCHAR);
    BOOL found = FALSE;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\urlmagic4", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        found = RegQueryValueExW(k, L"InstallDir", NULL, &type, (BYTE *)dir, &size) == ERROR_SUCCESS &&
                type == REG_SZ;
        RegCloseKey(k);
    }
    return found;
}

static void record_dir(const WCHAR *dir)
{
    HKEY k;
    DWORD disp;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\urlmagic4", 0, NULL, 0, KEY_WRITE, NULL, &k, &disp) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"InstallDir", 0, REG_SZ, (const BYTE *)dir, (lstrlenW(dir) + 1) * sizeof(WCHAR));
        RegSetValueExW(k, L"Version", 0, REG_SZ, (const BYTE *)UM_VERSION_WSTR, sizeof(UM_VERSION_WSTR));
        RegSetValueExW(k, L"Setup", 0, REG_SZ, (const BYTE *)g_self, (lstrlenW(g_self) + 1) * sizeof(WCHAR));
        RegCloseKey(k);
    }
}

/* ---------------------------------------------------------------- talking to the user */

/* first non-blank character of a line typed by the user, lowercased; 0 for just Enter */
static int ask(const char *question)
{
    char line[64];
    DWORD n = 0, i;
    color(C_YELLOW);
    out("  ");
    out(question);
    color(C_WHITE);
    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), line, sizeof line - 1, &n, NULL)) return 0;
    for (i = 0; i < n; i++) {
        char c = line[i];
        if (c == '\r' || c == '\n') break;
        if (c != ' ' && c != '\t') return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    return 0;
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
    color(C_DIM);
    out("  Nothing changed.\r\n");
    color(C_GREY);
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

static BOOL check_system(void)
{
    OSVERSIONINFOA v;
    HKEY k;
    DWORD major = 0, minor = 0, size, type;
    char line[200];

    section("Checking the system");
    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    GetVersionExA(&v);
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
    HGLOBAL g;
    const void *data;
    DWORD size, w, err = 0;
    HANDLE f;

    if (!r || !(g = LoadResource(NULL, r)) || !(data = LockResource(g))) return ERROR_RESOURCE_DATA_NOT_FOUND;
    size = SizeofResource(NULL, r);
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return GetLastError();
    if (!WriteFile(f, data, size, &w, NULL) || w != size) err = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
    CloseHandle(f);
    if (err) DeleteFileW(path);
    return err;
}

static int install(const WCHAR *dir)
{
    WCHAR src[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
    BOOL running = FALSE;
    int i;

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
    CreateDirectoryW(dir, NULL);
    for (i = 0; FILES[i]; i++) {
        DWORD e;
        join(to, dir, FILES[i]);
        if ((e = extract(FILE_IDS[i], to)) != 0) {
            fail("can't write a file", e);
            color(C_RED);
            out("      ");
            outw(to);
            out("\r\n");
            if (e == ERROR_SHARING_VIOLATION) note("It's in use: close Internet Service Manager and run setup again.");
            color(C_GREY);
            if (running) www(TRUE, NULL);
            return 1;
        }
    }
    /* a copy of this program, for uninstalling later */
    join(from, src, g_self);
    join(to, dir, g_self);
    if (lstrcmpiW(from, to) != 0) CopyFileW(from, to, FALSE);
    ok();
    record_dir(dir);

    step("Registering the URL Magic tab");
    join(to, dir, L"urlmagic4tab.dll");
    if (tab_register(to, TRUE)) ok();

    step("Adding the urlmagic4 ISAPI filter to IIS");
    join(to, dir, L"urlmagic4.dll");
    if (filter_register(to)) ok();

    step("Starting the WWW service");
    if (www(TRUE, NULL)) ok();

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
    color(C_GREY); out("Put a ");
    color(C_WHITE); out(".htaccess"); color(C_GREY); out(" in a web site's home directory, e.g.\r\n\r\n");
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
    WCHAR sub[256], full[600];
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

static int uninstall(BOOL purge)
{
    WCHAR dir[MAX_PATH], f[MAX_PATH];
    BOOL running = FALSE, reboot = FALSE;
    int i;

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

    step("Removing the urlmagic4 ISAPI filter from IIS");
    if (filter_unregister()) ok();

    step("Unregistering the URL Magic tab");
    join(f, dir, L"urlmagic4tab.dll");
    if (tab_register(f, FALSE)) ok();

    step("Deleting files");
    for (i = 0; FILES[i]; i++) {
        join(f, dir, FILES[i]);
        if (!DeleteFileW(f) && GetLastError() != ERROR_FILE_NOT_FOUND) {
            MoveFileExW(f, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
            reboot = TRUE;
        }
    }
    {
        /* the installer copy (maybe this very program, which can't delete itself while running) */
        WCHAR setup[MAX_PATH], here[MAX_PATH];
        HKEY k;
        DWORD type, size = sizeof setup;
        setup[0] = 0;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\urlmagic4", 0, KEY_READ, &k) == ERROR_SUCCESS) {
            if (RegQueryValueExW(k, L"Setup", NULL, &type, (BYTE *)setup, &size) != ERROR_SUCCESS || type != REG_SZ)
                setup[0] = 0;
            RegCloseKey(k);
        }
        if (setup[0]) {
            join(f, dir, setup);
            GetModuleFileNameW(NULL, here, MAX_PATH);
            if (lstrcmpiW(f, here) == 0 || (!DeleteFileW(f) && GetLastError() != ERROR_FILE_NOT_FOUND)) {
                MoveFileExW(f, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
                reboot = TRUE;
            }
        }
    }
    join(f, dir, L"urlmagic4.log");
    DeleteFileW(f);
    join(f, dir, L"urlmagic4.log.old");
    DeleteFileW(f);
    if (!RemoveDirectoryW(dir) && reboot) MoveFileExW(dir, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    ok();
    if (reboot) note("Some files were in use; they go at the next reboot.");

    if (purge) {
        step("Deleting every site's urlmagic4 settings");
        delete_tree(HKEY_LOCAL_MACHINE, L"SOFTWARE\\urlmagic4");
        ok();
    } else {
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\urlmagic4", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, L"InstallDir");
            RegDeleteValueW(k, L"Version");
            RegDeleteValueW(k, L"Setup");
            RegCloseKey(k);
        }
    }

    if (running) {
        step("Starting the WWW service");
        if (www(TRUE, NULL)) ok();
    }
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
    outw(g_self); out(" install [folder]   install or upgrade (default %SystemDrive%\\urlmagic4)\r\n  ");
    outw(g_self); out(" uninstall          remove urlmagic4 (keeps site settings)\r\n  ");
    outw(g_self); out(" uninstall /purge   remove urlmagic4 and every site's settings\r\n");
}

/* ---------------------------------------------------------------- entry point (no CRT) */

void WINAPI start(void)
{
    int argc, rc = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    WCHAR dir[MAX_PATH];

    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    {
        WCHAR path[MAX_PATH];
        int i = (int)GetModuleFileNameW(NULL, path, MAX_PATH);
        while (i > 0 && path[i - 1] != L'\\') i--;
        lstrcpynW(g_self, path + i, MAX_PATH);
    }
    CoInitialize(NULL);
    g_interactive = argv && argc == 1;

    ui_init();
    banner();
    if (!argv) rc = 1;
    else if (argc >= 2 && lstrcmpiW(argv[1], L"uninstall") != 0 && lstrcmpiW(argv[1], L"install") != 0) {
        usage();
        rc = (lstrcmpiW(argv[1], L"/?") == 0 || lstrcmpiW(argv[1], L"-h") == 0 ||
              lstrcmpiW(argv[1], L"help") == 0) ? 0 : 2;
    } else if (!check_system()) {
        rc = 1;
    } else if (g_interactive) {
        WCHAR have[MAX_PATH], ver[32];
        HKEY k;
        DWORD type, size = sizeof ver;
        int c;
        ver[0] = 0;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\urlmagic4", 0, KEY_READ, &k) == ERROR_SUCCESS) {
            RegQueryValueExW(k, L"Version", NULL, &type, (BYTE *)ver, &size);
            RegCloseKey(k);
        }
        if (installed_dir(have)) {
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
            c = ask("[I]nstall " UM_VERSION_STR " over it, [U]ninstall, or [Q]uit? ");
            out("\r\n");
            if (c == 'i') {
                if (confirm_restart("is updated")) rc = install(have);
            } else if (c == 'u') {
                c = ask("Also delete every web site's URL Magic settings (rules format and file)?\r\n"
                        "  Keep them if you might install urlmagic again. [y/N] ");
                out("\r\n");
                if (confirm_restart("is removed")) rc = uninstall(c == 'y');
            }
            else { color(C_DIM); out("  Nothing changed.\r\n"); color(C_GREY); }
        } else {
            default_dir(dir);
            {
                char q[MAX_PATH + 60], d[MAX_PATH];
                WideCharToMultiByte(CP_OEMCP, 0, dir, -1, d, MAX_PATH, NULL, NULL);
                wsprintfA(q, "Install urlmagic " UM_VERSION_STR " to %s? [Y/n] ", d);
                c = ask(q);
            }
            out("\r\n");
            if ((c == 0 || c == 'y') && confirm_restart("is installed")) rc = install(dir);
            else { color(C_DIM); out("  Nothing changed.\r\n"); color(C_GREY); }
        }
    } else if (lstrcmpiW(argv[1], L"install") == 0) {
        if (argc >= 3) {
            if (!GetFullPathNameW(argv[2], MAX_PATH, dir, NULL)) lstrcpynW(dir, argv[2], MAX_PATH);
        } else default_dir(dir);
        rc = install(dir);
    } else if (lstrcmpiW(argv[1], L"uninstall") == 0) {
        rc = uninstall(argc >= 3 && lstrcmpiW(argv[2], L"/purge") == 0);
    }

    if (g_interactive) wait_to_close();
    out("\r\n");
    ui_done();
    if (argv) LocalFree(argv);
    CoUninitialize();
    ExitProcess((UINT)rc);
}
