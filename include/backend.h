/** @file backend.h
 * @brief The single shared model backend: process ownership, the persistent
 *        connection and serialised /completion transactions.
 *
 * Modes:
 *  - managed: the gateway spawns argv[0] (PATH lookup, no shell) in its own
 *    process group. The child must print its listening port as one decimal
 *    line on stdout within the startup timeout and must not write to stdout
 *    afterwards. The gateway owns, reaps and restarts only this child.
 *  - external: the user runs the backend on a loopback port. The gateway only
 *    connects; it never signals, reaps or restarts the external process.
 *
 * Locking: Backend.lock serialises every transaction, restart and reconnect,
 * so exactly one thread can touch the connection or the child at a time. It
 * is always taken before (never while holding) the stats lock.
 */
#ifndef LLMGW_BACKEND_H
#define LLMGW_BACKEND_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "json.h"
#include "netio.h"

/** Consecutive reconnect retries after the first failed attempt. */
#define BACKEND_CONNECT_RETRIES 25
/** Largest backend response body accepted. */
#define BACKEND_MAX_BODY (8u * 1024u * 1024u)

/** How the backend process is owned. */
typedef enum { BACKEND_MANAGED, BACKEND_EXTERNAL } BackendMode;

/** Static backend settings. */
typedef struct {
    BackendMode mode;
    /** Managed mode: NULL-terminated argv; argv[0] is looked up in PATH. */
    char** argv;
    /** External mode: loopback port or service name. */
    const char* port;
    long long ioTimeoutMs;
    long long startupTimeoutMs;
    long long connectTimeoutMs;
    /** Resends allowed per prompt after transport failures. */
    unsigned maxResends;
    /** Emit diagnostic lines on stderr. */
    bool debug;
} BackendConfig;

/** Runtime backend state; all fields after cfg are guarded by lock. */
typedef struct {
    BackendConfig cfg;
    pthread_mutex_t lock;
    /** Stop-pipe read end: readable once shutdown has been requested. */
    int wakeFd;
    /** Managed child PID (also its process group), or -1. */
    pid_t pid;
    /** Number of managed children started so far. */
    unsigned long generation;
    /** Current backend port as text. */
    char port[16];
    /** Host header value, "localhost:<port>". */
    char host[32];
    /** Persistent connection, or -1 when disconnected. */
    int fd;
    Reader reader;
} Backend;

/** Transaction outcomes. */
typedef enum {
    /** 200 response with valid JSON; reply is filled in. */
    BACKEND_REPLY_OK,
    /** Transport failures exhausted the resend budget. */
    BACKEND_REPLY_FAILED,
    /** Complete response, but non-200 or unusable JSON (not retried). */
    BACKEND_REPLY_INVALID,
    /** Shutdown was requested. */
    BACKEND_REPLY_STOPPED
} BackendOutcome;

/** @return 0 or -1. */
int backend_init(Backend* backend, const BackendConfig* config, int wakeFd);

/**
 * Starts (managed) and connects to the backend before clients are accepted.
 * On failure an error is printed and any owned child is reaped.
 * @return 0, SERVER_EXIT_BACKEND_START, SERVER_EXIT_BACKEND_CONNECT, or -1
 * when shutdown was requested during startup.
 */
int backend_start(Backend* backend);

/**
 * Sends one completion request and waits for the reply. Before every send
 * the managed child is checked with waitid(WNOHANG); a terminated child is
 * reaped and replaced. Transport failures close the connection, reconnect
 * (1 + 25 attempts) and resend the same request, up to cfg.maxResends
 * times. Exhausting reconnect attempts is fatal: the process exits with
 * SERVER_EXIT_BACKEND_CONNECT after killing and reaping an owned child.
 */
BackendOutcome backend_complete(Backend* backend, const char* json,
        size_t jsonLen, CompletionReply* reply);

/** Closes the connection and terminates/reaps an owned child. */
void backend_shutdown(Backend* backend);

#endif
