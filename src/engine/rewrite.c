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

/* ---------------------------------------------------------------- rule model */

enum { C_REGEX, C_FILE, C_DIR, C_EQ, C_LT, C_GT };
enum { R_REWRITE, R_REDIRECT, R_REDIRECTMATCH };

enum {
    F_L = 1, F_NC = 2, F_R = 4, F_QSA = 8, F_QSD = 16, F_F = 32, F_G = 64, F_NE = 128,
    F_C = 256, F_END = 512, F_OR = 1024
};

/* Patterns simple enough to match without PCRE2: a plain string with optional ^ and $, or a
   catch-all such as ^(.*)$. Subjects containing a newline still go to PCRE2 ('.' and '$'
   treat it specially). */
enum { FP_NONE = 0, FP_LIT, FP_ALL };

typedef struct {
    int kind;
    int start, end;      /* FP_LIT: anchored with ^ / $ */
    int nocase;
    int groups;          /* capture pairs a match fills: 1, or 2 for (.*) */
    char *lit;           /* FP_LIT: the string, escapes resolved */
    size_t len;
} fastpat;

typedef struct {
    char *test;          /* TestString template */
    int kind, negate, flags;
    char *str;           /* C_EQ / C_LT / C_GT */
    pcre2_code *re;      /* C_REGEX */
    fastpat fp;
} cond;

typedef struct {
    int type, negate, flags, status, skip, line;
    pcre2_code *re;      /* R_REWRITE, R_REDIRECTMATCH */
    fastpat fp;
    char *prefix;        /* R_REDIRECT */
    size_t plen;         /* strlen(prefix) */
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
    free(c->fp.lit);
    free(c->test);
    free(c->str);
    if (c->re) pcre2_code_free(c->re);
}

static void free_rule(rule *r)
{
    int i;
    free(r->fp.lit);
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

/* Fill f if pat can be matched without PCRE2 (see fastpat); leaves f->kind FP_NONE if not. */
static void analyze(const char *pat, int nocase, fastpat *f)
{
    size_t n = strlen(pat), i, j = 0;
    char *lit;

    memset(f, 0, sizeof *f);
    f->nocase = nocase;
    if (*pat == '^') { f->start = 1; pat++; n--; }
    if (n && pat[n - 1] == '$') {
        if (n >= 2 && pat[n - 2] == '\\') return;          /* \$ : too subtle, leave to PCRE2 */
        f->end = 1;
        n--;
    }
    if ((n == 4 && memcmp(pat, "(.*)", 4) == 0) || (n == 2 && memcmp(pat, ".*", 2) == 0)) {
        f->kind = FP_ALL;                       /* matches the whole subject, anchored or not */
        f->groups = n == 4 ? 2 : 1;
        return;
    }
    lit = malloc(n + 1);
    if (!lit) return;
    for (i = 0; i < n; i++) {
        char c = pat[i];
        if (c == '\\') {
            char e = pat[++i];
            /* \. \/ \- ... are the character itself; \d \w \b etc. are not */
            if (i >= n || (e >= '0' && e <= '9') || (e >= 'a' && e <= 'z') || (e >= 'A' && e <= 'Z') ||
                (unsigned char)e >= 0x80) { free(lit); return; }
            lit[j++] = e;
        } else if (strchr(".[]()*+?{}|^$", c)) {
            free(lit);
            return;
        } else {
            lit[j++] = c;
        }
    }
    lit[j] = 0;
    f->kind = FP_LIT;
    f->groups = 1;
    f->lit = lit;
    f->len = j;
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
        analyze(pat, flags & F_NC, &c.fp);
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
    analyze(pat, flags & F_NC, &ru->fp);
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
        analyze(from, 0, &ru->fp);
    } else {
        ru->prefix = dup(from);
        if (!ru->prefix) { r->n--; free_rule(ru); return 0; }
        ru->plen = strlen(ru->prefix);
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

/* ---------------------------------------------------------------- per-request memory */

/* Strings made while applying rules are bump-allocated from the scratch's block and never
   freed one by one; the whole block is reused by the next request. A request that outgrows
   it gets overflow chunks, freed when the scratch is next reset. */
#ifndef SCRATCH_BLOCK
#define SCRATCH_BLOCK 16384
#endif
#ifndef CHUNK_MIN
#define CHUNK_MIN     4096
#endif
#define STRING_MAX    (16u * 1024 * 1024)

typedef struct chunk { struct chunk *next; } chunk;

struct um_scratch {
    pcre2_match_data *md;       /* keeps PCRE2's backtracking frames between requests too */
    char *cur;                  /* start of the free space */
    size_t left;                /* bytes free at cur */
    chunk *extra;               /* overflow chunks */
    int oom;
    char block[SCRATCH_BLOCK];
};

um_scratch *um_scratch_new(void)
{
    um_scratch *a = malloc(sizeof *a);
    if (!a) return NULL;
    a->md = pcre2_match_data_create(MAX_CAPS, NULL);
    if (!a->md) { free(a); return NULL; }
    a->extra = NULL;
    a->cur = a->block;
    a->left = sizeof a->block;
    a->oom = 0;
    return a;
}

static void scratch_reset(um_scratch *a)
{
    while (a->extra) {
        chunk *c = a->extra;
        a->extra = c->next;
        free(c);
    }
    a->cur = a->block;
    a->left = sizeof a->block;
    a->oom = 0;
}

void um_scratch_free(um_scratch *a)
{
    if (!a) return;
    scratch_reset(a);
    pcre2_match_data_free(a->md);
    free(a);
}

/* a string being built at the scratch's free space; only one may be open at a time */
typedef struct {
    um_scratch *a;
    char *s;
    size_t len, cap;
} abuf;

static void ab_open(abuf *b, um_scratch *a)
{
    b->a = a;
    b->s = a->cur;
    b->len = 0;
    b->cap = a->left;
}

/* room for n more bytes and the terminating NUL */
static int ab_reserve(abuf *b, size_t n)
{
    um_scratch *a = b->a;
    size_t need = b->len + n + 1, cap;
    chunk *c;
    if (need <= b->cap) return 1;
    if (a->oom || need > STRING_MAX) { a->oom = 1; return 0; }
#ifdef UM_EXACT_CHUNKS
    cap = need;                 /* tests: every string in its own allocation, so ASan sees it */
#else
    cap = need * 2 > CHUNK_MIN ? need * 2 : CHUNK_MIN;
#endif
    c = malloc(sizeof *c + cap);
    if (!c) { a->oom = 1; return 0; }
    c->next = a->extra;
    a->extra = c;
    if (b->len) memcpy(c + 1, b->s, b->len);
    b->s = (char *)(c + 1);
    b->cap = cap;
    a->cur = b->s;              /* the rest of the old space is given up */
    a->left = cap;
    return 1;
}

static void ab_addn(abuf *b, const char *t, size_t n)
{
    if (n && ab_reserve(b, n)) {
        memcpy(b->s + b->len, t, n);
        b->len += n;
    }
}

static void ab_add(abuf *b, const char *t) { ab_addn(b, t, strlen(t)); }

static void ab_addc(abuf *b, char c)
{
    if (b->len + 2 <= b->cap || ab_reserve(b, 1)) b->s[b->len++] = c;
}

/* finish the string (NULL if out of memory); it stays valid until the scratch is reset */
static char *ab_close(abuf *b, size_t *len)
{
    um_scratch *a = b->a;
    if (a->oom || !ab_reserve(b, 0)) return NULL;
    b->s[b->len] = 0;
    a->cur = b->s + b->len + 1;
    a->left = b->cap - b->len - 1;
    if (len) *len = b->len;
    return b->s;
}

/* a point to roll the scratch back to (only if no overflow chunk was added since) */
typedef struct { char *cur; size_t left; chunk *extra; } amark;

static amark a_mark(const um_scratch *a)
{
    amark m;
    m.cur = a->cur;
    m.left = a->left;
    m.extra = a->extra;
    return m;
}

static void a_rollback(um_scratch *a, amark m)
{
    if (a->extra == m.extra && !a->oom) { a->cur = m.cur; a->left = m.left; }
}

/* %XX -> byte (the whole string; "+" stays "+") */
static char *url_decode(um_scratch *a, const char *s, size_t n, size_t *len)
{
    abuf b;
    size_t i;
    ab_open(&b, a);
    if (!ab_reserve(&b, n)) return NULL;
    for (i = 0; i < n; i++) {
        int h = -1, l = -1;
        if (s[i] == '%' && i + 2 < n) {
            h = hexval((unsigned char)s[i + 1]);
            l = hexval((unsigned char)s[i + 2]);
        }
        if (h >= 0 && l >= 0) {
            b.s[b.len++] = (char)(h * 16 + l);
            i += 2;
        } else {
            b.s[b.len++] = s[i];
        }
    }
    return ab_close(&b, len);
}

/* s[0] may not appear raw in a URL (reserved characters and existing %XX are fine) */
static int needs_escape(const char *s)
{
    unsigned char c = (unsigned char)*s;
    if (c <= 0x20 || c >= 0x7f) return 1;
    switch (c) {
    case '"': case '<': case '>': case '\\': case '^': case '`': case '{': case '|': case '}':
        return 1;
    case '%':
        return !(hexval((unsigned char)s[1]) >= 0 && hexval((unsigned char)s[2]) >= 0);
    default:
        return 0;
    }
}

static void ab_add_escaped(abuf *b, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    while (*s) {
        const char *run = s;
        while (*s && !needs_escape(s)) s++;
        ab_addn(b, run, (size_t)(s - run));
        if (*s) {
            unsigned char c = (unsigned char)*s++;
            char e[3];
            e[0] = '%';
            e[1] = hex[c >> 4];
            e[2] = hex[c & 15];
            ab_addn(b, e, 3);
        }
    }
}

/* ---------------------------------------------------------------- applying */

typedef struct {
    const char *subj;           /* captures point into this */
    PCRE2_SIZE ov[MAX_CAPS * 2];
    int n;                      /* number of valid pairs */
} caps;

typedef struct {
    const um_env *env;
    um_scratch *a;
    const char *orig_path;      /* decoded, with leading "/" (REQUEST_URI) */
    const char *path;           /* current, decoded, with leading "/" */
    size_t plen;
    const char *query;          /* current query string (no "?") */
    int failed;
} state;

/* Every string here lives in the scratch and is never changed once made, so captures stay
   valid for the whole request. */

static int same(const char *a, const char *b, size_t n, int nocase)
{
    size_t i;
    if (!nocase) return memcmp(a, b, n) == 0;
    for (i = 0; i < n; i++)
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i])) return 0;
    return 1;
}

/* match a fastpat; same answers and captures as PCRE2 for subjects without a newline */
static int fast_match(const fastpat *f, const char *s, size_t len, caps *c)
{
    size_t at = 0;
    if (f->kind == FP_ALL) {
        c->ov[0] = c->ov[2] = 0;
        c->ov[1] = c->ov[3] = len;
    } else {
        if (len < f->len) return 0;
        if (f->start && f->end) {
            if (len != f->len || !same(s, f->lit, len, f->nocase)) return 0;
        } else if (f->start) {
            if (!same(s, f->lit, f->len, f->nocase)) return 0;
        } else if (f->end) {
            at = len - f->len;
            if (!same(s + at, f->lit, f->len, f->nocase)) return 0;
        } else {
            for (;; at++) {
                if (at + f->len > len) return 0;
                if (same(s + at, f->lit, f->len, f->nocase)) break;
            }
        }
        c->ov[0] = at;
        c->ov[1] = at + f->len;
    }
    c->subj = s;
    c->n = f->groups;
    return 1;
}

/* match re (or its fast form) against s (len bytes); fills c on success.
   1 = match, 0 = no match, -1 = error */
static int run(state *st, pcre2_code *re, const fastpat *fp, const char *s, size_t len, caps *c)
{
    pcre2_match_data *md = st->a->md;
    if (fp && fp->kind != FP_NONE && !memchr(s, '\n', len)
#ifdef UM_NO_FASTPATH
        && 0
#endif
       )
        return fast_match(fp, s, len, c);
    int rc = pcre2_match(re, (PCRE2_SPTR)s, len, 0, 0, md, NULL);
    PCRE2_SIZE *ov;
    int i;
    if (rc == PCRE2_ERROR_NOMATCH) return 0;
    if (rc < 0) { st->failed = 1; return -1; }
    ov = pcre2_get_ovector_pointer(md);
    if (rc == 0 || rc > MAX_CAPS) rc = MAX_CAPS;
    c->subj = s;
    c->n = rc;
    for (i = 0; i < rc * 2; i++) c->ov[i] = ov[i];
    return 1;
}

static void add_cap(abuf *b, const caps *c, int n)
{
    if (!c || n >= c->n || c->ov[n * 2] == PCRE2_UNSET) return;
    ab_addn(b, c->subj + c->ov[n * 2], c->ov[n * 2 + 1] - c->ov[n * 2]);
}

/* $N, %N, %{VAR}, \x -> text */
static char *expand(state *st, const char *t, const caps *rc, const caps *cc, size_t *len)
{
    abuf b;
    char vbuf[VAR_CAP];
    ab_open(&b, st->a);
    while (*t) {
        const char *lit = t;
        while (*t && *t != '\\' && *t != '$' && *t != '%') t++;
        if (t > lit) { ab_addn(&b, lit, (size_t)(t - lit)); continue; }
        if (*t == '\\' && t[1]) {
            ab_addc(&b, t[1]);
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
                    vbuf[0] = 0;
                    v = st->env->var(st->env->ctx, name, st->path, vbuf, sizeof vbuf);
                }
                if (v) ab_add(&b, v);
            }
            t = end + 1;
        } else {
            ab_addc(&b, *t++);
        }
    }
    return ab_close(&b, len);
}

/* a + b + c as one string in the scratch */
static char *concat(state *st, const char *a, const char *b, const char *c)
{
    abuf s;
    ab_open(&s, st->a);
    ab_add(&s, a);
    ab_add(&s, b);
    ab_add(&s, c);
    return ab_close(&s, NULL);
}

static int cmp(const char *a, const char *b) { return strcmp(a, b); }

/* evaluate one condition; updates *last (the %N captures) when a regex matches */
static int eval_cond(state *st, const cond *c, const caps *rc, caps *last)
{
    size_t n;
    amark m = a_mark(st->a);
    char *v = expand(st, c->test, rc, last, &n);
    int ok = 0, keep = 0;
    if (!v) { st->failed = 1; return 0; }
    switch (c->kind) {
    case C_FILE: ok = st->env && st->env->file_type && st->env->file_type(st->env->ctx, v) == UM_FT_FILE; break;
    case C_DIR:  ok = st->env && st->env->file_type && st->env->file_type(st->env->ctx, v) == UM_FT_DIR; break;
    case C_EQ:   ok = (c->flags & F_NC) ? ieq(v, c->str) : cmp(v, c->str) == 0; break;
    case C_LT:   ok = cmp(v, c->str) < 0; break;
    case C_GT:   ok = cmp(v, c->str) > 0; break;
    case C_REGEX: {
        caps mc;
        int r = run(st, c->re, &c->fp, v, n, &mc);
        ok = r == 1;
        if (ok && !c->negate) { *last = mc; keep = 1; }     /* %N point into v */
        break;
    }
    }
    if (!keep) a_rollback(st->a, m);
    return c->negate ? !ok : ok;
}

static int conds_ok(state *st, const rule *ru, const caps *rc, caps *last)
{
    int i = 0;
    while (i < ru->nconds) {
        int group = 0;
        /* a run of [OR] conditions counts as one */
        for (;;) {
            if (eval_cond(st, &ru->conds[i], rc, last)) group = 1;
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
static char *build_url(state *st, const char *path, const char *query, int noescape)
{
    abuf b;
    ab_open(&b, st->a);
    if (noescape) ab_add(&b, path); else ab_add_escaped(&b, path);
    if (query && *query) {
        ab_addc(&b, '?');
        if (noescape) ab_add(&b, query); else ab_add_escaped(&b, query);
    }
    return ab_close(&b, NULL);
}

int um_apply_s(const um_rules *r, const char *url, const um_env *env, um_scratch *a, um_result *out)
{
    state st;
    const char *q = strchr(url, '?');
    size_t plen = q ? (size_t)(q - url) : strlen(url);
    int i, skip = 0, changed = 0, applied = 0;

    finish(out, UM_NONE, 0, NULL);
    out->line = 0;
    if (!r || r->n == 0) return UM_NONE;
    if (!a) { finish(out, UM_FAIL, 500, NULL); return UM_FAIL; }
    scratch_reset(a);

    memset(&st, 0, sizeof st);
    st.env = env;
    st.a = a;
    st.query = q ? q + 1 : "";
    st.path = url_decode(a, url, plen, NULL);
    if (!st.path) { st.failed = 1; goto done; }
    st.plen = strlen(st.path);  /* a %00 ends the path, for the regexes too */
    st.orig_path = st.path;     /* path is replaced, never changed, so this stays put */

    for (i = 0; i < r->n; i++) {
        const rule *ru = &r->rules[i];
        caps rc, cc;
        amark m;

        if (skip > 0) { skip--; continue; }
        rc.n = 0;
        cc.n = 0;

        if (ru->type == R_REDIRECT) {
            size_t n = ru->plen;
            if (strncmp(st.path, ru->prefix, n) == 0 &&
                (st.path[n] == 0 || st.path[n] == '/' || (n && ru->prefix[n - 1] == '/'))) {
                char *u;
                applied = ru->line;
                if (ru->status == 410) { finish(out, UM_GONE, 410, NULL); goto done; }
                u = concat(&st, ru->subst, st.path + n, "");
                if (!u) { st.failed = 1; goto done; }
                finish(out, UM_REDIRECT, ru->status, build_url(&st, u, st.query, 0));
                goto done;
            }
            continue;
        }
        if (ru->type == R_REDIRECTMATCH) {
            int mm = run(&st, ru->re, &ru->fp, st.path, st.plen, &rc);
            if (mm < 0) goto done;
            if (mm == 1) {
                char *u;
                applied = ru->line;
                if (ru->status == 410) { finish(out, UM_GONE, 410, NULL); goto done; }
                u = expand(&st, ru->subst, &rc, NULL, NULL);
                if (!u) { st.failed = 1; goto done; }
                finish(out, UM_REDIRECT, ru->status, build_url(&st, u, NULL, 0));
                goto done;
            }
            continue;
        }

        /* RewriteRule */
        if (!r->engine_on) continue;
        m = a_mark(a);
        {
            /* .htaccess style: matched without the leading slash */
            const char *subject = st.plen ? st.path + 1 : st.path;
            int mm = run(&st, ru->re, &ru->fp, subject, st.plen ? st.plen - 1 : 0, &rc);
            int ok;
            if (mm < 0) goto done;
            ok = ru->negate ? mm == 0 : mm == 1;
            if (ru->negate) rc.n = 0;
            if (ok && ru->nconds) ok = conds_ok(&st, ru, &rc, &cc);
            if (st.failed) goto done;
            if (!ok) {
                a_rollback(a, m);
                /* a failed rule in a chain skips the rest of the chain */
                if (ru->flags & F_C)
                    while (i + 1 < r->n && (r->rules[i].flags & F_C)) i++;
                continue;
            }
        }

        applied = ru->line;
        if (ru->flags & F_F) { finish(out, UM_FORBIDDEN, 403, NULL); goto done; }
        if (ru->flags & F_G) { finish(out, UM_GONE, 410, NULL); goto done; }

        if (ru->subst) {
            char *s = expand(&st, ru->subst, &rc, &cc, NULL);
            char *qs;
            const char *np, *nq;
            if (!s) { st.failed = 1; goto done; }
            if (is_absolute_url(s)) {
                /* a full URL always means an external redirect */
                int status = (ru->flags & F_R) ? ru->status : 302;
                char *u = s;
                if (!strchr(s, '?') && !(ru->flags & F_QSD) && st.query[0]) {
                    u = concat(&st, s, "?", st.query);
                    if (!u) { st.failed = 1; goto done; }
                }
                if (!(ru->flags & F_NE)) {
                    abuf b;
                    ab_open(&b, a);
                    ab_add_escaped(&b, u);
                    u = ab_close(&b, NULL);
                }
                finish(out, UM_REDIRECT, status, u);
                goto done;
            }
            /* split the substitution into path and query (s is ours to change) */
            qs = strchr(s, '?');
            if (qs) *qs++ = 0;
            np = s[0] == '/' ? s : concat(&st, r->base, s, "");
            if (!qs) nq = (ru->flags & F_QSD) ? "" : st.query;
            else if ((ru->flags & F_QSA) && st.query[0]) nq = concat(&st, qs, *qs ? "&" : "", st.query);
            else nq = qs;
            if (!np || !nq) { st.failed = 1; goto done; }
            st.path = np;
            st.plen = strlen(np);
            st.query = nq;
            changed = 1;

            if (ru->flags & F_R) {
                finish(out, UM_REDIRECT, ru->status, build_url(&st, st.path, st.query, ru->flags & F_NE));
                goto done;
            }
        } else if (ru->flags & F_QSD) {
            st.query = "";
            changed = 1;
        }
        if (ru->skip) skip = ru->skip;
        if (ru->flags & (F_L | F_END)) break;
    }

    if (changed) finish(out, UM_REWRITE, 0, build_url(&st, st.path, st.query, 0));

done:
    out->line = applied;
    if (st.failed || a->oom) finish(out, UM_FAIL, 500, NULL);
    else if ((out->action == UM_REWRITE || out->action == UM_REDIRECT) && !out->url) finish(out, UM_FAIL, 500, NULL);
    return out->action;
}

int um_apply(const um_rules *r, const char *url, const um_env *env, um_result *out)
{
    um_scratch *a;
    finish(out, UM_NONE, 0, NULL);
    out->line = 0;
    if (!r || r->n == 0) return UM_NONE;
    a = um_scratch_new();
    um_apply_s(r, url, env, a, out);
    if (out->url) {
        char *u = dup(out->url);
        if (!u) finish(out, UM_FAIL, 500, NULL);
        else out->url = u;
    }
    um_scratch_free(a);
    return out->action;
}

void um_result_free(um_result *res)
{
    free(res->url);
    res->url = NULL;
}
