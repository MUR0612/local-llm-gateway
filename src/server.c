/** @file server.c
 * @brief Gateway runtime: listeners, connection handlers, statistics,
 *        signals and graceful shutdown.
 *
 * Request dispatch order (405 -> 404 -> 413), the per-client history and the
 * shared serialised backend follow the author's coursework server. New in
 * this version: the stats listener, stop-pipe based shutdown, joinable
 * handler threads, a condition-variable slot gate, a configurable home page
 * and 502 replies when the backend cannot answer.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "backend.h"
#include "common.h"
#include "config.h"
#include "history.h"
#include "http.h"
#include "netio.h"
#include "server.h"
#include "stats.h"
#include "workers.h"

/** Bound on writing one response to a client that stopped reading. */
#define CLIENT_WRITE_TIMEOUT_MS 30000
/** Back-off after accept() runs out of descriptors or memory. */
#define ACCEPT_BACKOFF_MS 100

#define BODY_405 "Invalid HTTP method\n"
#define BODY_404 "Invalid address on request line\n"
#define BODY_HOME_ERROR "Home page unavailable\n"
#define BODY_INTERNAL "Internal server error\n"
#define BODY_BACKEND_FAILED "Backend request failed\n"
#define BODY_BACKEND_INVALID "Invalid backend response\n"

/** Counting gate that limits concurrently served prompt clients. */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    /** 0 = unlimited. */
    unsigned limit;
    unsigned used;
    bool closing;
} SlotGate;

/** Process-wide state shared by all threads (no globals). */
typedef struct {
    ServerConfig config;
    int listenFd;
    int statsFd;
    /** stopPipe[0] becomes readable forever once shutdown is requested. */
    int stopPipe[2];
    Stats stats;
    SlotGate gate;
    Workers workers;
    Backend backend;
    pthread_t signalThread;
    bool signalThreadRunning;
    pthread_t statsThread;
    bool statsThreadRunning;
} ServerState;

/** Data owned by one connection thread (freed by the reaper). */
typedef struct {
    ServerState* server;
    int fd;
    bool isStats;
} ConnCtx;

/* ---- Slot gate ---------------------------------------------------- */

static int gate_init(SlotGate* gate, unsigned limit)
{
    gate->limit = limit;
    gate->used = 0;
    gate->closing = false;
    if (pthread_mutex_init(&gate->lock, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&gate->cond, NULL) != 0) {
        pthread_mutex_destroy(&gate->lock);
        return -1;
    }
    return 0;
}

/** Blocks until a slot is free. @return false when shutting down. */
static bool gate_acquire(SlotGate* gate)
{
    pthread_mutex_lock(&gate->lock);
    while (!gate->closing && gate->limit != 0 && gate->used >= gate->limit) {
        pthread_cond_wait(&gate->cond, &gate->lock);
    }
    bool ok = !gate->closing;
    if (ok) {
        gate->used++;
    }
    pthread_mutex_unlock(&gate->lock);
    return ok;
}

static void gate_release(SlotGate* gate)
{
    pthread_mutex_lock(&gate->lock);
    gate->used--;
    pthread_cond_signal(&gate->cond);
    pthread_mutex_unlock(&gate->lock);
}

static void gate_close(SlotGate* gate)
{
    pthread_mutex_lock(&gate->lock);
    gate->closing = true;
    pthread_cond_broadcast(&gate->cond);
    pthread_mutex_unlock(&gate->lock);
}

static void gate_destroy(SlotGate* gate)
{
    pthread_cond_destroy(&gate->cond);
    pthread_mutex_destroy(&gate->lock);
}

/* ---- Responses ---------------------------------------------------- */

/** Serialises and sends one response. @return true on success. */
static bool send_response(const ConnCtx* ctx, int status,
        const char* contentType, const void* body, size_t len, bool close)
{
    Buf out;
    buf_init(&out);
    bool ok = http_build_response(&out, status, contentType, body, len, close) == 0
            && net_send_all(ctx->fd, out.data, out.len, ctx->server->stopPipe[0],
                       deadline_after(CLIENT_WRITE_TIMEOUT_MS))
                    == IO_OK;
    buf_free(&out);
    return ok;
}

static bool send_text(const ConnCtx* ctx, int status, const char* text, bool close)
{
    return send_response(ctx, status, "text/plain", text, strlen(text), close);
}

/**
 * Drains a request body that will not be used, then sends a text reply.
 * Bodies above MAX_DRAIN_BODY are not drained: the reply announces
 * "Connection: close" and the connection is closed.
 * @return true when the connection may serve another request.
 */
static bool reply_and_skip_body(const ConnCtx* ctx, Reader* reader,
        const HttpRequestHead* head, int status, const char* text)
{
    bool drain = head->contentLength <= MAX_DRAIN_BODY;
    if (drain && http_discard_body(reader, head->contentLength) != HTTP_OK) {
        return false;
    }
    bool keepAlive = drain && !head->connectionClose;
    return send_text(ctx, status, text, !keepAlive) && keepAlive;
}

/** Reads the configured home page file. @return 0 or -1. */
static int load_home_page(const char* path, Buf* out)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    struct stat info;
    int rc = -1;
    if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode)
            && info.st_size <= (off_t)MAX_HOME_PAGE
            && buf_reserve(out, (size_t)info.st_size) == 0) {
        rc = 0;
        for (;;) {
            ssize_t got = read(fd, out->data + out->len, out->cap - out->len - 1);
            if (got < 0 && errno == EINTR) {
                continue;
            }
            if (got < 0 || out->len + (size_t)got > MAX_HOME_PAGE) {
                rc = -1;
                break;
            }
            if (got == 0) {
                break;
            }
            out->len += (size_t)got;
            if (out->len + 1 == out->cap && buf_reserve(out, 4096) != 0) {
                rc = -1;
                break;
            }
        }
        out->data[out->len] = '\0';
    }
    close(fd);
    return rc;
}

static bool serve_home(const ConnCtx* ctx, Reader* reader, const HttpRequestHead* head)
{
    if (head->contentLength > MAX_DRAIN_BODY
            || http_discard_body(reader, head->contentLength) != HTTP_OK) {
        return false;
    }
    Buf page;
    buf_init(&page);
    bool ok;
    if (load_home_page(ctx->server->config.homePage, &page) == 0) {
        ok = send_response(ctx, 200, "text/html", page.data, page.len,
                head->connectionClose);
    } else {
        ok = send_text(ctx, 500, BODY_HOME_ERROR, head->connectionClose);
    }
    buf_free(&page);
    return ok && !head->connectionClose;
}

/* ---- Prompt handling ---------------------------------------------- */

/** Builds "<content>\n" and sends it as the 200 reply. */
static bool send_completion(const ConnCtx* ctx, const CompletionReply* reply, bool close)
{
    Buf body;
    buf_init(&body);
    bool ok = buf_append(&body, reply->content, reply->contentLen) == 0
            && buf_append(&body, "\n", 1) == 0
            && send_response(ctx, 200, "text/plain", body.data, body.len, close);
    buf_free(&body);
    return ok;
}

/** Forwards one in-limit prompt to the backend and answers the client. */
static bool forward_prompt(const ConnCtx* ctx, History* history,
        const unsigned char* prompt, size_t len, bool close)
{
    ServerState* s = ctx->server;
    Buf json;
    buf_init(&json);
    if (history_build_completion_json(&json, history, (const char*)prompt, len) != 0) {
        buf_free(&json);
        return send_text(ctx, 500, BODY_INTERNAL, close) && !close;
    }
    CompletionReply reply;
    BackendOutcome outcome = backend_complete(&s->backend, json.data, json.len, &reply);
    buf_free(&json);
    bool ok;
    switch (outcome) {
    case BACKEND_REPLY_OK:
        /* Tokens count as soon as the backend has done the work, even if the
         * client then fails to receive the reply. A history allocation
         * failure only loses context; the reply is still sent. */
        stats_add_tokens(&s->stats, reply.tokensEvaluated);
        history_append(history, (const char*)prompt, len, reply.content,
                reply.contentLen);
        ok = send_completion(ctx, &reply, close);
        break;
    case BACKEND_REPLY_INVALID:
        ok = send_text(ctx, 502, BODY_BACKEND_INVALID, close);
        break;
    case BACKEND_REPLY_FAILED:
        ok = send_text(ctx, 502, BODY_BACKEND_FAILED, close);
        break;
    default:
        ok = false; /* Shutting down: close without a reply. */
        break;
    }
    completion_reply_free(&reply);
    return ok && !close;
}

/** Handles POST / once method and path are known to be valid. */
static bool handle_prompt(const ConnCtx* ctx, Reader* reader,
        const HttpRequestHead* head, History* history)
{
    stats_prompt_received(&ctx->server->stats);
    if (head->contentLength > MAX_PROMPT_BODY) {
        char text[64];
        snprintf(text, sizeof(text), "Input is too large: %llu bytes\n",
                head->contentLength);
        return reply_and_skip_body(ctx, reader, head, 413, text);
    }
    unsigned char* body = NULL;
    if (http_read_body(reader, (size_t)head->contentLength, &body) != HTTP_OK) {
        return false;
    }
    bool keep = forward_prompt(
            ctx, history, body, (size_t)head->contentLength, head->connectionClose);
    free(body);
    return keep;
}

/** Dispatches one prompt-port request. @return true to keep the connection. */
static bool dispatch_prompt_request(const ConnCtx* ctx, Reader* reader,
        const HttpRequestHead* head, History* history)
{
    bool isGet = strcmp(head->method, "GET") == 0;
    bool isPost = strcmp(head->method, "POST") == 0;
    if (!isGet && !isPost) {
        return reply_and_skip_body(ctx, reader, head, 405, BODY_405);
    }
    if (strcmp(head->target, "/") != 0) {
        return reply_and_skip_body(ctx, reader, head, 404, BODY_404);
    }
    if (isGet) {
        return serve_home(ctx, reader, head);
    }
    return handle_prompt(ctx, reader, head, history);
}

/** Dispatches one stats-port request. */
static bool dispatch_stats_request(
        const ConnCtx* ctx, Reader* reader, const HttpRequestHead* head)
{
    if (strcmp(head->method, "GET") != 0) {
        return reply_and_skip_body(ctx, reader, head, 405, BODY_405);
    }
    if (strcmp(head->target, "/stats") != 0) {
        return reply_and_skip_body(ctx, reader, head, 404, BODY_404);
    }
    if (head->contentLength > MAX_DRAIN_BODY
            || http_discard_body(reader, head->contentLength) != HTTP_OK) {
        return false;
    }
    StatsSnapshot snapshot;
    stats_snapshot(&ctx->server->stats, &snapshot);
    Buf text;
    buf_init(&text);
    bool ok = stats_format(&snapshot, &text) == 0
            && send_response(ctx, 200, "text/plain", text.data, text.len,
                    head->connectionClose);
    buf_free(&text);
    return ok && !head->connectionClose;
}

/** Serves requests on one connection until EOF, error, close or stop. */
static void serve_connection(const ConnCtx* ctx, History* history)
{
    Reader reader;
    reader_init(&reader, ctx->fd, ctx->server->stopPipe[0]);
    for (;;) {
        HttpRequestHead head;
        /* Malformed requests, early EOF and I/O errors close silently. */
        if (http_read_request_head(&reader, &head) != HTTP_OK) {
            return;
        }
        bool keep = ctx->isStats
                ? dispatch_stats_request(ctx, &reader, &head)
                : dispatch_prompt_request(ctx, &reader, &head, history);
        http_request_head_free(&head);
        if (!keep) {
            return;
        }
    }
}

static void* prompt_thread_main(void* arg)
{
    Worker* worker = arg;
    ConnCtx* ctx = worker->arg;
    ServerState* s = ctx->server;
    stats_client_started(&s->stats);
    History history;
    if (history_init(&history, s->config.historySize) == 0) {
        serve_connection(ctx, &history);
        history_free(&history);
    }
    close_fd(&ctx->fd);
    stats_client_finished(&s->stats);
    gate_release(&s->gate);
    workers_finish(&s->workers, worker);
    return NULL;
}

static void* stats_conn_thread_main(void* arg)
{
    Worker* worker = arg;
    ConnCtx* ctx = worker->arg;
    ServerState* s = ctx->server;
    serve_connection(ctx, NULL);
    close_fd(&ctx->fd);
    workers_finish(&s->workers, worker);
    return NULL;
}

/* ---- Accepting ---------------------------------------------------- */

/** Hands an accepted socket to a new handler thread. @return 0 or -1. */
static int start_connection(ServerState* s, int fd, bool isStats)
{
    ConnCtx* ctx = malloc(sizeof(*ctx));
    Worker* worker = calloc(1, sizeof(*worker));
    if (ctx == NULL || worker == NULL) {
        free(ctx);
        free(worker);
        return -1;
    }
    ctx->server = s;
    ctx->fd = fd;
    ctx->isStats = isStats;
    worker->arg = ctx;
    if (workers_spawn(&s->workers, worker,
                isStats ? stats_conn_thread_main : prompt_thread_main)
            != 0) {
        free(ctx);
        free(worker);
        return -1;
    }
    return 0;
}

/** Sleeps briefly unless shutdown is requested. @return true if stopping. */
static bool backoff(ServerState* s)
{
    return io_wait(s->stopPipe[0], POLLIN, -1, deadline_after(ACCEPT_BACKOFF_MS))
            == IO_OK;
}

/**
 * Accepts connections until shutdown. Prompt clients first take a slot, so
 * clients beyond maxclients stay in the kernel backlog, unaccepted and
 * uncounted, until a slot frees.
 */
static void accept_loop(ServerState* s, int listenFd, bool isStats)
{
    for (;;) {
        if (!isStats && !gate_acquire(&s->gate)) {
            return;
        }
        IoStatus waited = io_wait(listenFd, POLLIN, s->stopPipe[0], -1);
        int fd = -1;
        if (waited == IO_OK) {
            fd = accept4(listenFd, NULL, NULL, SOCK_CLOEXEC);
        }
        if (fd >= 0 && start_connection(s, fd, isStats) == 0) {
            continue;
        }
        int savedErrno = errno;
        if (fd >= 0) {
            close(fd); /* Handler could not be created: drop the client. */
        }
        if (!isStats) {
            gate_release(&s->gate);
        }
        if (waited == IO_STOPPED) {
            return;
        }
        bool exhausted = waited == IO_ERROR || fd >= 0 || savedErrno == EMFILE
                || savedErrno == ENFILE || savedErrno == ENOBUFS
                || savedErrno == ENOMEM;
        if (exhausted && backoff(s)) {
            return;
        }
    }
}

static void* stats_acceptor_main(void* arg)
{
    ServerState* s = arg;
    accept_loop(s, s->statsFd, true);
    return NULL;
}

/* ---- Signals and shutdown ----------------------------------------- */

static void request_stop(ServerState* s)
{
    char byte = 'x';
    ssize_t ignored = write(s->stopPipe[1], &byte, 1);
    (void)ignored;
    gate_close(&s->gate);
}

static void print_stats(ServerState* s)
{
    StatsSnapshot snapshot;
    stats_snapshot(&s->stats, &snapshot);
    Buf text;
    buf_init(&text);
    if (stats_format(&snapshot, &text) == 0) {
        /* One write() keeps the five lines together. */
        ssize_t ignored = write(STDERR_FILENO, text.data, text.len);
        (void)ignored;
    }
    buf_free(&text);
}

/**
 * Dedicated sigwait() thread: SIGHUP prints statistics; SIGINT/SIGTERM
 * request a graceful shutdown. No async signal handler is installed.
 */
static void* signal_thread_main(void* arg)
{
    ServerState* s = arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    for (;;) {
        int sig = 0;
        if (sigwait(&set, &sig) != 0) {
            continue;
        }
        if (sig == SIGHUP) {
            print_stats(s);
        } else {
            request_stop(s);
            return NULL;
        }
    }
}

/** Ignores SIGPIPE and blocks the sigwait() set before any thread exists. */
static void setup_signals(void)
{
    struct sigaction ignore;
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGPIPE, &ignore, NULL);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}

/** Opens the main and optional stats listeners. @return 0 or exit status. */
static int open_listeners(ServerState* s)
{
    s->listenFd = net_listen_loopback(s->config.port);
    if (s->listenFd < 0) {
        fprintf(stderr, "%s: unable to listen on given port \"%s\"\n",
                SERVER_NAME, s->config.port);
        return SERVER_EXIT_LISTEN;
    }
    if (s->config.statsPort != NULL) {
        /* getaddrinfo() treats an empty service as port 0; reject it so a
         * set-but-empty variable is an error rather than a random port. */
        s->statsFd = s->config.statsPort[0] == '\0'
                ? -1
                : net_listen_loopback(s->config.statsPort);
        if (s->statsFd < 0) {
            fprintf(stderr, "%s: cannot listen on given statistics port \"%s\"\n",
                    SERVER_NAME, s->config.statsPort);
            return SERVER_EXIT_STATS_LISTEN;
        }
    }
    return 0;
}

/** Creates shared state and service threads. @return 0 or -1. */
static int start_services(ServerState* s)
{
    if (pipe2(s->stopPipe, O_CLOEXEC | O_NONBLOCK) != 0
            || stats_init(&s->stats) != 0
            || gate_init(&s->gate, s->config.maxClients) != 0
            || workers_init(&s->workers) != 0
            || backend_init(&s->backend, &s->config.backend, s->stopPipe[0]) != 0) {
        return -1;
    }
    if (pthread_create(&s->signalThread, NULL, signal_thread_main, s) != 0) {
        return -1;
    }
    s->signalThreadRunning = true;
    if (s->statsFd >= 0) {
        if (pthread_create(&s->statsThread, NULL, stats_acceptor_main, s) != 0) {
            return -1;
        }
        s->statsThreadRunning = true;
    }
    return 0;
}

/** Stops every thread, terminates an owned backend and frees state. */
static void shutdown_server(ServerState* s)
{
    request_stop(s);
    if (s->statsThreadRunning) {
        pthread_join(s->statsThread, NULL);
    }
    workers_close(&s->workers);
    backend_shutdown(&s->backend);
    if (s->signalThreadRunning) {
        pthread_kill(s->signalThread, SIGTERM); /* No-op if already exiting. */
        pthread_join(s->signalThread, NULL);
    }
    close_fd(&s->listenFd);
    close_fd(&s->statsFd);
    close_fd(&s->stopPipe[0]);
    close_fd(&s->stopPipe[1]);
    stats_destroy(&s->stats);
    gate_destroy(&s->gate);
}

int run_server(int argc, char** argv)
{
    ServerState* s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return SERVER_EXIT_USAGE;
    }
    s->listenFd = s->statsFd = s->stopPipe[0] = s->stopPipe[1] = -1;
    int rc = server_config_parse(argc, argv, &s->config);
    if (rc != 0) {
        free(s);
        return rc < 0 ? SERVER_EXIT_OK : rc;
    }
    setup_signals();
    rc = open_listeners(s);
    if (rc == 0) {
        /* The listening port is reported before the backend is started. */
        fprintf(stderr, "%d\n", net_bound_port(s->listenFd));
        fflush(stderr);
        if (start_services(s) != 0) {
            fprintf(stderr, "%s: unable to initialise\n", SERVER_NAME);
            _exit(SERVER_EXIT_BACKEND_START);
        }
        int started = backend_start(&s->backend);
        if (started == 0) {
            accept_loop(s, s->listenFd, false);
        }
        rc = started > 0 ? started : SERVER_EXIT_OK;
        shutdown_server(s);
    } else {
        close_fd(&s->listenFd);
        close_fd(&s->statsFd);
    }
    server_config_free(&s->config);
    free(s);
    return rc;
}
