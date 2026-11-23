/* the validator's report (src/engine/validate.c).  make test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/engine/validate.h"

static int failures;
static int checked;
static char got[2000];

static void ok(int cond, const char *what)
{
    checked++;
    if (!cond) { failures++; printf("FAIL: %s\n", what); }
}

static void emit(void *ctx, const char *line)
{
    (void)ctx;
    strcat(got, line);
    strcat(got, "|");
}

static int run(const char *text, int *rules)
{
    got[0] = 0;
    return um_validate("rules.txt", text, strlen(text), emit, NULL, rules);
}

int main(void)
{
    int rules = -1;
    int n;
    char *big;

    n = run("RewriteEngine On\nRewriteRule ^a$ /b [L]\nRewriteRule ^c$ /d\n", &rules);
    ok(n == 0 && rules == 2 && !got[0], "a good file: no problems, two rules");

    n = run("RewriteEngine On\r\nRewriteRule ^a$ /b\r\nRewriteRule ^(c$ /d\r\n", &rules);
    ok(n == 1 && strstr(got, "rules.txt(3): ") == got && rules == 0, "bad regex on line 3, CRLF file");

    n = run("RewriteEngine On\nRewriteRule ^a$ /b [ZZ]\n\n\nRewriteRule ^(c$ /d\n", &rules);
    ok(n == 2 && strstr(got, "rules.txt(2): ") == got && strstr(got, "|rules.txt(5): "), "two problems, their lines");

    n = run("", &rules);
    ok(n == 0 && rules == 0, "an empty file is fine");

    big = malloc((size_t)UM_MAX_RULES_FILE + 2);
    memset(big, '#', (size_t)UM_MAX_RULES_FILE + 1);
    got[0] = 0;
    n = um_validate("big.txt", big, (size_t)UM_MAX_RULES_FILE + 1, emit, NULL, &rules);
    ok(n == 1 && strstr(got, "big.txt: too big") == got, "over 1 MB");
    got[0] = 0;
    n = um_validate("big.txt", big, (size_t)UM_MAX_RULES_FILE, emit, NULL, &rules);
    ok(n == 0, "exactly 1 MB is fine");
    free(big);

    printf("test_validate: %d checks, %d failed\n", checked, failures);
    return failures ? 1 : 0;
}
