/*
 * umtab.dll - spike: can we add a tab to IIS 4.0's Internet Service Manager?
 *
 * An MMC 1.x property-sheet extension. `regsvr32 umtab.dll` finds the IIS snap-in's node
 * types in the registry and registers this DLL as a property-sheet extension for each one.
 * If MMC loads us, every IIS node's Properties dialog gets a "urlmagic4" tab showing what
 * the IIS snap-in tells us about the node (node type, name, clipboard formats), which is
 * what the real tab needs to know which web site it is looking at.
 *
 * `regsvr32 /u umtab.dll` removes every registry entry this DLL made.
 *
 * Built without the C runtime; only NT 4.0 APIs.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <ole2.h>
#include <commctrl.h>
#include <prsht.h>
#include <mmc.h>

#include "resource.h"

/* ---------------------------------------------------------------- ids */

/* our CLSID: {8F7AC576-6CD7-43CB-B15B-67B3E5B1A8E6} */
static const CLSID CLSID_UmTab =
    {0x8f7ac576, 0x6cd7, 0x43cb, {0xb1, 0x5b, 0x67, 0xb3, 0xe5, 0xb1, 0xa8, 0xe6}};
static const WCHAR CLSID_UmTab_Str[] = L"{8F7AC576-6CD7-43CB-B15B-67B3E5B1A8E6}";
static const WCHAR UmTab_Name[] = L"urlmagic4 tab (spike)";

/* MMC interfaces (not in MinGW's libuuid) */
static const IID IID_UmExtendPropertySheet =
    {0x85de64dc, 0xef21, 0x11cf, {0xa2, 0x85, 0x00, 0xc0, 0x4f, 0xd8, 0xdb, 0xe6}};
static const IID IID_UmUnknown =
    {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID IID_UmClassFactory =
    {0x00000001, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

static const WCHAR MMC_SNAPINS[]   = L"Software\\Microsoft\\MMC\\SnapIns";
static const WCHAR MMC_NODETYPES[] = L"Software\\Microsoft\\MMC\\NodeTypes";

static HINSTANCE g_hinst;
static LONG g_locks;     /* objects alive + LockServer count */

/* ---------------------------------------------------------------- no-CRT helpers */

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}

static BOOL guid_eq(REFIID a, const IID *b)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    int i;
    for (i = 0; i < 16; i++)
        if (x[i] != y[i]) return FALSE;
    return TRUE;
}

static void *mem_alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void mem_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

/* growable wide-string buffer */
typedef struct { WCHAR *s; int len, cap; } wbuf;

static void wb_add(wbuf *b, const WCHAR *t)
{
    int n = lstrlenW(t);
    if (b->len + n + 1 > b->cap) {
        int cap = (b->cap ? b->cap * 2 : 1024);
        WCHAR *s;
        while (cap < b->len + n + 1) cap *= 2;
        s = mem_alloc(cap * sizeof(WCHAR));
        if (!s) return;
        if (b->s) { memcpy(s, b->s, b->len * sizeof(WCHAR)); mem_free(b->s); }
        b->s = s;
        b->cap = cap;
    }
    memcpy(b->s + b->len, t, (n + 1) * sizeof(WCHAR));
    b->len += n;
}

/* case-insensitive "does h contain n" */
static BOOL wcontains(const WCHAR *h, const WCHAR *n)
{
    int hl = lstrlenW(h), nl = lstrlenW(n), i;
    for (i = 0; i + nl <= hl; i++)
        if (CompareStringW(LOCALE_SYSTEM_DEFAULT, NORM_IGNORECASE, h + i, nl, n, nl) == CSTR_EQUAL)
            return TRUE;
    return FALSE;
}

/* ---------------------------------------------------------------- data object probing */

/* Ask the snap-in's data object for one clipboard format; returns a HeapAlloc'd copy. */
static BYTE *get_format(IDataObject *dobj, const WCHAR *name, DWORD *size)
{
    FORMATETC fe;
    STGMEDIUM sm;
    BYTE *out = NULL;
    HGLOBAL h;

    *size = 0;
    fe.cfFormat = (CLIPFORMAT)RegisterClipboardFormatW(name);
    fe.ptd = NULL;
    fe.dwAspect = DVASPECT_CONTENT;
    fe.lindex = -1;
    fe.tymed = TYMED_HGLOBAL;

    /* MMC snap-ins usually only implement GetDataHere into a caller-supplied HGLOBAL */
    h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, 4096);
    if (h) {
        memset(&sm, 0, sizeof sm);
        sm.tymed = TYMED_HGLOBAL;
        sm.hGlobal = h;
        if (SUCCEEDED(IDataObject_GetDataHere(dobj, &fe, &sm))) {
            BYTE *p = GlobalLock(sm.hGlobal);
            DWORD n = (DWORD)GlobalSize(sm.hGlobal);
            if (p && (out = mem_alloc(n + 2))) { memcpy(out, p, n); *size = n; }
            if (p) GlobalUnlock(sm.hGlobal);
        }
        GlobalFree(h);
    }
    if (!out) {
        memset(&sm, 0, sizeof sm);
        if (SUCCEEDED(IDataObject_GetData(dobj, &fe, &sm))) {
            if (sm.tymed == TYMED_HGLOBAL) {
                BYTE *p = GlobalLock(sm.hGlobal);
                DWORD n = (DWORD)GlobalSize(sm.hGlobal);
                if (p && (out = mem_alloc(n + 2))) { memcpy(out, p, n); *size = n; }
                if (p) GlobalUnlock(sm.hGlobal);
            }
            ReleaseStgMedium(&sm);
        }
    }
    return out;
}

/* Describe a blob: as a GUID if 16 bytes, as text if it looks like a wide string, else hex. */
static void describe(wbuf *b, const WCHAR *name, const BYTE *p, DWORD n, BOOL guid)
{
    WCHAR line[600];
    wsprintfW(line, L"%s = ", name);
    wb_add(b, line);
    if (!p) { wb_add(b, L"(not available)\r\n"); return; }
    if (guid && n >= 16) {
        WCHAR g[40];
        StringFromGUID2((REFGUID)p, g, 40);
        wb_add(b, g);
    } else if (n >= 4 && p[1] == 0 && p[0] >= 0x20) {
        WCHAR t[256];
        lstrcpynW(t, (const WCHAR *)p, 256);
        wb_add(b, L"\"");
        wb_add(b, t);
        wb_add(b, L"\"");
    } else {
        DWORD i;
        for (i = 0; i < n && i < 48; i++) {
            wsprintfW(line, L"%02X ", p[i]);
            wb_add(b, line);
        }
        wsprintfW(line, L"(%lu bytes)", n);
        wb_add(b, line);
    }
    wb_add(b, L"\r\n");
}

static WCHAR *probe(IDataObject *dobj)
{
    /* standard MMC formats, then guesses at what the IIS snap-in might publish */
    static const WCHAR *guesses[] = {
        L"CCF_DISPLAY_NAME", L"CCF_SZNODETYPE", L"CCF_NODEID", L"CCF_NODEID2",
        L"CCF_COLUMN_SET_ID", L"CCF_WINDOW_TITLE",
        L"ISM_MACHINE_NAME", L"ISM_SERVICE", L"ISM_INSTANCE", L"ISM_PARENT_PATH",
        L"ISM_NODE_NAME", L"ISM_META_PATH", L"ISM_METABASE_PATH", L"ISM_NODE",
        L"ISM_COMPUTER", L"ISM_SERVER", L"ISM_SITE", L"ISM_SITE_ID",
        L"MMC_SNAPIN_MACHINE_NAME", NULL };
    wbuf b = {0};
    BYTE *p;
    DWORD n;
    int i;
    IEnumFORMATETC *en = NULL;

    wb_add(&b, L"urlmagic4 spike: what the IIS snap-in tells us about this node\r\n\r\n");

    p = get_format(dobj, L"CCF_NODETYPE", &n);
    describe(&b, L"CCF_NODETYPE", p, n, TRUE);
    mem_free(p);
    p = get_format(dobj, L"CCF_SNAPIN_CLASSID", &n);
    describe(&b, L"CCF_SNAPIN_CLASSID", p, n, TRUE);
    mem_free(p);
    for (i = 0; guesses[i]; i++) {
        p = get_format(dobj, guesses[i], &n);
        if (p || i < 2) describe(&b, guesses[i], p, n, FALSE);
        mem_free(p);
    }

    wb_add(&b, L"\r\nEnumFormatEtc: ");
    if (SUCCEEDED(IDataObject_EnumFormatEtc(dobj, DATADIR_GET, &en)) && en) {
        FORMATETC fe;
        ULONG got;
        wb_add(&b, L"\r\n");
        while (IEnumFORMATETC_Next(en, 1, &fe, &got) == S_OK && got == 1) {
            WCHAR fname[128], line[200];
            if (!GetClipboardFormatNameW(fe.cfFormat, fname, 128))
                wsprintfW(fname, L"#%u", (unsigned)fe.cfFormat);
            wsprintfW(line, L"  %s (tymed %lu)\r\n", fname, fe.tymed);
            wb_add(&b, line);
            p = get_format(dobj, fname, &n);
            if (p) { wb_add(&b, L"    "); describe(&b, fname, p, n, n == 16); }
            mem_free(p);
        }
        IEnumFORMATETC_Release(en);
    } else {
        wb_add(&b, L"not supported\r\n");
    }
    return b.s;
}

/* ---------------------------------------------------------------- the property page */

static INT_PTR CALLBACK page_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp;
    if (msg == WM_INITDIALOG) {
        PROPSHEETPAGEW *psp = (PROPSHEETPAGEW *)lp;
        SetDlgItemTextW(dlg, IDC_INFO, (const WCHAR *)psp->lParam);
        return TRUE;
    }
    return FALSE;
}

static UINT CALLBACK page_cb(HWND hwnd, UINT msg, PROPSHEETPAGEW *psp)
{
    (void)hwnd;
    if (msg == PSPCB_RELEASE) {
        mem_free((void *)psp->lParam);
        InterlockedDecrement(&g_locks);
    }
    return 1;
}

/* ---------------------------------------------------------------- IExtendPropertySheet */

typedef struct {
    IExtendPropertySheetVtbl *lpVtbl;
    LONG refs;
} UmTab;

static HRESULT WINAPI tab_qi(IExtendPropertySheet *This, REFIID riid, void **ppv)
{
    if (guid_eq(riid, &IID_UmUnknown) || guid_eq(riid, &IID_UmExtendPropertySheet)) {
        *ppv = This;
        IExtendPropertySheet_AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI tab_addref(IExtendPropertySheet *This)
{
    return InterlockedIncrement(&((UmTab *)This)->refs);
}

static ULONG WINAPI tab_release(IExtendPropertySheet *This)
{
    UmTab *t = (UmTab *)This;
    LONG r = InterlockedDecrement(&t->refs);
    if (r == 0) {
        mem_free(t);
        InterlockedDecrement(&g_locks);
    }
    return r;
}

static HRESULT WINAPI tab_create_pages(IExtendPropertySheet *This, LPPROPERTYSHEETCALLBACK cb,
                                       LONG_PTR handle, LPDATAOBJECT dobj)
{
    PROPSHEETPAGEW psp;
    HPROPSHEETPAGE page;
    WCHAR *info;
    (void)This; (void)handle;

    info = probe(dobj);
    if (!info) return E_OUTOFMEMORY;

    memset(&psp, 0, sizeof psp);
    psp.dwSize = PROPSHEETPAGEW_V1_SIZE;   /* NT 4.0 comctl32 only knows the v1 layout */
    psp.dwFlags = PSP_USECALLBACK | PSP_USETITLE;
    psp.hInstance = g_hinst;
    psp.pszTemplate = MAKEINTRESOURCEW(IDD_PAGE);
    psp.pszTitle = L"urlmagic4";
    psp.pfnDlgProc = page_proc;
    psp.pfnCallback = page_cb;
    psp.lParam = (LPARAM)info;

    page = CreatePropertySheetPageW(&psp);
    if (!page) { mem_free(info); return E_FAIL; }
    InterlockedIncrement(&g_locks);   /* released in page_cb */
    return IPropertySheetCallback_AddPage(cb, page);
}

static HRESULT WINAPI tab_query_pages(IExtendPropertySheet *This, LPDATAOBJECT dobj)
{
    (void)This; (void)dobj;
    return S_OK;
}

static IExtendPropertySheetVtbl tab_vtbl = {
    tab_qi, tab_addref, tab_release, tab_create_pages, tab_query_pages
};

/* ---------------------------------------------------------------- class factory */

static HRESULT WINAPI cf_qi(IClassFactory *This, REFIID riid, void **ppv)
{
    if (guid_eq(riid, &IID_UmUnknown) || guid_eq(riid, &IID_UmClassFactory)) {
        *ppv = This;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI cf_addref(IClassFactory *This) { (void)This; return 2; }
static ULONG WINAPI cf_release(IClassFactory *This) { (void)This; return 1; }

static HRESULT WINAPI cf_create(IClassFactory *This, IUnknown *outer, REFIID riid, void **ppv)
{
    UmTab *t;
    HRESULT hr;
    (void)This;
    *ppv = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    t = mem_alloc(sizeof *t);
    if (!t) return E_OUTOFMEMORY;
    t->lpVtbl = &tab_vtbl;
    t->refs = 1;
    InterlockedIncrement(&g_locks);
    hr = tab_qi((IExtendPropertySheet *)t, riid, ppv);
    tab_release((IExtendPropertySheet *)t);
    return hr;
}

static HRESULT WINAPI cf_lock(IClassFactory *This, BOOL lock)
{
    (void)This;
    if (lock) InterlockedIncrement(&g_locks); else InterlockedDecrement(&g_locks);
    return S_OK;
}

static IClassFactoryVtbl cf_vtbl = { cf_qi, cf_addref, cf_release, cf_create, cf_lock };
static IClassFactory g_factory = { &cf_vtbl };

/* ---------------------------------------------------------------- COM exports */

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
    if (!guid_eq(clsid, &CLSID_UmTab)) { *ppv = NULL; return CLASS_E_CLASSNOTAVAILABLE; }
    return cf_qi(&g_factory, riid, ppv);
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return g_locks == 0 ? S_OK : S_FALSE;
}

/* ---------------------------------------------------------------- registration */

static LONG set_str(HKEY root, const WCHAR *path, const WCHAR *name, const WCHAR *val)
{
    HKEY k;
    DWORD disp;
    LONG r = RegCreateKeyExW(root, path, 0, NULL, 0, KEY_WRITE, NULL, &k, &disp);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)val, (lstrlenW(val) + 1) * sizeof(WCHAR));
    RegCloseKey(k);
    return r;
}

/* RegDeleteKey on NT does not delete subkeys */
static void delete_tree(HKEY root, const WCHAR *path)
{
    HKEY k;
    WCHAR sub[256];
    if (RegOpenKeyExW(root, path, 0, KEY_ALL_ACCESS, &k) == ERROR_SUCCESS) {
        for (;;) {
            DWORD n = 256;
            WCHAR full[600];
            if (RegEnumKeyExW(k, 0, sub, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            wsprintfW(full, L"%s\\%s", path, sub);
            delete_tree(root, full);
        }
        RegCloseKey(k);
    }
    RegDeleteKeyW(root, path);
}

/* Call fn(node type GUID string) for every node type of every IIS snap-in. */
typedef void (*node_fn)(const WCHAR *node, wbuf *log);

static int for_each_iis_node(node_fn fn, wbuf *log)
{
    HKEY snapins;
    DWORD i;
    int count = 0;
    WCHAR clsid[64], line[700];

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, MMC_SNAPINS, 0, KEY_READ, &snapins) != ERROR_SUCCESS) {
        wb_add(log, L"No MMC snap-ins registered (HKLM\\Software\\Microsoft\\MMC\\SnapIns missing)\r\n");
        return 0;
    }
    for (i = 0;; i++) {
        DWORD n = 64, type, size;
        HKEY si, nodes;
        WCHAR name[256];
        if (RegEnumKeyExW(snapins, i, clsid, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (RegOpenKeyExW(snapins, clsid, 0, KEY_READ, &si) != ERROR_SUCCESS) continue;
        size = sizeof name;
        name[0] = 0;
        RegQueryValueExW(si, L"NameString", NULL, &type, (BYTE *)name, &size);
        if (wcontains(name, L"Internet Information") || wcontains(name, L"IIS") ||
            wcontains(name, L"Internet Service")) {
            wsprintfW(line, L"Snap-in %s \"%s\"\r\n", clsid, name);
            wb_add(log, line);
            if (RegOpenKeyExW(si, L"NodeTypes", 0, KEY_READ, &nodes) == ERROR_SUCCESS) {
                DWORD j;
                for (j = 0;; j++) {
                    WCHAR node[64];
                    DWORD m = 64;
                    if (RegEnumKeyExW(nodes, j, node, &m, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
                    fn(node, log);
                    count++;
                }
                RegCloseKey(nodes);
            } else {
                wb_add(log, L"  (it lists no NodeTypes)\r\n");
            }
        }
        RegCloseKey(si);
    }
    RegCloseKey(snapins);
    return count;
}

static void register_node(const WCHAR *node, wbuf *log)
{
    WCHAR path[300], line[300];
    LONG r;
    wsprintfW(path, L"%s\\%s\\Extensions\\PropertySheet", MMC_NODETYPES, node);
    r = set_str(HKEY_LOCAL_MACHINE, path, CLSID_UmTab_Str, UmTab_Name);
    wsprintfW(line, L"  node %s: %s\r\n", node, r == ERROR_SUCCESS ? L"tab registered" : L"FAILED");
    wb_add(log, line);
}

static void unregister_node(const WCHAR *node, wbuf *log)
{
    WCHAR path[300];
    HKEY k;
    (void)log;
    wsprintfW(path, L"%s\\%s\\Extensions\\PropertySheet", MMC_NODETYPES, node);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, CLSID_UmTab_Str);
        RegCloseKey(k);
    }
}

HRESULT WINAPI DllRegisterServer(void)
{
    WCHAR dll[MAX_PATH], path[300];
    wbuf log = {0};
    int n;

    GetModuleFileNameW(g_hinst, dll, MAX_PATH);
    wsprintfW(path, L"CLSID\\%s", CLSID_UmTab_Str);
    set_str(HKEY_CLASSES_ROOT, path, NULL, UmTab_Name);
    wsprintfW(path, L"CLSID\\%s\\InprocServer32", CLSID_UmTab_Str);
    set_str(HKEY_CLASSES_ROOT, path, NULL, dll);
    set_str(HKEY_CLASSES_ROOT, path, L"ThreadingModel", L"Apartment");
    wsprintfW(path, L"%s\\%s", MMC_SNAPINS, CLSID_UmTab_Str);
    set_str(HKEY_LOCAL_MACHINE, path, L"NameString", UmTab_Name);

    n = for_each_iis_node(register_node, &log);
    if (n == 0)
        wb_add(&log, L"\r\nNo IIS node types found: the tab can't be attached this way.");
    else
        wb_add(&log, L"\r\nReopen Internet Service Manager, then open a web site's Properties.");
    MessageBoxW(NULL, log.s ? log.s : L"(no output)", L"urlmagic4 spike: regsvr32", MB_OK | MB_ICONINFORMATION);
    mem_free(log.s);
    return S_OK;
}

HRESULT WINAPI DllUnregisterServer(void)
{
    WCHAR path[300];
    wbuf log = {0};

    for_each_iis_node(unregister_node, &log);
    mem_free(log.s);
    wsprintfW(path, L"%s\\%s", MMC_SNAPINS, CLSID_UmTab_Str);
    delete_tree(HKEY_LOCAL_MACHINE, path);
    wsprintfW(path, L"CLSID\\%s", CLSID_UmTab_Str);
    delete_tree(HKEY_CLASSES_ROOT, path);
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinst = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
