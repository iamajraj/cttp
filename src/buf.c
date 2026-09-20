/* ==========================================================================
 * buf.c — a growable byte buffer.
 *
 * Why do we need this? Sockets deliver data in unpredictable chunk sizes,
 * and an HTTP message can arrive over many read() calls. We need somewhere
 * to accumulate bytes until a full message is present — that's this struct.
 * It is also used to build outgoing responses before writing them.
 * ========================================================================== */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "cttp.h"

/* Ensure capacity for at least 'need' more bytes, amortized doubling. */
void buf_reserve(buf_t *b, size_t need)
{
    if (b->len + need + 1 <= b->cap) return;
    size_t newcap = b->cap ? b->cap : 4096;
    while (newcap < b->len + need + 1) newcap *= 2;
    b->data = realloc(b->data, newcap);
    if (!b->data) { perror("realloc"); exit(1); }
    b->cap = newcap;
}

/* Append raw bytes. Always keeps data NUL-terminated for convenience,
 * although the NUL is NOT counted in ->len (binary-safe). */
void buf_append(buf_t *b, const void *p, size_t n)
{
    if (n == 0) return;
    buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_append_str(buf_t *b, const char *s) { buf_append(b, s, strlen(s)); }

void buf_printf(buf_t *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) buf_append(b, tmp, (size_t)n);
}

/* Discard the first n bytes (memmove the rest to the front).
 * Used when we've finished parsing a message out of the read buffer and
 * want to keep any leftover bytes that belong to the next request. */
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
