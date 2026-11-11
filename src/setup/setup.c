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

/* ---------------------------------------------------------------- console output */

static void out(const char *s)
{
    DWORD w;
    WriteFile(g_out, s, (DWORD)lstrlenA(s), &w, NULL);
}

static void outw(const WCHAR *s)
{
    char buf[1024];
    WideCharToMultiByte(CP_OEMCP, 0, s, -1, buf, sizeof buf, NULL, NULL);
    out(buf);
}

static void step(const char *what) { out("  "); out(what); out(" ... "); }
static void ok(void) { out("ok\r\n"); }

static void fail(const char *why, DWORD code)
{
    char buf[300];
    if (code) wsprintfA(buf, "FAILED: %s (error %lu)\r\n", why, (unsigned long)code);
    else wsprintfA(buf, "FAILED: %s\r\n", why);
    out(buf);
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
            Sleep(500);
            if (!QueryServiceStatus(svc, &st)) break;
        }
        if (st.dwCurrentState != SERVICE_STOPPED) { fail("the WWW service didn't stop", 0); goto done; }
    } else {
        if (st.dwCurrentState == SERVICE_RUNNING) { result = TRUE; goto done; }
        if (!StartServiceW(svc, 0, NULL)) { fail("can't start the WWW service", GetLastError()); goto done; }
        for (i = 0; i < 120; i++) {
            Sleep(500);
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
    out(question);
    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), line, sizeof line - 1, &n, NULL)) return 0;
    for (i = 0; i < n; i++) {
        char c = line[i];
        if (c == '\r' || c == '\n') break;
        if (c != ' ' && c != '\t') return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    return 0;
}

static void wait_to_close(void)
{
    char line[16];
    DWORD n;
    out("\r\nPress Enter to close this window.");
    ReadFile(GetStdHandle(STD_INPUT_HANDLE), line, sizeof line, &n, NULL);
}

/* ---------------------------------------------------------------- is this NT 4.0 with IIS 4.0? */

static BOOL check_system(void)
{
    OSVERSIONINFOA v;
    HKEY k;
    DWORD major = 0, minor = 0, size, type;
    char line[200];

    out("Checking the system\r\n");
    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    GetVersionExA(&v);
    step("Windows");
    if (v.dwPlatformId != VER_PLATFORM_WIN32_NT) {
        out("FAILED: this is Windows 95/98/Me. urlmagic needs Windows NT 4.0 with IIS 4.0.\r\n");
        return FALSE;
    }
    wsprintfA(line, "NT %lu.%lu%s%s", v.dwMajorVersion, v.dwMinorVersion,
              v.szCSDVersion[0] ? " " : "", v.szCSDVersion);
    out(line);
    if (v.dwMajorVersion != 4) out(" (not NT 4.0: untested, carrying on)");
    out("\r\n");

    step("IIS");
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\InetStp", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        size = sizeof major;
        RegQueryValueExW(k, L"MajorVersion", NULL, &type, (BYTE *)&major, &size);
        size = sizeof minor;
        RegQueryValueExW(k, L"MinorVersion", NULL, &type, (BYTE *)&minor, &size);
        RegCloseKey(k);
    }
    if (!major) {
        out("FAILED: IIS isn't installed (install it from the NT 4.0 Option Pack).\r\n");
        return FALSE;
    }
    wsprintfA(line, "%lu.%lu", major, minor);
    out(line);
    if (major != 4) out(" (not IIS 4.0: untested, carrying on)");
    out("\r\n\r\n");
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
    out("Installing urlmagic4 " UM_VERSION_STR " to ");
    outw(dir);
    out("\r\n\r\n");

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
            out("    ");
            outw(to);
            out(e == ERROR_SHARING_VIOLATION ? "\r\n    is in use: close Internet Service Manager and run setup again.\r\n" : "\r\n");
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

    if (g_errors) { out("\r\nSetup finished with errors (see above).\r\n"); return 1; }
    out("\r\nDone.\r\n\r\n"
        "  1. Open Internet Service Manager, right-click a web site > Properties >\r\n"
        "     URL Magic. If the tab isn't there, enable it once: Console > Add/Remove\r\n"
        "     Snap-in > Extensions, choose Internet Information Server, tick URL Magic,\r\n"
        "     OK, then Console > Save.\r\n"
        "  2. Add rules: a .htaccess file in a web site's home directory, e.g.\r\n\r\n"
        "       RewriteEngine On\r\n"
        "       RewriteCond %{REQUEST_FILENAME} !-f\r\n"
        "       RewriteCond %{REQUEST_FILENAME} !-d\r\n"
        "       RewriteRule ^(.*)$ index.cfm?page=$1 [L,QSA]\r\n\r\n"
        "     Each site's Properties > URL Magic tab chooses the format and file.\r\n\r\n"
        "Problems with rules are logged to ");
    join(to, dir, L"urlmagic4.log");
    outw(to);
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
    out("Removing urlmagic4 from ");
    outw(dir);
    out("\r\n\r\n");

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
    out(reboot ? "ok (some files were in use; they go at the next reboot)\r\n" : "ok\r\n");

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
    out(g_errors ? "\r\nFinished with errors (see above).\r\n" : "\r\nurlmagic4 is removed.\r\n");
    if (!purge) { out("Site settings were kept; `"); outw(g_self); out(" uninstall /purge` removes them too.\r\n"); }
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

    out("urlmagic " UM_VERSION_STR " setup - URL rewriting for IIS 4.0\r\n\r\n");
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
            out("urlmagic ");
            outw(ver[0] ? ver : L"(unknown version)");
            out(" is installed in ");
            outw(have);
            out(".\r\n\r\n");
            c = ask("[I]nstall " UM_VERSION_STR " over it, [U]ninstall, or [Q]uit? ");
            out("\r\n");
            if (c == 'i') rc = install(have);
            else if (c == 'u') {
                c = ask("Also delete every web site's URL Magic settings (rules format and file)?\r\n"
                        "Keep them if you might install urlmagic again. [y/N] ");
                out("\r\n");
                rc = uninstall(c == 'y');
            }
            else out("Nothing changed.\r\n");
        } else {
            default_dir(dir);
            out("Install urlmagic " UM_VERSION_STR " to ");
            outw(dir);
            c = ask("? [Y/n] ");
            out("\r\n");
            if (c == 0 || c == 'y') rc = install(dir);
            else out("Nothing changed.\r\n");
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
    if (argv) LocalFree(argv);
    CoUninitialize();
    ExitProcess((UINT)rc);
}
