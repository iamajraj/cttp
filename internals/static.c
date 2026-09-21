/* ==========================================================================
 * static.c — serving files from the webroot.
 *
 * Beyond open()+read()+write() this module layers on the modern bits:
 *   * path traversal protection  ("/../etc/passwd" must never escape)
 *   * MIME types by file extension
 *   * ETag + If-None-Match  ->  304 Not Modified
 *   * Range requests        ->  206 Partial Content (download resume)
 * ========================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "cttp.h"

/* MIME table: enough extensions to feel complete without bloat. */
static const struct { const char *ext, *mime; } MIMES[] = {
    { ".html", "text/html; charset=utf-8" },
    { ".htm",  "text/html; charset=utf-8" },
    { ".css",  "text/css; charset=utf-8" },
    { ".js",   "text/javascript; charset=utf-8" },
    { ".json", "application/json" },
    { ".txt",  "text/plain; charset=utf-8" },
    { ".png",  "image/png" },
    { ".jpg",  "image/jpeg" },
    { ".jpeg", "image/jpeg" },
    { ".gif",  "image/gif" },
    { ".svg",  "image/svg+xml" },
    { ".ico",  "image/x-icon" },
    { ".webp", "image/webp" },
    { ".woff", "font/woff" },
    { ".woff2","font/woff2" },
    { ".pdf",  "application/pdf" },
    { ".zip",  "application/zip" },
    { ".wasm", "application/wasm" },
    { ".mp3",  "audio/mpeg" },
    { ".mp4",  "video/mp4" },
};
#define NMIMES (int)(sizeof MIMES / sizeof MIMES[0])

static const char *mime_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    for (int i = 0; i < NMIMES; i++)
        if (strcasecmp(dot, MIMES[i].ext) == 0)
            return MIMES[i].mime;
    return "application/octet-stream";
}

/* Reject traversal attempts. The path is percent-decoded by the parser,
 * which is exactly why this check runs on req->path: "%2e%2e" has already
 * become ".." here, so encoded trips are caught by the same test that
 * catches literal ones.                                             */
static int is_unsafe_path(const char *p)
{
    if (!p || p[0] != '/') return 1;
    if (strstr(p, "..")) return 1;
    return 0;
}

/* Read the whole open file. Heap buffer of exactly *out_len bytes. */
static char *read_whole_file(int fd, size_t *out_len)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return NULL;
    size_t len = (size_t)st.st_size;
    char *data = malloc(len + 1);
    if (!data) return NULL;
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, data + total, len - total);
        if (n < 0) { if (errno == EINTR) continue; free(data); return NULL; }
        if (n == 0) break;             /* EOF early: trust read() */
        total += (size_t)n;
    }
    *out_len = total;
    data[total] = '\0';
    return data;
}

void static_serve(cttp_server *s, cttp_request *req, cttp_response *res)
{
    char path[2048];

    if (!s->webroot || is_unsafe_path(req->path)) {
        cttp_json_err(res, 403, "path not allowed");
        return;
    }

    /* URL path -> filesystem path (index.html for directories). */
    if (strcmp(req->path, "/") == 0)
        snprintf(path, sizeof path, "%s/index.html", s->webroot);
    else if (req->path[strlen(req->path) - 1] == '/')
        snprintf(path, sizeof path, "%s%sindex.html", s->webroot, req->path);
    else
        snprintf(path, sizeof path, "%s%s", s->webroot, req->path);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        cttp_json_err(res, 404, "resource not found");
        return;
    }
    struct stat st;
    fstat(fd, &st);
    if (S_ISDIR(st.st_mode)) {          /* "/dir" without the slash       */
        close(fd);
        snprintf(path, sizeof path, "%s%s/index.html", s->webroot, req->path);
        fd = open(path, O_RDONLY);
        if (fd < 0) { cttp_json_err(res, 404, "resource not found"); return; }
        fstat(fd, &st);
    }

    /* ETag = size + mtime. Same string => browser cache still valid. */
    char etag[64];
    snprintf(etag, sizeof etag, "\"%zx-%zx\"",
             (size_t)st.st_size, (size_t)st.st_mtime);

    const char *inm = cttp_header(req, "If-None-Match");
    if (inm && strstr(inm, etag)) {
        close(fd);
        res->status = 304;
        res->no_body = 1;
        cttp_set_header(res, "ETag", etag);
        cttp_cache(res, 3600);
        return;
    }

    /* Range: bytes=N-M, satisfied with a 206 slice. */
    const char *rh = cttp_header(req, "Range");
    if (rh) {
        unsigned long long start, end, total = (unsigned long long)st.st_size;
        if (sscanf(rh, "bytes=%llu-%llu", &start, &end) == 2 &&
            start < total && end >= start) {
            if (end >= total) end = total - 1;
        } else if (sscanf(rh, "bytes=%llu-", &start) == 1 && start < total) {
            end = total - 1;
        } else {
            /* unsatisfiable: RFC 9110 §14.2 wants the real size back */
            char cr[64];
            snprintf(cr, sizeof cr, "bytes */%llu", total);
            cttp_set_header(res, "Content-Range", cr);
            res->status = 416;
            res->no_body = 1;
            close(fd);
            return;
        }

        lseek(fd, (off_t)start, SEEK_SET);
        size_t len = (size_t)(end - start + 1);
        char *data = malloc(len);
        size_t got = 0;
        while (got < len) {
            ssize_t n = read(fd, data + got, len - got);
            if (n < 0) { if (errno == EINTR) continue; break; }
            if (n == 0) break;
            got += (size_t)n;
        }
        close(fd);

        res->status = 206;
        snprintf(res->ctype, sizeof res->ctype, "%s", mime_for(path));
        char cr[64];
        snprintf(cr, sizeof cr, "bytes %llu-%llu/%llu",
                 start, start + got - 1, total);
        cttp_set_header(res, "Content-Range", cr);
        buf_free(&res->body);
        buf_append(&res->body, data, got);
        free(data);
        return;
    }

    /* Plain 200 with the whole file. */
    size_t len = 0;
    char *data = read_whole_file(fd, &len);
    close(fd);
    if (!data) { cttp_json_err(res, 500, "read failed"); return; }

    res->status = 200;
    snprintf(res->ctype, sizeof res->ctype, "%s", mime_for(path));
    cttp_set_header(res, "ETag", etag);
    cttp_cache(res, 3600);
    buf_free(&res->body);
    buf_append(&res->body, data, len);
    free(data);
}
