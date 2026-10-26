/** @file test_basic.c
 * @brief Unit tests for Buf, numeric parsing, JSON and history.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "history.h"
#include "json.h"
#include "numparse.h"
#include "stats.h"
#include "testlib.h"

void test_buf_numparse(void)
{
    Buf b;
    buf_init(&b);
    CHECK_EQ_INT(buf_append(&b, "a\0b", 3), 0);
    CHECK_EQ_INT(buf_printf(&b, "%d-%s", 42, "x"), 0);
    CHECK_MEM(b.data, b.len, "a\0b42-x", 7);
    CHECK_EQ_INT(b.data[b.len], '\0');
    CHECK_EQ_INT(buf_reserve(&b, SIZE_MAX), -1);
    CHECK_EQ_INT(b.len, 7);
    buf_free(&b);

    struct {
        const char* text;
        unsigned long min, max;
        int ok;
        unsigned long value;
    } cases[] = {{"0", 0, 10000, 1, 0}, {"10000", 0, 10000, 1, 10000},
            {"10001", 0, 10000, 0, 0}, {"+5", 0, 10, 1, 5}, {"+", 0, 10, 0, 0},
            {"", 0, 10, 0, 0}, {"-1", 0, 10, 0, 0}, {" 1", 0, 10, 0, 0},
            {"1 ", 0, 10, 0, 0}, {"1x", 0, 10, 0, 0}, {"0x1", 0, 10, 0, 0},
            {"+-1", 0, 10, 0, 0}, {"007", 0, 10, 1, 7}, {"0", 1, 10, 0, 0},
            {"11", 1, 10, 0, 0},
            {"999999999999999999999999999", 0, (unsigned long)-1, 0, 0}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned long v = 12345;
        int ok = parse_bounded_uint(cases[i].text, cases[i].min, cases[i].max, &v);
        CHECK_EQ_INT(ok, cases[i].ok);
        if (ok) {
            CHECK_EQ_INT(v, cases[i].value);
        }
    }

    StatsSnapshot snap = {3, 9, 6, 20, UINT64_C(5000000000)};
    Buf text;
    buf_init(&text);
    CHECK_EQ_INT(stats_format(&snap, &text), 0);
    CHECK_STR(text.data,
            "Clients connected: 3\nTotal connected clients: 9\n"
            "Total sessions completed: 6\nNum prompts received: 20\n"
            "Total tokens evaluated: 5000000000\n");
    buf_free(&text);
}

static void check_escape(const char* in, size_t inLen, const char* expected)
{
    Buf out;
    buf_init(&out);
    CHECK_EQ_INT(json_append_escaped(&out, in, inLen), 0);
    CHECK_MEM(out.data == NULL ? "" : out.data, out.len, expected, strlen(expected));
    buf_free(&out);
}

static void check_parse(const char* doc, JsonStatus status, const char* content,
        size_t contentLen, int hasTokens, unsigned long long tokens)
{
    CompletionReply reply;
    JsonStatus rc = json_parse_completion(doc, strlen(doc), &reply);
    CHECK_EQ_INT(rc, status);
    if (rc != status) {
        fprintf(stderr, "  document: %s\n", doc);
    }
    if (rc == JSON_OK && status == JSON_OK) {
        CHECK_MEM(reply.content, reply.contentLen, content, contentLen);
        CHECK_EQ_INT(reply.hasTokens, hasTokens);
        CHECK_EQ_INT(reply.tokensEvaluated, tokens);
    }
    completion_reply_free(&reply);
}

/** Parses {"content":"x","a":[[...]]} with @p arrays nested arrays. */
static void check_depth(int arrays, JsonStatus status)
{
    char doc[512];
    size_t n = (size_t)sprintf(doc, "{\"content\":\"x\",\"a\":");
    for (int i = 0; i < arrays; i++) {
        doc[n++] = '[';
    }
    for (int i = 0; i < arrays; i++) {
        doc[n++] = ']';
    }
    doc[n++] = '}';
    doc[n] = '\0';
    check_parse(doc, status, "x", 1, 0, 0);
}

static void test_json_encode(void)
{
    check_escape("a\"b\\c\nd\re\tf", 11, "a\\\"b\\\\c\\nd\\re\\tf");
    check_escape("\b\f\x01\x1f\x7f", 5, "\\b\\f\\u0001\\u001f\x7f");
    check_escape("x\0y", 3, "x\\u0000y");
    check_escape("\xe4\xb8\xad\xe6\x96\x87", 6, "\xe4\xb8\xad\xe6\x96\x87");
    check_escape("\xf0\x9f\x98\x80", 4, "\xf0\x9f\x98\x80");
    check_escape("\xff", 1, "\\ufffd");
    check_escape("\xc0\x80", 2, "\\ufffd\\ufffd");        /* Overlong NUL. */
    check_escape("\xed\xa0\x80", 3, "\\ufffd\\ufffd\\ufffd"); /* Surrogate. */
    check_escape("\xe4\xb8", 2, "\\ufffd\\ufffd");        /* Truncated. */
    check_escape("\\\\\"", 3, "\\\\\\\\\\\"");
    check_escape("", 0, "");
}

static void test_json_parse(void)
{
    check_parse("{\"content\":\"hi\",\"tokens_evaluated\":25}", JSON_OK, "hi", 2, 1, 25);
    check_parse(" \n{ \"content\" : \"\" } \r\n", JSON_OK, "", 0, 0, 0);
    /* Nested and in-string look-alike keys must be ignored. */
    check_parse("{\"generation_settings\":{\"content\":\"FAKE\",\"tokens_evaluated\":999},"
                "\"prompt\":\"\\\"content\\\":\\\"FAKE2\\\"\",\"content\":\"REAL\","
                "\"tokens_evaluated\":7}",
            JSON_OK, "REAL", 4, 1, 7);
    check_parse("{\"content\":\"ends \\\\\",\"tokens_evaluated\":3}", JSON_OK,
            "ends \\", 6, 1, 3);
    check_parse("{\"content\":\"\\\\\\\\\\\"\"}", JSON_OK, "\\\\\"", 3, 0, 0);
    check_parse("{\"content\":\"caf\\u00e9 \\ud83d\\ude00 \\u4e2d\"}", JSON_OK,
            "caf\xc3\xa9 \xf0\x9f\x98\x80 \xe4\xb8\xad", 14, 0, 0);
    check_parse("{\"content\":\"\\n\\r\\t\\b\\f\\/\"}", JSON_OK, "\n\r\t\b\f/", 6, 0, 0);
    check_parse("{\"content\":\"a\\u0000b\"}", JSON_OK, "a\0b", 3, 0, 0);
    check_parse("{\"content\":\"\xe4\xb8\xad\xe6\x96\x87\"}", JSON_OK,
            "\xe4\xb8\xad\xe6\x96\x87", 6, 0, 0);
    check_parse("{\"cont\\u0065nt\":\"escaped key\"}", JSON_OK, "escaped key", 11, 0, 0);
    check_parse("{\"content\":\"x\",\"tokens_evaluated\":0,\"a\":[1,-2.5e+3,true,false,null,{}]}",
            JSON_OK, "x", 1, 1, 0);
    check_parse("{\"content\":\"x\",\"tokens_evaluated\":18446744073709551615}",
            JSON_OK, "x", 1, 1, 18446744073709551615ULL);

    check_parse("{\"content\":\"\\ud83d\"}", JSON_SYNTAX, NULL, 0, 0, 0);
    check_parse("{\"content\":\"\\ude00\"}", JSON_SYNTAX, NULL, 0, 0, 0);
    check_parse("{\"content\":\"\\ud83d\\u0041\"}", JSON_SYNTAX, NULL, 0, 0, 0);
    check_parse("{\"content\":\"\\x\"}", JSON_SYNTAX, NULL, 0, 0, 0);
    check_parse("{\"content\":\"\\u12g4\"}", JSON_SYNTAX, NULL, 0, 0, 0);
    check_parse("{\"content\":\"raw\nnewline\"}", JSON_SYNTAX, NULL, 0, 0, 0);
    check_parse("{\"content\":\"bad \xff utf8\"}", JSON_SYNTAX, NULL, 0, 0, 0);
    check_parse("{\"content\":\"unterminated}", JSON_SYNTAX, NULL, 0, 0, 0);
    check_parse("{\"content\":\"x\"} trailing", JSON_SYNTAX, NULL, 0, 0, 0);
    check_parse("{\"content\":\"x\",}", JSON_SYNTAX, NULL, 0, 0, 0);
    check_parse("{\"a\":01,\"content\":\"x\"}", JSON_SYNTAX, NULL, 0, 0, 0);
    check_parse("{\"a\":tru,\"content\":\"x\"}", JSON_SYNTAX, NULL, 0, 0, 0);
    check_parse("", JSON_SYNTAX, NULL, 0, 0, 0);
    check_parse("[\"content\"]", JSON_NOT_OBJECT, NULL, 0, 0, 0);
    check_parse("{\"tokens_evaluated\":5}", JSON_MISSING_CONTENT, NULL, 0, 0, 0);
    check_parse("{\"content\":5}", JSON_BAD_TYPE, NULL, 0, 0, 0);
    check_parse("{\"content\":null}", JSON_BAD_TYPE, NULL, 0, 0, 0);
    check_parse("{\"content\":\"x\",\"tokens_evaluated\":2.5}", JSON_BAD_TYPE, NULL, 0, 0, 0);
    check_parse("{\"content\":\"x\",\"tokens_evaluated\":-1}", JSON_BAD_TYPE, NULL, 0, 0, 0);
    check_parse("{\"content\":\"x\",\"tokens_evaluated\":1e2}", JSON_BAD_TYPE, NULL, 0, 0, 0);
    check_parse("{\"content\":\"x\",\"tokens_evaluated\":\"7\"}", JSON_BAD_TYPE, NULL, 0, 0, 0);
    check_parse("{\"content\":\"x\",\"tokens_evaluated\":18446744073709551616}",
            JSON_BAD_TYPE, NULL, 0, 0, 0);
    check_parse("{\"content\":\"x\",\"content\":\"y\"}", JSON_DUPLICATE, NULL, 0, 0, 0);

    /* Depth limit: the top-level object counts as depth 1. */
    check_depth(JSON_MAX_DEPTH - 1, JSON_OK);
    check_depth(JSON_MAX_DEPTH, JSON_TOO_DEEP);
}

void test_json(void)
{
    test_json_encode();
    test_json_parse();
}

void test_history(void)
{
    History h;
    CHECK_EQ_INT(history_init(&h, 0), -1);
    CHECK_EQ_INT(history_init(&h, 11), -1);

    size_t sizes[] = {1, 5, 10};
    for (size_t s = 0; s < 3; s++) {
        CHECK_EQ_INT(history_init(&h, sizes[s]), 0);
        CHECK_EQ_INT(h.capacity, sizes[s]); /* Regression: was (N + 1) / 2. */
        for (int i = 0; i < 13; i++) {
            char p[16];
            char r[16];
            int pl = sprintf(p, "p%d\n", i);
            int rl = sprintf(r, "r%d", i);
            CHECK_EQ_INT(history_append(&h, p, (size_t)pl, r, (size_t)rl), 0);
        }
        CHECK_EQ_INT(history_count(&h), sizes[s]);
        for (size_t i = 0; i < sizes[s]; i++) {
            char expected[16];
            int el = sprintf(expected, "p%zu\n", 13 - sizes[s] + i);
            const Interaction* item = history_get(&h, i);
            CHECK_MEM(item->prompt, item->promptLen, expected, (size_t)el);
        }
        CHECK(history_get(&h, sizes[s]) == NULL);
        history_free(&h);
    }

    CHECK_EQ_INT(history_init(&h, 2), 0);
    const char* first = "Client Prompt 1 containing \" \\ \n \r and \t characters\n";
    history_append(&h, first, strlen(first), "LLM Response 1", 14);
    history_append(&h, "Client Prompt 2\n", 16, "LLM Response 2", 14);
    Buf json;
    buf_init(&json);
    CHECK_EQ_INT(history_build_completion_json(&json, &h, "Client Prompt 3\n", 16), 0);
    CHECK_STR(json.data,
            "{\"prompt\": \"<|im_start|>system\\nYou are a helpful assistant.<|im_end|>\\n"
            "<|im_start|>user\\nClient Prompt 1 containing \\\" \\\\ \\n \\r and \\t "
            "characters\\n<|im_end|>\\n<|im_start|>assistant\\nLLM Response 1<|im_end|>\\n"
            "<|im_start|>user\\nClient Prompt 2\\n<|im_end|>\\n<|im_start|>assistant\\n"
            "LLM Response 2<|im_end|>\\n<|im_start|>user\\nClient Prompt 3\\n<|im_end|>\\n"
            "<|im_start|>assistant\\n\",\"n_predict\": 256,\"cache_prompt\": false}");
    buf_free(&json);
    history_free(&h);
}
