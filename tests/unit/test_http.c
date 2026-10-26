/** @file test_http.c
 * @brief Unit tests for the HTTP subset, configuration parsing and
 *        allocation-failure cleanup.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "common.h"
#include "config.h"
#include "history.h"
#include "http.h"
#include "json.h"
#include "testlib.h"

/** Feeds bytes into one end of a socketpair from a helper thread. */
typedef struct {
    int fd;
    const char* data;
    size_t len;
    size_t chunk;
    pthread_t thread;
    int peer;
    Reader reader;
} Feed;

static void* feeder_main(void* arg)
{
    Feed* feed = arg;
    size_t off = 0;
    while (off < feed->len) {
        size_t n = feed->len - off < feed->chunk ? feed->len - off : feed->chunk;
        ssize_t w = write(feed->fd, feed->data + off, n);
        if (w <= 0) {
            break;
        }
        off += (size_t)w;
        if (feed->chunk < feed->len) {
            struct timespec pause = {0, 200000};
            nanosleep(&pause, NULL);
        }
    }
    shutdown(feed->fd, SHUT_WR);
    return NULL;
}

static void feed_open(Feed* feed, const char* data, size_t len, size_t chunk)
{
    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    feed->fd = fds[1];
    feed->peer = fds[0];
    feed->data = data;
    feed->len = len;
    feed->chunk = chunk == 0 ? len + 1 : chunk;
    reader_init(&feed->reader, fds[0], -1);
    feed->reader.deadlineMs = deadline_after(5000);
    pthread_create(&feed->thread, NULL, feeder_main, feed);
}

static void feed_close(Feed* feed)
{
    /* Closing our end first unblocks a feeder stuck on a full buffer. */
    close(feed->peer);
    pthread_join(feed->thread, NULL);
    close(feed->fd);
}

static HttpStatus head_of(const char* text, size_t chunk, HttpRequestHead* head)
{
    Feed feed;
    feed_open(&feed, text, strlen(text), chunk);
    HttpStatus rc = http_read_request_head(&feed.reader, head);
    feed_close(&feed);
    return rc;
}

static void expect_malformed(const char* text)
{
    HttpRequestHead head;
    HttpStatus rc = head_of(text, 0, &head);
    CHECK_EQ_INT(rc, HTTP_MALFORMED);
    if (rc == HTTP_OK) {
        http_request_head_free(&head);
    }
}

static void test_requests(void)
{
    HttpRequestHead head;
    CHECK_EQ_INT(head_of("POST / HTTP/1.1\r\ncontent-LENGTH: 6\r\nX-A:  b \r\n\r\n", 0, &head),
            HTTP_OK);
    CHECK_STR(head.method, "POST");
    CHECK_STR(head.target, "/");
    CHECK_EQ_INT(head.contentLength, 6);
    CHECK(head.hasContentLength);
    CHECK(!head.connectionClose);
    CHECK_STR(http_find_header(head.headers, head.headerCount, "x-a"), "b");
    http_request_head_free(&head);

    /* Fragmented one byte at a time. */
    CHECK_EQ_INT(head_of("GET /x HTTP/1.1\r\nConnection: keep-alive, Close\r\n\r\n", 1, &head),
            HTTP_OK);
    CHECK_STR(head.target, "/x");
    CHECK(head.connectionClose);
    CHECK(!head.hasContentLength);
    http_request_head_free(&head);

    CHECK_EQ_INT(head_of("GET / HTTP/1.0\r\n\r\n", 0, &head), HTTP_OK);
    CHECK(head.connectionClose);
    http_request_head_free(&head);
    CHECK_EQ_INT(head_of("get / HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\n", 0, &head),
            HTTP_OK);
    CHECK_STR(head.method, "get"); /* Valid token; the server answers 405. */
    http_request_head_free(&head);

    CHECK_EQ_INT(head_of("", 0, &head), HTTP_EOF);
    expect_malformed("GET / HTTP/1.1\r\nHost: x\r\n");          /* EOF in headers */
    expect_malformed("GET / HTTP/1.1\n\n");                     /* bare LF */
    expect_malformed("GET / HTTP/1.1\r\nA: b\n\r\n");
    expect_malformed("GET /\r\n\r\n");
    expect_malformed("GET / HTTP/2.0\r\n\r\n");
    expect_malformed("GET  / HTTP/1.1\r\n\r\n");
    expect_malformed("G(T / HTTP/1.1\r\n\r\n");
    expect_malformed("GET / HTTP/1.1\r\nNoColon\r\n\r\n");
    expect_malformed("GET / HTTP/1.1\r\n: empty\r\n\r\n");
    expect_malformed("GET / HTTP/1.1\r\nName : v\r\n\r\n");
    expect_malformed("GET / HTTP/1.1\r\nA: b\r\n folded\r\n\r\n");
    expect_malformed("POST / HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\n");
    expect_malformed("POST / HTTP/1.1\r\nContent-Length: +3\r\n\r\n");
    expect_malformed("POST / HTTP/1.1\r\nContent-Length: 3, 3\r\n\r\n");
    expect_malformed("POST / HTTP/1.1\r\nContent-Length: \r\n\r\n");
    expect_malformed("POST / HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\n");
    expect_malformed("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
    expect_malformed("GET / HTTP/1.1\r\nA: b\x01\r\n\r\n");

    CHECK_EQ_INT(head_of("POST / HTTP/1.1\r\nContent-Length: 18446744073709551615\r\n\r\n", 0, &head),
            HTTP_OK);
    CHECK(head.contentLength == 18446744073709551615ULL);
    http_request_head_free(&head);

    Buf big;
    buf_init(&big);
    buf_append_str(&big, "GET / HTTP/1.1\r\nA: ");
    for (int i = 0; i < HTTP_MAX_LINE; i++) {
        buf_append(&big, "x", 1);
    }
    buf_append_str(&big, "\r\n\r\n");
    expect_malformed(big.data);
    buf_clear(&big);
    buf_append_str(&big, "GET / HTTP/1.1\r\n");
    for (int i = 0; i <= HTTP_MAX_HEADERS; i++) {
        buf_printf(&big, "H%d: v\r\n", i);
    }
    buf_append_str(&big, "\r\n");
    expect_malformed(big.data);
    buf_clear(&big);
    buf_append_str(&big, "GET / HTTP/1.1\r\n");
    for (int i = 0; i < 40; i++) {
        buf_append_str(&big, "H: ");
        for (int j = 0; j < 1000; j++) {
            buf_append(&big, "y", 1);
        }
        buf_append_str(&big, "\r\n");
    }
    buf_append_str(&big, "\r\n");
    expect_malformed(big.data); /* > HTTP_MAX_HEADER_BYTES in total. */
    buf_free(&big);
}

static void test_pipelining_and_bodies(void)
{
    const char* two = "POST / HTTP/1.1\r\nContent-Length: 4\r\n\r\nabc\nGET /next HTTP/1.1\r\n\r\n";
    for (size_t chunk = 0; chunk <= 3; chunk += 3) {
        Feed feed;
        feed_open(&feed, two, strlen(two), chunk);
        HttpRequestHead head;
        unsigned char* body = NULL;
        CHECK_EQ_INT(http_read_request_head(&feed.reader, &head), HTTP_OK);
        CHECK_EQ_INT(http_read_body(&feed.reader, 4, &body), HTTP_OK);
        CHECK_MEM(body, 4, "abc\n", 4);
        free(body);
        http_request_head_free(&head);
        CHECK_EQ_INT(http_read_request_head(&feed.reader, &head), HTTP_OK);
        CHECK_STR(head.target, "/next");
        http_request_head_free(&head);
        CHECK_EQ_INT(http_read_request_head(&feed.reader, &head), HTTP_EOF);
        feed_close(&feed);
    }

    /* Discard a 100 KiB body without allocating it. */
    size_t bodyLen = 100 * 1024;
    Buf msg;
    buf_init(&msg);
    buf_printf(&msg, "PUT / HTTP/1.1\r\nContent-Length: %zu\r\n\r\n", bodyLen);
    for (size_t i = 0; i < bodyLen; i++) {
        buf_append(&msg, "z", 1);
    }
    buf_append_str(&msg, "GET / HTTP/1.1\r\n\r\n");
    Feed feed;
    feed_open(&feed, msg.data, msg.len, 4096);
    HttpRequestHead head;
    CHECK_EQ_INT(http_read_request_head(&feed.reader, &head), HTTP_OK);
    CHECK_EQ_INT(http_discard_body(&feed.reader, head.contentLength), HTTP_OK);
    http_request_head_free(&head);
    CHECK_EQ_INT(http_read_request_head(&feed.reader, &head), HTTP_OK);
    http_request_head_free(&head);
    feed_close(&feed);
    buf_free(&msg);

    const char* early = "POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc";
    feed_open(&feed, early, strlen(early), 0);
    unsigned char* body = NULL;
    CHECK_EQ_INT(http_read_request_head(&feed.reader, &head), HTTP_OK);
    CHECK_EQ_INT(http_read_body(&feed.reader, 10, &body), HTTP_MALFORMED);
    CHECK(body == NULL);
    http_request_head_free(&head);
    feed_close(&feed);
}

static HttpStatus response_of(const char* text, size_t maxBody, HttpResponse* resp)
{
    Feed feed;
    feed_open(&feed, text, strlen(text), 0);
    HttpStatus rc = http_read_response(&feed.reader, maxBody, resp);
    feed_close(&feed);
    return rc;
}

static void test_responses(void)
{
    HttpResponse resp;
    CHECK_EQ_INT(response_of("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nX: y\r\n\r\na\0b", 100, &resp),
            HTTP_MALFORMED); /* strlen() stops at NUL: body is short. */
    const char ok[] = "HTTP/1.1 404 Not Found\r\ncontent-length: 3\r\n\r\nno\n";
    CHECK_EQ_INT(response_of(ok, 100, &resp), HTTP_OK);
    CHECK_EQ_INT(resp.status, 404);
    CHECK_STR(resp.reason, "Not Found");
    CHECK_MEM(resp.body, resp.bodyLen, "no\n", 3);
    http_response_free(&resp);
    CHECK_EQ_INT(response_of("HTTP/1.1 200\r\nContent-Length: 0\r\n\r\n", 100, &resp), HTTP_OK);
    CHECK_EQ_INT(resp.bodyLen, 0);
    http_response_free(&resp);
    CHECK_EQ_INT(response_of("HTTP/1.1 204 No Content\r\n\r\n", 100, &resp), HTTP_OK);
    http_response_free(&resp);
    CHECK_EQ_INT(response_of("HTTP/1.1 200 OK\r\n\r\nbody", 100, &resp), HTTP_MALFORMED);
    CHECK_EQ_INT(response_of("HTTP/1.1 100 Continue\r\n\r\n", 100, &resp), HTTP_MALFORMED);
    CHECK_EQ_INT(response_of("HTTP/1.1 2x0 OK\r\nContent-Length: 0\r\n\r\n", 100, &resp),
            HTTP_MALFORMED);
    CHECK_EQ_INT(response_of("HTTP/1.1 200 OK\r\nContent-Length: 101\r\n\r\n", 100, &resp),
            HTTP_TOO_LARGE);
    CHECK_EQ_INT(response_of("NOT-HTTP garbage\r\n\r\n", 100, &resp), HTTP_MALFORMED);
    CHECK_EQ_INT(response_of("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", 100, &resp),
            HTTP_MALFORMED);
    CHECK_EQ_INT(response_of("", 100, &resp), HTTP_EOF);

    Buf out;
    buf_init(&out);
    CHECK_EQ_INT(http_build_response(&out, 413, "text/plain", "Too\n", 4, false), 0);
    CHECK_STR(out.data, "HTTP/1.1 413 Content Too Large\r\nContent-Type: text/plain\r\n"
                        "Content-Length: 4\r\n\r\nToo\n");
    CHECK_EQ_INT(http_build_response(&out, 200, "text/html", "\xe4\xb8\xad", 3, true), 0);
    CHECK_STR(out.data, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 3\r\n"
                        "Connection: close\r\n\r\n\xe4\xb8\xad");
    CHECK_EQ_INT(http_build_request(&out, "POST", "/", "localhost", NULL, "a\0\n", 3), 0);
    static const char expectedRequest[]
            = "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 3\r\n\r\na\0\n";
    CHECK_MEM(out.data, out.len, expectedRequest, sizeof(expectedRequest) - 1);
    buf_free(&out);
}

void test_http(void)
{
    test_requests();
    test_pipelining_and_bodies();
    test_responses();
}

static int parse_args(const char* const* args)
{
    char* argv[16];
    int argc = 0;
    argv[argc++] = (char*)"llm-server";
    for (; args[argc - 1] != NULL; argc++) {
        argv[argc] = (char*)args[argc - 1];
    }
    argv[argc] = NULL;
    ServerConfig config;
    int rc = server_config_parse(argc, argv, &config);
    if (rc == 0) {
        server_config_free(&config);
    }
    return rc;
}

void test_config(void)
{
    unsetenv("LLMGW_BACKEND_CMD");
    unsetenv("LLMGW_BACKEND_PORT");
    /* Silence expected usage messages; CHECK output is restored below. */
    fflush(stderr);
    int savedStderr = dup(STDERR_FILENO);
    FILE* sink = fopen("/dev/null", "w");
    if (sink != NULL) {
        dup2(fileno(sink), STDERR_FILENO);
        fclose(sink);
    }
    struct {
        const char* args[8];
        int rc;
    } cases[] = {{{"5", "2", NULL}, 0}, {{"0", "10", "2310", NULL}, 0},
            {{"+10000", "+1", NULL}, 0}, {{NULL}, SERVER_EXIT_USAGE},
            {{"1", NULL}, SERVER_EXIT_USAGE}, {{"1", "1", "1", "1", NULL}, SERVER_EXIT_USAGE},
            {{"", "1", NULL}, SERVER_EXIT_USAGE}, {{"1", "", NULL}, SERVER_EXIT_USAGE},
            {{"1", "1", "", NULL}, SERVER_EXIT_USAGE}, {{"-1", "1", NULL}, SERVER_EXIT_USAGE},
            {{"10001", "1", NULL}, SERVER_EXIT_USAGE}, {{"1", "0", NULL}, SERVER_EXIT_USAGE},
            {{"1", "11", NULL}, SERVER_EXIT_USAGE}, {{" 1", "1", NULL}, SERVER_EXIT_USAGE},
            {{"1", "1x", NULL}, SERVER_EXIT_USAGE},
            {{"--backend-port", "9", "--backend-cmd", "x", "1", "1", NULL}, SERVER_EXIT_USAGE},
            {{"--backend-port", "9", "--backend-arg", "a", "1", "1", NULL}, SERVER_EXIT_USAGE},
            {{"--backend-cmd", "", "1", "1", NULL}, SERVER_EXIT_USAGE},
            {{"--bogus", "1", "1", NULL}, SERVER_EXIT_USAGE},
            {{"--backend-cmd", "prog", "--backend-arg", "a", "1", "1", NULL}, 0},
            {{"--", "1", "1", NULL}, 0}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int rc = parse_args(cases[i].args);
        CHECK_EQ_INT(rc, cases[i].rc);
        if (rc != cases[i].rc) {
            fprintf(stdout, "  config case %zu\n", i);
        }
    }

    const char* managed[] = {"--backend-cmd", "prog", "--backend-arg", "-x", "--backend-arg",
            "y z", "3", "4", "0", NULL};
    char* argv[16];
    int argc = 0;
    argv[argc++] = (char*)"llm-server";
    for (int i = 0; managed[i] != NULL; i++) {
        argv[argc++] = (char*)managed[i];
    }
    argv[argc] = NULL;
    ServerConfig config;
    CHECK_EQ_INT(server_config_parse(argc, argv, &config), 0);
    CHECK_EQ_INT(config.backend.mode, BACKEND_MANAGED);
    CHECK_STR(config.backend.argv[0], "prog");
    CHECK_STR(config.backend.argv[1], "-x");
    CHECK_STR(config.backend.argv[2], "y z");
    CHECK(config.backend.argv[3] == NULL);
    CHECK_EQ_INT(config.maxClients, 3);
    CHECK_EQ_INT(config.historySize, 4);
    CHECK_STR(config.port, "0");
    CHECK_EQ_INT(config.backend.maxResends, 3);
    server_config_free(&config);

    setenv("LLMGW_BACKEND_PORT", "4567", 1);
    const char* external[] = {"1", "1", NULL};
    CHECK_EQ_INT(parse_args(external), 0);
    setenv("LLMGW_BACKEND_CMD", "x", 1);
    CHECK_EQ_INT(parse_args(external), SERVER_EXIT_USAGE);
    const char* flagWins[] = {"--backend-cmd", "prog", "1", "1", NULL};
    CHECK_EQ_INT(parse_args(flagWins), 0); /* Flags override the env pair. */
    unsetenv("LLMGW_BACKEND_CMD");
    unsetenv("LLMGW_BACKEND_PORT");
    setenv("LLMGW_BACKEND_MAX_RESENDS", "101", 1);
    CHECK_EQ_INT(parse_args(external), SERVER_EXIT_USAGE);
    setenv("LLMGW_BACKEND_MAX_RESENDS", "0", 1);
    CHECK_EQ_INT(parse_args(external), 0);
    unsetenv("LLMGW_BACKEND_MAX_RESENDS");
    setenv("LLMGW_BACKEND_TIMEOUT_MS", "0", 1);
    CHECK_EQ_INT(parse_args(external), SERVER_EXIT_USAGE);
    unsetenv("LLMGW_BACKEND_TIMEOUT_MS");
    fflush(stderr);
    if (savedStderr >= 0) {
        dup2(savedStderr, STDERR_FILENO);
        close(savedStderr);
    }
}

/* ---- Allocation-failure sweeps --------------------------------------- */

/**
 * Runs @p op with the k-th allocation failing for k = 1, 2, ... until a run
 * completes without hitting the injected failure. Every failing run must
 * report failure; leaks are caught by ASan/LSan or Valgrind.
 * @return number of failure points exercised.
 */
static long sweep(const char* name, int (*op)(void))
{
    long points = 0;
    for (long k = 1; k < 10000; k++) {
        alloc_fail_reset(k);
        int ok = op();
        int injected = g_alloc_failed;
        alloc_fail_reset(0);
        if (!injected) {
            CHECK(ok);
            return points;
        }
        points++;
        if (ok) {
            fprintf(stderr, "%s: succeeded despite failing allocation %ld\n", name, k);
        }
        CHECK(!ok);
    }
    CHECK(0);
    return points;
}

static const char* const REQUEST_TEXT
        = "POST / HTTP/1.1\r\nHost: a\r\nX-1: 1\r\nX-2: 2\r\nContent-Length: 2\r\n\r\nhi";

static int op_request(void)
{
    /* Feed thread created before injection would also allocate; build the
     * socketpair synchronously instead. */
    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ssize_t w = write(fds[1], REQUEST_TEXT, strlen(REQUEST_TEXT));
    (void)w;
    shutdown(fds[1], SHUT_WR);
    Reader reader;
    reader_init(&reader, fds[0], -1);
    HttpRequestHead head;
    unsigned char* body = NULL;
    int ok = http_read_request_head(&reader, &head) == HTTP_OK;
    if (ok) {
        ok = http_read_body(&reader, 2, &body) == HTTP_OK;
        free(body);
        http_request_head_free(&head);
    }
    close(fds[0]);
    close(fds[1]);
    return ok;
}

static int op_response(void)
{
    const char* text = "HTTP/1.1 200 OK\r\nA: b\r\nC: d\r\nContent-Length: 5\r\n\r\nhello";
    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ssize_t w = write(fds[1], text, strlen(text));
    (void)w;
    shutdown(fds[1], SHUT_WR);
    Reader reader;
    reader_init(&reader, fds[0], -1);
    HttpResponse resp;
    int ok = http_read_response(&reader, 100, &resp) == HTTP_OK;
    if (ok) {
        http_response_free(&resp);
    }
    close(fds[0]);
    close(fds[1]);
    return ok;
}

static int op_json(void)
{
    const char* doc = "{\"x\":{\"k\":[1,2]},\"cont\\u0065nt\":\"a\\u00e9\\ud83d\\ude00b\","
                      "\"tokens_evaluated\":4}";
    CompletionReply reply;
    int ok = json_parse_completion(doc, strlen(doc), &reply) == JSON_OK;
    completion_reply_free(&reply);
    return ok;
}

static int op_history(void)
{
    History h;
    if (history_init(&h, 2) != 0) {
        return 0;
    }
    int ok = 1;
    for (int i = 0; i < 3 && ok; i++) {
        size_t before = history_count(&h);
        if (history_append(&h, "prompt\n", 7, "reply", 5) != 0) {
            CHECK_EQ_INT(history_count(&h), before); /* All-or-nothing. */
            ok = 0;
        }
    }
    Buf json;
    buf_init(&json);
    if (ok && history_build_completion_json(&json, &h, "now\"\n", 5) != 0) {
        ok = 0;
    }
    buf_free(&json);
    history_free(&h);
    return ok;
}

static int op_builders(void)
{
    Buf out;
    buf_init(&out);
    int ok = http_build_response(&out, 200, "text/plain", "x", 1, false) == 0
            && http_build_request(&out, "POST", "/completion", "localhost:1",
                       "application/json", "{}", 2)
                    == 0;
    buf_free(&out);
    return ok;
}

void test_alloc_failures(void)
{
    long total = sweep("request", op_request) + sweep("response", op_response)
            + sweep("json", op_json) + sweep("history", op_history)
            + sweep("builders", op_builders);
    CHECK(total > 20);
    printf("allocation-failure points exercised: %ld\n", total);
}
