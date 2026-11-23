/*
 * urlmagic4-validate <file>: checks a rules file with the engine's own parser, on Linux (or
 * anywhere with a C library), so it can be rejected before it is uploaded to a machine.
 *
 *   name(line): message        one for each problem, on standard output
 *   result=ok code=0 rules=N   or   result=error code=6 errors=N   or   result=error code=1 msg=...
 *
 * Exit codes are setup's: 0 fine, 1 can't read the file (or out of memory), 2 wrong command line,
 * 6 the file has problems. Same source as the filter (src/engine), built by `make validate`.
 */
#include <stdio.h>
#include <stdlib.h>
#include "../src/engine/validate.h"

static void emit(void *ctx, const char *line)
{
    (void)ctx;
    printf("%s\r\n", line);
}

int main(int argc, char **argv)
{
    FILE *f;
    char *text;
    size_t got;
    int rules = 0;
    int errors;

    if (argc != 2 || argv[1][0] == '-') {
        fprintf(stderr, "usage: %s <rules file>\n", argc ? argv[0] : "urlmagic4-validate");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        printf("result=error code=1 msg=can't read the file\r\n");
        return 1;
    }
    /* one byte over the limit is enough to know it is too big */
    text = malloc((size_t)UM_MAX_RULES_FILE + 2);
    if (!text) {
        fclose(f);
        printf("result=error code=1 msg=out of memory\r\n");
        return 1;
    }
    got = fread(text, 1, (size_t)UM_MAX_RULES_FILE + 1, f);
    if (ferror(f)) {
        fclose(f);
        free(text);
        printf("result=error code=1 msg=can't read the file\r\n");
        return 1;
    }
    fclose(f);
    errors = um_validate(argv[1], text, got, emit, NULL, &rules);
    free(text);
    if (errors < 0) {
        printf("result=error code=1 msg=out of memory\r\n");
        return 1;
    }
    if (errors) {
        printf("result=error code=6 errors=%d\r\n", errors);
        return 6;
    }
    printf("result=ok code=0 rules=%d\r\n", rules);
    return 0;
}
