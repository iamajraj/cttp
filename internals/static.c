/* ==========================================================================
 * static.c — serving files from the webroot.
 *
 * Modern static serving needs more than open()+read()+send():
 *   * path traversal protection  ("/../etc/passwd" must never escape)
 *   * MIME types by file extension
 *   * ETag + conditional requests (If-None-Match -> 304 Not Modified)
 *   * Range requests (resumable downloads, <video> seeking) -> 206
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

/* Reject traversal attempts. The path lives inside c->in, which is never
 * write raw to the socket, but it DOES end up in open() — so ".." and
 * NUL-ish shenanigans must be rejected before the filesystem call. */
static int is_unsafe_path(const char *p)
{
    if (!p || p[0] != '/') return 1;
    if (strstr(p, "..")) return 1;
    return 0;
}

/* Read the whole file referred to by `path` (already open, fd at 0).
 * Returns a heap buffer of exactly *out_len bytes, or NULL. */
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
        if (n == 0) break;               /* EOF before st_size? trust read */
        total += (size_t)n;
    }
    *out_len = total;
    data[total] = '\0';
    return data;
}

/* Serve fs paths under webroot for GET/HEAD. */
void static_serve(server *s, http_request *req, http_response *res)
{
    char path[2048];

    if (!s->webroot || is_unsafe_path(req->path)) {
        http_res_error(res, 403, "path not allowed");
        return;
    }

    /* Map the URL path onto the filesystem. Directory requests get
     * index.html appended (the classic "/path/" convention). */
    if (strcmp(req->path, "/") == 0)
        snprintf(path, sizeof path, "%s/index.html", s->webroot);
    else if (req->path[strlen(req->path) - 1] == '/')
        snprintf(path, sizeof path, "%s%sindex.html", s->webroot, req->path);
    else
        snprintf(path, sizeof path, "%s%s", s->webroot, req->path);

    int fd = open(path, O_RDONLY);
    if (fd < 0) { http_res_error(res, 404, "resource not found"); return; }
    struct stat st;
    fstat(fd, &st);
    if (S_ISDIR(st.st_mode)) {/* is a directory: retry with trailing-slash convention */
        close(fd);
        snprintf(path, sizeof path, "%s%s/index.html", s->webroot, req->path);
        fd = open(path, O_RDONLY);
        if (fd < 0) { http_res_error(res, 404, "resource not found"); return; }
        fstat(fd, &st);
    }

    /* ETag = size + mtime, hex. Cheap, effective for cache validation:
     * identical string means the browser's cached copy is still valid. */
    char etag[64];
    snprintf(etag, sizeof etag, "\"%zx-%zx\"",
             (size_t)st.st_size, (size_t)st.st_mtime);

    /* Conditional request: If-None-Match asks "is my cache still good?"  */
    const char *inm = req->get_header(req, "If-None-Match");
    if (inm && strstr(inm, etag)) {
        close(fd);
        res->status = 304;               /* 304 responses carry no body  */
        res->no_body = 1;
        snprintf(res->extra_hdr, sizeof res->extra_hdr,
                 "ETag: %s\r\nCache-Control: max-age=3600\r\n", etag);
        return;
    }

    /* Byte-range requests: if satisfiable, send exactly that slice. */
    const char *rh = req->get_header(req, "Range");
    if (rh) {
        unsigned long long start, end, total = (unsigned long long)st.st_size;
        if (sscanf(rh, "bytes=%llu-%llu", &start, &end) == 2 &&
            start < total && end >= start) {
            if (end >= total) end = total - 1;
        } else if (sscanf(rh, "bytes=%llu-", &start) == 1 && start < total) {
            end = total - 1;
        } else {
            /* Unsatisfiable: RFC 9110 §14.2 wants the real size back. */
            snprintf(res->extra_hdr, sizeof res->extra_hdr,
                     "Content-Range: bytes */%llu\r\n", total);
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
        snprintf(res->extra_hdr, sizeof res->extra_hdr,
                 "Content-Range: bytes %llu-%llu/%llu\r\n",
                 start, start + got - 1, total);
        buf_free(&res->body);
        buf_append(&res->body, data, got);
        free(data);
        return;
    }

    /* Plain full-file 200. */
    size_t len = 0;
    char *data = read_whole_file(fd, &len);
    close(fd);
    if (!data) { http_res_error(res, 500, "read failed"); return; }

    res->status = 200;
    snprintf(res->ctype, sizeof res->ctype, "%s", mime_for(path));
    snprintf(res->extra_hdr, sizeof res->extra_hdr,
             "ETag: %s\r\nCache-Control: max-age=3600\r\n", etag);
    buf_free(&res->body);
    buf_append(&res->body, data, len);
    free(data);
}
