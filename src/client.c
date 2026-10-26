/** @file client.c
 * @brief Interactive client: one stdin line -> one POST / -> one reply.
 *
 * The argument checks, prompt loop and stdout/stderr split follow the
 * author's coursework client. New: the project's own HTTP code instead of
 * the course library, byte-accurate bodies (embedded NUL is preserved),
 * a bounded response wait (LLMGW_CLIENT_TIMEOUT_MS) and explicit handling
 * of local output failures.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buf.h"
#include "client.h"
#include "common.h"
#include "http.h"
#include "netio.h"
#include "numparse.h"

#define DEFAULT_RESPONSE_TIMEOUT_MS 600000UL
#define CONNECT_TIMEOUT_MS 10000
#define MAX_RESPONSE_BODY (16u * 1024u * 1024u)
#define MAX_TIMEOUT_MS 86400000UL

#define MSG_COMM CLIENT_NAME ": unexpected communication error\n"

/** Client resources released on every exit path. */
typedef struct {
    int fd;
    char* line;
    size_t lineCap;
    Buf request;
    Reader reader;
    long long timeoutMs;
} ClientState;

static void client_cleanup(ClientState* c)
{
    close_fd(&c->fd);
    free(c->line);
    c->line = NULL;
    buf_free(&c->request);
}

/** @return 0 when the arguments are valid, otherwise an exit status. */
static int check_arguments(int argc, char** argv, ClientState* c)
{
    if (argc != 2) {
        fputs("Usage: " CLIENT_NAME " port\n", stderr);
        return CLIENT_EXIT_USAGE;
    }
    if (argv[1][0] == '\0') {
        fputs(CLIENT_NAME ": invalid port\n", stderr);
        return CLIENT_EXIT_EMPTY_PORT;
    }
    const char* timeout = getenv("LLMGW_CLIENT_TIMEOUT_MS");
    unsigned long value = DEFAULT_RESPONSE_TIMEOUT_MS;
    if (timeout != NULL && timeout[0] != '\0'
            && !parse_bounded_uint(timeout, 1, MAX_TIMEOUT_MS, &value)) {
        fputs(CLIENT_NAME ": invalid value for LLMGW_CLIENT_TIMEOUT_MS\n", stderr);
        return CLIENT_EXIT_USAGE;
    }
    c->timeoutMs = (long long)value;
    return 0;
}

/** Writes and flushes stdout text. @return 0 or -1. */
static int print_out(const char* text)
{
    return (fputs(text, stdout) == EOF || fflush(stdout) != 0) ? -1 : 0;
}

/** Prints "Response:" and the body to the stream chosen by the status. */
static int print_response(const HttpResponse* response)
{
    FILE* stream = response->status == 200 ? stdout : stderr;
    if (fputs("Response:\n", stream) == EOF
            || fwrite(response->body, 1, response->bodyLen, stream)
                    != response->bodyLen
            || fflush(stream) != 0) {
        return -1;
    }
    return 0;
}

/** Sends one prompt and prints its reply. @return 0 or an exit status. */
static int exchange(ClientState* c, size_t lineLen)
{
    /* The body is the line without its newline, plus exactly one '\n'. */
    if (lineLen > 0 && c->line[lineLen - 1] == '\n') {
        lineLen--;
    }
    c->line[lineLen] = '\n';
    long long deadline = deadline_after(c->timeoutMs);
    if (http_build_request(&c->request, "POST", "/", "localhost", "text/plain",
                c->line, lineLen + 1)
                    != 0
            || net_send_all(c->fd, c->request.data, c->request.len, -1, deadline)
                    != IO_OK) {
        fputs(MSG_COMM, stderr);
        return CLIENT_EXIT_COMM;
    }
    HttpResponse response;
    c->reader.deadlineMs = deadline;
    if (http_read_response(&c->reader, MAX_RESPONSE_BODY, &response) != HTTP_OK) {
        fputs(MSG_COMM, stderr);
        return CLIENT_EXIT_COMM;
    }
    int rc = print_response(&response) == 0 ? 0 : CLIENT_EXIT_OUTPUT;
    http_response_free(&response);
    return rc;
}

/** Runs the prompt loop until EOF on stdin. @return exit status. */
static int prompt_loop(ClientState* c)
{
    for (;;) {
        if (print_out("Enter your prompt:\n") != 0) {
            return CLIENT_EXIT_OUTPUT;
        }
        ssize_t got = getline(&c->line, &c->lineCap, stdin);
        if (got < 0) {
            return CLIENT_EXIT_OK; /* EOF (or unreadable stdin). */
        }
        if ((size_t)got + 1 > c->lineCap) {
            /* getline() guarantees got < lineCap, so the '\n' slot exists. */
            return CLIENT_EXIT_COMM;
        }
        int rc = exchange(c, (size_t)got);
        if (rc != 0) {
            return rc;
        }
    }
}

int run_client(int argc, char** argv)
{
    ClientState c;
    memset(&c, 0, sizeof(c));
    c.fd = -1;
    buf_init(&c.request);
    int rc = check_arguments(argc, argv, &c);
    if (rc != 0) {
        return rc;
    }
    struct sigaction ignore;
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGPIPE, &ignore, NULL);

    c.fd = net_connect_loopback(argv[1], -1, CONNECT_TIMEOUT_MS);
    if (c.fd < 0) {
        fputs(CLIENT_NAME ": unable to connect to the server\n", stderr);
        return CLIENT_EXIT_CONNECT;
    }
    reader_init(&c.reader, c.fd, -1);
    if (print_out("Welcome to " CLIENT_NAME ".\n") != 0) {
        rc = CLIENT_EXIT_OUTPUT;
    } else {
        rc = prompt_loop(&c);
    }
    if (rc == CLIENT_EXIT_OUTPUT) {
        fputs(CLIENT_NAME ": unable to write output\n", stderr);
    }
    client_cleanup(&c);
    return rc;
}
