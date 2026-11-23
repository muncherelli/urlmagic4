/*
 * The parts of setup that need no Windows: the command line, and editing IIS's FilterLoadOrder
 * (a comma-separated list). Header-only, with no C library, so setup.c (no CRT) and the native
 * tests (tests/test_setup.c) share it. Characters are wchar_t: 16 bits on Windows, like WCHAR.
 */
#ifndef UM_SETUPCORE_H
#define UM_SETUPCORE_H

#include <stddef.h>

/* ---------------------------------------------------------------- small string helpers */

static int sc_len(const wchar_t *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static wchar_t sc_lower(wchar_t c) { return (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c; }

/* ASCII case-insensitive: are a and b the same? */
static int sc_eqi(const wchar_t *a, const wchar_t *b)
{
    while (*a && sc_lower(*a) == sc_lower(*b)) { a++; b++; }
    return *a == *b;
}

/* same, for the first n characters of a (a is not necessarily terminated there) */
static int sc_eqni(const wchar_t *a, int n, const wchar_t *b)
{
    int i;
    for (i = 0; i < n; i++)
        if (!b[i] || sc_lower(a[i]) != sc_lower(b[i])) return 0;
    return b[n] == 0;
}

/* ---------------------------------------------------------------- FilterLoadOrder */

enum { ORDER_KEEP = 0, ORDER_FIRST, ORDER_LAST };   /* ORDER_KEEP: no /order given */

#define ORDER_LAST_NAME L"sspifilt"                  /* IIS's SSL/PCT filter: always last */

/* the next item of list at *p: trimmed, as *b and *len; 0 when there are no more */
static int order_next(const wchar_t **p, const wchar_t **b, int *len)
{
    const wchar_t *s = *p;
    while (*s) {
        const wchar_t *e = s;
        const wchar_t *t;
        while (*e && *e != L',') e++;
        t = e;
        while (s < t && *s == L' ') s++;
        while (t > s && t[-1] == L' ') t--;
        *p = *e ? e + 1 : e;
        if (t > s) { *b = s; *len = (int)(t - s); return 1; }
        s = *p;
    }
    *p = s;
    return 0;
}

/* case-insensitive: is name one of the items in list? */
static int order_has(const wchar_t *list, const wchar_t *name)
{
    const wchar_t *p = list;
    const wchar_t *b;
    int len;
    while (order_next(&p, &b, &len))
        if (sc_eqni(b, len, name)) return 1;
    return 0;
}

/* add item (len characters) to the comma-separated out, as far as it fits (cap characters);
   0 if it doesn't */
static int order_append(wchar_t *out, const wchar_t *item, int len, int cap)
{
    int n = sc_len(out);
    int i;
    if (n + (n ? 1 : 0) + len + 1 > cap) return 0;
    if (n) out[n++] = L',';
    for (i = 0; i < len; i++) out[n + i] = item[i];
    out[n + len] = 0;
    return 1;
}

/* list without name; 0 if it doesn't fit in out */
static int order_remove(const wchar_t *list, const wchar_t *name, wchar_t *out, int cap)
{
    const wchar_t *p = list;
    const wchar_t *b;
    int len;
    int ok = 1;
    out[0] = 0;
    while (order_next(&p, &b, &len))
        if (!sc_eqni(b, len, name) && !order_append(out, b, len, cap)) ok = 0;
    return ok;
}

/* list with name placed by pos, once, and no other item moved:
   ORDER_FIRST: first; ORDER_LAST: last (before sspifilt, if that is the final item);
   ORDER_KEEP (no /order): where name already is (several entries become one, at the first
   one's position), else right before sspifilt wherever it sits, or last if it isn't listed.
   0 if it doesn't fit in out. */
static int order_place(const wchar_t *list, const wchar_t *name, int pos, wchar_t *out, int cap)
{
    const wchar_t *p = list;
    const wchar_t *b;
    int len;
    int n = 0;                         /* items other than name */
    int ssl = -1;                      /* index of sspifilt among them */
    int first = -1;                    /* items other than name before its first entry */
    int ins;
    int idx = 0;
    int ok = 1;
    while (order_next(&p, &b, &len)) {
        if (sc_eqni(b, len, name)) { if (first < 0) first = n; continue; }
        if (ssl < 0 && sc_eqni(b, len, ORDER_LAST_NAME)) ssl = n;
        n++;
    }
    if (pos == ORDER_FIRST) ins = 0;
    else if (pos == ORDER_LAST) ins = (ssl >= 0 && ssl == n - 1) ? n - 1 : n;
    else if (first >= 0) ins = first;
    else ins = ssl >= 0 ? ssl : n;
    out[0] = 0;
    p = list;
    while (order_next(&p, &b, &len)) {
        if (sc_eqni(b, len, name)) continue;
        if (idx == ins && !order_append(out, name, sc_len(name), cap)) ok = 0;
        if (!order_append(out, b, len, cap)) ok = 0;
        idx++;
    }
    if (idx <= ins && !order_append(out, name, sc_len(name), cap)) ok = 0;
    return ok;
}

/* is name once in list where pos says (ORDER_KEEP: anywhere)? */
static int order_placed(const wchar_t *list, const wchar_t *name, int pos)
{
    const wchar_t *p = list;
    const wchar_t *b;
    int len;
    int idx = 0;
    int at = -1;
    int last_ok = 1;                   /* name is last, or only a final sspifilt follows */
    int after = 0;                     /* items after name */
    int after_ssl = 0;                 /* ... and the last of them is sspifilt */
    int copies = 0;
    while (order_next(&p, &b, &len)) {
        if (sc_eqni(b, len, name)) { copies++; if (at < 0) at = idx; after = 0; after_ssl = 0; }
        else if (at >= 0) { after++; after_ssl = sc_eqni(b, len, ORDER_LAST_NAME); }
        idx++;
    }
    if (at < 0 || copies > 1) return 0;   /* more than one entry: not current, to be repaired */
    last_ok = after == 0 || (after == 1 && after_ssl);
    if (pos == ORDER_FIRST) return at == 0;
    if (pos == ORDER_LAST) return last_ok;
    return 1;
}

/* does exit code rc mean "a reboot finishes the job" (3 reboot needed, 4 files staged)? Only
   those; every abort is an error. */
static int um_pending(int rc)
{
    return rc == 3 || rc == 4;
}

/* does the command line (argv[0] is the program) ask for /quiet? Looked at before parsing, so
   that even a wrong command line gets the one result line. */
static int um_quiet(int argc, wchar_t *const *argv)
{
    int i;
    for (i = 1; i < argc; i++)
        if ((argv[i][0] == L'/' || argv[i][0] == L'-') && sc_eqi(argv[i] + 1, L"quiet")) return 1;
    return 0;
}

/* ---------------------------------------------------------------- the command line */

enum { CMD_NONE = 0, CMD_INSTALL, CMD_UNINSTALL, CMD_VERSION, CMD_HELP, CMD_STATUS, CMD_SITE };

/* ---------------------------------------------------------------- per-site settings */

enum { SITE_NONE = 0, SITE_ENABLE, SITE_DISABLE, SITE_SET };
enum { SK_NONE = 0, SK_ENABLED, SK_FORMAT, SK_SOURCE, SK_RULESFILE, SK_DEBUG };

/* the spellings the registry holds (src/common/settings.c), by number */
static const wchar_t *const SC_FORMATS[] = { L"auto", L"htaccess", L"httpd.ini" };
static const wchar_t *const SC_SOURCES[] = { L"docroot", L"file" };

#define SITE_KV_MAX 8                    /* key=value pairs in one "site set" */
#define SITE_PATH_MAX 259                /* characters of a rules file's path (MAX_PATH - 1) */

/* which setting is the first n characters of k (SK_NONE if none) */
static int site_key_id(const wchar_t *k, int n)
{
    if (sc_eqni(k, n, L"enabled")) return SK_ENABLED;
    if (sc_eqni(k, n, L"format")) return SK_FORMAT;
    if (sc_eqni(k, n, L"source")) return SK_SOURCE;
    if (sc_eqni(k, n, L"rulesfile")) return SK_RULESFILE;
    if (sc_eqni(k, n, L"debug")) return SK_DEBUG;
    return SK_NONE;
}

/* the index of v in names (n of them), or -1 */
static int site_name_index(const wchar_t *v, const wchar_t *const *names, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (sc_eqi(v, names[i])) return i;
    return -1;
}

/* is v a site number: 1 to 9 digits, no leading zero? Sets *id. */
static int site_id_ok(const wchar_t *v, unsigned long *id)
{
    int i;
    unsigned long n = 0;
    if (v[0] == L'0') return 0;                    /* no leading zero: "03" would be site 3 twice */
    for (i = 0; v[i]; i++) {
        if (v[i] < L'0' || v[i] > L'9' || i >= 9) return 0;
        n = n * 10 + (unsigned long)(v[i] - L'0');
    }
    if (i == 0 || n == 0) return 0;
    *id = n;
    return 1;
}

/* is v an acceptable value for setting key? NULL if so, else why not. *num gets the number the
   registry stores (0/1, or the index into SC_FORMATS / SC_SOURCES); unused for rulesfile. A
   rules file is a full path (drive or UNC) or empty, which clears it. */
static const char *site_value_check(int key, const wchar_t *v, int *num)
{
    int n;
    int i;
    int drive;
    int unc;
    switch (key) {
    case SK_ENABLED:
    case SK_DEBUG:
        if (sc_eqi(v, L"1")) { *num = 1; return NULL; }
        if (sc_eqi(v, L"0")) { *num = 0; return NULL; }
        return key == SK_ENABLED ? "enabled must be 0 or 1" : "debug must be 0 or 1";
    case SK_FORMAT:
        *num = site_name_index(v, SC_FORMATS, 3);
        return *num < 0 ? "format must be auto, htaccess or httpd.ini" : NULL;
    case SK_SOURCE:
        *num = site_name_index(v, SC_SOURCES, 2);
        return *num < 0 ? "source must be docroot or file" : NULL;
    case SK_RULESFILE:
        n = sc_len(v);
        if (n == 0) return NULL;
        if (n > SITE_PATH_MAX) return "rulesfile is too long";
        for (i = 0; i < n; i++)
            if (v[i] < 32 || v[i] == L'"' || v[i] == L'<' || v[i] == L'>' || v[i] == L'|' ||
                v[i] == L'*' || v[i] == L'?')
                return "rulesfile has a character a path can't have";
        drive = n > 2 && ((v[0] >= L'A' && v[0] <= L'Z') || (v[0] >= L'a' && v[0] <= L'z')) &&
                v[1] == L':' && v[2] == L'\\';
        unc = n > 2 && v[0] == L'\\' && v[1] == L'\\';
        return drive || unc ? NULL : "rulesfile must be a full path (drive or UNC)";
    default:
        break;
    }
    return "unknown setting (enabled, format, source, rulesfile, debug)";
}

typedef struct {
    int cmd;
    int quiet;
    int norestart;
    int stopiis;
    int notab;
    int purge;
    int order;                         /* ORDER_KEEP, ORDER_FIRST or ORDER_LAST */
    const wchar_t *dir;                /* install's folder, or NULL */
    unsigned long site_id;             /* site: the W3SVC instance number */
    int site_op;                       /* site: SITE_ENABLE, SITE_DISABLE or SITE_SET */
    int npos;                          /* site: arguments seen after "site" */
    int nkv;                           /* site set: the key=value pairs ... */
    const wchar_t *kv[SITE_KV_MAX];    /* ... as typed */
    const char *err;                   /* why the command line is wrong, or NULL */
} um_args;

/* does arg (a switch such as "/quiet" or "-quiet") say name? */
static int sw_is(const wchar_t *arg, const wchar_t *name)
{
    return (arg[0] == L'/' || arg[0] == L'-') && sc_eqi(arg + 1, name);
}

/* one argument of "site": the number, then enable, disable or set, then for set the key=value
   pairs. Returns 0, 2 (not a command line) or 6 (a number, key or value that isn't valid). */
static int um_parse_site(const wchar_t *s, um_args *a)
{
    int n = 0;
    int num;
    if (a->npos == 0) {
        a->npos = 1;
        if (!site_id_ok(s, &a->site_id)) { a->err = "site needs a number from 1 to 999999999"; return 6; }
        return 0;
    }
    if (a->npos == 1) {
        a->npos = 2;
        if (sc_eqi(s, L"enable")) a->site_op = SITE_ENABLE;
        else if (sc_eqi(s, L"disable")) a->site_op = SITE_DISABLE;
        else if (sc_eqi(s, L"set")) a->site_op = SITE_SET;
        else { a->err = "site needs enable, disable or set"; return 2; }
        return 0;
    }
    if (a->site_op != SITE_SET) { a->err = "unexpected argument"; return 2; }
    while (s[n] && s[n] != L'=') n++;
    if (!s[n]) { a->err = "site set needs key=value"; return 2; }
    if (a->nkv >= SITE_KV_MAX) { a->err = "too many settings in one site set"; return 2; }
    a->err = site_value_check(site_key_id(s, n), s + n + 1, &num);
    if (a->err) return 6;
    a->kv[a->nkv++] = s;
    return 0;
}

/* argv[0] is the program. Returns 0, or nonzero with a->err set: 2 for a wrong command line,
   6 for a site number or setting that isn't valid. Switches may come before or after the
   command. */
static int um_parse(int argc, wchar_t *const *argv, um_args *a)
{
    int i;
    a->cmd = CMD_NONE; a->quiet = 0; a->norestart = 0; a->stopiis = 0; a->notab = 0; a->purge = 0;
    a->order = ORDER_KEEP; a->dir = NULL; a->err = NULL;
    a->site_id = 0; a->site_op = SITE_NONE; a->npos = 0; a->nkv = 0;
    for (i = 1; i < argc; i++) {
        const wchar_t *s = argv[i];
        if (sw_is(s, L"?") || sw_is(s, L"h") || (a->cmd == CMD_NONE && sc_eqi(s, L"help"))) {
            a->cmd = CMD_HELP;
            return 0;
        }
        if (sw_is(s, L"quiet")) a->quiet = 1;
        else if (sw_is(s, L"norestart")) a->norestart = 1;
        else if (sw_is(s, L"stopiis")) a->stopiis = 1;
        else if (sw_is(s, L"notab")) a->notab = 1;
        else if (sw_is(s, L"purge")) a->purge = 1;
        else if (sw_is(s, L"remove")) {
            if (a->cmd != CMD_NONE) { a->err = "more than one command"; return 2; }
            a->cmd = CMD_UNINSTALL;
        } else if (sw_is(s, L"order")) {
            if (i + 1 >= argc) { a->err = "/order needs first or last"; return 2; }
            s = argv[++i];
            if (sc_eqi(s, L"first")) a->order = ORDER_FIRST;
            else if (sc_eqi(s, L"last")) a->order = ORDER_LAST;
            else { a->err = "/order needs first or last"; return 2; }
        } else if (s[0] == L'/' || s[0] == L'-') {
            a->err = "unknown switch";
            return 2;
        } else if (a->cmd == CMD_NONE) {
            if (sc_eqi(s, L"install")) a->cmd = CMD_INSTALL;
            else if (sc_eqi(s, L"uninstall")) a->cmd = CMD_UNINSTALL;
            else if (sc_eqi(s, L"version")) a->cmd = CMD_VERSION;
            else if (sc_eqi(s, L"status")) a->cmd = CMD_STATUS;
            else if (sc_eqi(s, L"site")) a->cmd = CMD_SITE;
            else { a->err = "unknown command"; return 2; }
        } else if (a->cmd == CMD_SITE) {
            int rc = um_parse_site(s, a);
            if (rc) return rc;
        } else if (a->cmd == CMD_INSTALL && !a->dir) {
            a->dir = s;
        } else {
            a->err = "unexpected argument";
            return 2;
        }
    }
    if (a->cmd == CMD_NONE) { a->err = "no command given"; return 2; }
    if (a->cmd == CMD_SITE && a->site_op == SITE_NONE) {
        a->err = a->npos ? "site needs enable, disable or set" : "site needs a number";
        return 2;
    }
    if (a->cmd == CMD_SITE && a->site_op == SITE_SET && !a->nkv) { a->err = "site set needs key=value"; return 2; }
    if (a->purge && a->cmd != CMD_UNINSTALL) { a->err = "/purge is for uninstall"; return 2; }
    if (a->order != ORDER_KEEP && a->cmd != CMD_INSTALL) { a->err = "/order is for install"; return 2; }
    if (a->notab && a->cmd != CMD_INSTALL) { a->err = "/notab is for install"; return 2; }
    if (a->norestart && a->stopiis) { a->err = "/norestart and /stopiis can't be used together"; return 2; }
    return 0;
}

#endif
