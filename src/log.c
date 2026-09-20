/* ==========================================================================
 * log.c — tiny timestamped logger.
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
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime_r(&now, &tm));
    fprintf(stderr, "[%s] %s ", ts, tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void log_info(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vlog("INFO", fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vlog("ERROR", fmt, ap);
    va_end(ap);
}
