/* cdjot - djot to HTML converter */
#ifndef CDJOT_H
#define CDJOT_H
#include <stdio.h>

/*
 * Convert djot markup to HTML.
 * Reads len bytes from buf, writes HTML to out.
 * buf must remain valid for the duration of the call.
 * Not thread-safe; safe to call multiple times sequentially.
 * Returns 0 on success.
 */
int cdjot_convert(FILE *out, const char *buf, size_t len);

#endif
