/* Rewrite engine tests, run natively:  make test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/engine/rewrite.h"

static int failures, passed;

/* a pretend web site: these paths exist under the docroot "/site" */
static const char *FILES[] = { "/site/index.cfm", "/site/assets/site.css", "/site/old.htm", NULL };
static const char *DIRS[] = { "/site", "/site/assets", NULL };

static const char *var(void *ctx, const char *name, const char *path, char *buf, size_t cap)
{
    (void)ctx;
    if (strcmp(name, "HTTP_HOST") == 0) return "dev.muncherelli.com";
    if (strcmp(name, "HTTP:User-Agent") == 0) return "Mozilla/4.0 (compatible; MSIE 4.01; Windows NT)";
    if (strcmp(name, "REQUEST_FILENAME") == 0) {
        snprintf(buf, cap, "/site%s", path);
        return buf;
    }
    return NULL;
}

static int file_type(void *ctx, const char *p)
{
    int i;
    char path[256];
    size_t n;
    (void)ctx;
    /* like GetFileAttributes, a trailing slash doesn't matter */
    snprintf(path, sizeof path, "%s", p);
    n = strlen(path);
    if (n > 1 && path[n - 1] == '/') path[n - 1] = 0;
    for (i = 0; FILES[i]; i++) if (strcmp(path, FILES[i]) == 0) return UM_FT_FILE;
    for (i = 0; DIRS[i]; i++) if (strcmp(path, DIRS[i]) == 0) return UM_FT_DIR;
    return UM_FT_NONE;
}

static void logline(void *ctx, int line, const char *msg)
{
    int *count = ctx;
    (*count)++;
    if (getenv("VERBOSE")) printf("    log: line %d: %s\n", line, msg);
}

/* expect: action, then the URL (rewrite/redirect) or NULL */
static void check(const char *name, const char *rules, const char *url,
                  int action, int status, const char *expect_url)
{
    int logs = 0;
    um_env env = { NULL, var, file_type };
    um_rules *r = um_parse(rules, strlen(rules), UM_DIALECT_APACHE, logline, &logs);
    um_result res;
    int ok;

    um_apply(r, url, &env, &res);
    ok = res.action == action && (!status || res.status == status) &&
         ((expect_url == NULL && res.url == NULL) ||
          (expect_url && res.url && strcmp(expect_url, res.url) == 0));
    if (ok) {
        passed++;
    } else {
        failures++;
        printf("FAIL %s\n  url:      %s\n  expected: action %d status %d url %s\n  got:      action %d status %d url %s\n",
               name, url, action, status, expect_url ? expect_url : "(none)",
               res.action, res.status, res.url ? res.url : "(none)");
    }
    um_result_free(&res);
    um_rules_free(r);
}

static void check_log(const char *name, const char *rules, int expect_logs, int expect_rules)
{
    int logs = 0;
    um_rules *r = um_parse(rules, strlen(rules), UM_DIALECT_APACHE, logline, &logs);
    if (logs == expect_logs && um_rules_count(r) == expect_rules) passed++;
    else {
        failures++;
        printf("FAIL %s: expected %d log lines and %d rules, got %d and %d\n", name,
               expect_logs, expect_rules, logs, um_rules_count(r));
    }
    um_rules_free(r);
}

static const char *FRONT =
    "RewriteEngine On\n"
    "RewriteCond %{REQUEST_FILENAME} !-f\n"
    "RewriteCond %{REQUEST_FILENAME} !-d\n"
    "RewriteRule ^(.*)$ index.cfm?page=$1 [L,QSA]\n";

int main(void)
{
    /* the front controller */
    check("front: pretty url", FRONT, "/about/team", UM_REWRITE, 0, "/index.cfm?page=about/team");
    check("front: keeps query (QSA)", FRONT, "/about?x=1", UM_REWRITE, 0, "/index.cfm?page=about&x=1");
    check("front: real file untouched", FRONT, "/assets/site.css", UM_NONE, 0, NULL);
    check("front: real dir untouched", FRONT, "/assets", UM_NONE, 0, NULL);
    check("front: index.cfm untouched", FRONT, "/index.cfm?page=x", UM_NONE, 0, NULL);
    check("front: root is a dir", FRONT, "/", UM_NONE, 0, NULL);
    check("front: decodes then re-escapes", FRONT, "/hello%20world", UM_REWRITE, 0, "/index.cfm?page=hello%20world");

    /* engine off */
    check("engine off", "RewriteRule ^(.*)$ x.cfm [L]\n", "/a", UM_NONE, 0, NULL);
    check("engine off explicitly", "RewriteEngine Off\nRewriteRule ^(.*)$ x.cfm [L]\n", "/a", UM_NONE, 0, NULL);

    /* redirects */
    check("R=301", "RewriteEngine On\nRewriteRule ^old-page$ /about [R=301,L]\n", "/old-page",
          UM_REDIRECT, 301, "/about");
    check("R default 302 keeps query", "RewriteEngine On\nRewriteRule ^old$ /new [R]\n", "/old?a=1",
          UM_REDIRECT, 302, "/new?a=1");
    check("absolute URL redirects", "RewriteEngine On\nRewriteRule ^go$ https://example.com/x [L]\n", "/go",
          UM_REDIRECT, 302, "https://example.com/x");
    check("QSD drops query", "RewriteEngine On\nRewriteRule ^old$ /new [R=301,QSD]\n", "/old?a=1",
          UM_REDIRECT, 301, "/new");
    check("Redirect prefix", "Redirect permanent /blog /news\n", "/blog/2001/post?x=1",
          UM_REDIRECT, 301, "/news/2001/post?x=1");
    check("Redirect not partial word", "Redirect /blog /news\n", "/blogger", UM_NONE, 0, NULL);
    check("Redirect gone", "Redirect gone /dead\n", "/dead", UM_GONE, 410, NULL);
    check("RedirectMatch", "RedirectMatch 301 ^/item/([0-9]+)$ /items.cfm?id=$1\n", "/item/42",
          UM_REDIRECT, 301, "/items.cfm?id=42");

    /* conditions */
    check("host condition matches", "RewriteEngine On\nRewriteCond %{HTTP_HOST} ^dev\\. [NC]\nRewriteRule ^beta/(.*)$ pages/beta.cfm?p=$1 [L]\n",
          "/beta/x", UM_REWRITE, 0, "/pages/beta.cfm?p=x");
    check("host condition fails", "RewriteEngine On\nRewriteCond %{HTTP_HOST} ^www\\.\nRewriteRule ^beta/(.*)$ pages/beta.cfm?p=$1 [L]\n",
          "/beta/x", UM_NONE, 0, NULL);
    check("%N from condition", "RewriteEngine On\nRewriteCond %{HTTP_HOST} ^([a-z]+)\\.\nRewriteRule ^(.*)$ /sites/%1/$1 [L]\n",
          "/page", UM_REWRITE, 0, "/sites/dev/page");
    check("OR conditions", "RewriteEngine On\nRewriteCond %{HTTP_HOST} ^nope [OR]\nRewriteCond %{HTTP_HOST} ^dev\nRewriteRule ^x$ /y [L]\n",
          "/x", UM_REWRITE, 0, "/y");
    check("HTTP:header condition", "RewriteEngine On\nRewriteCond %{HTTP:User-Agent} MSIE\nRewriteRule ^$ /old-browser.htm [L]\n",
          "/", UM_REWRITE, 0, "/old-browser.htm");
    check("= string condition", "RewriteEngine On\nRewriteCond %{QUERY_STRING} =debug\nRewriteRule ^(.*)$ - [F]\n",
          "/x?debug", UM_FORBIDDEN, 403, NULL);

    /* flags */
    check("F", "RewriteEngine On\nRewriteRule \\.bak$ - [F]\n", "/db.bak", UM_FORBIDDEN, 403, NULL);
    check("G", "RewriteEngine On\nRewriteRule ^gone$ - [G]\n", "/gone", UM_GONE, 410, NULL);
    check("NC", "RewriteEngine On\nRewriteRule ^ABOUT$ /about.cfm [NC,L]\n", "/about", UM_REWRITE, 0, "/about.cfm");
    check("case matters without NC", "RewriteEngine On\nRewriteRule ^ABOUT$ /about.cfm [L]\n", "/about", UM_NONE, 0, NULL);
    check("negated pattern", "RewriteEngine On\nRewriteRule !^assets/ /index.cfm [L]\n", "/x", UM_REWRITE, 0, "/index.cfm");
    check("L stops", "RewriteEngine On\nRewriteRule ^a$ /b [L]\nRewriteRule ^b$ /c [L]\n", "/a", UM_REWRITE, 0, "/b");
    check("no L continues", "RewriteEngine On\nRewriteRule ^a$ /b\nRewriteRule ^b$ /c [L]\n", "/a", UM_REWRITE, 0, "/c");
    check("S skips", "RewriteEngine On\nRewriteRule ^a$ - [S=1]\nRewriteRule ^a$ /never [L]\nRewriteRule ^a$ /yes [L]\n",
          "/a", UM_REWRITE, 0, "/yes");
    check("C chain broken", "RewriteEngine On\nRewriteRule ^nope$ /x [C]\nRewriteRule ^a$ /never\nRewriteRule ^a$ /yes [L]\n",
          "/a", UM_REWRITE, 0, "/yes");
    check("RewriteBase", "RewriteEngine On\nRewriteBase /app\nRewriteRule ^(.*)$ index.cfm?p=$1 [L]\n",
          "/x", UM_REWRITE, 0, "/app/index.cfm?p=x");
    check("assets passthrough with -", "RewriteEngine On\nRewriteRule ^assets/ - [L]\nRewriteRule ^(.*)$ index.cfm?page=$1 [L,QSA]\n",
          "/assets/missing.png", UM_NONE, 0, NULL);
    check("IfModule wrapper ignored", "<IfModule mod_rewrite.c>\nRewriteEngine On\nRewriteRule ^a$ /b [L]\n</IfModule>\n",
          "/a", UM_REWRITE, 0, "/b");
    check("line continuation", "RewriteEngine On\nRewriteRule ^a$ \\\n  /b [L]\n", "/a", UM_REWRITE, 0, "/b");
    check("CRLF file", "RewriteEngine On\r\nRewriteRule ^a$ /b [L]\r\n", "/a", UM_REWRITE, 0, "/b");
    check("quoted args", "RewriteEngine On\nRewriteRule \"^a b$\" /c [L]\n", "/a%20b", UM_REWRITE, 0, "/c");

    /* bad input is reported and skipped, the rest still works */
    check_log("bad regex", "RewriteEngine On\nRewriteRule ^(a$ /b\nRewriteRule ^c$ /d\n", 1, 1);
    check_log("bad flag", "RewriteEngine On\nRewriteRule ^a$ /b [ZZ]\n", 1, 0);
    check_log("unknown directive", "Frobnicate on\n", 1, 0);
    check_log("dangling cond", "RewriteEngine On\nRewriteCond %{HTTP_HOST} x\n", 1, 0);
    check_log("comments and blanks", "# hello\n\n   # indented\nRewriteEngine On\n", 0, 0);

    /* the rules-file line of the rule that decided (for debug logging) */
    {
        const char *rules = "# comment\nRewriteEngine On\nRewriteRule ^x$ /y\nRewriteRule ^a$ /b [L]\n";
        um_env env = { NULL, var, file_type };
        um_rules *r = um_parse(rules, strlen(rules), UM_DIALECT_APACHE, NULL, NULL);
        um_result res;
        um_apply(r, "/a", &env, &res);
        if (res.line == 4) passed++; else { failures++; printf("FAIL rule line: expected 4, got %d\n", res.line); }
        um_result_free(&res);
        um_apply(r, "/zzz", &env, &res);
        if (res.line == 0) passed++; else { failures++; printf("FAIL rule line for no match: got %d\n", res.line); }
        um_result_free(&res);
        um_rules_free(r);
    }

    printf("%d passed, %d failed\n", passed, failures);
    return failures ? 1 : 0;
}
