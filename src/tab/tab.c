/*
 * urlmagic4tab.dll - the "urlmagic4" tab in IIS 4.0's Internet Service Manager.
 *
 * An MMC property-sheet extension for the IIS snap-in's web site node. The tab edits that
 * site's urlmagic4 settings (see ../common/settings.h) in the registry of the machine the
 * IIS manager is connected to.
 *
 *   regsvr32 urlmagic4tab.dll        register; also listed as a "required extension" of the
 *                                    IIS snap-in so consoles enable it without asking (where MMC
 *                                    supports that; otherwise Console > Add/Remove Snap-in >
 *                                    Extensions, once per console)
 *   regsvr32 /u urlmagic4tab.dll     remove
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <ole2.h>
#include <commctrl.h>
#include <commdlg.h>
#include <prsht.h>
#include <mmc.h>

#include "../common/settings.h"
#include "resource.h"

/* ---------------------------------------------------------------- ids */

/* {6349356F-8C6D-4813-94F6-7C8EB8085445} */
static const CLSID CLSID_Tab =
    {0x6349356f, 0x8c6d, 0x4813, {0x94, 0xf6, 0x7c, 0x8e, 0xb8, 0x08, 0x54, 0x45}};
static const WCHAR CLSID_Tab_Str[] = L"{6349356F-8C6D-4813-94F6-7C8EB8085445}";

/* IIS 4 snap-in's web site node */
static const GUID NODE_WEBSITE =
    {0xa841b6c7, 0x7577, 0x11d0, {0xbb, 0x1f, 0x00, 0xa0, 0xc9, 0x22, 0xe7, 0x9c}};
static const WCHAR NODE_WEBSITE_Str[] = L"{A841B6C7-7577-11D0-BB1F-00A0C922E79C}";
/* the IIS 4 snap-in itself */
static const WCHAR SNAPIN_IIS_Str[] = L"{A841B6C2-7577-11D0-BB1F-00A0C922E79C}";

static const IID IID_UmExtendPropertySheet =
    {0x85de64dc, 0xef21, 0x11cf, {0xa2, 0x85, 0x00, 0xc0, 0x4f, 0xd8, 0xdb, 0xe6}};
static const IID IID_UmUnknown =
    {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID IID_UmClassFactory =
    {0x00000001, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

static const WCHAR TAB_NAME[] = L"URL Magic";
static const WCHAR MMC_SNAPINS[]   = L"Software\\Microsoft\\MMC\\SnapIns";
static const WCHAR MMC_NODETYPES[] = L"Software\\Microsoft\\MMC\\NodeTypes";

static HINSTANCE g_hinst;
static LONG g_locks;

/* ---------------------------------------------------------------- helpers */

static BOOL guid_eq(const GUID *a, const GUID *b)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    int i;
    for (i = 0; i < 16; i++)
        if (x[i] != y[i]) return FALSE;
    return TRUE;
}

static void *mem_alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void mem_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

/* Read one clipboard format from the IIS snap-in's data object into buf (cap bytes).
   The snap-in only implements GetDataHere into a caller-supplied HGLOBAL. */
static BOOL get_format(IDataObject *dobj, const WCHAR *name, void *buf, DWORD cap)
{
    FORMATETC fe;
    STGMEDIUM sm;
    HGLOBAL h;
    BOOL ok = FALSE;

    fe.cfFormat = (CLIPFORMAT)RegisterClipboardFormatW(name);
    fe.ptd = NULL;
    fe.dwAspect = DVASPECT_CONTENT;
    fe.lindex = -1;
    fe.tymed = TYMED_HGLOBAL;
    h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, 4096);
    if (!h) return FALSE;
    sm.tymed = TYMED_HGLOBAL;
    sm.hGlobal = h;
    sm.pUnkForRelease = NULL;
    if (SUCCEEDED(IDataObject_GetDataHere(dobj, &fe, &sm))) {
        BYTE *p = GlobalLock(sm.hGlobal);
        if (p) {
            DWORD n = (DWORD)GlobalSize(sm.hGlobal);
            memcpy(buf, p, n < cap ? n : cap);
            GlobalUnlock(sm.hGlobal);
            ok = TRUE;
        }
    }
    GlobalFree(h);
    return ok;
}

static void get_string(IDataObject *dobj, const WCHAR *name, WCHAR *out, int cch)
{
    WCHAR tmp[256];
    memset(tmp, 0, sizeof tmp);
    out[0] = 0;
    if (get_format(dobj, name, tmp, sizeof tmp - sizeof(WCHAR)))
        lstrcpynW(out, tmp, cch);
}

static DWORD parse_uint(const WCHAR *s)
{
    DWORD v = 0;
    if (!*s) return 0;
    for (; *s; s++) {
        if (*s < L'0' || *s > L'9') return 0;
        v = v * 10 + (DWORD)(*s - L'0');
    }
    return v;
}

/* ---------------------------------------------------------------- the site we're editing */

typedef struct {
    WCHAR machine[64];      /* as the IIS manager names it, e.g. "ws1" */
    WCHAR service[16];      /* "W3SVC" */
    DWORD instance;         /* site number */
    um_site site;
    LONG load_error;
    BOOL loading;           /* ignore control notifications while filling the page */
} page_state;

static BOOL read_node(IDataObject *dobj, page_state *ps)
{
    GUID node;
    WCHAR inst[16];

    memset(&node, 0, sizeof node);
    if (!get_format(dobj, L"CCF_NODETYPE", &node, sizeof node) || !guid_eq(&node, &NODE_WEBSITE))
        return FALSE;
    get_string(dobj, L"ISM_SNAPIN_MACHINE_NAME", ps->machine, 64);
    get_string(dobj, L"ISM_SNAPIN_SERVICE", ps->service, 16);
    get_string(dobj, L"ISM_SNAPIN_INSTANCE", inst, 16);
    CharUpperW(ps->service);
    ps->instance = parse_uint(inst);
    return lstrcmpW(ps->service, L"W3SVC") == 0 && ps->instance > 0;
}

/* HKLM of the machine the IIS manager is connected to. *remote says whether to close it. */
static LONG open_hklm(const WCHAR *machine, HKEY *hklm, BOOL *remote)
{
    WCHAR local[MAX_COMPUTERNAME_LENGTH + 1], unc[80];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;

    *remote = FALSE;
    *hklm = HKEY_LOCAL_MACHINE;
    if (!machine[0] || (GetComputerNameW(local, &n) && lstrcmpiW(local, machine) == 0))
        return ERROR_SUCCESS;
    wsprintfW(unc, L"\\\\%s", machine);
    *remote = TRUE;
    return RegConnectRegistryW(unc, HKEY_LOCAL_MACHINE, hklm);
}

static LONG load_site(page_state *ps)
{
    HKEY hklm;
    BOOL remote;
    LONG r = open_hklm(ps->machine, &hklm, &remote);
    if (r != ERROR_SUCCESS) { um_site_defaults(&ps->site); return r; }
    r = um_site_read(hklm, ps->service, ps->instance, &ps->site);
    if (remote) RegCloseKey(hklm);
    return r;
}

static LONG save_site(page_state *ps)
{
    HKEY hklm;
    BOOL remote;
    LONG r = open_hklm(ps->machine, &hklm, &remote);
    if (r != ERROR_SUCCESS) return r;
    r = um_site_write(hklm, ps->service, ps->instance, &ps->site);
    if (remote) RegCloseKey(hklm);
    return r;
}

/* ---------------------------------------------------------------- the page */

static void update_controls(HWND dlg)
{
    BOOL on = IsDlgButtonChecked(dlg, IDC_ENABLED) == BST_CHECKED;
    BOOL file = IsDlgButtonChecked(dlg, IDC_SRC_FILE) == BST_CHECKED;
    static const int group[] = { IDC_FMT_GROUP, IDC_FMT_AUTO, IDC_FMT_HTACCESS, IDC_FMT_HTTPDINI,
                                 IDC_SRC_GROUP, IDC_SRC_DOCROOT, IDC_SRC_FILE, IDC_DEBUG, 0 };
    int i;
    for (i = 0; group[i]; i++) EnableWindow(GetDlgItem(dlg, group[i]), on);
    EnableWindow(GetDlgItem(dlg, IDC_FILE), on && file);
    EnableWindow(GetDlgItem(dlg, IDC_BROWSE), on && file);
}

static void fill_page(HWND dlg, page_state *ps)
{
    WCHAR line[200];

    ps->loading = TRUE;
    if (ps->load_error == ERROR_SUCCESS)
        wsprintfW(line, L"Web site %lu on %s", (unsigned long)ps->instance,
                  ps->machine[0] ? ps->machine : L"this computer");
    else
        wsprintfW(line, L"Couldn't read the settings (error %ld); showing defaults.", ps->load_error);
    SetDlgItemTextW(dlg, IDC_SITE, line);
    CheckDlgButton(dlg, IDC_ENABLED, ps->site.enabled ? BST_CHECKED : BST_UNCHECKED);
    CheckRadioButton(dlg, IDC_FMT_AUTO, IDC_FMT_HTTPDINI, IDC_FMT_AUTO + (int)ps->site.format);
    CheckRadioButton(dlg, IDC_SRC_DOCROOT, IDC_SRC_FILE, IDC_SRC_DOCROOT + (int)ps->site.source);
    SetDlgItemTextW(dlg, IDC_FILE, ps->site.rules_file);
    CheckDlgButton(dlg, IDC_DEBUG, ps->site.debug ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(dlg, IDC_FILE, EM_LIMITTEXT, MAX_PATH - 1, 0);
    update_controls(dlg);
    ps->loading = FALSE;
}

static void browse(HWND dlg)
{
    OPENFILENAMEW ofn;
    WCHAR file[MAX_PATH];

    GetDlgItemTextW(dlg, IDC_FILE, file, MAX_PATH);
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = OPENFILENAME_SIZE_VERSION_400W;   /* the size NT 4.0 knows */
    ofn.hwndOwner = dlg;
    ofn.lpstrFilter = L"Rule files (.htaccess, httpd.ini, *.conf)\0.htaccess;httpd.ini;*.conf;*.ini\0"
                      L"All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Choose the rules file";
    ofn.Flags = OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn))
        SetDlgItemTextW(dlg, IDC_FILE, file);
}

/* Read the controls into ps->site; FALSE (after telling the user) if they don't make sense. */
static BOOL read_page(HWND dlg, page_state *ps)
{
    um_site s;
    um_site_defaults(&s);
    s.enabled = IsDlgButtonChecked(dlg, IDC_ENABLED) == BST_CHECKED;
    s.format = IsDlgButtonChecked(dlg, IDC_FMT_HTACCESS) == BST_CHECKED ? UM_FMT_HTACCESS :
               IsDlgButtonChecked(dlg, IDC_FMT_HTTPDINI) == BST_CHECKED ? UM_FMT_HTTPDINI : UM_FMT_AUTO;
    s.source = IsDlgButtonChecked(dlg, IDC_SRC_FILE) == BST_CHECKED ? UM_SRC_FILE : UM_SRC_DOCROOT;
    GetDlgItemTextW(dlg, IDC_FILE, s.rules_file, MAX_PATH);
    s.debug = IsDlgButtonChecked(dlg, IDC_DEBUG) == BST_CHECKED;
    if (s.enabled && s.source == UM_SRC_FILE && !s.rules_file[0]) {
        MessageBoxW(dlg, L"Enter the path of the rules file, or choose \"In the site's home directory\".",
                    TAB_NAME, MB_OK | MB_ICONEXCLAMATION);
        SetFocus(GetDlgItem(dlg, IDC_FILE));
        return FALSE;
    }
    ps->site = s;
    return TRUE;
}

static INT_PTR CALLBACK page_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    page_state *ps = (page_state *)GetWindowLongPtrW(dlg, DWLP_USER);

    switch (msg) {
    case WM_INITDIALOG:
        ps = (page_state *)((PROPSHEETPAGEW *)lp)->lParam;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)ps);
        fill_page(dlg, ps);
        return TRUE;

    case WM_COMMAND:
        if (!ps || ps->loading) break;
        if (LOWORD(wp) == IDC_BROWSE && HIWORD(wp) == BN_CLICKED) {
            browse(dlg);
        } else if ((HIWORD(wp) == BN_CLICKED && LOWORD(wp) != IDC_BROWSE) ||
                   (LOWORD(wp) == IDC_FILE && HIWORD(wp) == EN_CHANGE)) {
            update_controls(dlg);
            PropSheet_Changed(GetParent(dlg), dlg);
        }
        break;

    case WM_NOTIFY:
        if (!ps) break;
        switch (((NMHDR *)lp)->code) {
        case PSN_KILLACTIVE:
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, read_page(dlg, ps) ? FALSE : TRUE);
            return TRUE;
        case PSN_APPLY: {
            LONG r;
            if (!read_page(dlg, ps)) {
                SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_INVALID_NOCHANGEPAGE);
                return TRUE;
            }
            r = save_site(ps);
            if (r != ERROR_SUCCESS) {
                WCHAR line[200];
                wsprintfW(line, L"Couldn't save the URL Magic settings (error %ld).", r);
                MessageBoxW(dlg, line, TAB_NAME, MB_OK | MB_ICONERROR);
                SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_INVALID_NOCHANGEPAGE);
                return TRUE;
            }
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        }
        break;
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
} Tab;

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
    return InterlockedIncrement(&((Tab *)This)->refs);
}

static ULONG WINAPI tab_release(IExtendPropertySheet *This)
{
    Tab *t = (Tab *)This;
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
    page_state *ps;
    HRESULT hr;
    (void)This; (void)handle;

    ps = mem_alloc(sizeof *ps);
    if (!ps) return E_OUTOFMEMORY;
    if (!read_node(dobj, ps)) { mem_free(ps); return S_FALSE; }
    ps->load_error = load_site(ps);

    memset(&psp, 0, sizeof psp);
    psp.dwSize = PROPSHEETPAGEW_V1_SIZE;   /* NT 4.0 comctl32 only knows the v1 layout */
    psp.dwFlags = PSP_USECALLBACK | PSP_USETITLE | PSP_USEICONID;
    psp.hInstance = g_hinst;
    psp.pszTemplate = MAKEINTRESOURCEW(IDD_SITE);
    psp.pszIcon = MAKEINTRESOURCEW(IDI_TAB);   /* the wand, on the tab itself */
    psp.pszTitle = TAB_NAME;
    psp.pfnDlgProc = page_proc;
    psp.pfnCallback = page_cb;
    psp.lParam = (LPARAM)ps;

    page = CreatePropertySheetPageW(&psp);
    if (!page) { mem_free(ps); return E_FAIL; }
    InterlockedIncrement(&g_locks);        /* released in page_cb */
    hr = IPropertySheetCallback_AddPage(cb, page);
    if (FAILED(hr)) DestroyPropertySheetPage(page);
    return hr;
}

static HRESULT WINAPI tab_query_pages(IExtendPropertySheet *This, LPDATAOBJECT dobj)
{
    page_state ps;
    (void)This;
    memset(&ps, 0, sizeof ps);
    return read_node(dobj, &ps) ? S_OK : S_FALSE;
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
    Tab *t;
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
    if (!guid_eq(clsid, &CLSID_Tab)) { *ppv = NULL; return CLASS_E_CLASSNOTAVAILABLE; }
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

/* RegDeleteKey on NT doesn't delete subkeys */
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

HRESULT WINAPI DllRegisterServer(void)
{
    WCHAR dll[MAX_PATH], path[300];
    LONG r;

    GetModuleFileNameW(g_hinst, dll, MAX_PATH);
    wsprintfW(path, L"CLSID\\%s", CLSID_Tab_Str);
    r = set_str(HKEY_CLASSES_ROOT, path, NULL, L"urlmagic4 IIS manager tab");
    wsprintfW(path, L"CLSID\\%s\\InprocServer32", CLSID_Tab_Str);
    if (r == ERROR_SUCCESS) r = set_str(HKEY_CLASSES_ROOT, path, NULL, dll);
    if (r == ERROR_SUCCESS) r = set_str(HKEY_CLASSES_ROOT, path, L"ThreadingModel", L"Apartment");
    wsprintfW(path, L"%s\\%s", MMC_SNAPINS, CLSID_Tab_Str);
    if (r == ERROR_SUCCESS) r = set_str(HKEY_LOCAL_MACHINE, path, L"NameString", TAB_NAME);
    wsprintfW(path, L"%s\\%s\\Extensions\\PropertySheet", MMC_NODETYPES, NODE_WEBSITE_Str);
    if (r == ERROR_SUCCESS) r = set_str(HKEY_LOCAL_MACHINE, path, CLSID_Tab_Str, TAB_NAME);
    /* best effort: MMC always enables a snap-in's required extensions */
    if (r == ERROR_SUCCESS) {
        wsprintfW(path, L"%s\\%s\\RequiredExtensions\\%s", MMC_SNAPINS, SNAPIN_IIS_Str, CLSID_Tab_Str);
        set_str(HKEY_LOCAL_MACHINE, path, NULL, TAB_NAME);
    }
    return r == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(r);
}

HRESULT WINAPI DllUnregisterServer(void)
{
    WCHAR path[300];
    HKEY k;

    wsprintfW(path, L"%s\\%s\\Extensions\\PropertySheet", MMC_NODETYPES, NODE_WEBSITE_Str);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, CLSID_Tab_Str);
        RegCloseKey(k);
    }
    wsprintfW(path, L"%s\\%s\\RequiredExtensions\\%s", MMC_SNAPINS, SNAPIN_IIS_Str, CLSID_Tab_Str);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, path);
    wsprintfW(path, L"%s\\%s", MMC_SNAPINS, CLSID_Tab_Str);
    delete_tree(HKEY_LOCAL_MACHINE, path);
    wsprintfW(path, L"CLSID\\%s", CLSID_Tab_Str);
    delete_tree(HKEY_CLASSES_ROOT, path);
    return S_OK;   /* site settings under HKLM\SOFTWARE\urlmagic4 are kept */
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
