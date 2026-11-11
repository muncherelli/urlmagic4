/*
 * The rewrite engine: parses rule files and applies them to a URL. Plain C, no Windows
 * dependencies, so it is tested natively on Linux (tests/) and linked into the IIS filter.
 */
#ifndef URLMAGIC4_REWRITE_H
#define URLMAGIC4_REWRITE_H

#include <stddef.h>

/* rule file formats */
enum { UM_DIALECT_APACHE = 0 };

/* what to do with the request */
enum { UM_NONE = 0, UM_REWRITE, UM_REDIRECT, UM_FORBIDDEN, UM_GONE, UM_FAIL };

/* um_env.file_type results */
enum { UM_FT_NONE = 0, UM_FT_FILE = 1, UM_FT_DIR = 2 };

typedef struct um_env {
    void *ctx;
    /* Value of %{NAME} (e.g. "HTTP_HOST", "HTTP:User-Agent", "REQUEST_FILENAME"), written to
       buf (cap bytes); return buf, a constant string, or NULL if unknown (treated as "").
       path is the current URL path, %-decoded, with a leading "/". REQUEST_URI and
       QUERY_STRING are answered by the engine itself. */
    const char *(*var)(void *ctx, const char *name, const char *path, char *buf, size_t cap);
    /* -f / -d tests: what is at this file-system path */
    int (*file_type)(void *ctx, const char *fspath);
} um_env;

/* parse messages: line number in the rules file and a description */
typedef void (*um_log_fn)(void *ctx, int line, const char *msg);

typedef struct um_rules um_rules;

typedef struct {
    int action;     /* UM_* */
    int status;     /* HTTP status for UM_REDIRECT (301, 302, 303, 307) */
    char *url;      /* new URL ("/path?query") for UM_REWRITE, Location for UM_REDIRECT */
    int line;       /* rules-file line of the last rule that matched; 0 = none */
} um_result;

/* Bad lines are reported through log and skipped; returns NULL only when out of memory. */
um_rules *um_parse(const char *text, size_t len, int dialect, um_log_fn log, void *logctx);
void um_rules_free(um_rules *r);
int um_rules_count(const um_rules *r);

/* Per-request working memory, reused from one request to the next so that applying rules
   normally allocates nothing. One per thread: a scratch must not be used by two threads at
   once. um_scratch_new returns NULL when out of memory. */
typedef struct um_scratch um_scratch;
um_scratch *um_scratch_new(void);
void um_scratch_free(um_scratch *s);

/* url: the request URL as received ("/path?query", path still %-encoded). Returns out->action.
   out->url lives in the scratch: valid until the scratch's next use; don't um_result_free it. */
int um_apply_s(const um_rules *r, const char *url, const um_env *env, um_scratch *s, um_result *out);

/* The same with a scratch of its own; out->url is then allocated: free with um_result_free. */
int um_apply(const um_rules *r, const char *url, const um_env *env, um_result *out);
void um_result_free(um_result *res);

#endif
