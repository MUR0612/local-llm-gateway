/** @file backend.c
 * @brief The single shared model backend.
 *
 * The coursework version used fork()+execlp() of a model command that only
 * existed on the course server, a FILE* over a dup()ed socket and no
 * reconnect or restart. This
 * rewrite uses posix_spawnp() with file actions (no unsafe work between
 * fork and exec in a multithreaded process), CLOEXEC descriptors, a bounded
 * ready-port handshake, poll()-based timeouts and an explicit recovery
 * policy. See backend.h for the contract.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "backend.h"
#include "common.h"
#include "http.h"

extern char** environ;

/** Internal step results. */
enum { STEP_OK = 0, STEP_FAIL_START = 1, STEP_FAIL_CONNECT = 2, STEP_STOPPED = 3 };

/** Grace period for SIGTERM before SIGKILL at shutdown. */
#define SHUTDOWN_GRACE_MS 2000
#define SHUTDOWN_POLL_MS 20

static void debug_log(const Backend* b, const char* format, ...)
        __attribute__((format(printf, 2, 3)));

static void debug_log(const Backend* b, const char* format, ...)
{
    if (!b->cfg.debug) {
        return;
    }
    char line[256];
    size_t prefix = (size_t)snprintf(line, sizeof(line), "%s: debug: ", SERVER_NAME);
    size_t room = sizeof(line) - prefix - 1; /* Keep one byte for '\n'. */
    va_list args;
    va_start(args, format);
    int body = vsnprintf(line + prefix, room, format, args);
    va_end(args);
    size_t used = body < 0 ? 0 : ((size_t)body >= room ? room - 1 : (size_t)body);
    size_t len = prefix + used;
    line[len++] = '\n';
    ssize_t ignored = write(STDERR_FILENO, line, len);
    (void)ignored;
}

/** Writes a fixed message with one write() so lines never interleave. */
static void write_message(const char* message)
{
    ssize_t ignored = write(STDERR_FILENO, message, strlen(message));
    (void)ignored;
}

static bool stop_requested(const Backend* b)
{
    struct pollfd pfd = {b->wakeFd, POLLIN, 0};
    return b->wakeFd >= 0 && poll(&pfd, 1, 0) > 0;
}

int backend_init(Backend* backend, const BackendConfig* config, int wakeFd)
{
    memset(backend, 0, sizeof(*backend));
    backend->cfg = *config;
    backend->wakeFd = wakeFd;
    backend->pid = -1;
    backend->fd = -1;
    reader_init(&backend->reader, -1, wakeFd);
    if (config->mode == BACKEND_EXTERNAL) {
        snprintf(backend->port, sizeof(backend->port), "%s", config->port);
        snprintf(backend->host, sizeof(backend->host), "localhost:%s",
                backend->port);
    }
    return pthread_mutex_init(&backend->lock, NULL) == 0 ? 0 : -1;
}

/** @return true when the owned child has terminated (it is not reaped). */
static bool child_has_exited(const Backend* b)
{
    if (b->pid <= 0) {
        return false;
    }
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    int rc;
    do {
        rc = waitid(P_PID, (id_t)b->pid, &info, WEXITED | WNOHANG | WNOWAIT);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        return errno == ECHILD;
    }
    return info.si_pid == b->pid;
}

/**
 * Optionally signals the child's process group (which also reaches helper
 * processes it started), then reaps the child. Signals are only sent while
 * the PID is still unreaped, so they cannot hit a recycled PID.
 */
static void reap_child(Backend* b, int sig)
{
    if (b->pid <= 0) {
        return;
    }
    if (sig != 0) {
        kill(-b->pid, sig);
        kill(b->pid, sig);
    }
    while (waitpid(b->pid, NULL, 0) < 0 && errno == EINTR) {
    }
    debug_log(b, "reaped backend pid %ld", (long)b->pid);
    b->pid = -1;
}

/** Kills and reaps an owned child, then prints a fatal message and exits. */
static void backend_fatal(Backend* b, int status, const char* message)
{
    if (b->cfg.mode == BACKEND_MANAGED) {
        reap_child(b, SIGKILL);
    }
    write_message(message);
    _exit(status);
}

/** Configures signal state and process group for the child. */
static int prepare_spawn_attr(posix_spawnattr_t* attr)
{
    sigset_t empty;
    sigset_t defaults;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGHUP);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    if (posix_spawnattr_init(attr) != 0) {
        return -1;
    }
    if (posix_spawnattr_setsigmask(attr, &empty) != 0
            || posix_spawnattr_setsigdefault(attr, &defaults) != 0
            || posix_spawnattr_setpgroup(attr, 0) != 0
            || posix_spawnattr_setflags(attr,
                       POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF
                               | POSIX_SPAWN_SETPGROUP)
                    != 0) {
        posix_spawnattr_destroy(attr);
        return -1;
    }
    return 0;
}

/** stdin and stderr -> /dev/null, stdout -> pipe write end. */
static int prepare_file_actions(posix_spawn_file_actions_t* actions, int writeFd)
{
    if (posix_spawn_file_actions_init(actions) != 0) {
        return -1;
    }
    if (posix_spawn_file_actions_addopen(
                actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0)
                    != 0
            || posix_spawn_file_actions_adddup2(actions, writeFd, STDOUT_FILENO)
                    != 0
            || posix_spawn_file_actions_addopen(
                       actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0)
                    != 0) {
        posix_spawn_file_actions_destroy(actions);
        return -1;
    }
    return 0;
}

/** Spawns the managed child. @return pipe read end, or -1. */
static int spawn_child(Backend* b)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        return -1;
    }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    if (prepare_file_actions(&actions, fds[1]) != 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (prepare_spawn_attr(&attr) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    pid_t pid = -1;
    int rc = posix_spawnp(
            &pid, b->cfg.argv[0], &actions, &attr, b->cfg.argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        debug_log(b, "spawn of %s failed: %s", b->cfg.argv[0], strerror(rc));
        return -1;
    }
    b->pid = pid;
    b->generation++;
    debug_log(b, "spawned backend pid %ld (generation %lu)", (long)pid,
            b->generation);
    return fds[0];
}

/** Validates "<1..65535>\n" (an optional CR before LF is allowed). */
static int parse_port_line(const char* line, size_t len)
{
    long value = 0;
    size_t digits = 0;
    while (digits < len && line[digits] >= '0' && line[digits] <= '9') {
        value = value * 10 + (line[digits] - '0');
        if (++digits > 5) {
            return 0;
        }
    }
    size_t rest = digits;
    if (rest < len && line[rest] == '\r') {
        rest++;
    }
    if (digits == 0 || rest >= len || line[rest] != '\n' || value < 1
            || value > 65535) {
        return 0;
    }
    return (int)value;
}

/** Reads the ready line. @return port, 0 on failure, -1 when stopped. */
static int read_ready_port(Backend* b, int fd)
{
    char line[16];
    size_t len = 0;
    long long deadline = deadline_after(b->cfg.startupTimeoutMs);
    for (;;) {
        IoStatus waited = io_wait(fd, POLLIN, b->wakeFd, deadline);
        if (waited == IO_STOPPED) {
            return -1;
        }
        if (waited != IO_OK) {
            debug_log(b, "startup handshake %s",
                    waited == IO_TIMEOUT ? "timed out" : "failed");
            return 0;
        }
        ssize_t got = read(fd, line + len, sizeof(line) - 1 - len);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            debug_log(b, "backend closed stdout before reporting a port");
            return 0;
        }
        len += (size_t)got;
        if (memchr(line, '\n', len) != NULL) {
            return parse_port_line(line, len);
        }
        if (len == sizeof(line) - 1) {
            return 0;
        }
    }
}

/** Spawns a child and completes the handshake. @return STEP_*. */
static int start_managed(Backend* b)
{
    int readFd = spawn_child(b);
    if (readFd < 0) {
        return STEP_FAIL_START;
    }
    int port = read_ready_port(b, readFd);
    close(readFd);
    if (port <= 0) {
        reap_child(b, SIGKILL);
        return port < 0 ? STEP_STOPPED : STEP_FAIL_START;
    }
    snprintf(b->port, sizeof(b->port), "%d", port);
    snprintf(b->host, sizeof(b->host), "localhost:%d", port);
    debug_log(b, "backend ready on port %d", port);
    return STEP_OK;
}

/** One initial attempt plus BACKEND_CONNECT_RETRIES retries, no delay. */
static int connect_with_retries(Backend* b)
{
    for (unsigned attempt = 0; attempt <= BACKEND_CONNECT_RETRIES; attempt++) {
        int fd = net_connect_loopback(b->port, b->wakeFd, b->cfg.connectTimeoutMs);
        if (fd == IO_STOPPED) {
            return STEP_STOPPED;
        }
        if (fd >= 0) {
            b->fd = fd;
            reader_reset(&b->reader, fd);
            debug_log(b, "connected to backend port %s", b->port);
            return STEP_OK;
        }
        debug_log(b, "connect attempt %u failed", attempt + 1);
    }
    return STEP_FAIL_CONNECT;
}

int backend_start(Backend* backend)
{
    if (backend->cfg.mode == BACKEND_MANAGED) {
        int rc = start_managed(backend);
        if (rc == STEP_STOPPED) {
            return -1;
        }
        if (rc != STEP_OK) {
            write_message(MSG_BACKEND_START);
            return SERVER_EXIT_BACKEND_START;
        }
    }
    int rc = connect_with_retries(backend);
    if (rc == STEP_OK) {
        return 0;
    }
    if (backend->cfg.mode == BACKEND_MANAGED) {
        reap_child(backend, SIGKILL);
    }
    if (rc == STEP_STOPPED) {
        return -1;
    }
    write_message(MSG_BACKEND_CONNECT);
    return SERVER_EXIT_BACKEND_CONNECT;
}

/** Replaces a terminated managed child. Fatal if the restart fails. */
static int restart_child(Backend* b)
{
    debug_log(b, "backend pid %ld terminated; restarting", (long)b->pid);
    close_fd(&b->fd);
    reap_child(b, SIGKILL); /* SIGKILL clears any leftover group members. */
    int rc = start_managed(b);
    if (rc == STEP_FAIL_START) {
        backend_fatal(b, SERVER_EXIT_BACKEND_START, MSG_BACKEND_START);
    }
    return rc;
}

/**
 * Makes sure a live child (managed) and a connection exist.
 * @return STEP_OK or STEP_STOPPED; exhaustion of retries is fatal.
 */
static int ensure_ready(Backend* b)
{
    bool restarted = false;
    for (;;) {
        if (b->cfg.mode == BACKEND_MANAGED && child_has_exited(b)) {
            if (restarted) {
                backend_fatal(b, SERVER_EXIT_BACKEND_CONNECT, MSG_BACKEND_CONNECT);
            }
            int rc = restart_child(b);
            if (rc != STEP_OK) {
                return rc;
            }
            restarted = true;
        }
        if (b->fd >= 0) {
            return STEP_OK;
        }
        int rc = connect_with_retries(b);
        if (rc != STEP_FAIL_CONNECT) {
            return rc;
        }
        /* The child may have died between the check and the connects. */
        if (b->cfg.mode != BACKEND_MANAGED || restarted || !child_has_exited(b)) {
            backend_fatal(b, SERVER_EXIT_BACKEND_CONNECT, MSG_BACKEND_CONNECT);
        }
    }
}

/** Sends the request and reads the response once on the current connection. */
static HttpStatus attempt_once(Backend* b, const Buf* request, HttpResponse* resp)
{
    long long deadline = deadline_after(b->cfg.ioTimeoutMs);
    IoStatus io = net_send_all(
            b->fd, request->data, request->len, b->wakeFd, deadline);
    if (io != IO_OK) {
        return io == IO_STOPPED ? HTTP_STOPPED
                                : (io == IO_TIMEOUT ? HTTP_TIMEOUT : HTTP_IO_ERROR);
    }
    b->reader.deadlineMs = deadline;
    return http_read_response(&b->reader, BACKEND_MAX_BODY, resp);
}

/** Runs the locked part of a transaction. */
static BackendOutcome locked_exchange(Backend* b, const char* json,
        size_t jsonLen, Buf* request, HttpResponse* resp)
{
    unsigned resends = 0;
    for (;;) {
        if (stop_requested(b) || ensure_ready(b) != STEP_OK) {
            return BACKEND_REPLY_STOPPED;
        }
        if (http_build_request(request, "POST", "/completion", b->host,
                    "application/json", json, jsonLen)
                != 0) {
            return BACKEND_REPLY_FAILED;
        }
        HttpStatus rc = attempt_once(b, request, resp);
        if (rc == HTTP_OK) {
            if (resp->connectionClose) {
                close_fd(&b->fd);
            }
            return BACKEND_REPLY_OK;
        }
        /* Framing is no longer trustworthy: never reuse this connection. */
        close_fd(&b->fd);
        if (rc == HTTP_STOPPED) {
            return BACKEND_REPLY_STOPPED;
        }
        if (rc == HTTP_NOMEM || resends >= b->cfg.maxResends) {
            debug_log(b, "giving up after %u resends (%s)", resends,
                    http_status_name(rc));
            return BACKEND_REPLY_FAILED;
        }
        resends++;
        debug_log(b, "transaction failed (%s); resend %u of %u",
                http_status_name(rc), resends, b->cfg.maxResends);
    }
}

BackendOutcome backend_complete(Backend* backend, const char* json,
        size_t jsonLen, CompletionReply* reply)
{
    memset(reply, 0, sizeof(*reply));
    Buf request;
    buf_init(&request);
    HttpResponse resp;
    memset(&resp, 0, sizeof(resp));

    pthread_mutex_lock(&backend->lock);
    BackendOutcome outcome
            = locked_exchange(backend, json, jsonLen, &request, &resp);
    pthread_mutex_unlock(&backend->lock);

    /* JSON parsing does not need the shared connection. */
    if (outcome == BACKEND_REPLY_OK) {
        JsonStatus js = JSON_SYNTAX;
        if (resp.status == 200) {
            js = json_parse_completion(resp.body, resp.bodyLen, reply);
        }
        if (js != JSON_OK) {
            debug_log(backend, "unusable backend reply (status %d, json %s)",
                    resp.status, json_status_name(js));
            outcome = BACKEND_REPLY_INVALID;
        }
    }
    http_response_free(&resp);
    buf_free(&request);
    return outcome;
}

void backend_shutdown(Backend* backend)
{
    pthread_mutex_lock(&backend->lock);
    close_fd(&backend->fd);
    if (backend->cfg.mode == BACKEND_MANAGED && backend->pid > 0) {
        kill(-backend->pid, SIGTERM);
        kill(backend->pid, SIGTERM);
        for (int waited = 0; waited < SHUTDOWN_GRACE_MS && !child_has_exited(backend);
                waited += SHUTDOWN_POLL_MS) {
            poll(NULL, 0, SHUTDOWN_POLL_MS);
        }
        reap_child(backend, SIGKILL);
    }
    pthread_mutex_unlock(&backend->lock);
    pthread_mutex_destroy(&backend->lock);
}
