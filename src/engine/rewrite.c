/*
 * Apache mod_rewrite-style rules (a subset; see docs/spec.md):
 *   RewriteEngine, RewriteBase, RewriteCond, RewriteRule, Redirect, RedirectMatch.
 * Rules run once, top to bottom, against the URL path without its leading "/" (as in an
 * Apache .htaccess). Regexes are PCRE2.
 */
#ifndef PCRE2_CODE_UNIT_WIDTH
#define PCRE2_CODE_UNIT_WIDTH 8
#endif
#ifndef PCRE2_STATIC
#define PCRE2_STATIC
#endif
#include <stdlib.h>
#include <string.h>
#include "pcre2.h"
#include "rewrite.h"

#define MAX_CAPS 10                 /* $0-$9, %0-%9 */
#define VAR_CAP 2048

/* ---------------------------------------------------------------- small helpers */

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ieq(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) a++, b++;
    return *a == 0 && *b == 0;
}

static int istarts(const char *s, const char *prefix)
{
    while (*prefix && lower((unsigned char)*s) == lower((unsigned char)*prefix)) s++, prefix++;
    return *prefix == 0;
}

static char *dup_n(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static char *dup(const char *s) { return dup_n(s, strlen(s)); }

/* decimal number; 0 if s isn't one */
static int to_int(const char *s)
{
    int v = 0;
    if (!*s) return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000) return 0;
        v = v * 10 + (*s - '0');
    }
    return v;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* growable string */
typedef struct { char *s; size_t len, cap; int oom; } sbuf;

static void sb_addn(sbuf *b, const char *t, size_t n)
{
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 128;
        char *s;
        while (cap < b->len + n + 1) cap *= 2;
        s = malloc(cap);
        if (!s) { b->oom = 1; return; }
        if (b->s) { memcpy(s, b->s, b->len); free(b->s); }
        b->s = s;
        b->cap = cap;
    }
    memcpy(b->s + b->len, t, n);
    b->len += n;
    b->s[b->len] = 0;
}

static void sb_add(sbuf *b, const char *t) { sb_addn(b, t, strlen(t)); }
static void sb_addc(sbuf *b, char c) { sb_addn(b, &c, 1); }

/* take ownership of the string (never NULL unless out of memory) */
static char *sb_take(sbuf *b)
{
    if (b->oom) { free(b->s); return NULL; }
    if (!b->s) return dup("");
    return b->s;
}

/* %XX -> byte (the whole string; "+" stays "+") */
static char *url_decode(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    size_t i, j = 0;
    if (!d) return NULL;
    for (i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n &&
            hexval((unsigned char)s[i + 1]) >= 0 && hexval((unsigned char)s[i + 2]) >= 0) {
            d[j++] = (char)(hexval((unsigned char)s[i + 1]) * 16 + hexval((unsigned char)s[i + 2]));
            i += 2;
        } else {
            d[j++] = s[i];
        }
    }
    d[j] = 0;
    return d;
}

/* Escape what may not appear raw in a URL; keeps reserved characters and existing %XX. */
static void sb_add_escaped(sbuf *b, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        int bad = c <= 0x20 || c >= 0x7f || strchr("\"<>\\^`{|}", c) != NULL;
        if (c == '%' && !(hexval((unsigned char)s[1]) >= 0 && hexval((unsigned char)s[2]) >= 0)) bad = 1;
        if (bad) {
            char e[3] = { '%', hex[c >> 4], hex[c & 15] };
            sb_addn(b, e, 3);
        } else {
            sb_addc(b, (char)c);
        }
    }
}

/* ---------------------------------------------------------------- rule model */

enum { C_REGEX, C_FILE, C_DIR, C_EQ, C_LT, C_GT };
enum { R_REWRITE, R_REDIRECT, R_REDIRECTMATCH };

enum {
    F_L = 1, F_NC = 2, F_R = 4, F_QSA = 8, F_QSD = 16, F_F = 32, F_G = 64, F_NE = 128,
    F_C = 256, F_END = 512, F_OR = 1024
};

typedef struct {
    char *test;          /* TestString template */
    int kind, negate, flags;
    char *str;           /* C_EQ / C_LT / C_GT */
    pcre2_code *re;      /* C_REGEX */
} cond;

typedef struct {
    int type, negate, flags, status, skip, line;
    pcre2_code *re;      /* R_REWRITE, R_REDIRECTMATCH */
    char *prefix;        /* R_REDIRECT */
    char *subst;         /* NULL = "-" (no substitution); for redirects the target URL */
    cond *conds;
    int nconds;
} rule;

struct um_rules {
    rule *rules;
    int n, cap;
    int engine_on;
    char *base;          /* RewriteBase, always ends with "/" */
};

typedef struct {
    um_log_fn log;
    void *ctx;
    int line;
} parser;

static void say(parser *p, const char *a, const char *b)
{
    sbuf m = {0};
    char *s;
    if (!p->log) return;
    sb_add(&m, a);
    if (b) { sb_add(&m, b); }
    s = sb_take(&m);
    if (s) { p->log(p->ctx, p->line, s); free(s); }
}

static void free_cond(cond *c)
{
    free(c->test);
    free(c->str);
    if (c->re) pcre2_code_free(c->re);
}

static void free_rule(rule *r)
{
    int i;
    if (r->re) pcre2_code_free(r->re);
    free(r->prefix);
    free(r->subst);
    for (i = 0; i < r->nconds; i++) free_cond(&r->conds[i]);
    free(r->conds);
}

void um_rules_free(um_rules *r)
{
    int i;
    if (!r) return;
    for (i = 0; i < r->n; i++) free_rule(&r->rules[i]);
    free(r->rules);
    free(r->base);
    free(r);
}

int um_rules_count(const um_rules *r) { return r ? r->n : 0; }

static pcre2_code *compile(parser *p, const char *pat, int nocase)
{
    int err;
    PCRE2_SIZE off;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pat, PCRE2_ZERO_TERMINATED,
                                   nocase ? PCRE2_CASELESS : 0, &err, &off, NULL);
    if (!re) {
        PCRE2_UCHAR msg[160];
        sbuf m = {0};
        char *s;
        pcre2_get_error_message(err, msg, sizeof msg);
        sb_add(&m, "bad regular expression \"");
        sb_add(&m, pat);
        sb_add(&m, "\": ");
        sb_add(&m, (const char *)msg);
        s = sb_take(&m);
        if (s) { say(p, s, NULL); free(s); }
    }
    return re;
}

/* ---------------------------------------------------------------- tokenizer */

#define MAX_ARGS 8

/* Split a line into arguments: whitespace separated, "double quotes" group (with \" inside).
   Unquoted backslashes are kept (regexes need them). Returns the count, or -1 if too many. */
static int split(char *line, char **argv)
{
    int n = 0;
    char *s = line;
    for (;;) {
        char *out;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (n == MAX_ARGS) return -1;
        out = s;
        argv[n++] = s;
        if (*s == '"') {
            s++;
            while (*s && *s != '"') {
                if (*s == '\\' && s[1] == '"') s++;
                *out++ = *s++;
            }
            if (*s) s++;
        } else {
            while (*s && *s != ' ' && *s != '\t') *out++ = *s++;
            if (*s) s++;
        }
        *out = 0;
    }
    return n;
}

/* "[L,NC,R=301]" -> flags; FALSE (after logging) on an unknown flag */
static int parse_flags(parser *p, const char *spec, int is_cond, int *flags, int *status, int *skip)
{
    char buf[256];
    char *s, *e;
    size_t n = strlen(spec);

    if (n < 2 || spec[0] != '[' || spec[n - 1] != ']' || n - 2 >= sizeof buf) {
        say(p, "flags must look like [L,NC]: ", spec);
        return 0;
    }
    memcpy(buf, spec + 1, n - 2);
    buf[n - 2] = 0;
    for (s = buf; *s; s = e) {
        char *val = NULL;
        e = s;
        while (*e && *e != ',') e++;
        if (*e) *e++ = 0;
        while (*s == ' ') s++;
        if (strchr(s, '=')) { val = strchr(s, '='); *val++ = 0; }
        if (!*s) continue;
        if (ieq(s, "NC") || ieq(s, "nocase")) *flags |= F_NC;
        else if (is_cond && (ieq(s, "OR") || ieq(s, "ornext"))) *flags |= F_OR;
        else if (is_cond) { say(p, "unknown RewriteCond flag: ", s); return 0; }
        else if (ieq(s, "L") || ieq(s, "last")) *flags |= F_L;
        else if (ieq(s, "END")) *flags |= F_END | F_L;
        else if (ieq(s, "QSA") || ieq(s, "qsappend")) *flags |= F_QSA;
        else if (ieq(s, "QSD") || ieq(s, "qsdiscard")) *flags |= F_QSD;
        else if (ieq(s, "F") || ieq(s, "forbidden")) *flags |= F_F | F_L;
        else if (ieq(s, "G") || ieq(s, "gone")) *flags |= F_G | F_L;
        else if (ieq(s, "NE") || ieq(s, "noescape")) *flags |= F_NE;
        else if (ieq(s, "C") || ieq(s, "chain")) *flags |= F_C;
        else if (ieq(s, "S") || ieq(s, "skip")) {
            *skip = val ? to_int(val) : 0;
            if (*skip <= 0) { say(p, "S= needs a number of rules to skip", NULL); return 0; }
        } else if (ieq(s, "R") || ieq(s, "redirect")) {
            *flags |= F_R | F_L;
            *status = 302;
            if (val) {
                if (ieq(val, "permanent")) *status = 301;
                else if (ieq(val, "temp")) *status = 302;
                else if (ieq(val, "seeother")) *status = 303;
                else *status = to_int(val);
                if (*status != 301 && *status != 302 && *status != 303 && *status != 307 && *status != 308) {
                    say(p, "unsupported redirect status: ", val);
                    return 0;
                }
            }
        } else {
            say(p, "unknown RewriteRule flag: ", s);
            return 0;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- directives */

static rule *add_rule(um_rules *r)
{
    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 16;
        rule *nr = malloc(sizeof(rule) * (size_t)cap);
        if (!nr) return NULL;
        if (r->rules) { memcpy(nr, r->rules, sizeof(rule) * (size_t)r->n); free(r->rules); }
        r->rules = nr;
        r->cap = cap;
    }
    memset(&r->rules[r->n], 0, sizeof(rule));
    return &r->rules[r->n++];
}

/* pending RewriteConds until the next RewriteRule */
typedef struct { cond *c; int n, cap; } condlist;

static void clear_conds(condlist *cl)
{
    int i;
    for (i = 0; i < cl->n; i++) free_cond(&cl->c[i]);
    free(cl->c);
    cl->c = NULL;
    cl->n = cl->cap = 0;
}

static int d_cond(parser *p, condlist *cl, int argc, char **argv)
{
    cond c;
    const char *pat;
    int flags = 0, status = 0, skip = 0;

    if (argc < 3 || argc > 4) { say(p, "RewriteCond needs: TestString CondPattern [flags]", NULL); return 0; }
    if (argc == 4 && !parse_flags(p, argv[3], 1, &flags, &status, &skip)) return 0;
    memset(&c, 0, sizeof c);
    c.flags = flags;
    pat = argv[2];
    if (*pat == '!') { c.negate = 1; pat++; }
    if (strcmp(pat, "-f") == 0 || strcmp(pat, "-F") == 0) c.kind = C_FILE;
    else if (strcmp(pat, "-d") == 0) c.kind = C_DIR;
    else if (*pat == '=') { c.kind = C_EQ; c.str = dup(pat + 1); }
    else if (*pat == '<') { c.kind = C_LT; c.str = dup(pat + 1); }
    else if (*pat == '>') { c.kind = C_GT; c.str = dup(pat + 1); }
    else if (*pat == '-' && pat[1] && !pat[2]) { say(p, "unsupported RewriteCond test: ", pat); return 0; }
    else {
        c.kind = C_REGEX;
        c.re = compile(p, pat, flags & F_NC);
        if (!c.re) return 0;
    }
    c.test = dup(argv[1]);
    if (cl->n == cl->cap) {
        int cap = cl->cap ? cl->cap * 2 : 4;
        cond *nc = malloc(sizeof(cond) * (size_t)cap);
        if (!nc) { free_cond(&c); return 0; }
        if (cl->c) { memcpy(nc, cl->c, sizeof(cond) * (size_t)cl->n); free(cl->c); }
        cl->c = nc;
        cl->cap = cap;
    }
    cl->c[cl->n++] = c;
    return 1;
}

static int d_rule(parser *p, um_rules *r, condlist *cl, int argc, char **argv)
{
    rule *ru;
    const char *pat;
    int flags = 0, status = 0, skip = 0, negate = 0;
    pcre2_code *re;

    if (argc < 3 || argc > 4) { say(p, "RewriteRule needs: Pattern Substitution [flags]", NULL); return 0; }
    if (argc == 4 && !parse_flags(p, argv[3], 0, &flags, &status, &skip)) return 0;
    pat = argv[1];
    if (*pat == '!') { negate = 1; pat++; }
    re = compile(p, pat, flags & F_NC);
    if (!re) return 0;
    ru = add_rule(r);
    if (!ru) { pcre2_code_free(re); return 0; }
    ru->type = R_REWRITE;
    ru->re = re;
    ru->negate = negate;
    ru->flags = flags;
    ru->status = status;
    ru->skip = skip;
    ru->line = p->line;
    ru->subst = strcmp(argv[2], "-") == 0 ? NULL : dup(argv[2]);
    ru->conds = cl->c;
    ru->nconds = cl->n;
    cl->c = NULL;
    cl->n = cl->cap = 0;
    return 1;
}

/* "permanent" / "temp" / "seeother" / "gone" / number; 0 if not a status word */
static int status_word(const char *s)
{
    if (ieq(s, "permanent")) return 301;
    if (ieq(s, "temp")) return 302;
    if (ieq(s, "seeother")) return 303;
    if (ieq(s, "gone")) return 410;
    if (s[0] >= '1' && s[0] <= '9' && strlen(s) == 3) return to_int(s);
    return 0;
}

static int d_redirect(parser *p, um_rules *r, int argc, char **argv, int match)
{
    rule *ru;
    int i = 1, status = 302;
    const char *from, *to = NULL;

    if (argc >= 2 && status_word(argv[1])) { status = status_word(argv[1]); i = 2; }
    if (argc - i < 1 || argc - i > 2) { say(p, match ? "RedirectMatch needs: [status] regex URL" : "Redirect needs: [status] URL-path URL", NULL); return 0; }
    from = argv[i];
    if (argc - i == 2) to = argv[i + 1];
    if (status != 410 && !to) { say(p, "missing target URL", NULL); return 0; }
    if (status != 410 && status != 301 && status != 302 && status != 303 && status != 307 && status != 308) {
        say(p, "unsupported redirect status: ", argv[1]);
        return 0;
    }
    ru = add_rule(r);
    if (!ru) return 0;
    ru->type = match ? R_REDIRECTMATCH : R_REDIRECT;
    ru->status = status;
    ru->line = p->line;
    ru->subst = to ? dup(to) : NULL;
    if (match) {
        ru->re = compile(p, from, 0);
        if (!ru->re) { r->n--; free_rule(ru); return 0; }
    } else {
        ru->prefix = dup(from);
    }
    return 1;
}

um_rules *um_parse(const char *text, size_t len, int dialect, um_log_fn log, void *logctx)
{
    um_rules *r = malloc(sizeof *r);
    parser p;
    condlist cl = {0};
    size_t i = 0;
    (void)dialect;

    if (!r) return NULL;
    memset(r, 0, sizeof *r);
    r->base = dup("/");
    p.log = log;
    p.ctx = logctx;
    p.line = 0;

    while (i < len) {
        sbuf line = {0};
        char *s, *argv[MAX_ARGS];
        int argc, first = p.line + 1;

        /* one logical line: physical lines joined where they end with a backslash */
        for (;;) {
            size_t start = i, end;
            while (i < len && text[i] != '\n') i++;
            end = i;
            if (i < len) i++;
            p.line++;
            if (end > start && text[end - 1] == '\r') end--;
            if (end > start && text[end - 1] == '\\' && i < len) {
                sb_addn(&line, text + start, end - start - 1);
                sb_addc(&line, ' ');
                continue;
            }
            sb_addn(&line, text + start, end - start);
            break;
        }
        s = sb_take(&line);
        if (!s) break;
        {
            int last = p.line;
            p.line = first;
            argc = split(s, argv);
            if (argc == -1) say(&p, "too many arguments", NULL);
            else if (argc == 0 || argv[0][0] == '#') ;
            else if (ieq(argv[0], "RewriteEngine")) {
                if (argc == 2 && (ieq(argv[1], "on") || ieq(argv[1], "off")))
                    r->engine_on = ieq(argv[1], "on");
                else say(&p, "RewriteEngine needs On or Off", NULL);
            } else if (ieq(argv[0], "RewriteBase")) {
                if (argc == 2 && argv[1][0] == '/') {
                    size_t n = strlen(argv[1]);
                    free(r->base);
                    r->base = malloc(n + 2);
                    if (r->base) {
                        memcpy(r->base, argv[1], n + 1);
                        if (argv[1][n - 1] != '/') { r->base[n] = '/'; r->base[n + 1] = 0; }
                    }
                } else say(&p, "RewriteBase needs a URL path starting with /", NULL);
            } else if (ieq(argv[0], "RewriteCond")) d_cond(&p, &cl, argc, argv);
            else if (ieq(argv[0], "RewriteRule")) d_rule(&p, r, &cl, argc, argv);
            else if (ieq(argv[0], "Redirect")) d_redirect(&p, r, argc, argv, 0);
            else if (ieq(argv[0], "RedirectMatch")) d_redirect(&p, r, argc, argv, 1);
            else if (istarts(argv[0], "<IfModule") || istarts(argv[0], "</IfModule")) ;
            else if (ieq(argv[0], "RewriteOptions") || ieq(argv[0], "Options") ||
                     ieq(argv[0], "DirectoryIndex")) say(&p, "ignored (not supported): ", argv[0]);
            else say(&p, "unknown directive: ", argv[0]);
            p.line = last;
        }
        free(s);
    }
    if (cl.n) { say(&p, "RewriteCond at the end of the file has no RewriteRule after it", NULL); }
    clear_conds(&cl);
    return r;
}

/* ---------------------------------------------------------------- applying */

typedef struct {
    const char *subj;           /* captures point into this */
    PCRE2_SIZE ov[MAX_CAPS * 2];
    int n;                      /* number of valid pairs */
} caps;

typedef struct {
    const um_env *env;
    const char *orig_path;      /* decoded, with leading "/" (REQUEST_URI) */
    char *path;                 /* current, decoded, with leading "/" */
    char *query;                /* current query string (no "?") */
    pcre2_match_data *md;
    int failed;
} state;

/* match re against s; fills c on success. 1 = match, 0 = no match, -1 = error */
static int run(state *st, pcre2_code *re, const char *s, caps *c)
{
    int rc = pcre2_match(re, (PCRE2_SPTR)s, PCRE2_ZERO_TERMINATED, 0, 0, st->md, NULL);
    PCRE2_SIZE *ov;
    int i;
    if (rc == PCRE2_ERROR_NOMATCH) return 0;
    if (rc < 0) { st->failed = 1; return -1; }
    ov = pcre2_get_ovector_pointer(st->md);
    if (rc == 0 || rc > MAX_CAPS) rc = MAX_CAPS;
    c->subj = s;
    c->n = rc;
    for (i = 0; i < rc * 2; i++) c->ov[i] = ov[i];
    return 1;
}

static void add_cap(sbuf *b, const caps *c, int n)
{
    if (!c || n >= c->n || c->ov[n * 2] == PCRE2_UNSET) return;
    sb_addn(b, c->subj + c->ov[n * 2], c->ov[n * 2 + 1] - c->ov[n * 2]);
}

/* $N, %N, %{VAR}, \x -> text */
static char *expand(state *st, const char *t, const caps *rc, const caps *cc)
{
    sbuf b = {0};
    char *buf = malloc(VAR_CAP);
    if (!buf) return NULL;
    while (*t) {
        if (*t == '\\' && t[1]) {
            sb_addc(&b, t[1]);
            t += 2;
        } else if (*t == '$' && t[1] >= '0' && t[1] <= '9') {
            add_cap(&b, rc, t[1] - '0');
            t += 2;
        } else if (*t == '%' && t[1] >= '0' && t[1] <= '9') {
            add_cap(&b, cc, t[1] - '0');
            t += 2;
        } else if (*t == '%' && t[1] == '{' && strchr(t, '}')) {
            const char *end = strchr(t, '}');
            char name[128];
            size_t n = (size_t)(end - t - 2);
            if (n < sizeof name) {
                const char *v = NULL;
                memcpy(name, t + 2, n);
                name[n] = 0;
                if (ieq(name, "REQUEST_URI")) v = st->orig_path;
                else if (ieq(name, "QUERY_STRING")) v = st->query;
                else if (st->env && st->env->var) {
                    buf[0] = 0;
                    v = st->env->var(st->env->ctx, name, st->path, buf, VAR_CAP);
                }
                if (v) sb_add(&b, v);
            }
            t = end + 1;
        } else {
            sb_addc(&b, *t++);
        }
    }
    free(buf);
    return sb_take(&b);
}

static int cmp(const char *a, const char *b) { return strcmp(a, b); }

/* evaluate one condition; updates *last (the %N captures) when a regex matches */
static int eval_cond(state *st, const cond *c, const caps *rc, caps *last, char **keep)
{
    char *v = expand(st, c->test, rc, last);
    int ok = 0;
    if (!v) { st->failed = 1; return 0; }
    switch (c->kind) {
    case C_FILE: ok = st->env && st->env->file_type && st->env->file_type(st->env->ctx, v) == UM_FT_FILE; break;
    case C_DIR:  ok = st->env && st->env->file_type && st->env->file_type(st->env->ctx, v) == UM_FT_DIR; break;
    case C_EQ:   ok = (c->flags & F_NC) ? ieq(v, c->str) : cmp(v, c->str) == 0; break;
    case C_LT:   ok = cmp(v, c->str) < 0; break;
    case C_GT:   ok = cmp(v, c->str) > 0; break;
    case C_REGEX: {
        caps m;
        int r = run(st, c->re, v, &m);
        ok = r == 1;
        if (ok && !c->negate) {
            /* captures must outlive v: hand v over to the caller to free later */
            *last = m;
            free(*keep);
            *keep = v;
            v = NULL;
        }
        break;
    }
    }
    free(v);
    return c->negate ? !ok : ok;
}

static int conds_ok(state *st, const rule *ru, const caps *rc, caps *last, char **keep)
{
    int i = 0;
    while (i < ru->nconds) {
        int group = 0;
        /* a run of [OR] conditions counts as one */
        for (;;) {
            if (eval_cond(st, &ru->conds[i], rc, last, keep)) group = 1;
            if (st->failed) return 0;
            if (!(ru->conds[i].flags & F_OR) || i + 1 >= ru->nconds) { i++; break; }
            i++;
        }
        if (!group) return 0;
    }
    return 1;
}

static int is_absolute_url(const char *s)
{
    return istarts(s, "http://") || istarts(s, "https://");
}

static void finish(um_result *out, int action, int status, char *url)
{
    out->action = action;
    out->status = status;
    out->url = url;
}

/* path (decoded, leading "/") + query -> escaped URL */
static char *build_url(const char *path, const char *query, int noescape)
{
    sbuf b = {0};
    if (noescape) sb_add(&b, path); else sb_add_escaped(&b, path);
    if (query && *query) {
        sb_addc(&b, '?');
        if (noescape) sb_add(&b, query); else sb_add_escaped(&b, query);
    }
    return sb_take(&b);
}

int um_apply(const um_rules *r, const char *url, const um_env *env, um_result *out)
{
    state st;
    const char *q = strchr(url, '?');
    size_t plen = q ? (size_t)(q - url) : strlen(url);
    int i, skip = 0, changed = 0, applied = 0;

    finish(out, UM_NONE, 0, NULL);
    out->line = 0;
    if (!r || r->n == 0) return UM_NONE;

    memset(&st, 0, sizeof st);
    st.env = env;
    st.path = url_decode(url, plen);
    st.query = dup(q ? q + 1 : "");
    st.md = pcre2_match_data_create(MAX_CAPS, NULL);
    if (!st.path || !st.query || !st.md) { st.failed = 1; goto done; }
    st.orig_path = st.path;
    {
        /* orig_path must stay put while path changes */
        char *o = dup(st.path);
        if (!o) { st.failed = 1; goto done; }
        st.orig_path = o;
    }

    for (i = 0; i < r->n; i++) {
        const rule *ru = &r->rules[i];
        caps rc, cc;
        char *keep = NULL;
        const char *subject;

        memset(&rc, 0, sizeof rc);
        memset(&cc, 0, sizeof cc);
        if (skip > 0) { skip--; continue; }

        if (ru->type == R_REDIRECT) {
            size_t n = strlen(ru->prefix);
            if (strncmp(st.path, ru->prefix, n) == 0 &&
                (st.path[n] == 0 || st.path[n] == '/' || (n && ru->prefix[n - 1] == '/'))) {
                sbuf b = {0};
                char *u;
                applied = ru->line;
                if (ru->status == 410) { finish(out, UM_GONE, 410, NULL); goto done; }
                sb_add(&b, ru->subst);
                sb_add(&b, st.path + n);
                u = sb_take(&b);
                if (!u) { st.failed = 1; goto done; }
                finish(out, UM_REDIRECT, ru->status, build_url(u, st.query, 0));
                free(u);
                goto done;
            }
            continue;
        }
        if (ru->type == R_REDIRECTMATCH) {
            int m = run(&st, ru->re, st.path, &rc);
            if (m < 0) goto done;
            if (m == 1) {
                char *u;
                applied = ru->line;
                if (ru->status == 410) { finish(out, UM_GONE, 410, NULL); goto done; }
                u = expand(&st, ru->subst, &rc, NULL);
                if (!u) { st.failed = 1; goto done; }
                finish(out, UM_REDIRECT, ru->status, build_url(u, NULL, 0));
                free(u);
                goto done;
            }
            continue;
        }

        /* RewriteRule */
        if (!r->engine_on) continue;
        subject = st.path + 1;          /* .htaccess style: no leading slash */
        {
            int m = run(&st, ru->re, subject, &rc);
            int ok;
            if (m < 0) goto done;
            ok = ru->negate ? m == 0 : m == 1;
            if (ru->negate) rc.n = 0;
            if (ok && ru->nconds) ok = conds_ok(&st, ru, &rc, &cc, &keep);
            if (st.failed) { free(keep); goto done; }
            if (!ok) {
                free(keep);
                /* a failed rule in a chain skips the rest of the chain */
                if (ru->flags & F_C)
                    while (i + 1 < r->n && (r->rules[i].flags & F_C)) i++;
                continue;
            }
        }

        applied = ru->line;
        if (ru->flags & F_F) { free(keep); finish(out, UM_FORBIDDEN, 403, NULL); goto done; }
        if (ru->flags & F_G) { free(keep); finish(out, UM_GONE, 410, NULL); goto done; }

        if (ru->subst) {
            char *s = expand(&st, ru->subst, &rc, &cc);
            char *qs, *np, *nq;
            free(keep);
            if (!s) { st.failed = 1; goto done; }
            if (is_absolute_url(s)) {
                /* a full URL always means an external redirect */
                int status = (ru->flags & F_R) ? ru->status : 302;
                char *u;
                qs = strchr(s, '?');
                if (!qs && !(ru->flags & F_QSD) && st.query[0]) {
                    sbuf b = {0};
                    sb_add(&b, s); sb_addc(&b, '?'); sb_add(&b, st.query);
                    free(s);
                    s = sb_take(&b);
                    if (!s) { st.failed = 1; goto done; }
                }
                if (ru->flags & F_NE) u = dup(s);
                else { sbuf b = {0}; sb_add_escaped(&b, s); u = sb_take(&b); }
                free(s);
                finish(out, UM_REDIRECT, status, u);
                goto done;
            }
            /* split the substitution into path and query */
            qs = strchr(s, '?');
            if (qs) *qs++ = 0;
            if (s[0] == '/') np = dup(s);
            else {
                sbuf b = {0};
                sb_add(&b, r->base);
                sb_add(&b, s);
                np = sb_take(&b);
            }
            if (qs) {
                sbuf b = {0};
                sb_add(&b, qs);
                if ((ru->flags & F_QSA) && st.query[0]) {
                    if (*qs) sb_addc(&b, '&');
                    sb_add(&b, st.query);
                }
                nq = sb_take(&b);
            } else {
                nq = dup((ru->flags & F_QSD) ? "" : st.query);
            }
            free(s);
            if (!np || !nq) { free(np); free(nq); st.failed = 1; goto done; }
            free(st.path);
            free(st.query);
            st.path = np;
            st.query = nq;
            changed = 1;

            if (ru->flags & F_R) {
                finish(out, UM_REDIRECT, ru->status, build_url(st.path, st.query, ru->flags & F_NE));
                goto done;
            }
        } else {
            free(keep);
            if (ru->flags & F_QSD) { st.query[0] = 0; changed = 1; }
        }
        if (ru->skip) skip = ru->skip;
        if (ru->flags & (F_L | F_END)) break;
    }

    if (changed) finish(out, UM_REWRITE, 0, build_url(st.path, st.query, 0));

done:
    out->line = applied;
    if (st.failed) { free(out->url); finish(out, UM_FAIL, 500, NULL); }
    else if ((out->action == UM_REWRITE || out->action == UM_REDIRECT) && !out->url) finish(out, UM_FAIL, 500, NULL);
    if (st.orig_path != st.path) free((char *)st.orig_path);
    free(st.path);
    free(st.query);
    if (st.md) pcre2_match_data_free(st.md);
    return out->action;
}

void um_result_free(um_result *res)
{
    free(res->url);
    res->url = NULL;
}
