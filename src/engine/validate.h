/*
 * Checks a rules file with the engine's own parser (rewrite.c), for the installer's `validate`
 * command and the Linux validator (tools/validate.c). Plain C; needs no C library beyond what
 * the engine uses. Deliberately does not include rewrite.h, so setup.c can include this.
 */
#ifndef URLMAGIC4_VALIDATE_H
#define URLMAGIC4_VALIDATE_H

#include <stddef.h>

/* the filter ignores a rules file bigger than this (src/filter/filter.c) */
#define UM_MAX_RULES_FILE (1024 * 1024)

/* gets each line of the report: "name(line): message", one per problem */
typedef void (*um_emit_fn)(void *ctx, const char *line);

/* Parses text (len bytes) as an Apache-style rules file named name. Every problem the filter
   would log is passed to emit. Returns the number of problems (0 = fine), or -1 if out of
   memory; *rules gets the number of rules when there are no problems. */
int um_validate(const char *name, const char *text, size_t len, um_emit_fn emit, void *ctx, int *rules);

#endif
