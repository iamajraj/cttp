/* ==========================================================================
 * log.c — tiny timestamped logger (stderr). Swap vlog() for anything.
 * ========================================================================== */
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "cttp.h"

static void vlog(const char *tag, const char *fmt, va_list ap)
{
    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "[%s] %s ", ts, tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void cttp_log_info(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vlog("INFO", fmt, ap);
    va_end(ap);
}

void cttp_log_error(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vlog("ERROR", fmt, ap);
    va_end(ap);
}
