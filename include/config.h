/** @file config.h
 * @brief llm-server command line and environment configuration.
 *
 * Usage: llm-server [options] maxclients historysize [port]
 * Precedence: command-line option > LLMGW_* environment variable > default.
 */
#ifndef LLMGW_CONFIG_H
#define LLMGW_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "backend.h"

/** Default managed backend program (looked up in PATH). */
#define DEFAULT_BACKEND_CMD "llmgw-backend"

/** Validated server settings. */
typedef struct {
    /** 0 means no application-level limit. */
    unsigned maxClients;
    /** Interaction blocks retained per client (1..10). */
    size_t historySize;
    /** Main port text as given ("0" when absent). */
    const char* port;
    /** LLMGW_STATS_PORT value, or NULL when unset. */
    const char* statsPort;
    /** Home page file path (owned). */
    char* homePage;
    BackendConfig backend;
    /** Owned argv array for managed mode (strings borrowed). */
    char** backendArgv;
} ServerConfig;

/**
 * Parses arguments. Prints a message on error.
 * @return 0 on success, -1 when --help was printed, otherwise an exit status.
 */
int server_config_parse(int argc, char** argv, ServerConfig* config);

/** Frees owned members. */
void server_config_free(ServerConfig* config);

#endif
