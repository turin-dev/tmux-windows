/* strbuf.h — a small growable byte buffer.
 *
 * Used to assemble a frame of terminal output before a single write, and to
 * accumulate bytes generally. Not thread-safe.
 */
#ifndef TMUXW_UTIL_STRBUF_H
#define TMUXW_UTIL_STRBUF_H

#include <stddef.h>

typedef struct strbuf {
    char  *data;
    size_t len;
    size_t cap;
} strbuf_t;

void  strbuf_init(strbuf_t *b);
void  strbuf_free(strbuf_t *b);
void  strbuf_clear(strbuf_t *b);              /* keep capacity, reset length */
void  strbuf_append(strbuf_t *b, const void *bytes, size_t n);
void  strbuf_putc(strbuf_t *b, char c);
void  strbuf_printf(strbuf_t *b, const char *fmt, ...);

#endif /* TMUXW_UTIL_STRBUF_H */
