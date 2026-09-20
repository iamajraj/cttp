/* ==========================================================================
 * buf.h — growable byte buffer.
 * ========================================================================== */
#ifndef CTTP_BUF_H
#define CTTP_BUF_H

#include <stddef.h>

typedef struct buf {
    char  *data;    /* malloc'd, always NUL-terminated after every append */
    size_t len;     /* bytes used (excludes the implicit NUL)             */
    size_t cap;     /* bytes allocated                                    */
} buf_t;

void buf_reserve(buf_t *b, size_t need);
void buf_append(buf_t *b, const void *p, size_t n);
void buf_append_str(buf_t *b, const char *s);
void buf_printf(buf_t *b, const char *fmt, ...);
void buf_consume(buf_t *b, size_t n);   /* drop n leading bytes */
void buf_free(buf_t *b);

#endif
