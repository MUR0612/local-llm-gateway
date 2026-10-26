/** @file history.c
 * @brief Per-connection chat history measured in interaction blocks.
 *
 * Adapted from the author's coursework history module. Changes: interaction
 * capacity equals historysize, a ring buffer replaces shifting, appends are
 * all-or-nothing, strings are length-tracked, and every allocation in the
 * JSON builder is checked.
 */
#include <stdlib.h>
#include <string.h>

#include "history.h"
#include "json.h"

#define SYSTEM_BLOCK "<|im_start|>system\\nYou are a helpful assistant.<|im_end|>\\n"
#define USER_OPEN "<|im_start|>user\\n"
#define ASSISTANT_OPEN "<|im_end|>\\n<|im_start|>assistant\\n"
#define BLOCK_CLOSE "<|im_end|>\\n"
#define REQUEST_TAIL "\",\"n_predict\": 256,\"cache_prompt\": false}"

int history_init(History* history, size_t capacity)
{
    memset(history, 0, sizeof(*history));
    if (capacity < HISTORY_MIN || capacity > HISTORY_MAX) {
        return -1;
    }
    history->items = calloc(capacity, sizeof(Interaction));
    if (history->items == NULL) {
        return -1;
    }
    history->capacity = capacity;
    return 0;
}

static void free_interaction(Interaction* item)
{
    free(item->prompt);
    free(item->response);
    memset(item, 0, sizeof(*item));
}

void history_free(History* history)
{
    for (size_t i = 0; i < history->count; i++) {
        free_interaction(&history->items[(history->head + i) % history->capacity]);
    }
    free(history->items);
    memset(history, 0, sizeof(*history));
}

size_t history_count(const History* history)
{
    return history->count;
}

const Interaction* history_get(const History* history, size_t index)
{
    if (index >= history->count) {
        return NULL;
    }
    return &history->items[(history->head + index) % history->capacity];
}

int history_append(History* history, const char* prompt, size_t promptLen,
        const char* response, size_t responseLen)
{
    if (history->capacity == 0) {
        return -1;
    }
    /* Copy first so that a failed allocation leaves the history intact. */
    Interaction fresh;
    fresh.prompt = mem_dup(prompt, promptLen);
    fresh.response = mem_dup(response, responseLen);
    if (fresh.prompt == NULL || fresh.response == NULL) {
        free(fresh.prompt);
        free(fresh.response);
        return -1;
    }
    fresh.promptLen = promptLen;
    fresh.responseLen = responseLen;
    if (history->count == history->capacity) {
        free_interaction(&history->items[history->head]);
        history->items[history->head] = fresh;
        history->head = (history->head + 1) % history->capacity;
    } else {
        history->items[(history->head + history->count) % history->capacity]
                = fresh;
        history->count++;
    }
    return 0;
}

/** Appends one user block (open tag, escaped text, assistant open tag). */
static int append_user_turn(Buf* out, const char* text, size_t len)
{
    return (buf_append_str(out, USER_OPEN) != 0
                   || json_append_escaped(out, text, len) != 0
                   || buf_append_str(out, ASSISTANT_OPEN) != 0)
            ? -1
            : 0;
}

int history_build_completion_json(Buf* out, const History* history,
        const char* prompt, size_t promptLen)
{
    buf_clear(out);
    if (buf_append_str(out, "{\"prompt\": \"" SYSTEM_BLOCK) != 0) {
        return -1;
    }
    for (size_t i = 0; i < history_count(history); i++) {
        const Interaction* item = history_get(history, i);
        if (append_user_turn(out, item->prompt, item->promptLen) != 0
                || json_append_escaped(out, item->response, item->responseLen)
                        != 0
                || buf_append_str(out, BLOCK_CLOSE) != 0) {
            return -1;
        }
    }
    if (append_user_turn(out, prompt, promptLen) != 0
            || buf_append_str(out, REQUEST_TAIL) != 0) {
        return -1;
    }
    return 0;
}
