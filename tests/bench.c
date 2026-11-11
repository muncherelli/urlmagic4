/*
 * urlmagic-bench.exe: times the rewrite engine on the machine it runs on (NT 4.0 and up).
 *   urlmagic-bench > bench.txt
 * Each scenario applies its rules to one URL many times; the result is microseconds per request.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../src/engine/rewrite.h"

#define ROUNDS 20000

static HANDLE out;

static void say(const char *s)
{
    DWORD w;
    WriteFile(out, s, (DWORD)lstrlenA(s), &w, NULL);
}

static const char *var(void *ctx, const char *name, const char *path, char *buf, size_t cap)
{
    (void)ctx; (void)path; (void)buf; (void)cap;
    if (lstrcmpiA(name, "HTTP_HOST") == 0) return "dev.muncherelli.com";
    return NULL;
}

static int file_type(void *ctx, const char *p)
{
    (void)ctx; (void)p;
    return UM_FT_NONE;
}

static void scenario(const char *name, const char *rules, const char *url)
{
    um_env env = { NULL, var, file_type };
    um_rules *r = um_parse(rules, (size_t)lstrlenA(rules), UM_DIALECT_APACHE, NULL, NULL);
    um_scratch *sc = um_scratch_new();
    um_result res;
    LARGE_INTEGER f, t0, t1;
    DWORD ns;
    int i;
    char line[300];

    if (!r || !sc) { say("out of memory\r\n"); return; }
    for (i = 0; i < 100; i++) um_apply_s(r, url, &env, sc, &res);     /* warm up */
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (i = 0; i < ROUNDS; i++) um_apply_s(r, url, &env, sc, &res);
    QueryPerformanceCounter(&t1);
    /* nanoseconds per request */
    ns = (DWORD)((t1.QuadPart - t0.QuadPart) * 1000000000 / f.QuadPart / ROUNDS);
    wsprintfA(line, "%-34s %5lu.%02lu us  (%s)\r\n", name, ns / 1000, (ns % 1000) / 10,
              res.action == UM_NONE ? "unchanged" : res.action == UM_REWRITE ? "rewrite" :
              res.action == UM_REDIRECT ? "redirect" : "other");
    say(line);
    um_scratch_free(sc);
    um_rules_free(r);
}

#define TEN_MISSES \
    "RewriteRule ^a1$ /x [L]\n" "RewriteRule ^a2$ /x [L]\n" "RewriteRule ^a3$ /x [L]\n" \
    "RewriteRule ^a4$ /x [L]\n" "RewriteRule ^a5$ /x [L]\n" "RewriteRule ^a6$ /x [L]\n" \
    "RewriteRule ^a7$ /x [L]\n" "RewriteRule ^a8$ /x [L]\n" "RewriteRule ^a9$ /x [L]\n" \
    "RewriteRule ^a10$ /x [L]\n"

void WINAPI start(void)
{
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    say("urlmagic4 engine benchmark, " "20000" " requests per line\r\n\r\n");
    scenario("no rule matches (1 rule)", "RewriteEngine On\nRewriteRule ^magic$ index.cfm [L]\n",
             "/assets/site.css");
    scenario("no rule matches (10 rules)", "RewriteEngine On\n" TEN_MISSES, "/assets/site.css");
    scenario("static folder ^assets/ - [L]",
             "RewriteEngine On\nRewriteRule ^assets/ - [L]\nRewriteRule ^(.*)$ index.cfm?page=$1 [L,QSA]\n",
             "/assets/site.css");
    scenario("front controller ^(.*)$",
             "RewriteEngine On\nRewriteRule ^assets/ - [L]\nRewriteRule ^(.*)$ index.cfm?page=$1 [L,QSA]\n",
             "/blog/some-post?x=1");
    scenario("front controller with -f/-d",
             "RewriteEngine On\nRewriteCond %{REQUEST_FILENAME} !-f\nRewriteCond %{REQUEST_FILENAME} !-d\n"
             "RewriteRule ^(.*)$ index.cfm?page=$1 [L,QSA]\n", "/blog/some-post");
    scenario("redirect ^old$ / [R=301]",
             "RewriteEngine On\nRewriteRule ^magic$ index.cfm [L]\nRewriteRule ^old$ / [R=301,L]\n", "/old");
    scenario("host condition + capture",
             "RewriteEngine On\nRewriteCond %{HTTP_HOST} ^(dev)\\.\nRewriteRule ^(.*)$ /%1/$1 [L]\n", "/x/y");
    {
        /* run from Explorer the window would close at once: wait for a key (not when the
           output goes to a file) */
        DWORD mode, got;
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        INPUT_RECORD ir;
        if (GetConsoleMode(out, &mode)) {
            say("\r\nPress any key to exit.");
            SetConsoleMode(in, 0);
            FlushConsoleInputBuffer(in);
            while (ReadConsoleInputA(in, &ir, 1, &got) &&
                   !(ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown)) {}
            say("\r\n");
        }
    }
    ExitProcess(0);
}
