/* ==========================================================================
 * buf.c — growable byte buffer.
 *
 * Sockets deliver data in unpredictable chunk sizes, and an HTTP message
 * may arrive over many read() calls. Somewhere must accumulate bytes until
 * a full message exists — that is this struct (and it also builds every
 * outgoing response before writing).
 *
 * buf_printf is UNBOUNDED printf-to-buffer: it sizes the output first, so
 * handlers never need snprintf() (which forces fixed sizes on the caller).
 * ========================================================================== */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cttp.h"

/* Guarantee room for `need` more bytes (amortized doubling). */
void buf_reserve(buf_t *b, size_t need)
{
    if (b->len + need + 1 <= b->cap) return;
    size_t newcap = b->cap ? b->cap : 4096;
    while (newcap < b->len + need + 1) newcap *= 2;
    b->data = realloc(b->data, newcap);
    if (!b->data) { perror("realloc"); exit(1); }
    b->cap = newcap;
}

/* Append raw bytes. Always NUL-terminates (not counted in len) so the
 * buffer can be passed straight to strcmp & friends: binary-safe. */
void buf_append(buf_t *b, const void *p, size_t n)
{
    if (n == 0) return;
    buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_append_str(buf_t *b, const char *s) { buf_append(b, s, strlen(s)); }

/* printf-style append that grows the buffer to fit. */
void buf_vappendf(buf_t *b, const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);     /* measure first */
    va_end(ap2);
    if (n < 0) return;
    buf_reserve(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
}

void buf_printf(buf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    buf_vappendf(b, fmt, ap);
    va_end(ap);
}

/* Discard the first n bytes (used when a message is fully parsed and we
 * keep leftover bytes that belong to the next pipelined request). */
void buf_consume(buf_t *b, size_t n)
{
    if (n >= b->len) { b->len = 0; if (b->data) b->data[0] = '\0'; return; }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
    b->data[b->len] = '\0';
}

void buf_free(buf_t *b)
{
    free(b->data);
    b->data = NULL; b->len = 0; b->cap = 0;
}
