# Verification record

Status values: **PASS** means the check was actually executed and passed.
**NOT RUN** means it was not executed (the reason is given). No result here
comes from the original course's tests, and no claim is made about them.

## Environment and commands

- Date: 2026-10-26
- Host: AMD Ryzen 5 7535HS, Windows 11, WSL2 kernel 6.6.87.2-microsoft-standard-WSL2
- Userland: Ubuntu 26.04 LTS, GCC 15.2.0, glibc 2.43, GNU make, Python 3.14.4, Valgrind 3.26.0
- Backend in every run: `tests/mock_backend.py`, a scripted fixture. It is
  **not a language model.**
- Clean-tree run: the source was copied (without build outputs) to a new
  directory on the WSL ext4 filesystem, and these commands were run in order:

```sh
make EXTRA_CFLAGS=-Werror   # rc=0: no warnings with -Wall -Wextra -pedantic -std=gnu99
make test                   # rc=0: unit 370 checks / 0 failures; integration 107 tests OK, 0 skipped
make sanitize               # rc=0: ASan+UBSan unit 370/0; integration 107 OK, 5 skipped (fault injection)
make tsan                   # rc=0: TSan integration 107 OK, 5 skipped (fault injection)
make valgrind               # rc=0: 2 Memcheck tests OK; server "in use at exit: 0 bytes in 0 blocks"
make clean                  # removes only build/{release,asan,tsan}, the two binaries, and an empty build/
```

The `make test` suite also passed three times in a row in the development
tree. Logs from the clean-tree run are kept locally in `build/test-logs/`
and are not part of the deliverable.

Sanitizer runs: every server's stderr is scanned at teardown for
`ThreadSanitizer`, `AddressSanitizer`, `LeakSanitizer` and `runtime error:`,
and the test fails on any match. UBSan runs with
`-fno-sanitize-recover=undefined` and TSan with `halt_on_error=1`.

## Baseline reproduction (coursework version, before changes)

Some baseline modules were compiled privately, with a local stub for the
unavailable course helper library, and run under ASan. The harness is not
part of this project.

| Defect (static reading) | Dynamic result | Regression test in this project |
|---|---|---|
| History capacity `(N+1)/2` | `historysize=5 → capacity=3` (reproduced) | unit `test_history` checks `capacity == N` for 1/5/10; `test_server_http.History` |
| Escaped backslash before the closing quote | `"ends with backslash \\"` extracted as `ends with backslash \",` (reproduced) | unit `test_json_parse` (`ends \\`, `\\\\\\"`) |
| `\uXXXX` not decoded | `café 😀` → `cafu00e9 ud83dude00` (reproduced) | unit `test_json_parse` (surrogate pairs, CJK) |
| `strstr` key lookup matches nested keys | nested `tokens_evaluated: 999` returned instead of the top-level 25 (reproduced) | unit `test_json_parse` (nested and in-string look-alikes); mock replies always contain a nested decoy |
| No SIGPIPE protection in the server | not reproduced dynamically (needs the full baseline server) | `test_client_disconnect_mid_transaction`, `test_client_that_never_reads_does_not_block_others` |
| Unchecked malloc/realloc in history, unchecked `fdopen`/`dup`/`sem_init` | static only | unit allocation-failure sweep (45 failure points); `dup`/`fdopen`/semaphores no longer used |
| `accept` result tested with `> 1`; `write_all` not retrying EINTR | static only | `net_send_all` retries EINTR/EAGAIN; the accept loop treats `fd >= 0` as valid |
| Course library, campus home page and model command required | static only (the baseline cannot build off campus) | clean-tree build and tests above |

## The 21 functional categories

| # | Category | Status | Evidence (test module / class or test) |
|---|---|---|---|
| 1 | Client invalid CLI: missing, extra, empty port | PASS | `test_client.ClientCli` (exit 8/9, exact stderr) |
| 2 | Client connect success/failure, numeric and service port | PASS | `test_connect_failure`, `test_unknown_service_name`, `test_connect_by_service_name` (uses a free `/etc/services` entry ≥ 1024) |
| 3 | stdin loop, 200 vs non-200 output, several prompts on one connection | PASS | `test_client.ClientExchange` (byte lengths, UTF-8, NUL, unterminated last line, waits for reply) |
| 4 | Broken pipe, malformed reply, disconnect, timeout | PASS | `test_client.ClientFailures` (exit 13, never -SIGPIPE; exit 14 on `/dev/full`) |
| 5 | Client ownership/leaks on normal and error exits | PASS | `test_valgrind.ClientMemory`: exits 0/8/9/18, "All heap blocks were freed", 3 fds at exit |
| 6 | Server CLI: ranges, `+`, empty, missing, overflow, trailing junk, spaces, negatives | PASS | `test_server_startup.ServerCli` (20 invalid forms), unit `test_config` |
| 7 | Main listener: explicit/ephemeral/service port, reported port, bind failure | PASS | `test_server_startup.Listeners`; port reported before the backend (`test_port_reported_before_backend_ready`); loopback-only bind. Failure to listen on an ephemeral port: NOT RUN (cannot be provoked) |
| 8 | Managed startup: success, exec failure, EOF before ready, timeout, bad line, connect failure | PASS | `test_server_startup.BackendStartup` (child reaped in every failure case; exactly 26 connect attempts) |
| 9 | Single client GET/POST | PASS | `test_server_http.SingleClient` (home page bytes, exact `/completion` request and JSON) |
| 10 | 404/405/413, order, byte lengths | PASS | `test_server_http.InvalidRequests` (2047/2048/2049; 404 before 413; 405 before 404; huge Content-Length not allocated; 12 malformed forms closed silently) |
| 11 | Sequential clients | PASS | `test_sequential_clients`, `History.test_history_is_per_connection` |
| 12 | Concurrent clients, serialised backend, isolated history | PASS | `test_concurrent_clients_share_one_serialised_backend` (8×3 prompts, in-flight max 1, one backend connection, no cross-client history) |
| 13 | Disconnect/write failure isolation | PASS | `test_client_disconnect_mid_transaction`, `test_client_that_never_reads_does_not_block_others`, `test_early_eof_in_body` |
| 14 | Connection limit and slot recovery; waiters not counted | PASS | `test_limits_stats.ConnectionLimit`; `FaultInjection.test_handler_thread_creation_failure` (slot released) |
| 15 | SIGHUP snapshot, repeated signals, five counters | PASS | `test_limits_stats.Sighup` (exact values; 20 SIGHUPs under load, connected + completed = total, monotonic) |
| 16 | Stats env unset/set/invalid/empty/in use | PASS | `test_server_startup.StatsListener` (checks the listening sockets in `/proc`) |
| 17 | Single stats client: valid, wrong method, wrong path, malformed | PASS | `test_limits_stats.StatsHttp.test_single_stats_client`, `test_stats_disconnect_mid_request` |
| 18 | Many stats clients, keep-alive, no prompt quota | PASS | `test_many_stats_clients_keepalive_and_no_quota`, `test_stats_answer_while_backend_is_stuck` |
| 19 | Backend EOF/malformed → reconnect + resend; 25 retries exhausted | PASS | `test_recovery.Reconnect` (same JSON resent on a new connection, counted once; 26 attempts then exit 1 with the child reaped; external backend never signalled) |
| 20 | Child death detection, reap, one replacement, concurrent recovery | PASS | `test_recovery.Restart` (zombie confirmed via `/proc` before the prompt; generations 0→1; tokens not reset; 6 concurrent clients → exactly 2 starts; failed replacement → exit 17) |
| 21 | No growth in heap/fd/thread over cycles; no busy loop when idle | PASS | `test_resources.Stability` (150 measured connect/prompt/GET/stats cycles after 30 warm-up cycles: fd count equal, threads back to base, RSS growth < 1 MiB; 5 restarts with fds equal; ≤ 5 CPU ticks over 2 s idle with blocked clients); `test_valgrind.ServerMemory` (0 bytes in use, 3 fds at exit) |

## Additional reliability checks

| Check | Status | Evidence |
|---|---|---|
| JSON: quote, backslash runs, `\n \r \t \b \f`, other controls, `\u0000`, surrogate pairs, lone surrogates, CJK, invalid UTF-8, missing/wrong-type fields, duplicates, trailing data, depth 64/65 | PASS | unit `test_json_encode`, `test_json_parse` (over 40 documents and byte strings) |
| Header/body split byte by byte; two requests in one TCP write | PASS | unit `test_pipelining_and_bodies`; `test_request_split_across_writes`, `test_pipelined_requests_in_one_write` |
| Overflowing / conflicting Content-Length, Transfer-Encoding, oversized lines/headers | PASS | unit `test_requests`; `InvalidRequests.test_malformed_requests_close_silently` |
| historysize 1/5/10 eviction; failed request does not pollute history | PASS | `test_server_http.History` |
| Backend timeout does not block stats; permanent garbage does not retry forever | PASS | `test_stats_answer_while_backend_is_stuck`; `test_persistent_garbage_hits_resend_budget` (1 + 3 attempts, then 502) |
| Unusable replies (bad JSON, 500, no content, wrong types) → 502 without retry | PASS | `test_bad_replies_are_not_retried` |
| Allocation-failure cleanup (request/response parsers, JSON, history, builders) | PASS | unit `test_alloc_failures`: 45 injected failure points, each reports failure, no leaks under ASan/LSan |
| Fault injection: `pthread_create` for a handler, `accept4` EMFILE, `posix_spawnp`, spawn `pipe2`, restart spawn | PASS (plain build only) | `test_resources.FaultInjection` via `tests/faultinject.c` (LD_PRELOAD) |
| `dup`/`fdopen` failure injection | N/A | The new code does not use `dup`/`fdopen`/`FILE*` on sockets |
| Allocation failure inside the running server process (`server.c`, `backend.c` paths) | NOT RUN | Only the library modules are swept. The server's own small allocations are not fault-injected. |
| Graceful shutdown: idle clients, in-flight transaction, stats client, startup in progress, waiting client, managed vs external ownership | PASS | `test_resources.Shutdown`, `test_external_shutdown_leaves_backend_running` |
| llama.cpp adapter wiring (handshake, `/completion`, process-group shutdown) | PASS (fake server) | `test_adapter` with `tests/fake_llama_server.py`, which mimics `llama-server` flags and `/health`. It is not a model. |
| Headers compile on their own | PASS | each `include/*.h` with `gcc -fsyntax-only` (`common.h` contains only macros, so on its own it only produces the pedantic empty-unit note) |

## Not run / not verified

| Item | Reason |
|---|---|
| Real model inference (llama.cpp + GGUF) | Would need a model download and runtime; deliberately not done. The adapter is checked only against documentation and a fake server. |
| CI workflow (`.github/workflows/ci.yml`) | No remote repository exists. The file has never been executed. |
| Other compilers (Clang), other distributions, non-WSL Linux | Not available in this environment |
| Native Windows | Not supported (POSIX APIs). WSL2 is the supported route on Windows. |
| Performance / latency benchmarks | Not performed. No latency figures are claimed. |
| 10000 concurrent clients | Not tested. Concurrency was tested with up to 30 simultaneous connections. |
| Original course public/hidden tests, demo binary comparison | Not applicable (see requirements.md §4) |
