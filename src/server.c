/* ==========================================================================
 * server.c — sockets + the poll() event loop.
 *
 * ARCHITECTURE (the heart of the whole program)
 * ---------------------------------------------
 * One thread multiplexes every socket with poll(), using non-blocking I/O
 * (O_NONBLOCK). An fd is never allowed to block: when write/read would
 * block we simply stop and resume when poll() says the socket is ready.
 *
 * pfds[] and conns[] are parallel arrays: pfds[i] describes fd state for
 * conns[i]. pfds[0] is always the listening socket.
 *
 *   accept() --+--> conn state machine (see http.c)
 *              |
 *   idle sockets are reaped after server.timeout_secs seconds
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
#include <sys/time.h>

#include "cttp.h"

#define LISTEN_BACKLOG     128
#define IO_CHUNK           16384   /* bytes per read()/write() attempt   */
#define POLL_INTERVAL_MS   1000    /* wake up at least this often        */

/* Signal handlers can't get arguments, so the handler needs a way back to
 * the server struct: a file-scope pointer, written only at startup. */
static server *g_server;
static void handle_signal(int sig)
{
    (void)sig;
    /* The only "work" in a signal handler: set a volatile-sig_atomic_t
     * flag; the event loop checks it between iterations. For more
     * complex designs use the "self-pipe" trick. */
    if (g_server) g_server->running = 0;
}

/* ---- connection table -------------------------------------------------- */

static void table_reserve(server *s, int need)
{
    if (need <= s->cap) return;
    int newcap = s->cap ? s->cap : 16;
    while (newcap < need) newcap *= 2;
    s->pfds  = realloc(s->pfds,  (size_t)newcap * sizeof *s->pfds);
    s->conns = realloc(s->conns, (size_t)newcap * sizeof *s->conns);
    if (!s->pfds || !s->conns) { perror("realloc"); exit(1); }
    s->cap = newcap;
}

/* Set fd non-blocking: an O_NONBLOCK socket's read()/write() return
 * EAGAIN instead of stalling the whole server (classic blocking servers
 * used create one thread instead — a trade-off discussed in the README). */
static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void conn_reset_for_next(conn *c)
{
    http_free_request(&c->req);
    http_init_request(&c->req);
    c->out_off        = 0;
    c->body_remaining = 0;
    c->chunk_rem      = 0;
    c->chunk_stage    = 0;
    c->keep_alive     = 0;
    c->want_continue  = 0;
    c->state          = CONN_READ_HEADERS;
    buf_consume(&c->out, c->out.len);
    /* NOTE: c->in is NOT emptied — anything the client already sent for
     * the next request (pipelining) must survive for the next parse. */
}

static void conn_close(server *s, int idx)
{
    conn *c = s->conns[idx];
    close(c->fd);
    http_free_request(&c->req);
    buf_free(&c->in);
    buf_free(&c->out);
    free(c);

    /* Remove slot idx by moving the last entry into it (O(1), but it
     * changes iteration order — our loop tolerates re-checking slots). */
    s->conns[idx] = s->conns[s->nconns - 1];
    s->pfds[idx]  = s->pfds[s->nconns - 1];
    s->nconns--;
}

/* === setup & teardown ===================================================== */

/* Create the listening socket: the classic four-socket-API steps —
 * socket() -> setsockopt() -> bind() -> listen(). We keep it referenced
 * as pfds[0] in the poll array (see server_run). */
int server_init(server *s, const char *host, int port, const char *webroot)
{
    memset(s, 0, sizeof *s);
    table_reserve(s, 1);

    /* AF_INET + SOCK_STREAM = TCP over IPv4. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    /* SO_REUSEADDR removes the TIME_WAIT wait after restarts, otherwise
     * a crash/restart loop would lose the port for up to 2*MSL (~2 min). */
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);              /* network byte order */
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid address: %s\n", host);
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    /* Also make the LISTENING socket non-blocking: poll() then wakes us
     * only when a connection is *guaranteed* (no thundering-herd accept),
     * and the accept-for loop below can safely spin until EAGAIN. */
    set_nonblock(fd);

    s->listen_fd = fd;
    s->port = port;
    s->timeout_secs = 30;
    snprintf(s->host, sizeof s->host, "%s", host);
    s->webroot = webroot;
    s->pfds[0] = (struct pollfd){ .fd = fd, .events = POLLIN };
    s->conns[0] = NULL;                    /* slot 0 is the listener */
    s->nconns = 1;
    return 0;
}

int server_route(server *s, http_method m, const char *pattern, http_handler h)
{
    if (s->nroutes >= CT_MAX_ROUTES) return -1;
    snprintf(s->routes[s->nroutes].pattern, 256, "%s", pattern);
    s->routes[s->nroutes].method = m;
    s->routes[s->nroutes].handler = h;
    s->nroutes++;
    return 0;
}

void server_free(server *s)
{
    for (int i = s->nconns - 1; i >= 1; i--)
        conn_close(s, i);
    close(s->listen_fd);
    free(s->pfds);
    free(s->conns);
}

/* ---- accepting connections ---------------------------------------------- */

static void accept_new_clients(server *s)
{
    for (;;) {                                /* accept until EAGAIN:
                                                  multiple clients may be
                                                  queued at once */
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
        /* Nagle's algorithm batches small writes (40ms delay); an HTTP
         * server with small responses usually wants TCP_NODELAY. */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        table_reserve(s, s->nconns + 1);
        conn *c = calloc(1, sizeof *c);
        c->fd = fd;
        c->state = CONN_READ_HEADERS;
        c->last_activity = time(NULL);
        http_init_request(&c->req);
        s->conns[s->nconns] = c;
        s->pfds[s->nconns]  = (struct pollfd){
            .fd = fd, .events = POLLIN, .revents = 0 };
        s->nconns++;
    }
}

/* ---- flushing outgoing bytes --------------------------------------------- */

/* Write as much of conn->out as the socket kernel buffer accepts right
 * now (non-blocking). Returns -1 on hard error (peer gone). */
static int flush_conn(conn *c)
{
    while (c->out_off < c->out.len) {
        ssize_t n = send(c->fd, c->out.data + c->out_off,
                         c->out.len - c->out_off, MSG_NOSIGNAL);
        if (n > 0) {
            c->out_off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;                       /* kernel buffer full; retry
                                               when poll says POLLOUT   */
        return -1;                          /* EPIPE and friends: gone  */
    }
    return 1;                               /* fully flushed */
}

static void on_flush_complete(server *s, int idx)
{
    conn *c = s->conns[idx];

    if (c->want_continue) {
        /* The "100 Continue" interim response just went out. Resume
         * exactly where the header parse left off (usually reading the
         * body the client has presumably already started sending). */
        c->want_continue = 0;
        c->state = c->next_state;
        c->out_off = 0;
        buf_consume(&c->out, c->out.len);
        return;                             /* next poll loop continues */
    }

    if (c->keep_alive) {
        conn_reset_for_next(c);             /* ready for next request (keep-alive) */
        return;
    }
    conn_close(s, idx);                     /* Connection: close */
}

/* ---- the event loop ------------------------------------------------------ */

void server_run(server *s)
{
    g_server = s;
    s->running = 1;

    /* SIGPIPE: writing to a socket the peer already closed would kill the
     * process by default. We ignore it and treat writes as errors instead. */
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa = { .sa_handler = handle_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    log_info("listening on http://%s:%d (webroot: %s)",
             s->host, s->port, s->webroot ? s->webroot : "(none)");

    while (s->running) {
        /* --- RENEW INTEREST: recompute what each socket should listen for.
         * This is the event-loop contract most tutorials skip: poll()'s
         * .events are static, so we must update them every iteration to
         * reflect each connection's state machine —
         *   pending response bytes -> also want POLLOUT (writability),
         *   otherwise -> want POLLIN (new request bytes). */
        for (int i = 1; i < s->nconns; i++)
            s->pfds[i].events = POLLIN |
                ((s->conns[i]->out_off < s->conns[i]->out.len)
                     ? POLLOUT : 0);

        int rc = poll(s->pfds, (nfds_t)s->nconns, POLL_INTERVAL_MS);
        if (rc < 0) {
            if (errno == EINTR) continue;   /* signal interrupted us */
            perror("poll");
            break;
        }

        /* --- listening socket readable => pending client connections --- */
        if (s->pfds[0].revents & POLLIN)
            accept_new_clients(s);
        s->pfds[0].revents = 0;

        /* --- service each ready connection ---------------------------- */
        for (int i = 1; i < s->nconns; ) {
            conn *c = s->conns[i];
            short rev = s->pfds[i].revents; /* copy: slot may move on close */
            s->pfds[i].revents = 0;
            int handled = 1;                /* did we consume this slot?   */

            if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
                conn_close(s, i);
                handled = 0;
            }
            else if ((rev & POLLIN)) {
                /* Caller wants new bytes. Drain until the socket would
                 * block — fewer poll() round trips per request. */
                char tmp[IO_CHUNK];
                for (;;) {
                    ssize_t n = recv(c->fd, tmp, sizeof tmp, 0);
                    if (n > 0) {
                        buf_append(&c->in, tmp, (size_t)n);
                        c->last_activity = time(NULL);
                        if (n < (ssize_t)sizeof tmp) break;   /* drained */
                        continue;
                    }
                    if (n == 0) {                 /* peer closed cleanly */
                        conn_close(s, i);
                        handled = 0;
                        break;
                    }
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    conn_close(s, i);             /* ECONNRESET etc     */
                    handled = 0;
                    break;
                }
                if (handled) {
                    c->last_activity = time(NULL);
                    if (http_read_step(c, s) < 0) {   /* STEP_FATAL */
                        conn_close(s, i);
                        handled = 0;
                    }
                }
            }
            else if (rev & POLLOUT) {
                int r = flush_conn(c);
                if (r < 0) { conn_close(s, i); handled = 0; }
                else if (r == 1) on_flush_complete(s, i);
                /* r == 0: kernel buffer full; retry on next POLLOUT */
            }

            if (handled) i++;
            /* if the slot was freed, the last entry moved into it — do
             * NOT advance: examine the replacement slot as well */
        }

        /* --- reap idle connections (simple DoS hygiene) ---------------- */
        time_t now = time(NULL);
        for (int i = s->nconns - 1; i >= 1; i--) {
            if (now - s->conns[i]->last_activity > s->timeout_secs)
                conn_close(s, i);
        }
    }

    log_info("shutting down");
}
