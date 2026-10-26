/** @file workers.c
 * @brief Registry of joinable connection threads plus a reaper thread.
 */
#include <stdlib.h>

#include "workers.h"

static void list_remove(Worker** head, Worker* worker)
{
    if (worker->prev != NULL) {
        worker->prev->next = worker->next;
    } else {
        *head = worker->next;
    }
    if (worker->next != NULL) {
        worker->next->prev = worker->prev;
    }
    worker->next = NULL;
    worker->prev = NULL;
}

static void list_push(Worker** head, Worker* worker)
{
    worker->prev = NULL;
    worker->next = *head;
    if (*head != NULL) {
        (*head)->prev = worker;
    }
    *head = worker;
}

/** Joins finished handlers until closing and nothing is left. */
static void* reaper_main(void* arg)
{
    Workers* workers = arg;
    pthread_mutex_lock(&workers->lock);
    for (;;) {
        while (workers->done != NULL) {
            Worker* worker = workers->done;
            list_remove(&workers->done, worker);
            pthread_mutex_unlock(&workers->lock);
            pthread_join(worker->thread, NULL);
            free(worker->arg);
            free(worker);
            pthread_mutex_lock(&workers->lock);
        }
        if (workers->closing && workers->active == NULL) {
            break;
        }
        pthread_cond_wait(&workers->cond, &workers->lock);
    }
    pthread_mutex_unlock(&workers->lock);
    return NULL;
}

int workers_init(Workers* workers)
{
    workers->active = NULL;
    workers->done = NULL;
    workers->activeCount = 0;
    workers->closing = false;
    workers->reaperRunning = false;
    if (pthread_mutex_init(&workers->lock, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&workers->cond, NULL) != 0) {
        pthread_mutex_destroy(&workers->lock);
        return -1;
    }
    if (pthread_create(&workers->reaper, NULL, reaper_main, workers) != 0) {
        pthread_cond_destroy(&workers->cond);
        pthread_mutex_destroy(&workers->lock);
        return -1;
    }
    workers->reaperRunning = true;
    return 0;
}

int workers_spawn(Workers* workers, Worker* worker, void* (*fn)(void*))
{
    /* Holding the lock across pthread_create() guarantees worker->thread is
     * written before the new thread can move itself to the done list. */
    pthread_mutex_lock(&workers->lock);
    int rc = pthread_create(&worker->thread, NULL, fn, worker);
    if (rc == 0) {
        list_push(&workers->active, worker);
        workers->activeCount++;
    }
    pthread_mutex_unlock(&workers->lock);
    return rc;
}

void workers_finish(Workers* workers, Worker* worker)
{
    pthread_mutex_lock(&workers->lock);
    list_remove(&workers->active, worker);
    workers->activeCount--;
    list_push(&workers->done, worker);
    pthread_cond_broadcast(&workers->cond);
    pthread_mutex_unlock(&workers->lock);
}

size_t workers_active(Workers* workers)
{
    pthread_mutex_lock(&workers->lock);
    size_t count = workers->activeCount;
    pthread_mutex_unlock(&workers->lock);
    return count;
}

void workers_close(Workers* workers)
{
    if (!workers->reaperRunning) {
        return;
    }
    pthread_mutex_lock(&workers->lock);
    workers->closing = true;
    pthread_cond_broadcast(&workers->cond);
    pthread_mutex_unlock(&workers->lock);
    pthread_join(workers->reaper, NULL);
    workers->reaperRunning = false;
    pthread_cond_destroy(&workers->cond);
    pthread_mutex_destroy(&workers->lock);
}
