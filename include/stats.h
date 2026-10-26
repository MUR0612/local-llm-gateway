/** @file stats.h
 * @brief Mutex-protected server counters with consistent snapshots.
 *
 * The stats lock is a leaf lock: it is held only to update or copy the
 * counters, never across network I/O or while waiting for the backend.
 */
#ifndef LLMGW_STATS_H
#define LLMGW_STATS_H

#include <pthread.h>
#include <stdint.h>

#include "buf.h"

/** A consistent copy of all counters. */
typedef struct {
    /** Prompt clients currently being served. */
    uint64_t current;
    /** Prompt clients accepted since start. */
    uint64_t total;
    /** Prompt-client sessions that have ended. */
    uint64_t completed;
    /** Accepted POST / requests (counted before the size check). */
    uint64_t prompts;
    /** Sum of tokens_evaluated from successful backend replies. */
    uint64_t tokens;
} StatsSnapshot;

/** Shared counters. */
typedef struct {
    pthread_mutex_t lock;
    StatsSnapshot values;
} Stats;

/** @return 0 or -1. */
int stats_init(Stats* stats);
void stats_destroy(Stats* stats);

void stats_client_started(Stats* stats);
void stats_client_finished(Stats* stats);
void stats_prompt_received(Stats* stats);
void stats_add_tokens(Stats* stats, uint64_t tokens);

/** Copies all counters under one lock acquisition. */
void stats_snapshot(Stats* stats, StatsSnapshot* out);

/** Formats the five-line report. @return 0 or -1. */
int stats_format(const StatsSnapshot* snapshot, Buf* out);

#endif
