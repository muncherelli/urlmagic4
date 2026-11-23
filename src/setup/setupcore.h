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

/* list with name first or last (pos is ORDER_FIRST or ORDER_LAST), once; sspifilt, if the list
   has it, stays last whatever pos is. 0 if it doesn't fit in out. */
static int order_place(const wchar_t *list, const wchar_t *name, int pos, wchar_t *out, int cap)
{
    const wchar_t *p = list;
    const wchar_t *b;
    int len;
    int ok = 1;
    int ssl = order_has(list, ORDER_LAST_NAME);
    out[0] = 0;
    if (pos == ORDER_FIRST && !order_append(out, name, sc_len(name), cap)) ok = 0;
    while (order_next(&p, &b, &len)) {
        if (sc_eqni(b, len, name) || sc_eqni(b, len, ORDER_LAST_NAME)) continue;
        if (!order_append(out, b, len, cap)) ok = 0;
    }
    if (pos != ORDER_FIRST && !order_append(out, name, sc_len(name), cap)) ok = 0;
    if (ssl && !order_append(out, ORDER_LAST_NAME, sc_len(ORDER_LAST_NAME), cap)) ok = 0;
    return ok;
}

/* is name in list where pos says (ORDER_KEEP: anywhere), with sspifilt last? */
static int order_placed(const wchar_t *list, const wchar_t *name, int pos)
{
    const wchar_t *p = list;
    const wchar_t *b;
    int len;
    int idx = 0;
    int at = -1;
    int last_ok = 1;                   /* name is last, ignoring sspifilt at the end */
    int ssl_last = 1;                  /* sspifilt, if present, is the final item */
    int prev_ssl = 0;
    if (!order_has(list, name)) return 0;
    while (order_next(&p, &b, &len)) {
        int is_name = sc_eqni(b, len, name);
        int is_ssl = sc_eqni(b, len, ORDER_LAST_NAME);
        if (prev_ssl) ssl_last = 0;
        if (is_name) { at = idx; last_ok = 1; }
        else if (!is_ssl) last_ok = 0;
        prev_ssl = is_ssl;
        idx++;
    }
    if (!ssl_last) return 0;
    if (pos == ORDER_FIRST) return at == 0;
    if (pos == ORDER_LAST) return last_ok;
    return 1;
}

/* ---------------------------------------------------------------- the command line */

enum { CMD_NONE = 0, CMD_INSTALL, CMD_UNINSTALL, CMD_VERSION, CMD_HELP };

typedef struct {
    int cmd;
    int quiet;
    int norestart;
    int stopiis;
    int notab;
    int purge;
    int order;                         /* ORDER_KEEP, ORDER_FIRST or ORDER_LAST */
    const wchar_t *dir;                /* install's folder, or NULL */
    const char *err;                   /* why the command line is wrong, or NULL */
} um_args;

/* does arg (a switch such as "/quiet" or "-quiet") say name? */
static int sw_is(const wchar_t *arg, const wchar_t *name)
{
    return (arg[0] == L'/' || arg[0] == L'-') && sc_eqi(arg + 1, name);
}

/* argv[0] is the program. Returns 0, or nonzero with a->err set. Switches may come before or
   after the command. */
static int um_parse(int argc, wchar_t *const *argv, um_args *a)
{
    int i;
    a->cmd = CMD_NONE; a->quiet = 0; a->norestart = 0; a->stopiis = 0; a->notab = 0; a->purge = 0;
    a->order = ORDER_KEEP; a->dir = NULL; a->err = NULL;
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
            else { a->err = "unknown command"; return 2; }
        } else if (a->cmd == CMD_INSTALL && !a->dir) {
            a->dir = s;
        } else {
            a->err = "unexpected argument";
            return 2;
        }
    }
    if (a->cmd == CMD_NONE) { a->err = "no command given"; return 2; }
    if (a->purge && a->cmd != CMD_UNINSTALL) { a->err = "/purge is for uninstall"; return 2; }
    if (a->order != ORDER_KEEP && a->cmd != CMD_INSTALL) { a->err = "/order is for install"; return 2; }
    if (a->notab && a->cmd != CMD_INSTALL) { a->err = "/notab is for install"; return 2; }
    if (a->norestart && a->stopiis) { a->err = "/norestart and /stopiis can't be used together"; return 2; }
    return 0;
}

#endif
