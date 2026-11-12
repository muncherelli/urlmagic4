#include "settings.h"

static const WCHAR *FMT_NAMES[] = { L"auto", L"htaccess", L"httpd.ini" };
static const WCHAR *SRC_NAMES[] = { L"docroot", L"file" };

void um_site_defaults(um_site *s)
{
    s->enabled = 0;               /* off until turned on in the site's URL Magic tab */
    s->format = UM_FMT_AUTO;
    s->source = UM_SRC_DOCROOT;
    s->rules_file[0] = 0;
    s->debug = 0;
}

static void site_key(WCHAR *out, const WCHAR *service, DWORD instance)
{
    wsprintfW(out, L"%s\\%s\\%lu", UM_REG_SITES, service, (unsigned long)instance);
}

static int lookup(const WCHAR *v, const WCHAR **names, int n, int dflt)
{
    for (int i = 0; i < n; i++)
        if (lstrcmpiW(v, names[i]) == 0) return i;
    return dflt;
}

LONG um_site_read(HKEY hklm, const WCHAR *service, DWORD instance, um_site *s)
{
    WCHAR path[200];
    WCHAR buf[MAX_PATH];
    HKEY k;
    DWORD type;
    DWORD size;
    DWORD dw;
    LONG r;

    um_site_defaults(s);
    site_key(path, service, instance);
    r = RegOpenKeyExW(hklm, path, 0, KEY_READ, &k);
    if (r == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (r != ERROR_SUCCESS) return r;

    size = sizeof dw;
    if (RegQueryValueExW(k, L"Enabled", NULL, &type, (BYTE *)&dw, &size) == ERROR_SUCCESS &&
        type == REG_DWORD)
        s->enabled = dw ? 1 : 0;
    size = sizeof buf - sizeof(WCHAR);
    if (RegQueryValueExW(k, L"Format", NULL, &type, (BYTE *)buf, &size) == ERROR_SUCCESS &&
        type == REG_SZ) {
        buf[size / sizeof(WCHAR)] = 0;
        s->format = (DWORD)lookup(buf, FMT_NAMES, 3, UM_FMT_AUTO);
    }
    size = sizeof buf - sizeof(WCHAR);
    if (RegQueryValueExW(k, L"Source", NULL, &type, (BYTE *)buf, &size) == ERROR_SUCCESS &&
        type == REG_SZ) {
        buf[size / sizeof(WCHAR)] = 0;
        s->source = (DWORD)lookup(buf, SRC_NAMES, 2, UM_SRC_DOCROOT);
    }
    size = sizeof dw;
    if (RegQueryValueExW(k, L"Debug", NULL, &type, (BYTE *)&dw, &size) == ERROR_SUCCESS &&
        type == REG_DWORD)
        s->debug = dw ? 1 : 0;
    size = sizeof s->rules_file - sizeof(WCHAR);
    if (RegQueryValueExW(k, L"RulesFile", NULL, &type, (BYTE *)s->rules_file, &size) == ERROR_SUCCESS &&
        type == REG_SZ)
        s->rules_file[size / sizeof(WCHAR)] = 0;
    else
        s->rules_file[0] = 0;
    RegCloseKey(k);
    return ERROR_SUCCESS;
}

static LONG set_sz(HKEY k, const WCHAR *name, const WCHAR *v)
{
    return RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v, (lstrlenW(v) + 1) * sizeof(WCHAR));
}

LONG um_site_write(HKEY hklm, const WCHAR *service, DWORD instance, const um_site *s)
{
    WCHAR path[200];
    HKEY k;
    DWORD disp;
    DWORD dw = s->enabled ? 1 : 0;
    LONG r;

    site_key(path, service, instance);
    r = RegCreateKeyExW(hklm, path, 0, NULL, 0, KEY_WRITE, NULL, &k, &disp);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExW(k, L"Enabled", 0, REG_DWORD, (const BYTE *)&dw, sizeof dw);
    if (r == ERROR_SUCCESS) r = set_sz(k, L"Format", FMT_NAMES[s->format <= 2 ? s->format : 0]);
    if (r == ERROR_SUCCESS) r = set_sz(k, L"Source", SRC_NAMES[s->source <= 1 ? s->source : 0]);
    if (r == ERROR_SUCCESS) r = set_sz(k, L"RulesFile", s->rules_file);
    dw = s->debug ? 1 : 0;
    if (r == ERROR_SUCCESS) r = RegSetValueExW(k, L"Debug", 0, REG_DWORD, (const BYTE *)&dw, sizeof dw);
    RegCloseKey(k);
    return r;
}
