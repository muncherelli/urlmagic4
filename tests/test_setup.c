/* setup's command line and FilterLoadOrder editing (src/setup/setupcore.h).  make test */
#include <stdio.h>
#include <wchar.h>
#include "../src/setup/setupcore.h"

static int failures;
static int checked;

static void ok(int cond, const char *what)
{
    checked++;
    if (!cond) { failures++; printf("FAIL: %s\n", what); }
}

/* order_place's result is want */
static void place(const wchar_t *list, int pos, const wchar_t *want, const char *what)
{
    wchar_t out[256];
    int fit = order_place(list, L"urlmagic4", pos, out, 256);
    if (!fit || wcscmp(out, want) != 0) printf("  got \"%ls\" for %s\n", out, what);
    ok(fit && wcscmp(out, want) == 0, what);
}

/* parse the arguments (argv[0] is added) */
static int parse(um_args *a, const wchar_t *x1, const wchar_t *x2, const wchar_t *x3, const wchar_t *x4)
{
    const wchar_t *argv[6] = { L"setup.exe", x1, x2, x3, x4, NULL };
    int argc = 1;
    while (argv[argc]) argc++;
    return um_parse(argc, (wchar_t *const *)argv, a);
}

static void test_order(void)
{
    wchar_t out[256];
    wchar_t small[8];

    place(L"", ORDER_LAST, L"urlmagic4", "last, empty list");
    place(L"", ORDER_FIRST, L"urlmagic4", "first, empty list");
    place(L"Compression,md5filt", ORDER_LAST, L"Compression,md5filt,urlmagic4", "last");
    place(L"Compression,md5filt", ORDER_FIRST, L"urlmagic4,Compression,md5filt", "first");
    place(L"Compression,md5filt", ORDER_KEEP, L"Compression,md5filt,urlmagic4", "default: appended");
    place(L"Compression,sspifilt", ORDER_LAST, L"Compression,urlmagic4,sspifilt", "last stays before a final sspifilt");
    place(L"Compression,sspifilt", ORDER_FIRST, L"urlmagic4,Compression,sspifilt", "first");
    place(L"Compression,sspifilt", ORDER_KEEP, L"Compression,urlmagic4,sspifilt", "default: before sspifilt");
    place(L"sspifilt,Compression", ORDER_KEEP, L"urlmagic4,sspifilt,Compression", "default: sspifilt not moved");
    place(L"sspifilt,Compression", ORDER_LAST, L"sspifilt,Compression,urlmagic4", "last: sspifilt not moved");
    place(L"sspifilt,Compression", ORDER_FIRST, L"urlmagic4,sspifilt,Compression", "first: sspifilt not moved");
    place(L"a,sspifilt,b", ORDER_KEEP, L"a,urlmagic4,sspifilt,b", "default: before a middle sspifilt");
    place(L"sspifilt", ORDER_FIRST, L"urlmagic4,sspifilt", "only sspifilt, first");
    place(L"SSPIFILT", ORDER_KEEP, L"urlmagic4,SSPIFILT", "sspifilt is case-insensitive, kept as is");
    place(L"a, urlmagic4 ,b", ORDER_LAST, L"a,b,urlmagic4", "already there: moved, trimmed");
    place(L"a,URLMAGIC4,b", ORDER_FIRST, L"urlmagic4,a,b", "already there, other case: once");
    place(L"a,,b,", ORDER_LAST, L"a,b,urlmagic4", "empty items dropped");
    place(L"", ORDER_KEEP, L"urlmagic4", "default, empty list");

    ok(!order_place(L"aaaa", L"urlmagic4", ORDER_LAST, small, 8), "doesn't fit: reported");
    ok(sc_len(small) < 8, "doesn't fit: still terminated");

    ok(order_has(L"a,urlmagic4,b", L"urlmagic4"), "has");
    ok(order_has(L"a, URLMagic4 ,b", L"urlmagic4"), "has, case and spaces");
    ok(!order_has(L"a,urlmagic4x,b", L"urlmagic4"), "no prefix match");
    ok(!order_has(L"a,xurlmagic4,b", L"urlmagic4"), "no suffix match");
    ok(!order_has(L"", L"urlmagic4"), "has, empty");

    ok(order_remove(L"a,urlmagic4,b", L"urlmagic4", out, 256) && wcscmp(out, L"a,b") == 0, "remove");
    ok(order_remove(L"urlmagic4", L"urlmagic4", out, 256) && wcscmp(out, L"") == 0, "remove the only one");
    ok(order_remove(L"a,b", L"urlmagic4", out, 256) && wcscmp(out, L"a,b") == 0, "remove, not there");

    ok(order_placed(L"a,urlmagic4", L"urlmagic4", ORDER_LAST), "placed last");
    ok(order_placed(L"a,urlmagic4,sspifilt", L"urlmagic4", ORDER_LAST), "placed last before sspifilt");
    ok(!order_placed(L"urlmagic4,a", L"urlmagic4", ORDER_LAST), "not last");
    ok(order_placed(L"urlmagic4,a", L"urlmagic4", ORDER_FIRST), "placed first");
    ok(!order_placed(L"a,urlmagic4", L"urlmagic4", ORDER_FIRST), "not first");
    ok(order_placed(L"a,urlmagic4,b", L"urlmagic4", ORDER_KEEP), "anywhere");
    ok(!order_placed(L"a,b", L"urlmagic4", ORDER_KEEP), "missing");
    /* an upgraded 4.1.x box: sspifilt, urlmagic4. With no /order that is current */
    ok(order_placed(L"sspifilt,urlmagic4", L"urlmagic4", ORDER_KEEP), "after sspifilt: current without /order");
    ok(order_placed(L"sspifilt,urlmagic4", L"urlmagic4", ORDER_LAST), "after sspifilt: last");
    ok(!order_placed(L"urlmagic4,a,sspifilt", L"urlmagic4", ORDER_LAST), "not last, sspifilt after");
    ok(!order_placed(L"a,urlmagic4,b,sspifilt", L"urlmagic4", ORDER_LAST), "not last, two after");
}

static void test_parse(void)
{
    um_args a;

    ok(parse(&a, L"install", NULL, NULL, NULL) == 0 && a.cmd == CMD_INSTALL && !a.quiet && !a.dir, "install");
    ok(parse(&a, L"INSTALL", L"D:\\um", NULL, NULL) == 0 && wcscmp(a.dir, L"D:\\um") == 0, "install folder");
    ok(parse(&a, L"install", L"/quiet", NULL, NULL) == 0 && a.quiet, "/quiet");
    ok(parse(&a, L"/QUIET", L"install", NULL, NULL) == 0 && a.quiet && a.cmd == CMD_INSTALL, "switch first");
    ok(parse(&a, L"install", L"/quiet", L"D:\\um", NULL) == 0 && a.quiet && wcscmp(a.dir, L"D:\\um") == 0,
       "folder after a switch");
    ok(parse(&a, L"install", L"-quiet", NULL, NULL) == 0 && a.quiet, "dash switch");
    ok(parse(&a, L"install", L"/norestart", L"/notab", NULL) == 0 && a.norestart && a.notab && !a.stopiis,
       "/norestart /notab");
    ok(parse(&a, L"install", L"/stopiis", NULL, NULL) == 0 && a.stopiis, "/stopiis");
    ok(parse(&a, L"install", L"/order", L"first", NULL) == 0 && a.order == ORDER_FIRST, "/order first");
    ok(parse(&a, L"install", L"/order", L"LAST", NULL) == 0 && a.order == ORDER_LAST, "/order last");
    ok(parse(&a, L"install", NULL, NULL, NULL) == 0 && a.order == ORDER_KEEP, "no /order");
    ok(parse(&a, L"install", L"/order", NULL, NULL) != 0 && a.err, "/order alone");
    ok(parse(&a, L"install", L"/order", L"middle", NULL) != 0, "/order middle");
    ok(parse(&a, L"uninstall", NULL, NULL, NULL) == 0 && a.cmd == CMD_UNINSTALL && !a.purge, "uninstall");
    ok(parse(&a, L"uninstall", L"/purge", NULL, NULL) == 0 && a.purge, "uninstall /purge");
    ok(parse(&a, L"/remove", NULL, NULL, NULL) == 0 && a.cmd == CMD_UNINSTALL, "/remove");
    ok(parse(&a, L"version", NULL, NULL, NULL) == 0 && a.cmd == CMD_VERSION, "version");
    ok(parse(&a, L"help", NULL, NULL, NULL) == 0 && a.cmd == CMD_HELP, "help");
    ok(parse(&a, L"/?", NULL, NULL, NULL) == 0 && a.cmd == CMD_HELP, "/?");
    ok(parse(&a, L"-h", NULL, NULL, NULL) == 0 && a.cmd == CMD_HELP, "-h");
    ok(parse(&a, L"install", L"/?", NULL, NULL) == 0 && a.cmd == CMD_HELP, "install /?");

    ok(parse(&a, L"/quiet", NULL, NULL, NULL) != 0 && a.quiet, "no command; /quiet still known");
    ok(parse(&a, L"frobnicate", NULL, NULL, NULL) != 0, "unknown command");
    ok(parse(&a, L"install", L"/frob", NULL, NULL) != 0, "unknown switch");
    ok(parse(&a, L"install", L"a", L"b", NULL) != 0, "two folders");
    ok(parse(&a, L"uninstall", L"a", NULL, NULL) != 0, "uninstall takes no folder");
    ok(parse(&a, L"install", L"/purge", NULL, NULL) != 0, "/purge with install");
    ok(parse(&a, L"uninstall", L"/order", L"first", NULL) != 0, "/order with uninstall");
    ok(parse(&a, L"uninstall", L"/notab", NULL, NULL) != 0, "/notab with uninstall");
    ok(parse(&a, L"install", L"/norestart", L"/stopiis", NULL) != 0, "/norestart with /stopiis");
    ok(parse(&a, L"uninstall", L"install", NULL, NULL) != 0, "two commands");
}

/* /quiet is found even when the rest of the command line is wrong */
static void test_quiet(void)
{
    const wchar_t *a1[] = { L"setup.exe", L"install", L"/bogus", L"/quiet" };
    const wchar_t *a2[] = { L"setup.exe", L"install", L"/bogus" };
    const wchar_t *a3[] = { L"setup.exe", L"-QUIET" };
    um_args a;
    ok(um_quiet(4, (wchar_t *const *)a1), "quiet found after a bad switch");
    ok(um_parse(4, (wchar_t *const *)a1, &a) == 2 && a.err, "bad switch: usage error");
    ok(!um_quiet(3, (wchar_t *const *)a2), "no /quiet");
    ok(um_quiet(2, (wchar_t *const *)a3), "-QUIET");
    ok(parse(&a, L"version", L"/quiet", NULL, NULL) == 0 && a.cmd == CMD_VERSION && a.quiet, "version /quiet");
}

int main(void)
{
    test_order();
    test_parse();
    test_quiet();
    printf("test_setup: %d checks, %d failed\n", checked, failures);
    return failures ? 1 : 0;
}
