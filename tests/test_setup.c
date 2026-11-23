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
    /* more than one entry is not current, whatever /order says */
    ok(!order_placed(L"urlmagic4,a,urlmagic4", L"urlmagic4", ORDER_KEEP), "twice: not current");
    ok(!order_placed(L"urlmagic4,urlmagic4", L"urlmagic4", ORDER_FIRST), "twice, first: not current");
    ok(!order_placed(L"a,URLMAGIC4,urlmagic4", L"urlmagic4", ORDER_LAST), "twice, last: not current");
    place(L"a,urlmagic4,b,urlmagic4,sspifilt", ORDER_KEEP, L"a,urlmagic4,b,sspifilt", "twice: one, at the first's place");
    place(L"urlmagic4,a,URLMagic4", ORDER_KEEP, L"urlmagic4,a", "twice: one, first place, others unmoved");
    ok(um_pending(3) && um_pending(4), "reboot needed and staged are pending");
    ok(!um_pending(0) && !um_pending(1) && !um_pending(2) && !um_pending(5), "an abort, in use or not, is an error");
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

/* the command line as an array (argv[0] is added), up to 8 arguments, NULL-ended */
static int parsev(um_args *a, const wchar_t *const *args)
{
    const wchar_t *argv[16] = { L"setup.exe" };
    int argc = 1;
    while (*args) argv[argc++] = *args++;
    return um_parse(argc, (wchar_t *const *)argv, a);
}

static void test_status_site(void)
{
    um_args a;
    int num = -1;
    const wchar_t *st[] = { L"status", L"/quiet", NULL };
    const wchar_t *st2[] = { L"status", L"x", NULL };
    const wchar_t *en[] = { L"site", L"3", L"enable", NULL };
    const wchar_t *di[] = { L"SITE", L"12", L"Disable", L"/quiet", NULL };
    const wchar_t *set[] = { L"site", L"3", L"set", L"format=httpd.ini", L"Debug=1", L"rulesfile=D:\\rules\\a.txt", NULL };
    const wchar_t *unc[] = { L"site", L"3", L"set", L"rulesfile=\\\\host.example\\share\\a", L"source=file", NULL };
    const wchar_t *clr[] = { L"site", L"3", L"set", L"rulesfile=", NULL };
    const wchar_t *noid[] = { L"site", NULL };
    const wchar_t *noop[] = { L"site", L"3", NULL };
    const wchar_t *badop[] = { L"site", L"3", L"on", NULL };
    const wchar_t *noset[] = { L"site", L"3", L"set", NULL };
    const wchar_t *nokv[] = { L"site", L"3", L"set", L"debug", NULL };
    const wchar_t *extra[] = { L"site", L"3", L"enable", L"debug=1", NULL };
    const wchar_t *id0[] = { L"site", L"0", L"enable", NULL };
    const wchar_t *idneg[] = { L"site", L"-1", L"enable", NULL };
    const wchar_t *idbig[] = { L"site", L"1234567890", L"enable", NULL };
    const wchar_t *idtxt[] = { L"site", L"abc", L"enable", NULL };
    const wchar_t *badkey[] = { L"site", L"3", L"set", L"colour=red", NULL };
    const wchar_t *badfmt[] = { L"site", L"3", L"set", L"format=json", NULL };
    const wchar_t *badsrc[] = { L"site", L"3", L"set", L"source=web", NULL };
    const wchar_t *baden[] = { L"site", L"3", L"set", L"enabled=yes", NULL };
    const wchar_t *relpath[] = { L"site", L"3", L"set", L"rulesfile=rules.txt", NULL };
    const wchar_t *badch[] = { L"site", L"3", L"set", L"rulesfile=C:\\a|b", NULL };
    const wchar_t *many[] = { L"site", L"3", L"set", L"debug=1", L"debug=1", L"debug=1", L"debug=1", L"debug=1",
                              L"debug=1", L"debug=1", L"debug=1", NULL };
    const wchar_t *toomany[] = { L"site", L"3", L"set", L"debug=1", L"debug=1", L"debug=1", L"debug=1", L"debug=1",
                                 L"debug=1", L"debug=1", L"debug=1", L"debug=1", NULL };

    ok(parsev(&a, st) == 0 && a.cmd == CMD_STATUS && a.quiet, "status /quiet");
    ok(parsev(&a, st2) == 2, "status takes no argument");
    ok(parsev(&a, en) == 0 && a.cmd == CMD_SITE && a.site_id == 3 && a.site_op == SITE_ENABLE, "site enable");
    ok(parsev(&a, di) == 0 && a.site_id == 12 && a.site_op == SITE_DISABLE && a.quiet, "site disable, any case");
    ok(parsev(&a, set) == 0 && a.site_op == SITE_SET && a.nkv == 3, "site set, three pairs");
    ok(parsev(&a, unc) == 0 && a.nkv == 2, "site set, UNC path");
    ok(parsev(&a, clr) == 0 && a.nkv == 1, "site set, empty rulesfile clears it");
    ok(parsev(&a, noid) == 2 && a.err, "site alone");
    ok(parsev(&a, noop) == 2, "site without a verb");
    ok(parsev(&a, badop) == 2, "site, unknown verb");
    ok(parsev(&a, noset) == 2, "site set without pairs");
    ok(parsev(&a, nokv) == 2, "site set, no equals sign");
    ok(parsev(&a, extra) == 2, "site enable takes no pairs");
    ok(parsev(&a, id0) == 6, "site 0");
    ok(parsev(&a, idneg) == 2 || parsev(&a, idneg) == 6, "site -1 is no site");
    ok(parsev(&a, idbig) == 6, "site number too long");
    ok(parsev(&a, idtxt) == 6, "site number not a number");
    ok(parsev(&a, badkey) == 6, "unknown setting");
    ok(parsev(&a, badfmt) == 6, "bad format");
    ok(parsev(&a, badsrc) == 6, "bad source");
    ok(parsev(&a, baden) == 6, "bad enabled");
    ok(parsev(&a, relpath) == 6, "relative rulesfile");
    ok(parsev(&a, badch) == 6, "bad character in rulesfile");
    ok(parsev(&a, many) == 0 && a.nkv == 8, "eight pairs");
    ok(parsev(&a, toomany) == 2, "nine pairs");

    ok(site_value_check(SK_FORMAT, L"HTACCESS", &num) == NULL && num == 1, "format, any case");
    ok(site_value_check(SK_SOURCE, L"File", &num) == NULL && num == 1, "source file");
    ok(site_value_check(SK_ENABLED, L"1", &num) == NULL && num == 1, "enabled 1");
    ok(site_value_check(SK_DEBUG, L"0", &num) == NULL && num == 0, "debug 0");
    ok(site_value_check(SK_RULESFILE, L"c:\\x", &num) == NULL, "lower-case drive");
    ok(site_value_check(SK_RULESFILE, L"c:x", &num) != NULL, "drive-relative path");
    ok(site_value_check(SK_RULESFILE, L"\\\\", &num) != NULL, "bare backslashes");
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
    test_status_site();
    test_quiet();
    printf("test_setup: %d checks, %d failed\n", checked, failures);
    return failures ? 1 : 0;
}
