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
typedef struct {
    char *s;
    size_t len;
    size_t cap;
    int oom;
} sbuf;

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

/* A substitution or TestString, split up once when the rules are loaded: literal runs (escapes
   already resolved), $N, %N and %{VAR}. */
enum { T_LIT, T_RCAP, T_CCAP, T_URI, T_QS, T_VAR };

typedef struct {
    unsigned char op;
    unsigned char n;     /* capture number */
    size_t off;          /* T_LIT: text; T_VAR: the NUL-terminated name */
    size_t len;
} tok;

typedef struct {
    int n;
    char *text;          /* points just past t[] in the same allocation */
    tok t[1];
} tmpl;

/* Patterns simple enough to match without PCRE2: a plain string with optional ^ and $, or a
   catch-all such as ^(.*)$. Subjects containing a newline still go to PCRE2 ('.' and '$'
   treat it specially). */
enum { FP_NONE = 0, FP_LIT, FP_ALL };

typedef struct {
    int kind;
    int start;           /* FP_LIT: anchored with ^ / $ */
    int end;
    int nocase;
    int groups;          /* capture pairs a match fills: 1, or 2 for (.*) */
    char *lit;           /* FP_LIT: the string, escapes resolved */
    size_t len;
} fastpat;

typedef struct {
    char *test;          /* TestString template */
    tmpl *tt;            /* ... compiled */
    int kind;
    int negate;
    int flags;
    char *str;           /* C_EQ / C_LT / C_GT */
    pcre2_code *re;      /* C_REGEX */
    fastpat fp;
} cond;

typedef struct {
    int type;
    int negate;
    int flags;
    int status;
    int skip;
    int line;
    pcre2_code *re;      /* R_REWRITE, R_REDIRECTMATCH */
    fastpat fp;
    char *prefix;        /* R_REDIRECT */
    size_t plen;         /* strlen(prefix) */
    char *subst;         /* NULL = "-" (no substitution); for redirects the target URL */
    tmpl *ts;            /* subst compiled (not for R_REDIRECT, whose target is used as is) */
    cond *conds;
    int nconds;
} rule;

struct um_rules {
    rule *rules;
    int n;
    int cap;
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
    free(c->tt);
    free(c->fp.lit);
    free(c->test);
    free(c->str);
    if (c->re) pcre2_code_free(c->re);
}

static void free_rule(rule *r)
{
    free(r->ts);
    free(r->fp.lit);
    if (r->re) pcre2_code_free(r->re);
    free(r->prefix);
    free(r->subst);
    for (int i = 0; i < r->nconds; i++) free_cond(&r->conds[i]);
    free(r->conds);
}

void um_rules_free(um_rules *r)
{
    if (!r) return;
    for (int i = 0; i < r->n; i++) free_rule(&r->rules[i]);
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

/* %{NAME} -> T_URI, T_QS or T_VAR (name is k bytes; k < 128) */
static int var_op(const char *name, size_t k)
{
    char buf[128];
    memcpy(buf, name, k);
    buf[k] = 0;
    if (ieq(buf, "REQUEST_URI")) return T_URI;
    if (ieq(buf, "QUERY_STRING")) return T_QS;
    return T_VAR;
}

/* Split a template into tokens (same reading as Apache: \x is x, $N, %N, %{VAR}; anything else is
   literal). NULL only when out of memory. */
static tmpl *tmpl_compile(const char *t)
{
    size_t len = strlen(t);
    size_t tl = 0;
    tmpl *o = malloc(sizeof(tmpl) + sizeof(tok) * (len + 1) + len * 2 + 2);
    int n = 0;
    if (!o) return NULL;
    o->text = (char *)(o->t + len + 1);
#define T_ADD(op_, n_, off_, len_) \
    (o->t[n].op = (unsigned char)(op_), o->t[n].n = (unsigned char)(n_), o->t[n].off = (off_), o->t[n].len = (len_), n++)
#define T_CHAR(c_) do { \
        if (!n || o->t[n - 1].op != T_LIT) T_ADD(T_LIT, 0, tl, 0); \
        o->text[tl++] = (c_); o->t[n - 1].len++; \
    } while (0)
    while (*t) {
        if (*t == '\\' && t[1]) {
            T_CHAR(t[1]);
            t += 2;
        } else if (*t == '$' && t[1] >= '0' && t[1] <= '9') {
            T_ADD(T_RCAP, t[1] - '0', 0, 0);
            t += 2;
        } else if (*t == '%' && t[1] >= '0' && t[1] <= '9') {
            T_ADD(T_CCAP, t[1] - '0', 0, 0);
            t += 2;
        } else if (*t == '%' && t[1] == '{' && strchr(t, '}')) {
            const char *end = strchr(t, '}');
            size_t k = (size_t)(end - t - 2);
            int op = k < 128 ? var_op(t + 2, k) : -1;  /* longer names expand to nothing */
            if (op == T_VAR) {
                T_ADD(T_VAR, 0, tl, k);
                memcpy(o->text + tl, t + 2, k);
                o->text[tl + k] = 0;
                tl += k + 1;
            } else if (op >= 0) {
                T_ADD(op, 0, 0, 0);
            }
            t = end + 1;
        } else {
            T_CHAR(*t);
            t++;
        }
    }
#undef T_CHAR
#undef T_ADD
    o->n = n;
    return o;
}

/* Fill f if pat can be matched without PCRE2 (see fastpat); leaves f->kind FP_NONE if not.
   Built with UM_NO_FASTPATH (a test build), no pattern takes the fast path. */
#ifdef UM_NO_FASTPATH
static void analyze(const char *pat, int nocase, fastpat *f)
{
    (void)pat;
    memset(f, 0, sizeof *f);
    f->nocase = nocase;
}
#else
static void analyze(const char *pat, int nocase, fastpat *f)
{
    size_t n = strlen(pat);
    size_t i = 0;
    size_t j = 0;
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
    while (i < n) {
        char c = pat[i];
        i++;
        if (c == '\\') {
            char e = pat[i];
            /* \. \/ \- ... are the character itself; \d \w \b etc. are not */
            if (i >= n || (e >= '0' && e <= '9') || (e >= 'a' && e <= 'z') || (e >= 'A' && e <= 'Z') ||
                (unsigned char)e >= 0x80) { free(lit); return; }
            lit[j++] = e;
            i++;
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
#endif

/* ---------------------------------------------------------------- tokenizer */

#define MAX_ARGS 8

/* end the argument that starts at s (copied down in place: out never passes s); returns where
   the next one may start */
static char *take_arg(char *s)
{
    char *out = s;
    if (*s == '"') {
        s++;
        while (*s && *s != '"') {
            if (*s == '\\' && s[1] == '"') s++;
            *out++ = *s++;
        }
    } else {
        while (*s && *s != ' ' && *s != '\t') *out++ = *s++;
    }
    if (*s) s++;
    *out = 0;
    return s;
}

/* Split a line into arguments: whitespace separated, "double quotes" group (with \" inside).
   Unquoted backslashes are kept (regexes need them). Returns the count, or -1 if too many. */
static int split(char *line, char **argv)
{
    int n = 0;
    char *s = line;
    for (;;) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (n == MAX_ARGS) return -1;
        argv[n++] = s;
        s = take_arg(s);
    }
    return n;
}

static int redirect_code(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/* R=...: the redirect status (302 without one) */
static int redirect_status(parser *p, const char *val, int *status)
{
    *status = 302;
    if (!val) return 1;
    if (ieq(val, "permanent")) *status = 301;
    else if (ieq(val, "temp")) *status = 302;
    else if (ieq(val, "seeother")) *status = 303;
    else *status = to_int(val);
    if (!redirect_code(*status)) {
        say(p, "unsupported redirect status: ", val);
        return 0;
    }
    return 1;
}

static int cond_flag(parser *p, const char *s, int *flags)
{
    if (ieq(s, "NC") || ieq(s, "nocase")) *flags |= F_NC;
    else if (ieq(s, "OR") || ieq(s, "ornext")) *flags |= F_OR;
    else { say(p, "unknown RewriteCond flag: ", s); return 0; }
    return 1;
}

/* RewriteRule flags that take no value */
static const struct {
    const char *name;
    const char *alt;     /* long form; NULL if none */
    int bits;
} RULE_FLAGS[] = {
    { "NC", "nocase", F_NC }, { "L", "last", F_L }, { "END", NULL, F_END | F_L },
    { "QSA", "qsappend", F_QSA }, { "QSD", "qsdiscard", F_QSD }, { "F", "forbidden", F_F | F_L },
    { "G", "gone", F_G | F_L }, { "NE", "noescape", F_NE }, { "C", "chain", F_C }
};

static int rule_flag(parser *p, const char *s, const char *val, int *flags, int *status, int *skip)
{
    for (size_t i = 0; i < sizeof RULE_FLAGS / sizeof RULE_FLAGS[0]; i++) {
        if (ieq(s, RULE_FLAGS[i].name) || (RULE_FLAGS[i].alt && ieq(s, RULE_FLAGS[i].alt))) {
            *flags |= RULE_FLAGS[i].bits;
            return 1;
        }
    }
    if (ieq(s, "S") || ieq(s, "skip")) {
        *skip = val ? to_int(val) : 0;
        if (*skip <= 0) { say(p, "S= needs a number of rules to skip", NULL); return 0; }
        return 1;
    }
    if (ieq(s, "R") || ieq(s, "redirect")) {
        *flags |= F_R | F_L;
        return redirect_status(p, val, status);
    }
    say(p, "unknown RewriteRule flag: ", s);
    return 0;
}

/* "[L,NC,R=301]" -> flags; FALSE (after logging) on an unknown flag */
static int parse_flags(parser *p, const char *spec, int is_cond, int *flags, int *status, int *skip)
{
    char buf[256];
    char *s = buf;
    size_t n = strlen(spec);

    if (n < 2 || spec[0] != '[' || spec[n - 1] != ']' || n - 2 >= sizeof buf) {
        say(p, "flags must look like [L,NC]: ", spec);
        return 0;
    }
    memcpy(buf, spec + 1, n - 2);
    buf[n - 2] = 0;
    while (*s) {
        char *e = s;
        char *val;
        int ok;
        while (*e && *e != ',') e++;
        if (*e) { *e = 0; e++; }
        while (*s == ' ') s++;
        val = strchr(s, '=');
        if (val) { *val = 0; val++; }
        if (!*s) ok = 1;
        else if (is_cond) ok = cond_flag(p, s, flags);
        else ok = rule_flag(p, s, val, flags, status, skip);
        if (!ok) return 0;
        s = e;
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
typedef struct {
    cond *c;
    int n;
    int cap;
} condlist;

static void clear_conds(condlist *cl)
{
    for (int i = 0; i < cl->n; i++) free_cond(&cl->c[i]);
    free(cl->c);
    cl->c = NULL;
    cl->n = 0;
    cl->cap = 0;
}

static int d_cond(parser *p, condlist *cl, int argc, char **argv)
{
    cond c;
    const char *pat;
    int flags = 0;
    int status = 0;
    int skip = 0;

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
    c.tt = tmpl_compile(argv[1]);
    if (!c.test || !c.tt) { free_cond(&c); say(p, "out of memory", NULL); return 0; }
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
    int flags = 0;
    int status = 0;
    int skip = 0;
    int negate = 0;
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
    if (ru->subst) ru->ts = tmpl_compile(ru->subst);
    if (ru->subst && !ru->ts) {
        r->n--;
        free_rule(ru);
        say(p, "out of memory", NULL);
        return 0;
    }
    ru->conds = cl->c;
    ru->nconds = cl->n;
    cl->c = NULL;
    cl->n = 0;
    cl->cap = 0;
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
    int i = 1;
    int status = 302;
    const char *from;
    const char *to = NULL;

    if (argc >= 2 && status_word(argv[1])) { status = status_word(argv[1]); i = 2; }
    if (argc - i < 1 || argc - i > 2) { say(p, match ? "RedirectMatch needs: [status] regex URL" : "Redirect needs: [status] URL-path URL", NULL); return 0; }
    from = argv[i];
    if (argc - i == 2) to = argv[i + 1];
    if (status != 410 && !to) { say(p, "missing target URL", NULL); return 0; }
    if (status != 410 && !redirect_code(status)) {
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
        if (ru->re && ru->subst) ru->ts = tmpl_compile(ru->subst);
        if (!ru->re || (ru->subst && !ru->ts)) { r->n--; free_rule(ru); return 0; }
        analyze(from, 0, &ru->fp);
    } else {
        ru->prefix = dup(from);
        if (!ru->prefix) { r->n--; free_rule(ru); return 0; }
        ru->plen = strlen(ru->prefix);
    }
    return 1;
}

static void d_engine(parser *p, um_rules *r, int argc, char **argv)
{
    if (argc == 2 && (ieq(argv[1], "on") || ieq(argv[1], "off")))
        r->engine_on = ieq(argv[1], "on");
    else say(p, "RewriteEngine needs On or Off", NULL);
}

static void d_base(parser *p, um_rules *r, int argc, char **argv)
{
    size_t n;
    if (argc != 2 || argv[1][0] != '/') {
        say(p, "RewriteBase needs a URL path starting with /", NULL);
        return;
    }
    n = strlen(argv[1]);
    free(r->base);
    r->base = malloc(n + 2);
    if (!r->base) return;
    memcpy(r->base, argv[1], n + 1);
    if (argv[1][n - 1] != '/') { r->base[n] = '/'; r->base[n + 1] = 0; }
}

static void directive(parser *p, um_rules *r, condlist *cl, int argc, char **argv)
{
    const char *d = argv[0];
    if (ieq(d, "RewriteEngine")) d_engine(p, r, argc, argv);
    else if (ieq(d, "RewriteBase")) d_base(p, r, argc, argv);
    else if (ieq(d, "RewriteCond")) d_cond(p, cl, argc, argv);
    else if (ieq(d, "RewriteRule")) d_rule(p, r, cl, argc, argv);
    else if (ieq(d, "Redirect")) d_redirect(p, r, argc, argv, 0);
    else if (ieq(d, "RedirectMatch")) d_redirect(p, r, argc, argv, 1);
    else if (istarts(d, "<IfModule") || istarts(d, "</IfModule")) { /* read through */ }
    else if (ieq(d, "RewriteOptions") || ieq(d, "Options") || ieq(d, "DirectoryIndex"))
        say(p, "ignored (not supported): ", d);
    else say(p, "unknown directive: ", d);
}

/* one logical line from text at *pos: physical lines joined where they end with a backslash.
   Counts them in p->line. NULL if out of memory. */
static char *read_line(parser *p, const char *text, size_t len, size_t *pos)
{
    sbuf line = {0};
    size_t i = *pos;
    for (;;) {
        size_t start = i;
        size_t end;
        while (i < len && text[i] != '\n') i++;
        end = i;
        if (i < len) i++;
        p->line++;
        if (end > start && text[end - 1] == '\r') end--;
        if (end > start && text[end - 1] == '\\' && i < len) {
            sb_addn(&line, text + start, end - start - 1);
            sb_addc(&line, ' ');
            continue;
        }
        sb_addn(&line, text + start, end - start);
        break;
    }
    *pos = i;
    return sb_take(&line);
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
        char *argv[MAX_ARGS];
        int first = p.line + 1;
        int last;
        int argc;
        char *s = read_line(&p, text, len, &i);
        if (!s) break;
        last = p.line;
        p.line = first;
        argc = split(s, argv);
        if (argc == -1) say(&p, "too many arguments", NULL);
        else if (argc > 0 && argv[0][0] != '#') directive(&p, r, &cl, argc, argv);
        p.line = last;
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
#define FRAMES_KEEP   (256u * 1024)     /* PCRE2 backtracking memory a scratch may keep */

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
    /* PCRE2 keeps its backtracking memory in the match data; after a request that needed a
       lot (up to HEAP_LIMIT), let it go rather than hold it for the life of the thread */
    if (pcre2_get_match_data_heapframes_size(a->md) > FRAMES_KEEP) {
        pcre2_match_data *md = pcre2_match_data_create(MAX_CAPS, NULL);
        if (md) {
            pcre2_match_data_free(a->md);
            a->md = md;
        }
    }
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
    size_t len;
    size_t cap;
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
    size_t need = b->len + n + 1;
    size_t cap;
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

/* Tidy a decoded path in place, as Apache does before per-directory rules and as IIS does
   before it maps the URL to a file: "\" is a "/", runs of "/" become one, "." segments go and
   ".." takes away the segment before it (never above the root). Otherwise "/x/../admin", or
   admin after a double slash, would slip past a rule for ^admin and still be served as /admin.
   A path that doesn't start with "/" (e.g. "*") is left alone. Returns the new length. */
static size_t clean_path(char *s, size_t n)
{
    size_t i = 0;
    size_t o = 0;
    int dir = 1;                /* the path ends in a folder (keep a trailing "/") */
    if (!n || s[0] != '/') return n;
    while (i < n) {
        size_t start;
        size_t len;
        if (s[i] == '/' || s[i] == '\\') { i++; dir = 1; continue; }
        start = i;
        while (i < n && s[i] != '/' && s[i] != '\\') i++;
        len = i - start;
        if (len == 1 && s[start] == '.') {
            dir = 1;
        } else if (len == 2 && s[start] == '.' && s[start + 1] == '.') {
            while (o > 0 && s[o - 1] != '/') o--;
            if (o > 0) o--;
            dir = 1;
        } else {
            /* o < start: every segment read so far came with at least one separator */
            s[o++] = '/';
            memmove(s + o, s + start, len);
            o += len;
            dir = 0;
        }
    }
    if (dir && (o == 0 || s[o - 1] != '/')) s[o++] = '/';
    s[o] = 0;
    return o;
}

/* %XX -> byte ("+" stays "+"); a %00 ends the string, so the rules never see past it. The
   result is then tidied (clean_path). */
static char *url_decode(um_scratch *a, const char *s, size_t n, size_t *len)
{
    abuf b;
    size_t i = 0;
    ab_open(&b, a);
    if (!ab_reserve(&b, n)) return NULL;
    while (i < n) {
        int h = -1;
        int l = -1;
        if (s[i] == '%' && i + 2 < n) {
            h = hexval((unsigned char)s[i + 1]);
            l = hexval((unsigned char)s[i + 2]);
        }
        if (h >= 0 && l >= 0) {
            char ch = (char)(h * 16 + l);
            if (!ch) break;
            b.s[b.len++] = ch;
            i += 3;
        } else {
            b.s[b.len++] = s[i];
            i++;
        }
    }
    b.len = clean_path(b.s, b.len);
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

/* s[0] is a control character: never sent raw, [NE] or not (a CR or LF in a Location header
   would let a request add headers of its own) */
static int is_ctl(const char *s)
{
    unsigned char c = (unsigned char)*s;
    return c < 0x20 || c == 0x7f;
}

/* append s, %-escaping the characters test picks */
static void ab_add_esc(abuf *b, const char *s, int (*test)(const char *))
{
    static const char hex[] = "0123456789ABCDEF";
    while (*s) {
        const char *run = s;
        while (*s && !test(s)) s++;
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

static void ab_add_escaped(abuf *b, const char *s) { ab_add_esc(b, s, needs_escape); }

/* [NE]: as written, except control characters */
static void ab_add_raw(abuf *b, const char *s) { ab_add_esc(b, s, is_ctl); }

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
    int path_nl;                /* path contains a newline (no fast-path matching then) */
    const char *ft_path;        /* the last -f/-d test in this request, and its answer */
    int ft_type;
    int failed;
    int changed;                /* path or query rewritten */
    int applied;                /* line of the last rule that matched */
    int skip;                   /* rules still to skip ([S=N]) */
} state;

/* Every string here lives in the scratch and is never changed once made, so captures stay
   valid for the whole request. */

static int same(const char *a, const char *b, size_t n, int nocase)
{
    if (!nocase) return memcmp(a, b, n) == 0;
    for (size_t i = 0; i < n; i++)
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i])) return 0;
    return 1;
}

/* where an FP_LIT pattern's string is in s, by its anchors; 0 if it isn't there */
static int find_lit(const fastpat *f, const char *s, size_t len, size_t *at)
{
    if (len < f->len) return 0;
    if (f->start) {
        *at = 0;
        if (f->end && len != f->len) return 0;
        return same(s, f->lit, f->len, f->nocase);
    }
    if (f->end) {
        *at = len - f->len;
        return same(s + *at, f->lit, f->len, f->nocase);
    }
    for (size_t i = 0; i + f->len <= len; i++) {
        if (same(s + i, f->lit, f->len, f->nocase)) { *at = i; return 1; }
    }
    return 0;
}

/* match a fastpat; same answers and captures as PCRE2 for subjects without a newline */
static int fast_match(const fastpat *f, const char *s, size_t len, caps *c)
{
    if (f->kind == FP_ALL) {
        c->ov[0] = 0;
        c->ov[1] = len;
        c->ov[2] = 0;
        c->ov[3] = len;
    } else {
        size_t at = 0;
        if (!find_lit(f, s, len, &at)) return 0;
        c->ov[0] = at;
        c->ov[1] = at + f->len;
    }
    c->subj = s;
    c->n = f->groups;
    return 1;
}

/* match re (or its fast form) against s (len bytes); fills c on success.
   1 = match, 0 = no match, -1 = error */
static int run(state *st, pcre2_code *re, const fastpat *fp, const char *s, size_t len, int nl, caps *c)
{
    pcre2_match_data *md = st->a->md;
    if (fp && fp->kind != FP_NONE && !(nl < 0 ? memchr(s, '\n', len) != NULL : nl))
        return fast_match(fp, s, len, c);
    int rc = pcre2_match(re, (PCRE2_SPTR)s, len, 0, 0, md, NULL);
    PCRE2_SIZE *ov;
    if (rc == PCRE2_ERROR_NOMATCH) return 0;
    if (rc < 0) { st->failed = 1; return -1; }
    ov = pcre2_get_ovector_pointer(md);
    if (rc == 0 || rc > MAX_CAPS) rc = MAX_CAPS;
    c->subj = s;
    c->n = rc;
    for (int i = 0; i < rc * 2; i++) c->ov[i] = ov[i];
    return 1;
}

static void add_cap(abuf *b, const caps *c, int n)
{
    if (!c || n >= c->n || c->ov[n * 2] == PCRE2_UNSET) return;
    ab_addn(b, c->subj + c->ov[n * 2], c->ov[n * 2 + 1] - c->ov[n * 2]);
}

/* %{NAME} from the environment (nothing if unknown) */
static void add_var(abuf *b, const state *st, const char *name)
{
    char vbuf[VAR_CAP];
    const char *v;
    if (!st->env || !st->env->var) return;
    vbuf[0] = 0;
    v = st->env->var(st->env->ctx, name, st->path, vbuf, sizeof vbuf);
    if (v) ab_add(b, v);
}

/* a compiled template -> text */
static char *expand(state *st, const tmpl *tp, const caps *rc, const caps *cc, size_t *len)
{
    abuf b;
    ab_open(&b, st->a);
    for (int i = 0; i < tp->n; i++) {
        const tok *k = &tp->t[i];
        switch (k->op) {
        case T_LIT:  ab_addn(&b, tp->text + k->off, k->len); break;
        case T_RCAP: add_cap(&b, rc, k->n); break;
        case T_CCAP: add_cap(&b, cc, k->n); break;
        case T_URI:  ab_add(&b, st->orig_path); break;
        case T_QS:   ab_add(&b, st->query); break;
        default:     add_var(&b, st, tp->text + k->off); break;
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

/* -f / -d: what is at fspath. "!-f" then "!-d" on the same path is the usual pair, so the
   last answer in this request is kept (fspath must stay in the scratch). */
static int file_type(state *st, const char *fspath)
{
    if (!st->env || !st->env->file_type) return UM_FT_NONE;
    if (st->ft_path && strcmp(st->ft_path, fspath) == 0) return st->ft_type;
    st->ft_type = st->env->file_type(st->env->ctx, fspath);
    st->ft_path = fspath;
    return st->ft_type;
}

/* evaluate one condition; updates *last (the %N captures) when a regex matches */
static int eval_cond(state *st, const cond *c, const caps *rc, caps *last)
{
    size_t n;
    amark m = a_mark(st->a);
    const char *v = expand(st, c->tt, rc, last, &n);
    int ok = 0;
    int keep = 0;
    if (!v) { st->failed = 1; return 0; }
    switch (c->kind) {
    case C_FILE: ok = file_type(st, v) == UM_FT_FILE; keep = 1; break;   /* keep v for the cache */
    case C_DIR:  ok = file_type(st, v) == UM_FT_DIR; keep = 1; break;
    case C_EQ:   ok = (c->flags & F_NC) ? ieq(v, c->str) : cmp(v, c->str) == 0; break;
    case C_LT:   ok = cmp(v, c->str) < 0; break;
    case C_GT:   ok = cmp(v, c->str) > 0; break;
    case C_REGEX: {
        caps mc;
        int r = run(st, c->re, &c->fp, v, n, -1, &mc);
        ok = r == 1;
        if (ok && !c->negate) { *last = mc; keep = 1; }     /* %N point into v */
        break;
    }
    default:
        break;
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
    /* http:// or https:// */
    if (!istarts(s, "http")) return 0;
    s += 4;
    if (lower((unsigned char)*s) == 's') s++;
    return strncmp(s, "://", 3) == 0;
}

static void finish(um_result *out, int action, int status, char *url)
{
    out->action = action;
    out->status = status;
    out->url = url;
}

/* a full URL, %-escaped ([NE]: only its control characters) */
static char *escape_url(state *st, const char *u, int noescape)
{
    abuf b;
    ab_open(&b, st->a);
    if (noescape) ab_add_raw(&b, u); else ab_add_escaped(&b, u);
    return ab_close(&b, NULL);
}

/* path (decoded, leading "/") + query -> escaped URL. For a redirect, a path that starts with
   two slashes (or a slash and a backslash) would send the browser to another host, so the
   extra ones go. */
static char *build_url(state *st, const char *path, const char *query, int noescape, int redirect)
{
    abuf b;
    ab_open(&b, st->a);
    if (redirect && (path[0] == '/' || path[0] == '\\')) {
        while (path[1] == '/' || path[1] == '\\') path++;
        if (*path == '\\') { ab_addc(&b, '/'); path++; }
    }
    if (noescape) ab_add_raw(&b, path); else ab_add_escaped(&b, path);
    if (query && *query) {
        ab_addc(&b, '?');
        if (noescape) ab_add_raw(&b, query); else ab_add_escaped(&b, query);
    }
    return ab_close(&b, NULL);
}

/* what applying a rule leads to: the next rule, no more rules (as [L]), or the result is
   settled (out is set, or st->failed) */
enum { STEP_NEXT, STEP_LAST, STEP_DONE };

/* Redirect's URL-path: a whole segment (or more) at the start of path */
static int prefix_match(const char *path, const rule *ru)
{
    size_t n = ru->plen;
    return strncmp(path, ru->prefix, n) == 0 &&
           (path[n] == 0 || path[n] == '/' || (n && ru->prefix[n - 1] == '/'));
}

static int apply_redirect(state *st, const rule *ru, um_result *out)
{
    const char *u;
    if (!prefix_match(st->path, ru)) return STEP_NEXT;
    st->applied = ru->line;
    if (ru->status == 410) { finish(out, UM_GONE, 410, NULL); return STEP_DONE; }
    u = concat(st, ru->subst, st->path + ru->plen, "");
    if (!u) { st->failed = 1; return STEP_DONE; }
    finish(out, UM_REDIRECT, ru->status, build_url(st, u, st->query, 0, 1));
    return STEP_DONE;
}

static int apply_redirect_match(state *st, const rule *ru, um_result *out)
{
    caps rc;
    const char *u;
    int mm;
    rc.n = 0;
    mm = run(st, ru->re, &ru->fp, st->path, st->plen, st->path_nl, &rc);
    if (mm < 0) return STEP_DONE;
    if (mm == 0) return STEP_NEXT;
    st->applied = ru->line;
    if (ru->status == 410) { finish(out, UM_GONE, 410, NULL); return STEP_DONE; }
    u = expand(st, ru->ts, &rc, NULL, NULL);
    if (!u) { st->failed = 1; return STEP_DONE; }
    finish(out, UM_REDIRECT, ru->status, build_url(st, u, NULL, 0, 1));
    return STEP_DONE;
}

/* does a RewriteRule (pattern, then conditions) match? Fills rc and cc. -1 on an error */
static int rule_matches(state *st, const rule *ru, caps *rc, caps *cc)
{
    /* .htaccess style: matched without the leading slash */
    const char *subject = st->plen ? st->path + 1 : st->path;
    int mm = run(st, ru->re, &ru->fp, subject, st->plen ? st->plen - 1 : 0, st->path_nl, rc);
    int ok;
    if (mm < 0) return -1;
    ok = ru->negate ? mm == 0 : mm == 1;
    if (ru->negate) rc->n = 0;
    if (ok && ru->nconds) ok = conds_ok(st, ru, rc, cc);
    if (st->failed) return -1;
    return ok;
}

/* a failed rule in a chain skips the rest of the chain: the index of the chain's last rule */
static int chain_end(const um_rules *r, int i)
{
    while (i + 1 < r->n && (r->rules[i].flags & F_C)) i++;
    return i;
}

/* a substitution that is a full URL: always an external redirect */
static int redirect_to_url(state *st, const rule *ru, const char *s, um_result *out)
{
    int status = (ru->flags & F_R) ? ru->status : 302;
    const char *u = s;
    if (!strchr(s, '?') && !(ru->flags & F_QSD) && st->query[0]) {
        u = concat(st, s, "?", st->query);
        if (!u) { st->failed = 1; return STEP_DONE; }
    }
    finish(out, UM_REDIRECT, status, escape_url(st, u, ru->flags & F_NE));
    return STEP_DONE;
}

/* cut s at its "?": the query after it, or NULL if there is none */
static char *cut_query(char *s)    /* NOSONAR: writes the cut into s */
{
    char *q = strchr(s, '?');
    if (q) { *q = 0; q++; }
    return q;
}

/* the query string after a substitution whose own query is qs (NULL: it has none) */
static const char *new_query(state *st, const rule *ru, const char *qs)
{
    if (!qs) return (ru->flags & F_QSD) ? "" : st->query;
    if ((ru->flags & F_QSA) && st->query[0]) return concat(st, qs, *qs ? "&" : "", st->query);
    return qs;
}

/* a matched rule's substitution: a new path and query, or a redirect */
static int apply_subst(state *st, const um_rules *r, const rule *ru, const caps *rc, const caps *cc,
                       um_result *out)
{
    char *s = expand(st, ru->ts, rc, cc, NULL);
    const char *qs;
    const char *np;
    const char *nq;
    if (!s) { st->failed = 1; return STEP_DONE; }
    if (is_absolute_url(s)) return redirect_to_url(st, ru, s, out);
    /* split the substitution into path and query (s is ours to change) */
    qs = cut_query(s);
    np = s[0] == '/' ? s : concat(st, r->base, s, "");
    nq = new_query(st, ru, qs);
    if (!np || !nq) { st->failed = 1; return STEP_DONE; }
    st->path = np;
    st->plen = strlen(np);
    st->path_nl = memchr(np, '\n', st->plen) != NULL;
    st->query = nq;
    st->changed = 1;
    if (ru->flags & F_R) {
        finish(out, UM_REDIRECT, ru->status, build_url(st, st->path, st->query, ru->flags & F_NE, 1));
        return STEP_DONE;
    }
    return STEP_NEXT;
}

/* rule *i; a failed chain moves *i on to the chain's last rule */
static int apply_rule(state *st, const um_rules *r, int *i, um_result *out)
{
    const rule *ru = &r->rules[*i];
    caps rc;
    caps cc;
    amark m;
    int ok;

    if (ru->type == R_REDIRECT) return apply_redirect(st, ru, out);
    if (ru->type == R_REDIRECTMATCH) return apply_redirect_match(st, ru, out);

    /* RewriteRule */
    if (!r->engine_on) return STEP_NEXT;
    rc.n = 0;
    cc.n = 0;
    m = a_mark(st->a);
    ok = rule_matches(st, ru, &rc, &cc);
    if (ok < 0) return STEP_DONE;
    if (!ok) {
        a_rollback(st->a, m);
        st->ft_path = NULL;     /* it may point into what was just rolled back */
        if (ru->flags & F_C) *i = chain_end(r, *i);
        return STEP_NEXT;
    }

    st->applied = ru->line;
    if (ru->flags & F_F) { finish(out, UM_FORBIDDEN, 403, NULL); return STEP_DONE; }
    if (ru->flags & F_G) { finish(out, UM_GONE, 410, NULL); return STEP_DONE; }
    if (ru->subst) {
        int step = apply_subst(st, r, ru, &rc, &cc, out);
        if (step != STEP_NEXT) return step;
    } else if (ru->flags & F_QSD) {
        st->query = "";
        st->changed = 1;
    }
    if (ru->skip) st->skip = ru->skip;
    return (ru->flags & (F_L | F_END)) ? STEP_LAST : STEP_NEXT;
}

/* the rules, top to bottom */
static void apply_rules(state *st, const um_rules *r, um_result *out)
{
    int step = STEP_NEXT;
    int i = 0;
    while (step == STEP_NEXT && i < r->n) {
        if (st->skip > 0) st->skip--;
        else step = apply_rule(st, r, &i, out);
        i++;
    }
    if (step != STEP_DONE && st->changed)
        finish(out, UM_REWRITE, 0, build_url(st, st->path, st->query, 0, 0));
}

int um_apply_s(const um_rules *r, const char *url, const um_env *env, um_scratch *a, um_result *out)
{
    state st;
    const char *q = strchr(url, '?');
    size_t plen = q ? (size_t)(q - url) : strlen(url);

    finish(out, UM_NONE, 0, NULL);
    out->line = 0;
    if (!r || r->n == 0) return UM_NONE;
    if (!a) { finish(out, UM_FAIL, 500, NULL); return UM_FAIL; }
    scratch_reset(a);

    memset(&st, 0, sizeof st);
    st.env = env;
    st.a = a;
    st.query = q ? q + 1 : "";
    st.path = url_decode(a, url, plen, &st.plen);
    if (st.path) {
        st.path_nl = memchr(st.path, '\n', st.plen) != NULL;
        st.orig_path = st.path;     /* path is replaced, never changed, so this stays put */
        apply_rules(&st, r, out);
    } else {
        st.failed = 1;
    }

    out->line = st.applied;
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

/* ---------------------------------------------------------------- rules files */

static const char *const RULES_FILES[] = {
    ".htaccess", "httpd.htaccess", "httpd.ini", "httpd.parse.errors", "nginx.conf",
    "urlmagic4.conf", NULL
};

/* The part of an 8.3 alias before its "~" (pre bytes) fits the long name's base: its start,
   or (the hashed form) its first two letters and four hex digits. */
static int alias_prefix(const char *seg, size_t pre, const char *base, size_t bl)
{
    if (pre < 1 || pre > 6) return 0;
    if (pre <= bl && memcmp(seg, base, pre) == 0 && (pre >= 2 || pre == bl)) return 1;
    if (pre != 6 || bl < 2 || memcmp(seg, base, 2) != 0) return 0;
    for (size_t i = 2; i < 6; i++)
        if (hexval((unsigned char)seg[i]) < 0) return 0;
    return 1;
}

/* The part after the "~" (p up to end) is digits, then the first three letters of ext (the
   long name's extension with its dot; NULL if none). */
static int alias_tail(const char *p, const char *end, const char *ext)
{
    size_t i;
    if (p == end || *p < '0' || *p > '9') return 0;
    while (p < end && *p >= '0' && *p <= '9') p++;
    if (!ext) return p == end;
    if (p == end || *p != '.') return 0;
    p++;
    for (i = 0; i < 3 && ext[1 + i]; i++)
        if (p + i >= end || p[i] != ext[1 + i]) return 0;
    return p + i == end;
}

/* Is seg (lower case, n bytes) the DOS 8.3 alias Windows gives name, e.g. httpd~1.hta for
   httpd.htaccess or htacce~1 for .htaccess? IIS opens a file by either name. */
static int short_alias(const char *seg, size_t n, const char *name)
{
    const char *tilde = memchr(seg, '~', n);
    const char *ext = NULL;
    const char *p;
    char base[16];
    size_t bl = 0;

    if (!tilde) return 0;
    while (*name == '.') name++;
    for (const char *d = name; *d; d++)
        if (*d == '.') ext = d;                 /* the last dot */
    p = name;
    while (*p && p != ext && bl < sizeof base) {
        if (*p != '.') base[bl++] = *p;
        p++;
    }
    return alias_prefix(seg, (size_t)(tilde - seg), base, bl) && alias_tail(tilde + 1, seg + n, ext);
}

/* The last non-empty segment of url's path, %-decoded, in lower case, into seg (cap bytes);
   its length, or -1 if it's longer than any rules file's name. A %00 ends the path. */
static int last_segment(const char *url, char *seg, size_t cap)
{
    size_t j = 0;
    int fresh = 1;
    int over = 0;
    const char *s = url;

    while (*s && *s != '?') {
        int c = (unsigned char)*s;
        if (c == '%' && hexval((unsigned char)s[1]) >= 0 && hexval((unsigned char)s[2]) >= 0) {
            c = hexval((unsigned char)s[1]) * 16 + hexval((unsigned char)s[2]);
            s += 2;
        }
        if (!c) break;
        if (c == '/' || c == '\\') {
            fresh = 1;
        } else {
            if (fresh) {
                j = 0;
                over = 0;
                fresh = 0;
            }
            if (j + 1 < cap) seg[j++] = (char)lower(c);
            else if (c != '.' && c != ' ') over = 1;    /* trailing dots and spaces don't count */
        }
        s++;
    }
    seg[j] = 0;
    return over ? -1 : (int)j;
}

int um_is_rules_file(const char *url)
{
    char seg[64];
    int n = last_segment(url, seg, sizeof seg);
    const char *colon;

    if (n < 0) return 0;
    /* NTFS: "name::$DATA" and "name:stream" open name, and trailing dots and spaces go */
    colon = memchr(seg, ':', (size_t)n);
    if (colon) n = (int)(colon - seg);
    while (n && (seg[n - 1] == '.' || seg[n - 1] == ' ')) n--;
    seg[n] = 0;
    for (int i = 0; RULES_FILES[i]; i++)
        if (strcmp(seg, RULES_FILES[i]) == 0 || short_alias(seg, (size_t)n, RULES_FILES[i])) return 1;
    return 0;
}
