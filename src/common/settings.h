/*
 * Per-site settings, shared by the IIS manager tab (writes) and the filter (reads).
 *
 *   HKLM\SOFTWARE\URLMagic4\Sites\<SERVICE>\<instance>      e.g. ...\Sites\W3SVC\3
 *
 * The key mirrors IIS's own metabase path /LM/<SERVICE>/<instance>: the service ("W3SVC" for
 * web sites) and the site's number on that service, so any number of sites, and other IIS
 * services, each get their own key. Every server keeps its own settings in its own registry
 * (the tab writes to the registry of whichever server Internet Service Manager is managing).
 *     Enabled    REG_DWORD  1 = rewrite this site (default 0: off)
 *     Format     REG_SZ     "auto" | "htaccess" | "httpd.ini"           (default "auto")
 *     Source     REG_SZ     "docroot" | "file"                          (default "docroot")
 *     RulesFile  REG_SZ     full path of the rules file when Source = "file"
 *     Debug      REG_DWORD  1 = log every request with timing to urlmagic4.log (default 0)
 *
 * A site without a key is off: URL Magic leaves it alone (no file checks at all) until
 * rewriting is turned on in the site's URL Magic tab.
 */
#ifndef URLMAGIC4_SETTINGS_H
#define URLMAGIC4_SETTINGS_H

#include <windows.h>

#define UM_REG_ROOT  L"SOFTWARE\\URLMagic4"
#define UM_REG_SITES UM_REG_ROOT L"\\Sites"

enum { UM_FMT_AUTO = 0, UM_FMT_HTACCESS = 1, UM_FMT_HTTPDINI = 2 };
enum { UM_SRC_DOCROOT = 0, UM_SRC_FILE = 1 };

typedef struct {
    DWORD enabled;
    DWORD format;                 /* one of the UM_FMT_ values */
    DWORD source;                 /* one of the UM_SRC_ values */
    WCHAR rules_file[MAX_PATH];
    DWORD debug;                  /* log each request and its timing */
} um_site;

void um_site_defaults(um_site *s);

/* hklm: HKEY_LOCAL_MACHINE of the target machine (local or from RegConnectRegistry).
   service: e.g. L"W3SVC". Both return a Win32 error code (ERROR_SUCCESS on success);
   reading a site that has no key yields the defaults and ERROR_SUCCESS. */
LONG um_site_read(HKEY hklm, const WCHAR *service, DWORD instance, um_site *s);
LONG um_site_write(HKEY hklm, const WCHAR *service, DWORD instance, const um_site *s);

#endif
