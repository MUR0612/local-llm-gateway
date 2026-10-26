/** @file config.c
 * @brief llm-server command line and environment configuration.
 *
 * Range rules for maxclients/historysize follow the coursework version; the
 * options, LLMGW_* variables and the overflow-safe parser are new.
 */
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buf.h"
#include "common.h"
#include "config.h"
#include "history.h"
#include "numparse.h"

#define USAGE_TEXT "Usage: " SERVER_NAME " [options] maxclients historysize [port]\n"
#define HOME_RELATIVE "/assets/index.html"

#define DEFAULT_IO_TIMEOUT_MS 120000
#define DEFAULT_STARTUP_TIMEOUT_MS 60000
#define DEFAULT_CONNECT_TIMEOUT_MS 5000
#define DEFAULT_MAX_RESENDS 3
#define MAX_TIMEOUT_MS 3600000UL
#define MAX_RESENDS_LIMIT 100UL

enum { OPT_BACKEND_CMD = 1000, OPT_BACKEND_ARG, OPT_BACKEND_PORT, OPT_HOME, OPT_HELP };

/** Raw option values before resolution. */
typedef struct {
    const char* backendCmd;
    const char* backendPort;
    const char* homePage;
    const char** args;
    size_t argCount;
} RawOptions;

static const char* const HELP_TEXT
        = USAGE_TEXT
        "\n"
        "  maxclients   0..10000 concurrent prompt clients (0 = no limit)\n"
        "  historysize  1..10 interaction blocks kept per client\n"
        "  port         loopback port or service name (default/0: ephemeral)\n"
        "\n"
        "Options (override the matching environment variable):\n"
        "  --backend-cmd PROGRAM  managed backend executable (LLMGW_BACKEND_CMD,\n"
        "                         default " DEFAULT_BACKEND_CMD ")\n"
        "  --backend-arg ARG      append one argument for PROGRAM (repeatable)\n"
        "  --backend-port PORT    use an external backend (LLMGW_BACKEND_PORT)\n"
        "  --home-page PATH       file served for GET / (LLMGW_HOME_PAGE)\n"
        "  --help                 show this text\n"
        "\n"
        "Environment only: LLMGW_STATS_PORT, LLMGW_BACKEND_TIMEOUT_MS,\n"
        "LLMGW_BACKEND_STARTUP_TIMEOUT_MS, LLMGW_BACKEND_CONNECT_TIMEOUT_MS,\n"
        "LLMGW_BACKEND_MAX_RESENDS, LLMGW_DEBUG. See README.md.\n";

static int usage_error(void)
{
    fputs(USAGE_TEXT, stderr);
    return SERVER_EXIT_USAGE;
}

/** @return the value of a set, non-empty environment variable, or NULL. */
static const char* env_value(const char* name)
{
    const char* value = getenv(name);
    return (value != NULL && value[0] != '\0') ? value : NULL;
}

/** Reads a numeric environment variable with a default. */
static int env_number(const char* name, unsigned long min, unsigned long max,
        unsigned long fallback, unsigned long* out)
{
    const char* value = env_value(name);
    if (value == NULL) {
        *out = fallback;
        return 0;
    }
    if (!parse_bounded_uint(value, min, max, out)) {
        fprintf(stderr, "%s: invalid value for %s\n", SERVER_NAME, name);
        return SERVER_EXIT_USAGE;
    }
    return 0;
}

/** Collects --options. @return 0, -1 for --help, or an exit status. */
static int parse_options(int argc, char** argv, RawOptions* raw)
{
    static const struct option options[] = {
            {"backend-cmd", required_argument, NULL, OPT_BACKEND_CMD},
            {"backend-arg", required_argument, NULL, OPT_BACKEND_ARG},
            {"backend-port", required_argument, NULL, OPT_BACKEND_PORT},
            {"home-page", required_argument, NULL, OPT_HOME},
            {"help", no_argument, NULL, OPT_HELP}, {NULL, 0, NULL, 0}};
    opterr = 0;
    optind = 0; /* Full re-initialisation (glibc) so tests can re-parse. */
    int opt;
    while ((opt = getopt_long(argc, argv, "+", options, NULL)) != -1) {
        if (opt == OPT_HELP) {
            fputs(HELP_TEXT, stdout);
            return -1;
        }
        if (opt == '?' || optarg == NULL || optarg[0] == '\0') {
            return usage_error();
        }
        if (opt == OPT_BACKEND_CMD) {
            raw->backendCmd = optarg;
        } else if (opt == OPT_BACKEND_PORT) {
            raw->backendPort = optarg;
        } else if (opt == OPT_HOME) {
            raw->homePage = optarg;
        } else {
            raw->args[raw->argCount++] = optarg;
        }
    }
    return 0;
}

/** Validates maxclients, historysize and the optional port. */
static int parse_positionals(int count, char** args, ServerConfig* config)
{
    if (count < 2 || count > 3) {
        return usage_error();
    }
    for (int i = 0; i < count; i++) {
        if (args[i][0] == '\0') {
            return usage_error();
        }
    }
    unsigned long maxClients;
    unsigned long historySize;
    if (!parse_bounded_uint(args[0], 0, MAX_CLIENTS_LIMIT, &maxClients)
            || !parse_bounded_uint(
                    args[1], HISTORY_MIN, HISTORY_MAX, &historySize)) {
        return usage_error();
    }
    config->maxClients = (unsigned)maxClients;
    config->historySize = (size_t)historySize;
    config->port = count == 3 ? args[2] : "0";
    return 0;
}

/** Chooses managed or external mode and builds the managed argv. */
static int resolve_backend(const RawOptions* raw, ServerConfig* config)
{
    BackendConfig* backend = &config->backend;
    const char* cmd = raw->backendCmd;
    const char* port = raw->backendPort;
    if (cmd != NULL && port != NULL) {
        fprintf(stderr, "%s: --backend-cmd and --backend-port are exclusive\n",
                SERVER_NAME);
        return usage_error();
    }
    if (cmd == NULL && port == NULL) {
        cmd = env_value("LLMGW_BACKEND_CMD");
        port = env_value("LLMGW_BACKEND_PORT");
        if (cmd != NULL && port != NULL) {
            fprintf(stderr,
                    "%s: LLMGW_BACKEND_CMD and LLMGW_BACKEND_PORT are exclusive\n",
                    SERVER_NAME);
            return usage_error();
        }
    }
    if (port != NULL) {
        if (raw->argCount > 0) {
            return usage_error(); /* Arguments only apply to managed mode. */
        }
        backend->mode = BACKEND_EXTERNAL;
        backend->port = port;
        return 0;
    }
    backend->mode = BACKEND_MANAGED;
    config->backendArgv = calloc(raw->argCount + 2, sizeof(char*));
    if (config->backendArgv == NULL) {
        return SERVER_EXIT_USAGE;
    }
    config->backendArgv[0] = (char*)(cmd != NULL ? cmd : DEFAULT_BACKEND_CMD);
    for (size_t i = 0; i < raw->argCount; i++) {
        config->backendArgv[i + 1] = (char*)raw->args[i];
    }
    backend->argv = config->backendArgv;
    return 0;
}

/** Reads numeric environment settings. */
static int resolve_tunables(BackendConfig* backend)
{
    unsigned long io;
    unsigned long startup;
    unsigned long connect;
    unsigned long resends;
    unsigned long debug;
    int rc;
    if ((rc = env_number("LLMGW_BACKEND_TIMEOUT_MS", 1, MAX_TIMEOUT_MS,
                 DEFAULT_IO_TIMEOUT_MS, &io))
                    != 0
            || (rc = env_number("LLMGW_BACKEND_STARTUP_TIMEOUT_MS", 1,
                        MAX_TIMEOUT_MS, DEFAULT_STARTUP_TIMEOUT_MS, &startup))
                    != 0
            || (rc = env_number("LLMGW_BACKEND_CONNECT_TIMEOUT_MS", 1,
                        MAX_TIMEOUT_MS, DEFAULT_CONNECT_TIMEOUT_MS, &connect))
                    != 0
            || (rc = env_number("LLMGW_BACKEND_MAX_RESENDS", 0,
                        MAX_RESENDS_LIMIT, DEFAULT_MAX_RESENDS, &resends))
                    != 0
            || (rc = env_number("LLMGW_DEBUG", 0, 1, 0, &debug)) != 0) {
        return rc;
    }
    backend->ioTimeoutMs = (long long)io;
    backend->startupTimeoutMs = (long long)startup;
    backend->connectTimeoutMs = (long long)connect;
    backend->maxResends = (unsigned)resends;
    backend->debug = debug != 0;
    return 0;
}

/** Default home page: <directory of the executable>/assets/index.html. */
static char* default_home_page(void)
{
    char exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0) {
        return mem_dup("assets/index.html", strlen("assets/index.html"));
    }
    exe[len] = '\0';
    char* slash = strrchr(exe, '/');
    size_t dirLen = slash == NULL ? 0 : (size_t)(slash - exe);
    Buf path;
    buf_init(&path);
    if (buf_append(&path, exe, dirLen) != 0
            || buf_append_str(&path, HOME_RELATIVE) != 0) {
        buf_free(&path);
        return NULL;
    }
    return path.data;
}

int server_config_parse(int argc, char** argv, ServerConfig* config)
{
    memset(config, 0, sizeof(*config));
    RawOptions raw;
    memset(&raw, 0, sizeof(raw));
    raw.args = calloc((size_t)argc + 1, sizeof(char*));
    if (raw.args == NULL) {
        return SERVER_EXIT_USAGE;
    }
    int rc = parse_options(argc, argv, &raw);
    if (rc == 0) {
        rc = parse_positionals(argc - optind, argv + optind, config);
    }
    if (rc == 0) {
        rc = resolve_backend(&raw, config);
    }
    if (rc == 0) {
        rc = resolve_tunables(&config->backend);
    }
    if (rc == 0) {
        const char* home = raw.homePage != NULL ? raw.homePage
                                                : env_value("LLMGW_HOME_PAGE");
        config->homePage = home != NULL ? mem_dup(home, strlen(home))
                                        : default_home_page();
        rc = config->homePage == NULL ? SERVER_EXIT_USAGE : 0;
    }
    config->statsPort = getenv("LLMGW_STATS_PORT");
    free(raw.args);
    if (rc != 0) {
        server_config_free(config);
    }
    return rc;
}

void server_config_free(ServerConfig* config)
{
    free(config->homePage);
    free(config->backendArgv);
    config->homePage = NULL;
    config->backendArgv = NULL;
}
