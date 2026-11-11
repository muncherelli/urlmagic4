/*
 * urlmagic-bench.exe: times the rewrite engine on the machine it runs on (NT 4.0 and up).
 *   urlmagic-bench > bench.txt
 * Each scenario applies its rules to one URL many times; the result is microseconds per request,
 * the best of 5 runs. -f/-d tests look at the real disk (C:\ plus the URL path).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../src/engine/rewrite.h"

#define ROUNDS 4000
#define RUNS   5

static HANDLE out;

static void say(const char *s)
{
    DWORD w;
    WriteFile(out, s, (DWORD)lstrlenA(s), &w, NULL);
}

/* like the filter's: a few names, compared without locale-aware calls */
static const char *var(void *ctx, const char *name, const char *path, char *buf, size_t cap)
{
    size_t i;
    (void)ctx;
    if (name[0] == 'H' && name[5] == 'H') return "dev.muncherelli.com";             /* HTTP_HOST */
    if (name[0] == 'R' && name[8] == 'F') {                                         /* REQUEST_FILENAME */
        lstrcpynA(buf, "C:", (int)cap);
        lstrcpynA(buf + 2, path, (int)cap - 2);
        for (i = 0; buf[i]; i++) if (buf[i] == '/') buf[i] = '\\';
        return buf;
    }
    return NULL;
}

static int file_type(void *ctx, const char *p)
{
    DWORD a = GetFileAttributesA(p);
    (void)ctx;
    if (a == 0xFFFFFFFF) return UM_FT_NONE;
    return (a & FILE_ATTRIBUTE_DIRECTORY) ? UM_FT_DIR : UM_FT_FILE;
}

static void scenario(const char *name, const char *rules, const char *url)
{
    um_env env = { NULL, var, file_type };
    um_rules *r = um_parse(rules, (size_t)lstrlenA(rules), UM_DIALECT_APACHE, NULL, NULL);
    um_scratch *sc = um_scratch_new();
    um_result res;
    LARGE_INTEGER f, t0, t1;
    LONGLONG best = 0;
    DWORD ns;
    int i, run;
    char line[300];

    if (!r || !sc) { say("out of memory\r\n"); return; }
    for (i = 0; i < 100; i++) um_apply_s(r, url, &env, sc, &res);     /* warm up */
    QueryPerformanceFrequency(&f);
    for (run = 0; run < RUNS; run++) {
        QueryPerformanceCounter(&t0);
        for (i = 0; i < ROUNDS; i++) um_apply_s(r, url, &env, sc, &res);
        QueryPerformanceCounter(&t1);
        if (!run || t1.QuadPart - t0.QuadPart < best) best = t1.QuadPart - t0.QuadPart;
    }
    /* nanoseconds per request */
    ns = (DWORD)(best * 1000000000 / f.QuadPart / ROUNDS);
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
    say("urlmagic4 engine benchmark: microseconds per request, best of 5 x 4000\r\n\r\n");
    scenario("no rule matches (1 rule)", "RewriteEngine On\nRewriteRule ^magic$ index.cfm [L]\n",
             "/assets/site.css");
    scenario("no rule matches (10 rules)", "RewriteEngine On\n" TEN_MISSES, "/assets/site.css");
    scenario("no match, fails at the last byte", "RewriteEngine On\nRewriteRule ^assets/site.csx$ /x [L]\n",
             "/assets/site.css");
    scenario("^assets/ - [L] alone", "RewriteEngine On\nRewriteRule ^assets/ - [L]\n", "/assets/site.css");
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
