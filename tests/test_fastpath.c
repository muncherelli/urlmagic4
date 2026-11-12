/* The engine's PCRE2-free fast path must agree with PCRE2 exactly: same match, same
   captures. Includes the engine itself to reach its internals.  make test */
#include <stdio.h>
#include "../src/engine/rewrite.c"

static const char *PATTERNS[] = {
    "^magic$", "magic", "^magic", "magic$", "^(.*)$", "(.*)", "^(.*)", "(.*)$", "^.*$", ".*",
    "^.*", ".*$", "^", "$", "^$", "", "^assets/", "\\.cfm$", "^a\\.b$", "a-b", "^old$", "ab",
    "x/y", "^A", "A$", "^ABC$", "b", "^\\/x", "^\\-$", "\\.", "aa", "^index\\.cfm$", "^ ",
    /* not fast-path: PCRE2 must keep them */
    "^a.b$", "\\d+", "a|b", "^(a)$", "^a*$", "[ab]", "\\$", "^x\\$$", "a{2}", "\\bab",
    NULL
};
static const char *SUBJECTS[] = {
    "", "magic", "Magic", "MAGIC", "magicx", "xmagic", "xmagicx", "assets/site.css", "ASSETS/x",
    "a.b", "axb", "old", "olD", "x.cfm", "x.CFM", "abab", "x/y", "ba", "/x", "-", "aaa", "AbC",
    "abc", "ABC", "a-b", "index.cfm", " x", "x$", ".", "\r", "a\rb", "\x80\xff", NULL
};

static int failures;
static int checked;
static int fast_patterns;

/* the fast path's captures are PCRE2's */
static int same_caps(const caps *c, pcre2_match_data *md, int rc)
{
    const PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
    if (c->n != rc) return 0;
    for (size_t i = 0; i < (size_t)rc * 2; i++)
        if (c->ov[i] != ov[i]) return 0;
    return 1;
}

/* one subject through PCRE2 and through the fast path */
static void compare(pcre2_code *re, pcre2_match_data *md, const fastpat *f, const char *pattern,
                    int nc, const char *subj)
{
    size_t len = strlen(subj);
    caps c;
    int rc = pcre2_match(re, (PCRE2_SPTR)subj, len, 0, 0, md, NULL);
    int fm = fast_match(f, subj, len, &c);
    int ok = (rc > 0) == (fm == 1);
    if (ok && fm == 1) ok = same_caps(&c, md, rc);
    checked++;
    if (!ok) {
        failures++;
        printf("FAIL fast path differs from PCRE2: pattern \"%s\"%s subject \"%s\": "
               "pcre2 rc %d, fast %d\n", pattern, nc ? " [NC]" : "", subj, rc, fm);
    }
}

/* one pattern, as written (nc 0) or [NC] (nc 1), against every subject */
static void check_pattern(const char *pattern, int nc, pcre2_match_data *md)
{
    fastpat f;
    int err;
    PCRE2_SIZE off;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                                   nc ? PCRE2_CASELESS : 0, &err, &off, NULL);
    if (!re) { printf("FAIL pattern %s doesn't compile\n", pattern); failures++; return; }
    analyze(pattern, nc, &f);
    if (f.kind == FP_NONE) { pcre2_code_free(re); return; }
    if (!nc) fast_patterns++;
    for (int s = 0; SUBJECTS[s]; s++) compare(re, md, &f, pattern, nc, SUBJECTS[s]);
    free(f.lit);
    pcre2_code_free(re);
}

/* sanity: the common shapes really take the fast path, and regexes don't */
static void check_shapes(void)
{
    static const char *fast[] = { "^magic$", "^assets/", "^(.*)$", "\\.cfm$", "^", NULL };
    static const char *slow[] = { "^a.b$", "\\d+", "a|b", "^(a)$", "\\$", NULL };
    fastpat f;
    for (int p = 0; fast[p]; p++) {
        analyze(fast[p], 0, &f);
        if (f.kind == FP_NONE) { failures++; printf("FAIL %s should use the fast path\n", fast[p]); }
        free(f.lit);
    }
    for (int p = 0; slow[p]; p++) {
        analyze(slow[p], 0, &f);
        if (f.kind != FP_NONE) { failures++; printf("FAIL %s must not use the fast path\n", slow[p]); }
        free(f.lit);
    }
}

int main(void)
{
    pcre2_match_data *md = pcre2_match_data_create(MAX_CAPS, NULL);

    for (int p = 0; PATTERNS[p]; p++)
        for (int nc = 0; nc < 2; nc++) check_pattern(PATTERNS[p], nc, md);
    check_shapes();
    pcre2_match_data_free(md);
    printf("fast path: %d patterns, %d comparisons with PCRE2, %d failed\n", fast_patterns, checked, failures);
    return failures ? 1 : 0;
}
