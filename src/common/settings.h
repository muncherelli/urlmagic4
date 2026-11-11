/*
 * Per-site settings, shared by the IIS manager tab (writes) and the filter (reads).
 *
 *   HKLM\SOFTWARE\urlmagic4\Sites\<SERVICE>\<instance>      e.g. ...\Sites\W3SVC\3
 *     Enabled    REG_DWORD  1 = rewrite this site (default 1)
 *     Format     REG_SZ     "auto" | "htaccess" | "httpd.ini"           (default "auto")
 *     Source     REG_SZ     "docroot" | "file"                          (default "docroot")
 *     RulesFile  REG_SZ     full path of the rules file when Source = "file"
 *     Debug      REG_DWORD  1 = log every request with timing to urlmagic4.log (default 0)
 *
 * A site without a key uses the defaults: rules from .htaccess or httpd.ini in its home
 * directory, format picked by file name.
 */
#ifndef URLMAGIC4_SETTINGS_H
#define URLMAGIC4_SETTINGS_H

#include <windows.h>

#define UM_REG_SITES L"SOFTWARE\\urlmagic4\\Sites"

enum { UM_FMT_AUTO = 0, UM_FMT_HTACCESS = 1, UM_FMT_HTTPDINI = 2 };
enum { UM_SRC_DOCROOT = 0, UM_SRC_FILE = 1 };

typedef struct {
    DWORD enabled;
    DWORD format;                 /* UM_FMT_* */
    DWORD source;                 /* UM_SRC_* */
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
