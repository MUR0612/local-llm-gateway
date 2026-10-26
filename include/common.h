/** @file common.h
 * @brief Product names, exit statuses and protocol constants.
 *
 * Exit statuses keep the coursework semantics; only the names and message
 * texts changed (see docs/requirements.md).
 */
#ifndef LLMGW_COMMON_H
#define LLMGW_COMMON_H

#define CLIENT_NAME "llm-client"
#define SERVER_NAME "llm-server"

/* llm-client exit statuses. */
#define CLIENT_EXIT_OK 0
#define CLIENT_EXIT_USAGE 8
#define CLIENT_EXIT_EMPTY_PORT 9
#define CLIENT_EXIT_COMM 13
#define CLIENT_EXIT_OUTPUT 14
#define CLIENT_EXIT_CONNECT 18

/* llm-server exit statuses. */
#define SERVER_EXIT_OK 0
#define SERVER_EXIT_BACKEND_CONNECT 1
#define SERVER_EXIT_STATS_LISTEN 4
#define SERVER_EXIT_LISTEN 9
#define SERVER_EXIT_USAGE 16
#define SERVER_EXIT_BACKEND_START 17

#define MSG_BACKEND_START SERVER_NAME ": could not start the backend\n"
#define MSG_BACKEND_CONNECT SERVER_NAME ": could not connect to the backend\n"

/** Largest accepted POST / body in bytes (including the trailing newline). */
#define MAX_PROMPT_BODY 2048
/** Bodies up to this size are drained so keep-alive framing survives. */
#define MAX_DRAIN_BODY (64u * 1024u)
/** Largest home page file served. */
#define MAX_HOME_PAGE (1024u * 1024u)
/** Largest prompt-client limit. */
#define MAX_CLIENTS_LIMIT 10000

#endif
