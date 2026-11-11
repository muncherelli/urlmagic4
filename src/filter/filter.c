/*
 * urlmagic4.dll - the ISAPI filter. Install it as a global filter (Internet Service Manager >
 * server Properties > WWW Service > Edit > ISAPI Filters).
 *
 * For every request, before IIS maps the URL to a file:
 *   1. refuse requests for rule files (httpd.htaccess, .htaccess, httpd.ini, ...) with 403;
 *   2. look up the site's settings (written by the URL Magic tab, ../common/settings.h);
 *   3. take its cached rules;
 *   4. run the rewrite engine and rewrite the URL, redirect, or refuse.
 *
 * Requests never wait on the disk: a background thread watches each site's settings, home
 * directory and rules file and swaps in a new snapshot when one changes. Only a site's very
 * first request loads it. Each IIS thread keeps its own engine scratch, so a request
 * normally allocates no memory at all.
 *
 * Problems with rules files go to urlmagic4.log next to this DLL.
 */
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#include <windows.h>
#include <ole2.h>
#include <httpfilt.h>
#include <iadmw.h>
#include <iiscnfg.h>

#include "../common/settings.h"
#include "../engine/rewrite.h"
#include "version.h"

/* IMSAdminBase (IIS metabase), from iadmw.h */
static const GUID CLSID_UmMSAdminBase =
    {0xa9e69610, 0xb80d, 0x11d0, {0xb9, 0xb9, 0x00, 0xa0, 0xc9, 0x22, 0xe7, 0x50}};
static const GUID IID_UmIMSAdminBase =
    {0x70b51430, 0xb6ca, 0x11d0, {0xb9, 0xb9, 0x00, 0xa0, 0xc9, 0x22, 0xe7, 0x50}};

#define MAX_SITES       64
#define SETTINGS_EVERY  5000        /* ms between registry checks */
#define HOME_EVERY      60000       /* ms between metabase checks */
#define RULES_EVERY     2000        /* ms between rules-file checks */
#define MAX_RULES_FILE  (1024 * 1024)
#define URL_CAP         4096
#define LOG_MAX         (1024 * 1024)

static HINSTANCE g_hinst;
static CRITICAL_SECTION g_lock, g_loglock;
static WCHAR g_logpath[MAX_PATH];

/* ---------------------------------------------------------------- tiny helpers */

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ieq(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) a++, b++;
    return *a == 0 && *b == 0;
}

static void cpy(char *d, const char *s, size_t cap)
{
    size_t i = 0;
    if (!cap) return;
    while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void cat(char *d, const char *s, size_t cap)
{
    size_t n = strlen(d);
    if (n < cap) cpy(d + n, s, cap - n);
}

static DWORD tick_since(DWORD then) { return GetTickCount() - then; }

/* ---------------------------------------------------------------- log file */

static void log_line(const char *msg)
{
    SYSTEMTIME t;
    char line[1200];
    HANDLE f;
    DWORD w;

    GetLocalTime(&t);
    wsprintfA(line, "%04u-%02u-%02u %02u:%02u:%02u ", t.wYear, t.wMonth, t.wDay,
              t.wHour, t.wMinute, t.wSecond);
    cat(line, msg, sizeof line - 3);
    cat(line, "\r\n", sizeof line);

    EnterCriticalSection(&g_loglock);
    f = CreateFileW(g_logpath, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        if (GetFileSize(f, NULL) > LOG_MAX) {           /* keep one old log */
            WCHAR old[MAX_PATH];
            CloseHandle(f);
            lstrcpynW(old, g_logpath, MAX_PATH - 4);
            lstrcatW(old, L".old");
            DeleteFileW(old);
            MoveFileW(g_logpath, old);
            f = CreateFileW(g_logpath, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
        }
        if (f != INVALID_HANDLE_VALUE) {
            SetFilePointer(f, 0, NULL, FILE_END);
            WriteFile(f, line, (DWORD)strlen(line), &w, NULL);
            CloseHandle(f);
        }
    }
    LeaveCriticalSection(&g_loglock);
}

/* ---------------------------------------------------------------- per-site cache */

typedef struct {
    um_rules *r;
    LONG refs;
} rules_ref;

/* Everything a request needs from its site, replaced as a whole when anything changes. */
typedef struct {
    LONG refs;
    BOOL enabled, debug;
    rules_ref *rules;               /* NULL = no rules */
    char home_a[MAX_PATH];          /* site home directory, ANSI, no trailing backslash */
} snap;

/* Requests only touch id and cur (under g_lock). The rest belongs to whoever refreshes the
   site: the request that adds it (under g_lock, before anyone else can see it), then only
   the refresh thread. */
typedef struct {
    DWORD id;                       /* 0 = free slot */
    snap *cur;
    um_site cfg;
    DWORD cfg_at;
    WCHAR home[MAX_PATH];           /* site home directory from the metabase */
    char home_a[MAX_PATH];          /* same, ANSI, no trailing backslash */
    DWORD home_at;
    WCHAR path[MAX_PATH];           /* rules file in use */
    FILETIME mtime;
    DWORD size;
    BOOL present;
    DWORD rules_at;
    rules_ref *rules;
} site;

static site g_sites[MAX_SITES];
static HANDLE g_thread, g_stop;
static DWORD g_tls = TLS_OUT_OF_INDEXES;

static void ref_release(rules_ref *rr)
{
    if (rr && InterlockedDecrement(&rr->refs) == 0) {
        um_rules_free(rr->r);
        HeapFree(GetProcessHeap(), 0, rr);
    }
}

static void snap_release(snap *sn)
{
    if (sn && InterlockedDecrement(&sn->refs) == 0) {
        ref_release(sn->rules);
        HeapFree(GetProcessHeap(), 0, sn);
    }
}

/* the site's home directory (MD_VR_PATH of /LM/W3SVC/<id>/Root) */
static void read_home(site *s)
{
    IMSAdminBaseW *mb = NULL;
    HRESULT init = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    WCHAR key[64], buf[MAX_PATH];
    METADATA_RECORD rec;
    DWORD need = 0;

    s->home[0] = 0;
    s->home_a[0] = 0;
    wsprintfW(key, L"/LM/W3SVC/%lu/Root", (unsigned long)s->id);
    if (SUCCEEDED(CoCreateInstance(&CLSID_UmMSAdminBase, NULL, CLSCTX_ALL, &IID_UmIMSAdminBase,
                                   (void **)&mb)) && mb) {
        memset(&rec, 0, sizeof rec);
        rec.dwMDIdentifier = MD_VR_PATH;
        rec.dwMDAttributes = METADATA_INHERIT;
        rec.dwMDUserType = IIS_MD_UT_FILE;
        rec.dwMDDataType = STRING_METADATA;
        rec.dwMDDataLen = sizeof buf;
        rec.pbMDData = (unsigned char *)buf;
        if (SUCCEEDED(mb->lpVtbl->GetData(mb, METADATA_MASTER_ROOT_HANDLE, key, &rec, &need))) {
            int n;
            buf[MAX_PATH - 1] = 0;
            n = lstrlenW(buf);
            while (n > 3 && buf[n - 1] == L'\\') buf[--n] = 0;     /* keep "C:\" */
            lstrcpynW(s->home, buf, MAX_PATH);
            WideCharToMultiByte(CP_ACP, 0, s->home, -1, s->home_a, MAX_PATH, NULL, NULL);
            n = (int)strlen(s->home_a);
            if (n && s->home_a[n - 1] == '\\') s->home_a[n - 1] = 0;
        }
        mb->lpVtbl->Release(mb);
    }
    if (init == S_OK || init == S_FALSE) CoUninitialize();
    if (!s->home[0]) {
        char m[120];
        wsprintfA(m, "site %lu: couldn't read its home directory from the metabase", (unsigned long)s->id);
        log_line(m);
    }
}

typedef struct { char file[MAX_PATH]; } parse_ctx;

static void parse_log(void *ctx, int line, const char *msg)
{
    parse_ctx *pc = ctx;
    char m[1000];
    wsprintfA(m, "%s(%d): ", pc->file, line);
    cat(m, msg, sizeof m);
    log_line(m);
}

/* httpd.ini format: chosen in the tab, or Automatic and the file is called httpd.ini */
static BOOL wants_httpdini(const site *s, const WCHAR *path)
{
    int n = lstrlenW(path);
    if (s->cfg.format == UM_FMT_HTTPDINI) return TRUE;
    if (s->cfg.format == UM_FMT_HTACCESS) return FALSE;
    return n >= 9 && lstrcmpiW(path + n - 9, L"httpd.ini") == 0;
}

/* (Re)load the rules file if it changed. */
static void refresh_rules(site *s, const WCHAR *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    BOOL present = GetFileAttributesExW(path, GetFileExInfoStandard, &fa) &&
                   !(fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    BOOL same = lstrcmpiW(path, s->path) == 0 && present == s->present &&
                (!present || (CompareFileTime(&fa.ftLastWriteTime, &s->mtime) == 0 &&
                              fa.nFileSizeLow == s->size));
    rules_ref *nr = NULL;
    parse_ctx pc;

    if (same) return;

    WideCharToMultiByte(CP_ACP, 0, path, -1, pc.file, MAX_PATH, NULL, NULL);
    if (present && wants_httpdini(s, path)) {
        char m[MAX_PATH + 80];
        wsprintfA(m, "%s: httpd.ini (ISAPI_Rewrite 2) rules aren't supported yet; ignored", pc.file);
        log_line(m);
    } else if (present) {
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(f, NULL), got = 0;
            char *text = NULL;
            if (size <= MAX_RULES_FILE && (text = HeapAlloc(GetProcessHeap(), 0, size + 1)) &&
                ReadFile(f, text, size, &got, NULL)) {
                um_rules *r = um_parse(text, got, UM_DIALECT_APACHE, parse_log, &pc);
                if (r && (nr = HeapAlloc(GetProcessHeap(), 0, sizeof *nr))) {
                    char m[MAX_PATH + 80];
                    nr->r = r;
                    nr->refs = 1;
                    wsprintfA(m, "site %lu: loaded %d rule(s) from %s", (unsigned long)s->id,
                              um_rules_count(r), pc.file);
                    log_line(m);
                } else {
                    um_rules_free(r);
                }
            } else if (size > MAX_RULES_FILE) {
                char m[MAX_PATH + 60];
                wsprintfA(m, "%s: too big (over 1 MB), ignored", pc.file);
                log_line(m);
            }
            if (text) HeapFree(GetProcessHeap(), 0, text);
            CloseHandle(f);
        }
    } else if (s->present || lstrcmpiW(path, s->path) != 0) {
        char m[MAX_PATH + 60];
        wsprintfA(m, "site %lu: no rules file at %s", (unsigned long)s->id, pc.file);
        log_line(m);
    }
    lstrcpynW(s->path, path, MAX_PATH);
    s->present = present;
    if (present) { s->mtime = fa.ftLastWriteTime; s->size = fa.nFileSizeLow; }
    ref_release(s->rules);
    s->rules = nr;
}

/* Bring s up to date as its timers fall due: settings, home directory, rules file. */
static void refresh_site(site *s)
{
    BOOL rules_due = tick_since(s->rules_at) > RULES_EVERY;

    if (tick_since(s->cfg_at) > SETTINGS_EVERY) {
        um_site_read(HKEY_LOCAL_MACHINE, L"W3SVC", s->id, &s->cfg);
        s->cfg_at = GetTickCount();
        rules_due = TRUE;                       /* settings may point elsewhere now */
    }
    if (!s->cfg.enabled) return;
    if (tick_since(s->home_at) > HOME_EVERY) {
        read_home(s);
        s->home_at = GetTickCount();
    }
    if (rules_due) {
        WCHAR path[MAX_PATH];
        if (s->cfg.source == UM_SRC_FILE) {
            lstrcpynW(path, s->cfg.rules_file, MAX_PATH);
        } else if (s->home[0]) {
            /* the first of these that exists (Automatic tries all three; a chosen
               format only its own names) */
            static const WCHAR *names[] = { L"\\httpd.htaccess", L"\\.htaccess", L"\\httpd.ini" };
            int first = s->cfg.format == UM_FMT_HTTPDINI ? 2 : 0;
            int last = s->cfg.format == UM_FMT_HTACCESS ? 1 : 2, i;
            for (i = first; i <= last; i++) {
                lstrcpynW(path, s->home, MAX_PATH - 20);
                lstrcatW(path, names[i]);
                if (GetFileAttributesW(path) != 0xFFFFFFFF) break;
            }
            if (i > last) {                     /* none: watch the first name */
                lstrcpynW(path, s->home, MAX_PATH - 20);
                lstrcatW(path, names[first]);
            }
        } else {
            path[0] = 0;
        }
        if (path[0]) refresh_rules(s, path);
        s->rules_at = GetTickCount();
    }
}

/* A new snapshot of s if it differs from what requests use now, else NULL. */
static snap *new_snap(site *s)
{
    snap *n, *c = s->cur;
    BOOL enabled = s->cfg.enabled != 0, debug = s->cfg.enabled && s->cfg.debug;
    if (c && c->enabled == enabled && c->debug == debug && c->rules == s->rules &&
        lstrcmpA(c->home_a, s->home_a) == 0)
        return NULL;
    if (!(n = HeapAlloc(GetProcessHeap(), 0, sizeof *n))) return NULL;
    n->refs = 1;
    n->enabled = enabled;
    n->debug = debug;
    n->rules = s->rules;
    if (n->rules) InterlockedIncrement(&n->rules->refs);
    lstrcpynA(n->home_a, s->home_a, MAX_PATH);
    return n;
}

/* The site's current snapshot, with a reference the caller must release; NULL if there are
   too many sites. A site's first request loads it. */
static snap *site_snap(DWORD id)
{
    site *s = NULL, *free_slot = NULL;
    snap *out = NULL;
    int i;

    EnterCriticalSection(&g_lock);
    for (i = 0; i < MAX_SITES; i++) {
        if (g_sites[i].id == id) { s = &g_sites[i]; break; }
        if (!free_slot && !g_sites[i].id) free_slot = &g_sites[i];
    }
    if (!s && free_slot) {
        s = free_slot;
        memset(s, 0, sizeof *s);
        s->id = id;
        s->cfg_at = s->home_at = s->rules_at = GetTickCount() - HOME_EVERY - 1;
        refresh_site(s);
        s->cur = new_snap(s);
    }
    if (s && s->cur) {
        out = s->cur;
        InterlockedIncrement(&out->refs);
    }
    LeaveCriticalSection(&g_lock);
    return out;
}

/* The refresh thread: every second, bring every known site up to date. */
static DWORD WINAPI refresher(LPVOID unused)
{
    HRESULT init = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)unused;
    while (WaitForSingleObject(g_stop, 1000) == WAIT_TIMEOUT) {
        int i;
        for (i = 0; i < MAX_SITES; i++) {
            site *s = &g_sites[i];
            snap *n, *old = NULL;
            DWORD id;
            EnterCriticalSection(&g_lock);
            id = s->id;
            LeaveCriticalSection(&g_lock);
            if (!id) continue;
            refresh_site(s);                    /* the slow part, without the lock */
            if (!(n = new_snap(s))) continue;
            EnterCriticalSection(&g_lock);
            old = s->cur;
            s->cur = n;
            LeaveCriticalSection(&g_lock);
            snap_release(old);
        }
    }
    if (init == S_OK || init == S_FALSE) CoUninitialize();
    return 0;
}

/* this thread's engine scratch (made on first use, freed when the thread ends) */
static um_scratch *thread_scratch(void)
{
    um_scratch *sc;
    if (g_tls == TLS_OUT_OF_INDEXES) return NULL;
    sc = TlsGetValue(g_tls);
    if (!sc && (sc = um_scratch_new()) != NULL) TlsSetValue(g_tls, sc);
    return sc;
}

/* ---------------------------------------------------------------- request environment */

typedef struct {
    HTTP_FILTER_CONTEXT *pfc;
    HTTP_FILTER_PREPROC_HEADERS *hdr;
    const char *home;               /* ANSI, no trailing backslash */
    const char *url;
} request;

static const char *server_var(request *rq, const char *name, char *buf, size_t cap)
{
    DWORD n = (DWORD)cap;
    if (rq->pfc->GetServerVariable(rq->pfc, (LPSTR)name, buf, &n)) return buf;
    return NULL;
}

static const char *env_var(void *ctx, const char *name, const char *path, char *buf, size_t cap)
{
    request *rq = ctx;
    char tmp[200];

    if (ieq(name, "REQUEST_FILENAME") || ieq(name, "SCRIPT_FILENAME")) {
        size_t i;
        if (!rq->home[0]) return NULL;
        cpy(buf, rq->home, cap);
        cat(buf, path, cap);
        for (i = 0; buf[i]; i++) if (buf[i] == '/') buf[i] = '\\';
        return buf;
    }
    if (ieq(name, "DOCUMENT_ROOT")) { cpy(buf, rq->home, cap); return buf; }
    if (ieq(name, "REQUEST_METHOD")) {
        DWORD n = (DWORD)cap;
        return rq->hdr->GetHeader(rq->pfc, "method", buf, &n) ? buf : NULL;
    }
    if (ieq(name, "THE_REQUEST")) {
        DWORD n = sizeof tmp;
        if (!rq->hdr->GetHeader(rq->pfc, "method", tmp, &n)) return NULL;
        cpy(buf, tmp, cap);
        cat(buf, " ", cap);
        cat(buf, rq->url, cap);
        n = sizeof tmp;
        if (rq->hdr->GetHeader(rq->pfc, "version", tmp, &n)) { cat(buf, " ", cap); cat(buf, tmp, cap); }
        return buf;
    }
    if ((name[0] == 'H' || name[0] == 'h') && (name[1] == 'T' || name[1] == 't') &&
        (name[2] == 'T' || name[2] == 't') && (name[3] == 'P' || name[3] == 'p') && name[4] == ':') {
        /* %{HTTP:Some-Header} -> server variable HTTP_SOME_HEADER */
        size_t i, j = 5;
        cpy(tmp, "HTTP_", sizeof tmp);
        for (i = 5; name[i] && j + 1 < sizeof tmp; i++, j++) {
            char c = name[i];
            tmp[j] = c == '-' ? '_' : (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        }
        tmp[j] = 0;
        return server_var(rq, tmp, buf, cap);
    }
    if (ieq(name, "HTTP_HOST") || ieq(name, "HTTP_USER_AGENT") || ieq(name, "HTTP_REFERER") ||
        ieq(name, "HTTP_COOKIE") || ieq(name, "HTTP_ACCEPT") || ieq(name, "HTTPS") ||
        ieq(name, "REMOTE_ADDR") || ieq(name, "REMOTE_HOST") || ieq(name, "SERVER_NAME") ||
        ieq(name, "SERVER_PORT") || ieq(name, "SERVER_PROTOCOL") || ieq(name, "INSTANCE_ID"))
        return server_var(rq, name, buf, cap);
    return NULL;
}

static int env_file_type(void *ctx, const char *fspath)
{
    DWORD a = GetFileAttributesA(fspath);
    (void)ctx;
    if (a == 0xFFFFFFFF) return UM_FT_NONE;
    return (a & FILE_ATTRIBUTE_DIRECTORY) ? UM_FT_DIR : UM_FT_FILE;
}

/* ---------------------------------------------------------------- responses */

/* Send a whole small response in one piece (headers and body together: two small sends can
   stall on Nagle + delayed ACK). GET and HEAD over HTTP/1.1 keep the connection open, so a
   proxy in front can reuse it. */
static DWORD respond(HTTP_FILTER_CONTEXT *pfc, HTTP_FILTER_PREPROC_HEADERS *hdr,
                     const char *status, const char *location)
{
    char buf[URL_CAP + 700], body[300], method[16], version[16];
    DWORD n;
    BOOL head, keep;

    n = sizeof method;
    if (!hdr->GetHeader(pfc, "method", method, &n)) method[0] = 0;
    n = sizeof version;
    if (!hdr->GetHeader(pfc, "version", version, &n)) version[0] = 0;
    head = ieq(method, "HEAD");
    keep = (head || ieq(method, "GET")) && ieq(version, "HTTP/1.1");

    wsprintfA(body, "<html><head><title>%s</title></head><body><h1>%s</h1></body></html>", status, status);
    buf[0] = 0;
    if (location) {
        cpy(buf, "Location: ", sizeof buf);
        cat(buf, location, URL_CAP + 20);
        cat(buf, "\r\n", sizeof buf);
    }
    n = (DWORD)strlen(buf);
    wsprintfA(buf + n, "Content-Type: text/html\r\nContent-Length: %u\r\n%s\r\n",
              (unsigned)strlen(body), keep ? "" : "Connection: close\r\n");
    if (!head) cat(buf, body, sizeof buf);
    pfc->ServerSupportFunction(pfc, SF_REQ_SEND_RESPONSE_HEADER, (PVOID)status, (ULONG_PTR)buf, 0);
    return keep ? SF_STATUS_REQ_FINISHED_KEEP_CONN : SF_STATUS_REQ_FINISHED;
}

static const char *redirect_status(int code)
{
    switch (code) {
    case 301: return "301 Moved Permanently";
    case 303: return "303 See Other";
    case 307: return "307 Temporary Redirect";
    case 308: return "308 Permanent Redirect";
    default:  return "302 Found";
    }
}

/* the last path segment of url names a rules file? (decoded, any case) */
static BOOL is_rules_file(const char *url)
{
    static const char *names[] = { ".htaccess", "httpd.htaccess", "httpd.ini", "httpd.parse.errors",
                                   "nginx.conf", "urlmagic4.conf", NULL };
    char seg[64];
    const char *s = url, *last = url;
    size_t j = 0;
    int i;

    for (; *s && *s != '?'; s++) if (*s == '/' || *s == '\\') last = s + 1;
    for (s = last; *s && *s != '?' && j + 1 < sizeof seg; s++) {
        char c = *s;
        if (c == '%' && s[1] && s[2]) {
            int h = lower((unsigned char)s[1]), l = lower((unsigned char)s[2]);
            h = (h >= 'a') ? h - 'a' + 10 : h - '0';
            l = (l >= 'a') ? l - 'a' + 10 : l - '0';
            c = (char)(h * 16 + l);
            s += 2;
        }
        seg[j++] = (char)lower((unsigned char)c);
    }
    seg[j] = 0;
    /* NTFS ignores trailing dots and spaces */
    while (j && (seg[j - 1] == '.' || seg[j - 1] == ' ')) seg[--j] = 0;
    for (i = 0; names[i]; i++) if (ieq(seg, names[i])) return TRUE;
    return FALSE;
}

/* ---------------------------------------------------------------- ISAPI exports */

/* At startup: one log line per site that has URL Magic settings (Sites\<service>\<n>). */
static void log_sites(void)
{
    static const char *fmts[] = { "automatic", "Apache", "httpd.ini" };
    HKEY root, svc;
    WCHAR service[32], inst[16];
    DWORD i, j, n;
    int count = 0;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, UM_REG_SITES, 0, KEY_READ, &root) != ERROR_SUCCESS) {
        log_line("no site has URL Magic settings yet: sites use the defaults");
        return;
    }
    for (i = 0; n = 32, RegEnumKeyExW(root, i, service, &n, NULL, NULL, NULL, NULL) == ERROR_SUCCESS; i++) {
        if (RegOpenKeyExW(root, service, 0, KEY_READ, &svc) != ERROR_SUCCESS) continue;
        for (j = 0; n = 16, RegEnumKeyExW(svc, j, inst, &n, NULL, NULL, NULL, NULL) == ERROR_SUCCESS; j++) {
            um_site cfg;
            DWORD id = 0;
            const WCHAR *p;
            char sv[32], file[MAX_PATH], m[MAX_PATH + 160];
            for (p = inst; *p >= L'0' && *p <= L'9'; p++) id = id * 10 + (DWORD)(*p - L'0');
            if (!id || *p) continue;
            um_site_read(HKEY_LOCAL_MACHINE, service, id, &cfg);
            WideCharToMultiByte(CP_ACP, 0, service, -1, sv, sizeof sv, NULL, NULL);
            if (cfg.source == UM_SRC_FILE)
                WideCharToMultiByte(CP_ACP, 0, cfg.rules_file, -1, file, MAX_PATH, NULL, NULL);
            else
                lstrcpyA(file, "the site's home directory");
            wsprintfA(m, "site %s/%lu: rewriting %s, format %s, rules from %s%s", sv, (unsigned long)id,
                      cfg.enabled ? "on" : "off", fmts[cfg.format <= 2 ? cfg.format : 0], file,
                      cfg.debug ? ", debug on" : "");
            log_line(m);
            count++;
        }
        RegCloseKey(svc);
    }
    RegCloseKey(root);
    if (!count) log_line("no site has URL Magic settings yet: sites use the defaults");
}

BOOL WINAPI GetFilterVersion(HTTP_FILTER_VERSION *ver)
{
    ver->dwFilterVersion = HTTP_FILTER_REVISION;
    lstrcpynA(ver->lpszFilterDesc, "urlmagic4 " UM_VERSION_STR " URL rewriter", SF_MAX_FILTER_DESC_LEN);
    ver->dwFlags = SF_NOTIFY_ORDER_HIGH | SF_NOTIFY_SECURE_PORT | SF_NOTIFY_NONSECURE_PORT |
                   SF_NOTIFY_PREPROC_HEADERS;
    log_line("urlmagic4 " UM_VERSION_STR " loaded");
    log_sites();
    if (!g_thread) {
        DWORD tid;
        g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (g_stop) g_thread = CreateThread(NULL, 0, refresher, NULL, 0, &tid);
        if (!g_thread) log_line("couldn't start the refresh thread: rules files won't be reloaded");
    }
    return TRUE;
}

/* "site 3 debug: GET /old (Host dev.example.com) -> redirect 301 / by line 3, 42 µs" */
static void debug_line(request *rq, DWORD id, const um_result *res, LONGLONG ticks, LONGLONG freq)
{
    char m[URL_CAP * 2 + 200], method[16], host[128], what[URL_CAP + 40], took[40];
    DWORD n;
    ULONGLONG us = freq > 0 ? (ULONGLONG)ticks * 1000000u / (ULONGLONG)freq : 0;

    n = sizeof method;
    if (!rq->hdr->GetHeader(rq->pfc, "method", method, &n)) lstrcpyA(method, "?");
    if (!server_var(rq, "HTTP_HOST", host, sizeof host)) lstrcpyA(host, "?");
    switch (res->action) {
    case UM_REWRITE:   wsprintfA(what, "rewrite to %s", res->url ? res->url : "?"); break;
    case UM_REDIRECT:  wsprintfA(what, "redirect %d to %s", res->status, res->url ? res->url : "?"); break;
    case UM_FORBIDDEN: lstrcpyA(what, "403 forbidden"); break;
    case UM_GONE:      lstrcpyA(what, "410 gone"); break;
    case UM_FAIL:      lstrcpyA(what, "500 (rules failed: regex too complex?)"); break;
    default:           lstrcpyA(what, "unchanged"); break;
    }
    /* \xB5 = the micro sign in Windows-1252, which the log (and Notepad) use */
    if (us < 10000) wsprintfA(took, "%lu \xB5s", (unsigned long)us);
    else wsprintfA(took, "%lu.%03lu ms", (unsigned long)(us / 1000), (unsigned long)(us % 1000));
    if (res->line) {
        char by[24];
        wsprintfA(by, " by line %d", res->line);
        lstrcatA(what, by);
    }
    wsprintfA(m, "site %lu debug: %s %s (Host %s) -> ", (unsigned long)id, method, rq->url, host);
    cat(m, what, sizeof m);
    cat(m, ", rules took ", sizeof m);
    cat(m, took, sizeof m);
    log_line(m);
}

static DWORD preproc(HTTP_FILTER_CONTEXT *pfc, HTTP_FILTER_PREPROC_HEADERS *hdr)
{
    char url[URL_CAP], idbuf[16];
    DWORD n = sizeof url, id = 0;
    snap *sn;
    um_scratch *sc;
    request rq;
    um_env env;
    um_result res;
    const char *p;
    DWORD ret = SF_STATUS_REQ_NEXT_NOTIFICATION;
    LARGE_INTEGER t0, t1, freq;

    if (!hdr->GetHeader(pfc, "url", url, &n)) return ret;
    if (is_rules_file(url)) return respond(pfc, hdr, "403 Forbidden", NULL);

    n = sizeof idbuf;
    if (!pfc->GetServerVariable(pfc, "INSTANCE_ID", idbuf, &n)) return ret;
    for (p = idbuf; *p >= '0' && *p <= '9'; p++) id = id * 10 + (DWORD)(*p - '0');
    if (!id) return ret;

    sn = site_snap(id);
    if (!sn) return ret;
    if (!sn->enabled || !sn->rules) {
        if (sn->debug) {
            char m[URL_CAP + 80];
            wsprintfA(m, "site %lu debug: ", (unsigned long)id);
            cat(m, url, sizeof m);
            cat(m, " -> no rules loaded (see the lines above)", sizeof m);
            log_line(m);
        }
        snap_release(sn);
        return ret;
    }

    rq.pfc = pfc;
    rq.hdr = hdr;
    rq.home = sn->home_a;
    rq.url = url;
    env.ctx = &rq;
    env.var = env_var;
    env.file_type = env_file_type;

    if (sn->debug) QueryPerformanceCounter(&t0);
    sc = thread_scratch();
    if (sc) um_apply_s(sn->rules->r, url, &env, sc, &res);
    else um_apply(sn->rules->r, url, &env, &res);
    if (sn->debug) {
        QueryPerformanceCounter(&t1);
        debug_line(&rq, id, &res, t1.QuadPart - t0.QuadPart,
                   QueryPerformanceFrequency(&freq) ? freq.QuadPart : 0);
    }
    switch (res.action) {
    case UM_REWRITE:
        if (!hdr->SetHeader(pfc, "url", res.url)) log_line("couldn't set the rewritten URL");
        break;
    case UM_REDIRECT:  ret = respond(pfc, hdr, redirect_status(res.status), res.url); break;
    case UM_FORBIDDEN: ret = respond(pfc, hdr, "403 Forbidden", NULL); break;
    case UM_GONE:      ret = respond(pfc, hdr, "410 Gone", NULL); break;
    case UM_FAIL: {
        char m[URL_CAP + 80];
        wsprintfA(m, "site %lu: rules failed for ", (unsigned long)id);
        cat(m, url, sizeof m);
        cat(m, " (regex too complex?)", sizeof m);
        log_line(m);
        ret = respond(pfc, hdr, "500 Internal Server Error", NULL);
        break;
    }
    default: break;
    }
    if (!sc) um_result_free(&res);
    snap_release(sn);
    return ret;
}

DWORD WINAPI HttpFilterProc(HTTP_FILTER_CONTEXT *pfc, DWORD type, LPVOID data)
{
    if (type == SF_NOTIFY_PREPROC_HEADERS)
        return preproc(pfc, (HTTP_FILTER_PREPROC_HEADERS *)data);
    return SF_STATUS_REQ_NEXT_NOTIFICATION;
}

BOOL WINAPI TerminateFilter(DWORD flags)
{
    int i;
    (void)flags;
    if (g_thread) {
        /* no timeout: the sites must outlive the thread (a dead share can hold a check up
           until SMB gives up) */
        SetEvent(g_stop);
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_stop) { CloseHandle(g_stop); g_stop = NULL; }
    EnterCriticalSection(&g_lock);
    for (i = 0; i < MAX_SITES; i++) {
        snap_release(g_sites[i].cur);
        ref_release(g_sites[i].rules);
        g_sites[i].cur = NULL;
        g_sites[i].rules = NULL;
        g_sites[i].id = 0;
    }
    LeaveCriticalSection(&g_lock);
    log_line("urlmagic4 unloaded");
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        int i;
        g_hinst = h;
        g_tls = TlsAlloc();
        InitializeCriticalSection(&g_lock);
        InitializeCriticalSection(&g_loglock);
        i = (int)GetModuleFileNameW(h, g_logpath, MAX_PATH);
        while (i > 0 && g_logpath[i - 1] != L'\\') i--;
        lstrcpynW(g_logpath + i, L"urlmagic4.log", MAX_PATH - i);
    } else if (reason == DLL_THREAD_DETACH || reason == DLL_PROCESS_DETACH) {
        /* free this thread's scratch (IIS ends idle pool threads) */
        if (g_tls != TLS_OUT_OF_INDEXES) {
            um_scratch_free(TlsGetValue(g_tls));
            TlsSetValue(g_tls, NULL);
        }
        if (reason == DLL_PROCESS_DETACH) {
            if (g_tls != TLS_OUT_OF_INDEXES) TlsFree(g_tls);
            DeleteCriticalSection(&g_lock);
            DeleteCriticalSection(&g_loglock);
        }
    }
    return TRUE;
}
