/* Rewrite engine tests, run natively:  make test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/engine/rewrite.h"

static int failures, passed;
static um_scratch *shared;      /* reused by every check, like one IIS thread's */

/* a pretend web site: these paths exist under the docroot "/site" */
static const char *FILES[] = { "/site/index.cfm", "/site/assets/site.css", "/site/old.htm", NULL };
static const char *DIRS[] = { "/site", "/site/assets", NULL };

static const char *var(void *ctx, const char *name, const char *path, char *buf, size_t cap)
{
    (void)ctx;
    if (strcmp(name, "HTTP_HOST") == 0) return "dev.muncherelli.com";
    if (strcmp(name, "HTTP:User-Agent") == 0) return "Mozilla/4.0 (compatible; MSIE 4.01; Windows NT)";
    if (strcmp(name, "HTTP:X-Evil") == 0) return "a\r\nb";
    if (strcmp(name, "HTTP:X-Back") == 0) return "\\evil.example";
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
    um_result rs;
    int ok_s;

    /* the same rules through the reused scratch must give the same answer */
    um_apply_s(r, url, &env, shared, &rs);
    ok_s = rs.action == action && (!status || rs.status == status) &&
           ((expect_url == NULL && rs.url == NULL) ||
            (expect_url && rs.url && strcmp(expect_url, rs.url) == 0));
    um_apply(r, url, &env, &res);
    ok = ok_s && res.action == action && (!status || res.status == status) &&
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
    shared = um_scratch_new();
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

    /* -f answers are reused within a request, but never for a different path (a failed rule's
       strings are rolled back, so the next path can land at the same address) */
    check("-f cache: a failed rule, then another path",
          "RewriteEngine On\nRewriteCond /site/nothing -f\nRewriteRule ^ - [F]\n"
          "RewriteCond /site/old.htm -f\nRewriteRule ^(.*)$ /found [L]\n", "/x", UM_REWRITE, 0, "/found");
    check("-f cache: !-f then !-d on the same path", FRONT, "/assets", UM_NONE, 0, NULL);

    /* how substitutions are read (split up once at load time) */
    check("subst: $ without a digit is literal", "RewriteEngine On\nRewriteRule ^a$ /a$x [L]\n", "/a",
          UM_REWRITE, 0, "/a$x");
    check("subst: %{ without } is literal", "RewriteEngine On\nRewriteRule ^a$ /a%{HTTP_HOST [L]\n", "/a",
          UM_REWRITE, 0, "/a%25%7BHTTP_HOST");
    check("subst: \\x is x", "RewriteEngine On\nRewriteRule ^a$ /a\\b [L]\n", "/a", UM_REWRITE, 0, "/ab");
    check("subst: variables", "RewriteEngine On\nRewriteRule ^a$ /%{HTTP_HOST}/%{NOPE}/x [L]\n", "/a",
          UM_REWRITE, 0, "/dev.muncherelli.com//x");
    check("subst: REQUEST_URI and QUERY_STRING", "RewriteEngine On\nRewriteRule ^a$ /b?u=%{REQUEST_URI}&q=%{query_string} [L]\n",
          "/a?z=1", UM_REWRITE, 0, "/b?u=/a&q=z=1");
    check("subst: $1 then literal then %1", "RewriteEngine On\nRewriteCond %{HTTP_HOST} ^(dev)\nRewriteRule ^(a)$ /$1-x-%1 [L]\n",
          "/a", UM_REWRITE, 0, "/a-x-dev");

    /* a %00 ends the path for every rule (it can't be used to slip past a deny rule) */
    check("%00 in the path", "RewriteEngine On\nRewriteRule \\.cfm$ - [F]\n", "/x.cfm%00.jpg",
          UM_FORBIDDEN, 403, NULL);

    /* the path is tidied before the rules see it, the way IIS will map it: "..", "." and double
       slashes can't be used to get past a rule */
    {
        static const char *DENY = "RewriteEngine On\nRewriteRule ^admin - [F]\n";
        static const char *SNEAKY[] = {
            "/x/../admin", "//admin", "/./admin", "/%2e%2e/admin", "/x/%2E%2E/admin", "/../../admin",
            "/%5cadmin", "/x%5c..%5cadmin", "/x/..\\admin", "/x/y/../../admin/", NULL
        };
        int i;
        for (i = 0; SNEAKY[i]; i++) check(SNEAKY[i], DENY, SNEAKY[i], UM_FORBIDDEN, 403, NULL);
    }
    check("clean path: kept trailing slash", FRONT, "/about/./team/", UM_REWRITE, 0, "/index.cfm?page=about/team/");
    check("clean path: .. ending a path", FRONT, "/about/team/..", UM_REWRITE, 0, "/index.cfm?page=about/");
    check("clean path: above the root stays at the root", "RewriteEngine On\nRewriteRule ^$ /home [L]\n",
          "/../..", UM_REWRITE, 0, "/home");
    check("clean path: REQUEST_URI", "RewriteEngine On\nRewriteRule ^b$ /c?u=%{REQUEST_URI} [L]\n",
          "/a/../b", UM_REWRITE, 0, "/c?u=/b");
    check("clean path: dots inside names stay", FRONT, "/a..b/.c/c.", UM_REWRITE, 0, "/index.cfm?page=a..b/.c/c.");

    /* a redirect never names another host by accident (a path after two slashes is one) */
    check("redirect: //host from the trailing-slash rule", "RewriteEngine On\nRewriteRule ^(.*)/$ /$1 [R=301,L]\n",
          "//evil.example/", UM_REDIRECT, 301, "/evil.example");
    check("redirect: //host from a capture", "RewriteEngine On\nRewriteRule ^x(.*)$ /$1 [R,L]\n",
          "/x/evil.example", UM_REDIRECT, 302, "/evil.example");
    check("redirect: /\\host from a capture", "RewriteEngine On\nRewriteRule ^x(.*)$ /$1 [R,NE,L]\n",
          "/x%5cevil.example", UM_REDIRECT, 302, "/evil.example");
    check("redirect: Redirect prefix and //", "Redirect /old /\n", "/old//evil.example", UM_REDIRECT, 302,
          "/evil.example");
    check("redirect: RedirectMatch to //", "RedirectMatch ^/x(.*)$ /$1\n", "/x/evil.example", UM_REDIRECT, 302,
          "/evil.example");

    /* CR and LF never reach the Location header, [NE] or not */
    check("NE: no CRLF in the Location", "RewriteEngine On\nRewriteRule ^go/([^/]*)$ /y?q=$1 [R,NE,L]\n",
          "/go/a%0d%0aSet-Cookie:x=1", UM_REDIRECT, 302, "/y?q=a%0D%0ASet-Cookie:x=1");
    check("NE: no CRLF in an absolute Location", "RewriteEngine On\nRewriteRule ^go/([^/]*)$ http://example.com/$1 [NE,L]\n",
          "/go/a%0d%0aSet-Cookie:x=1", UM_REDIRECT, 302, "http://example.com/a%0D%0ASet-Cookie:x=1");
    check("NE: no CRLF from a header", "RewriteEngine On\nRewriteCond %{HTTP:X-Evil} ([^z]+)\nRewriteRule ^a$ /b?%1 [R,NE,L]\n",
          "/a", UM_REDIRECT, 302, "/b?a%0D%0Ab");
    check("NE: the rest stays as written", "RewriteEngine On\nRewriteRule ^a$ /b?x=a+b&c=<d> [R,NE,L]\n",
          "/a", UM_REDIRECT, 302, "/b?x=a+b&c=<d>");
    check("redirect: \\host from a header", "RewriteEngine On\nRewriteCond %{HTTP:X-Back} (.+)\nRewriteRule ^a$ %1 [R,NE,L]\n",
          "/a", UM_REDIRECT, 302, "/evil.example");

    /* rules files are refused under every name Windows opens them by */
    {
        static char dots[200];
        static const char *YES[] = {
            "/httpd.htaccess", "/HTTPD.HTACCESS", "/sub/.htaccess", "/httpd.htaccess?x=1", "/%2ehtaccess",
            "/x%5chttpd.ini", "/x\\httpd.ini", "/httpd.htaccess.", "/httpd.htaccess%20", "/httpd.htaccess::$DATA",
            "/httpd.htaccess:x", "/httpd.htaccess%00.jpg", "/httpd.htaccess/", "/HTTPD~1.HTA", "/httpd~12.hta",
            "/HTACCE~1", "/HTF3A1~1.HTA", "/NGINX~1.CON", "/URLMAG~1.CON", "/HTTPDP~1.ERR", "/nginx.conf",
            "/urlmagic4.conf", "/httpd.parse.errors", dots, NULL
        };
        static const char *NO[] = {
            "/", "/index.cfm", "/httpd.htaccess.txt", "/htaccess", "/HTMLFI~1", "/x?f=httpd.htaccess",
            "/PROGRA~1", "/HTTPD~1.HTM", "/httpd~x.hta", "/httpd~1", "/a.htaccess", "/%zz", "", NULL
        };
        int i;
        memcpy(dots, "/httpd.htaccess", 15);         /* and 150 dots, which Windows drops */
        memset(dots + 15, '.', 150);
        for (i = 0; YES[i]; i++) {
            if (um_is_rules_file(YES[i])) passed++;
            else { failures++; printf("FAIL rules file not refused: %s\n", YES[i]); }
        }
        for (i = 0; NO[i]; i++) {
            if (!um_is_rules_file(NO[i])) passed++;
            else { failures++; printf("FAIL refused as a rules file: %s\n", NO[i]); }
        }
    }

    /* bigger than the scratch's block: overflow chunks */
    {
        static char url[6000], want[6000 * 4];
        size_t i;
        url[0] = '/';
        for (i = 1; i < 5000; i++) url[i] = (char)('a' + i % 26);
        url[i] = 0;
        snprintf(want, sizeof want, "%s%s%s%s", url, url + 1, url + 1, url + 1);
        check("long URL x4", "RewriteEngine On\nRewriteRule ^(.*)$ /$1$1$1$1 [L]\n", url,
              UM_REWRITE, 0, want);
        check("long URL x4 again (scratch reused after overflow)",
              "RewriteEngine On\nRewriteRule ^(.*)$ /$1$1$1$1 [L]\n", url, UM_REWRITE, 0, want);
    }
    /* many rules whose conditions fail: their strings are rolled back, not piled up */
    {
        static char rules[200000];
        size_t n = 0;
        int i;
        n += (size_t)snprintf(rules + n, sizeof rules - n, "RewriteEngine On\n");
        for (i = 0; i < 2000; i++)
            n += (size_t)snprintf(rules + n, sizeof rules - n,
                                  "RewriteCond %%{HTTP_HOST}/%%{REQUEST_URI} ^nope%d\nRewriteRule ^ - [F]\n", i);
        snprintf(rules + n, sizeof rules - n,
                 "RewriteCond %%{HTTP_HOST} ^(dev)\\.\nRewriteRule ^(.*)$ /%%1/$1 [L]\n");
        check("2000 failing rules, then one", rules, "/x", UM_REWRITE, 0, "/dev/x");
    }

    um_scratch_free(shared);
    printf("%d passed, %d failed\n", passed, failures);
    return failures ? 1 : 0;
}
