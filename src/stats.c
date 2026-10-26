/** @file stats.c
 * @brief Mutex-protected server counters with consistent snapshots.
 */
#include <inttypes.h>
#include <string.h>

#include "stats.h"

int stats_init(Stats* stats)
{
    memset(&stats->values, 0, sizeof(stats->values));
    return pthread_mutex_init(&stats->lock, NULL) == 0 ? 0 : -1;
}

void stats_destroy(Stats* stats)
{
    pthread_mutex_destroy(&stats->lock);
}

void stats_client_started(Stats* stats)
{
    pthread_mutex_lock(&stats->lock);
    stats->values.current++;
    stats->values.total++;
    pthread_mutex_unlock(&stats->lock);
}

void stats_client_finished(Stats* stats)
{
    pthread_mutex_lock(&stats->lock);
    stats->values.current--;
    stats->values.completed++;
    pthread_mutex_unlock(&stats->lock);
}

void stats_prompt_received(Stats* stats)
{
    pthread_mutex_lock(&stats->lock);
    stats->values.prompts++;
    pthread_mutex_unlock(&stats->lock);
}

void stats_add_tokens(Stats* stats, uint64_t tokens)
{
    pthread_mutex_lock(&stats->lock);
    stats->values.tokens += tokens;
    pthread_mutex_unlock(&stats->lock);
}

void stats_snapshot(Stats* stats, StatsSnapshot* out)
{
    pthread_mutex_lock(&stats->lock);
    *out = stats->values;
    pthread_mutex_unlock(&stats->lock);
}

int stats_format(const StatsSnapshot* snapshot, Buf* out)
{
    buf_clear(out);
    return buf_printf(out,
            "Clients connected: %" PRIu64 "\n"
            "Total connected clients: %" PRIu64 "\n"
            "Total sessions completed: %" PRIu64 "\n"
            "Num prompts received: %" PRIu64 "\n"
            "Total tokens evaluated: %" PRIu64 "\n",
            snapshot->current, snapshot->total, snapshot->completed,
            snapshot->prompts, snapshot->tokens);
}
