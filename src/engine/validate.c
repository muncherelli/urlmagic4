#include "rewrite.h"
#include "validate.h"

typedef struct {
    const char *name;
    um_emit_fn emit;
    void *ctx;
    int errors;
} vstate;

/* "name(line): message" for emit, cut to fit */
static void report(vstate *v, int line, const char *msg)
{
    char buf[600];
    char num[16];
    size_t n = 0;
    int i = 0;
    const char *p;
    unsigned u = line > 0 ? (unsigned)line : 0;
    do { num[i++] = (char)('0' + u % 10); u /= 10; } while (u && i < 15);
    for (p = v->name; *p && n < 200; p++) buf[n++] = *p;
    if (line > 0) {
        buf[n++] = '(';
        while (i) buf[n++] = num[--i];
        buf[n++] = ')';
    }
    buf[n++] = ':';
    buf[n++] = ' ';
    for (p = msg; *p && n < sizeof buf - 1; p++) buf[n++] = (*p == '\r' || *p == '\n') ? ' ' : *p;
    buf[n] = 0;
    v->emit(v->ctx, buf);
    v->errors++;
}

static void parse_log(void *ctx, int line, const char *msg)
{
    report((vstate *)ctx, line, msg);
}

int um_validate(const char *name, const char *text, size_t len, um_emit_fn emit, void *ctx, int *rules)
{
    vstate v;
    um_rules *r;
    v.name = name;
    v.emit = emit;
    v.ctx = ctx;
    v.errors = 0;
    *rules = 0;
    if (len > UM_MAX_RULES_FILE) {
        report(&v, 0, "too big (over 1 MB), the filter ignores it");
        return 1;
    }
    r = um_parse(text, len, UM_DIALECT_APACHE, parse_log, &v);
    if (!r) return -1;
    if (!v.errors) *rules = um_rules_count(r);
    um_rules_free(r);
    return v.errors;
}
