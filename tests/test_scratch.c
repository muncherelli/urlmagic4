/* A thread's scratch must not keep PCRE2's backtracking memory from one heavy request for
   the rest of the thread's life. Includes the engine itself to see inside.  make test */
#include <stdio.h>
#include "../src/engine/rewrite.c"

int main(void)
{
    static char url[8000];
    const char *rules = "RewriteEngine On\nRewriteRule ^(?:(a)|b)*c$ /x [L]\n";
    um_rules *r = um_parse(rules, strlen(rules), UM_DIALECT_APACHE, NULL, NULL);
    um_scratch *sc = um_scratch_new();
    um_env env = { NULL, NULL, NULL };
    um_result res;
    size_t heavy, after;
    int failures = 0;

    url[0] = '/';
    memset(url + 1, 'a', sizeof url - 2);
    um_apply_s(r, url, &env, sc, &res);             /* deep backtracking, no match */
    heavy = pcre2_get_match_data_heapframes_size(sc->md);
    um_apply_s(r, "/c", &env, sc, &res);            /* the next request */
    after = pcre2_get_match_data_heapframes_size(sc->md);
    if (heavy <= FRAMES_KEEP) {
        printf("FAIL the heavy request used only %lu bytes of frames; make it heavier\n", (unsigned long)heavy);
        failures++;
    }
    if (after > FRAMES_KEEP) {
        printf("FAIL the scratch still holds %lu bytes of frames\n", (unsigned long)after);
        failures++;
    }
    if (res.action != UM_REWRITE || strcmp(res.url, "/x") != 0) {
        printf("FAIL the scratch doesn't work after letting the frames go\n");
        failures++;
    }
    printf("frames: %lu bytes after the heavy request, %lu after the next; %d failed\n",
           (unsigned long)heavy, (unsigned long)after, failures);
    um_scratch_free(sc);
    um_rules_free(r);
    return failures ? 1 : 0;
}
