/* ==========================================================================
 * server.c — sockets + the poll() event loop.
 *
 * ARCHITECTURE
 * ------------
 * One thread multiplexes every socket with poll() on non-blocking fds.
 * Nothing blocks: read()/write()/accept() return EAGAIN when the kernel
 * can't proceed, and the loop resumes when poll() says the socket is
 * ready. pfds[] and conns[] are parallel arrays; pfds[0] is the listener.
 * ========================================================================== */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "cttp.h"

#define LISTEN_BACKLOG   128
#define IO_CHUNK         16384    /* bytes per read()/write() attempt   */
#define POLL_INTERVAL_MS 1000     /* wake up at least this often        */

/* Signal handlers get no arguments; the loop checks this flag instead.
 * (fancier designs use the "self-pipe" trick to wake poll() instantly). */
static cttp_server *g_server;
static void handle_signal(int sig)
{
    (void)sig;
    if (g_server) g_server->running = 0;
}

/* ---- connection table -------------------------------------------------- */

static void table_reserve(cttp_server *s, int need)
{
    if (need <= s->cap) return;
    int newcap = s->cap ? s->cap : 16;
    while (newcap < need) newcap *= 2;
    s->pfds  = realloc(s->pfds,  (size_t)newcap * sizeof *s->pfds);
    s->conns = realloc(s->conns, (size_t)newcap * sizeof *s->conns);
    if (!s->pfds || !s->conns) { perror("realloc"); exit(1); }
    s->cap = newcap;
}

/* O_NONBLOCK sockets return EAGAIN instead of stalling the whole server. */
static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Reset a connection slot for the next keep-alive request. */
static void conn_reset_for_next(conn *c, cttp_server *s)
{
    http_free_request(&c->req);
    http_init_request(&c->req);
    c->out_off        = 0;
    c->body_remaining = 0;
    c->chunk_rem      = 0;
    c->chunk_stage    = 0;
    c->keep_alive     = 0;
    c->streamed       = 0;
    c->want_continue  = 0;
    c->state          = CONN_READ_HEADERS;
    buf_consume(&c->out, c->out.len);

    /* X-Request-Id: unique per request, visible in responses and logs. */
    s->req_counter++;
    snprintf(c->req.req_id, sizeof c->req.req_id, "%zx-%zx",
             (size_t)time(NULL), (size_t)s->req_counter);
    /* NOTE: c->in is NOT emptied — bytes the client already sent for
     * the next pipelined request survive for the next parse. */
}

static void conn_close(cttp_server *s, int idx)
{
    conn *c = s->conns[idx];
    close(c->fd);
    http_free_request(&c->req);
    buf_free(&c->in);
    buf_free(&c->out);
    free(c);

    /* O(1) removal by moving the last slot into idx (the loop tolerates
     * re-checking the replacement slot). */
    s->conns[idx] = s->conns[s->nconns - 1];
    s->pfds[idx]  = s->pfds[s->nconns - 1];
    s->nconns--;
}

/* ---- accepting connections ---------------------------------------------- */

static void accept_new_clients(cttp_server *s)
{
    for (;;) {
        struct sockaddr_in addr;
        socklen_t alen = sizeof addr;
        int fd = accept(s->listen_fd, (struct sockaddr *)&addr, &alen);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            perror("accept");
            return;
        }

        set_nonblock(fd);
        /* Nagle batches small writes (~40ms latency); responses want it off */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        table_reserve(s, s->nconns + 1);
        conn *c = calloc(1, sizeof *c);
        c->fd = fd;
        c->state = CONN_READ_HEADERS;
        c->last_activity = time(NULL);
        http_init_request(&c->req);
        s->conns[s->nconns] = c;
        s->pfds[s->nconns]  = (struct pollfd){ .fd = fd, .events = POLLIN };
        s->nconns++;
        conn_reset_for_next(c, s);      /* request id + fresh state */
    }
}

/* ---- flushing outgoing bytes --------------------------------------------- */

/* Write what the kernel accepts right now; 0 = kernel full, -1 gone. */
static int flush_conn(conn *c)
{
    while (c->out_off < c->out.len) {
        ssize_t n = send(c->fd, c->out.data + c->out_off,
                         c->out.len - c->out_off, MSG_NOSIGNAL);
        if (n > 0) { c->out_off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;                   /* retry when POLLOUT is ready */
        return -1;
    }
    return 1;                           /* fully flushed */
}

static void on_flush_complete(cttp_server *s, int idx)
{
    conn *c = s->conns[idx];

    if (c->want_continue) {             /* 100 Continue just went out */
        c->want_continue = 0;
        c->state = c->next_state;
        c->out_off = 0;
        buf_consume(&c->out, c->out.len);
        return;
    }

    if (c->keep_alive) {
        conn_reset_for_next(c, s);      /* ready for the next request */
        return;
    }
    conn_close(s, idx);                 /* Connection: close */
}

/* ---- config & routes ----------------------------------------------------- */

int cttp_init(cttp_server *s)
{
    memset(s, 0, sizeof *s);
    /* friendly defaults; override the fields before cttp_listen */
    snprintf(s->host, sizeof s->host, "127.0.0.1");
    s->port         = 8080;
    s->timeout_secs = 30;
    s->listen_fd    = -1;
    return 0;
}

static int route_add(cttp_server *s, cttp_method m,
                     const char *pattern, cttp_handler h)
{
    if (s->nroutes >= CTTP_MAX_ROUTES) {
        cttp_log_error("route limit reached: %s", pattern);
        return -1;
    }
    snprintf(s->routes[s->nroutes].pattern, 256, "%s", pattern);
    s->routes[s->nroutes].method  = m;
    s->routes[s->nroutes].handler = h;
    s->nroutes++;
    return 0;
}

int cttp_route(cttp_server *s, cttp_method m,
               const char *pattern, cttp_handler h)
{
    return route_add(s, m, pattern, h);
}

void cttp_get(cttp_server *s, const char *p, cttp_handler h)
{ route_add(s, CTTP_GET, p, h); }
void cttp_head(cttp_server *s, const char *p, cttp_handler h)
{ route_add(s, CTTP_HEAD, p, h); }
void cttp_post(cttp_server *s, const char *p, cttp_handler h)
{ route_add(s, CTTP_POST, p, h); }
void cttp_put(cttp_server *s, const char *p, cttp_handler h)
{ route_add(s, CTTP_PUT, p, h); }
void cttp_delete(cttp_server *s, const char *p, cttp_handler h)
{ route_add(s, CTTP_DELETE, p, h); }
void cttp_patch(cttp_server *s, const char *p, cttp_handler h)
{ route_add(s, CTTP_PATCH, p, h); }
void cttp_options(cttp_server *s, const char *p, cttp_handler h)
{ route_add(s, CTTP_OPTIONS, p, h); }

void cttp_use(cttp_server *s, cttp_middleware mw)
{
    if (s->nmw < CTTP_MAX_MIDDLEWARE)
        s->mw[s->nmw++] = mw;
}

void cttp_on_error(cttp_server *s,
                   void (*h)(cttp_request *, cttp_response *))
{
    s->on_error = h;
}

void cttp_on_log(cttp_server *s,
                 void (*h)(const cttp_request *, int, size_t))
{
    s->on_log = h;
}

/* ---- the event loop ------------------------------------------------------ */

void cttp_listen(cttp_server *s)
{
    g_server = s;
    s->running = 1;
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa = { .sa_handler = handle_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* socket() -> setsockopt() -> bind() -> listen(); the listener is
     * also non-blocking so the accept loop below spins until EAGAIN.  */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)s->port);
    if (inet_pton(AF_INET, s->host, &addr.sin_addr) != 1) {
        cttp_log_error("invalid address: %s", s->host);
        close(fd);
        return;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(fd);
        return;
    }
    if (listen(fd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return;
    }
    set_nonblock(fd);

    table_reserve(s, 1);
    s->listen_fd = fd;
    s->pfds[0]   = (struct pollfd){ .fd = fd, .events = POLLIN };
    s->conns[0]  = NULL;                 /* slot 0 = listener      */
    s->nconns    = 1;

    cttp_log_info("listening on http://%s:%d (webroot: %s)",
                  s->host, s->port, s->webroot ? s->webroot : "(none)");

    while (s->running) {
        /* RENEW INTEREST: poll()'s .events are ours to maintain —
         * pending response bytes => also ask for POLLOUT, else POLLIN. */
        for (int i = 1; i < s->nconns; i++)
            s->pfds[i].events = POLLIN |
                ((s->conns[i]->out_off < s->conns[i]->out.len)
                     ? POLLOUT : 0);

        int rc = poll(s->pfds, (nfds_t)s->nconns, POLL_INTERVAL_MS);
        if (rc < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (s->pfds[0].revents & POLLIN)
            accept_new_clients(s);
        s->pfds[0].revents = 0;

        for (int i = 1; i < s->nconns; ) {
            conn *c = s->conns[i];
            short rev = s->pfds[i].revents;   /* slot may move on close */
            s->pfds[i].revents = 0;
            int handled = 1;

            if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
                conn_close(s, i);
                handled = 0;
            }
            else if (rev & POLLIN) {
                char tmp[IO_CHUNK];
                for (;;) {
                    ssize_t n = recv(c->fd, tmp, sizeof tmp, 0);
                    if (n > 0) {
                        buf_append(&c->in, tmp, (size_t)n);
                        c->last_activity = time(NULL);
                        if (n < (ssize_t)sizeof tmp) break;
                        continue;
                    }
                    if (n == 0) { conn_close(s, i); handled = 0; break; }
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    conn_close(s, i); handled = 0; break;
                }
                if (handled && http_read_step(c, s) < 0) {
                    conn_close(s, i); handled = 0;
                }
            }
            else if (rev & POLLOUT) {
                int r = flush_conn(c);
                if (r < 0) { conn_close(s, i); handled = 0; }
                else if (r == 1) on_flush_complete(s, i);
            }

            if (handled) i++;
        }

        /* reap idle connections (simple DoS hygiene) */
        time_t now = time(NULL);
        for (int i = s->nconns - 1; i >= 1; i--)
            if (now - s->conns[i]->last_activity > s->timeout_secs)
                conn_close(s, i);
    }

    cttp_log_info("shutting down");
}

void cttp_free(cttp_server *s)
{
    close(s->listen_fd);
    free(s->pfds);
    free(s->conns);
    s->pfds = NULL;
    s->conns = NULL;
    s->nconns = s->cap = 0;
}
