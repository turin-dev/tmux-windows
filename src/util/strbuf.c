/* strbuf.c — see strbuf.h. */
#include "util/strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void strbuf_init(strbuf_t *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void strbuf_free(strbuf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void strbuf_clear(strbuf_t *b)
{
    b->len = 0;
}

static void strbuf_reserve(strbuf_t *b, size_t extra)
{
    size_t need = b->len + extra;
    if (need <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need)
        cap *= 2;
    b->data = (char *)realloc(b->data, cap);
    b->cap = cap;
}

void strbuf_append(strbuf_t *b, const void *bytes, size_t n)
{
    if (n == 0)
        return;
    strbuf_reserve(b, n);
    memcpy(b->data + b->len, bytes, n);
    b->len += n;
}

void strbuf_putc(strbuf_t *b, char c)
{
    strbuf_reserve(b, 1);
    b->data[b->len++] = c;
}

void strbuf_printf(strbuf_t *b, const char *fmt, ...)
{
    va_list ap, ap2;
    int n;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        strbuf_reserve(b, (size_t)n + 1);
        vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
        b->len += (size_t)n; /* drop the NUL terminator from length */
    }
    va_end(ap2);
}
