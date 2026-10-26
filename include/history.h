/** @file history.h
 * @brief Per-connection chat history measured in interaction blocks.
 *
 * One interaction block is one completed prompt plus the backend's reply.
 * historysize N keeps exactly the N most recent completed blocks (the
 * coursework version wrongly kept (N + 1) / 2). Failed transactions are
 * never appended. A History is owned by exactly one connection thread and
 * is therefore not locked.
 */
#ifndef LLMGW_HISTORY_H
#define LLMGW_HISTORY_H

#include <stddef.h>

#include "buf.h"

/** Smallest and largest accepted history sizes. */
#define HISTORY_MIN 1
#define HISTORY_MAX 10

/** One completed exchange (both byte strings owned, length-tracked). */
typedef struct {
    char* prompt;
    size_t promptLen;
    char* response;
    size_t responseLen;
} Interaction;

/** Fixed-capacity ring of interactions, oldest first. */
typedef struct {
    Interaction* items;
    size_t capacity;
    size_t count;
    /** Index of the oldest interaction. */
    size_t head;
} History;

/** @return 0, or -1 for a capacity outside 1..10 or allocation failure. */
int history_init(History* history, size_t capacity);

/** Frees all stored interactions. */
void history_free(History* history);

/** @return number of stored interactions. */
size_t history_count(const History* history);

/** @return interaction @p index, where 0 is the oldest. */
const Interaction* history_get(const History* history, size_t index);

/**
 * Appends a completed interaction, evicting the oldest when full.
 * All-or-nothing: on allocation failure the history is unchanged.
 * @return 0 or -1.
 */
int history_append(History* history, const char* prompt, size_t promptLen,
        const char* response, size_t responseLen);

/**
 * Builds the backend /completion JSON body: system block, stored blocks in
 * chronological order, the current prompt and an empty assistant prefix,
 * plus n_predict=256 and cache_prompt=false.
 * @return 0 or -1.
 */
int history_build_completion_json(Buf* out, const History* history,
        const char* prompt, size_t promptLen);

#endif
