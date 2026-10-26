/** @file workers.h
 * @brief Registry of joinable connection threads plus a reaper thread.
 *
 * Handler threads are joinable rather than detached so that shutdown can
 * prove every handler has returned before shared state is destroyed. A
 * finishing handler moves itself to a "done" list; the reaper joins it and
 * frees its record, so finished threads never accumulate.
 */
#ifndef LLMGW_WORKERS_H
#define LLMGW_WORKERS_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

/** Per-thread record; the thread's own data follows via @p arg. */
typedef struct Worker {
    struct Worker* next;
    struct Worker* prev;
    pthread_t thread;
    void* arg;
} Worker;

/** Thread registry. */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    Worker* active;
    Worker* done;
    size_t activeCount;
    bool closing;
    bool reaperRunning;
    pthread_t reaper;
} Workers;

/** Starts the reaper thread. @return 0 or -1. */
int workers_init(Workers* workers);

/**
 * Creates a joinable thread running @p fn(worker). On failure nothing is
 * registered and the caller still owns @p worker.
 * @return 0 or the pthread_create() error.
 */
int workers_spawn(Workers* workers, Worker* worker, void* (*fn)(void*));

/**
 * Called by a handler as its very last action on @p worker; afterwards the
 * handler must not touch @p worker (the reaper frees it).
 */
void workers_finish(Workers* workers, Worker* worker);

/** @return number of handlers that have not called workers_finish(). */
size_t workers_active(Workers* workers);

/** Waits for every handler to finish, joins them and stops the reaper. */
void workers_close(Workers* workers);

#endif
