# Requirements

This project is an independent extension of a university systems-programming
assignment (see [../PROVENANCE.md](../PROVENANCE.md)). This document restates
the requirements in the project's own words. It does not reproduce the
original specification. The requirements fall into three groups:

1. **Retained technical semantics**: behaviour kept from the original design.
2. **Deliberately replaced course coupling**: names, paths, libraries and
   deployment details that tied the code to one campus server.
3. **Not applicable**: grading and submission processes. These are not
   simulated and no records of them are fabricated.

Behaviour the original specification left to a reference implementation
(which is no longer available) is **defined here** and tested. It is not
claimed to match that reference.

## 1. Retained technical semantics

| Area | Requirement as implemented | Tests |
|---|---|---|
| Client CLI | Exactly one argument. Wrong count → usage, exit 8. Empty → exit 9. Arguments are validated before any network activity. | `test_client.ClientCli` |
| Client connect | localhost, numeric port or service name. Failure → exit 18. | `test_client.ClientCli` |
| Client loop | Prompt line → `POST /` whose body is the line plus `\n` (Content-Length in bytes). Waits for the full reply. 200 bodies go to stdout, other bodies to stderr, both after `Response:\n`. EOF → exit 0. | `test_client.ClientExchange` |
| Client failures | Malformed reply, EOF, send failure → exit 13. Never killed by SIGPIPE. All memory freed on every exit path. | `test_client.ClientFailures`, `test_valgrind` |
| Server CLI | `maxclients` 0..10000, `historysize` 1..10, optional `+` sign, port optional. Empty, extra or invalid arguments → exit 16. | `test_server_startup.ServerCli`, unit `test_config` |
| Listening | Loopback; port number or service name; `0`/absent = ephemeral. The actual number is printed on stderr after listening and **before** the backend starts. Failure → exit 9 with the port text in quotes. | `test_server_startup.Listeners`, `test_port_reported_before_backend_ready` |
| Stats listener | Only when `LLMGW_STATS_PORT` is set. Checked right after the main port. Failure → exit 4. | `test_server_startup.StatsListener` |
| Backend startup | Pipe, child process, stdout → pipe, stderr → `/dev/null`, read one port line, close the pipe, connect. EOF before the line → reap, exit 17. No client is accepted before the backend is ready. | `test_server_startup.BackendStartup` |
| Shared connection | One persistent backend connection used by all handlers, one transaction at a time under a mutex. | `test_concurrent_clients_share_one_serialised_backend` (max in-flight = 1, one backend connection) |
| Reconnect | Connect failure or EOF/malformed reply → reconnect with 25 consecutive retries and no delay, then resend the unanswered prompt. Exhaustion → SIGKILL and reap the child, exit 1. | `test_recovery.Reconnect` |
| Restart | Before each send, `waitid(..., WNOHANG)` on the child. A terminated child is reaped and replaced. No other condition causes a restart. | `test_recovery.Restart` |
| Request JSON | `POST /completion`, `Host: localhost:N`, `Content-Type: application/json`, body with the system block, history blocks in order, current prompt, empty assistant block, `"n_predict": 256`, `"cache_prompt": false`. `"`, `\`, LF, CR, TAB are escaped. | `test_prompt_round_trip_and_backend_request_format`, unit `test_history` |
| Reply JSON | Extract `content` and `tokens_evaluated`; ignore everything else. | unit `test_json_parse` |
| History | Per connection, N = number of interaction blocks. The oldest block is dropped once N is exceeded. | `test_server_http.History` |
| HTTP | Multiple requests per connection. GET `/` → home page (text/html). POST `/` → reply (text/plain). Unsupported method → 405, then bad path → 404, then body > 2048 → 413 `Input is too large: N bytes`. Correct Content-Type and byte-count Content-Length on every reply. | `test_server_http.*` |
| Robustness | Malformed requests or client disconnects affect only that client. The server never exits on SIGPIPE. | `test_server_http.InvalidRequests`, `test_client_disconnect_mid_transaction` |
| Connection limit | Clients above `maxclients` are not `accept()`ed until a slot frees, and are not counted while waiting. | `test_limits_stats.ConnectionLimit` |
| SIGHUP | Five counters written to stderr and flushed, collected without global variables or async-signal-unsafe code. | `test_limits_stats.Sighup` |
| Stats HTTP | `GET /stats` → the five lines. Other methods → 405, other paths → 404, malformed → close without reply. Keep-alive, concurrent stats clients, not limited or counted as prompt clients. | `test_limits_stats.StatsHttp` |
| Resources | No busy waiting, no growth in memory, descriptors or threads over time, one thread per connected client. | `test_resources.Stability`, `test_valgrind` |

### Statistics semantics (defined here)

- *Clients connected* / *Total connected clients* / *Total sessions
  completed* count prompt-port connections that a handler thread has started
  serving. Clients waiting behind the limit, clients whose thread could not
  be created, and stats-port clients are not counted. In a steady state,
  connected + completed = total.
- *Num prompts received* counts each `POST /` whose method and path are
  valid. It is counted **before** the 413 check, so oversized prompts count.
  Malformed requests, 404 and 405 do not count. A resend after reconnect does
  not count again.
- *Total tokens evaluated* adds `tokens_evaluated` from every successful (200
  + valid JSON) backend reply, even if the client disconnected before
  receiving it, because the backend did the work. A missing
  `tokens_evaluated` adds 0. A restarted backend does not reset the total.
- Counters are `uint64_t`, printed with `PRIu64`. A snapshot is copied under
  one lock acquisition.

### JSON, text and NUL policy (defined here)

- Prompt bytes are passed through as-is when they are valid UTF-8. Each byte
  of an invalid UTF-8 sequence becomes `�`. Control bytes other than
  the five named above and `\b` `\f` become `\u00XX`, including NUL.
- Replies must be valid UTF-8 JSON (at most 64 levels deep) with exactly one
  top-level string `content`. Nested `content`/`tokens_evaluated` keys and
  look-alikes inside strings are ignored. Escapes and surrogate pairs are
  decoded. A duplicate key of interest, a wrong type, a negative, fractional
  or overflowing token count, or malformed JSON → 502 `Invalid backend
  response`. These are not retried, because the HTTP framing was intact.
- `content` is length-tracked, so a decoded `\u0000` reaches the client as a
  NUL byte.

## 2. Deliberately replaced course coupling

| Original | Replacement | Reason |
|---|---|---|
| Course executable names and message prefixes | `llm-client`, `llm-server`; messages say "backend" instead of naming one model | Product identity. Output therefore cannot match the original byte for byte. |
| Course helper library (line reading, HTTP request/response parsing and construction) | Own `http.c`, `netio.c`, `buf.c` | The library's source was never available and was not redistributable. Its exact error behaviour is unknown, so the HTTP subset is defined in [architecture.md](architecture.md#http-subset). |
| Home page at a campus path | `assets/index.html` (new), configurable path, `500` if missing | Portable. The original allowed undefined behaviour if the file was missing. |
| Model command found on the campus `PATH` | Managed adapter contract plus `tools/llamacpp_backend.py`, or external `--backend-port` | The original command existed only on the course server. |
| `fork()` + `execlp()` | `posix_spawnp()` with file actions, CLOEXEC everywhere, process group | Avoids running library code between fork and exec in a threaded process, and makes it possible to kill the backend's helper processes. |
| Course-specific stats environment variable | `LLMGW_STATS_PORT`; other settings under the `LLMGW_` prefix | Naming |
| Welcome line containing a student number | `Welcome to llm-client.` | Privacy |
| Course build paths (`-I`/`-L` to campus directories) | Self-contained Makefile with `-MMD -MP` dependency files | Reproducible build |

## 3. Portfolio extensions and intentional differences

| Extension | Note |
|---|---|
| I/O deadlines (`poll`) for backend send/receive, startup, connect, client reply wait, and client writes | The course banned timeout functions. Here they bound every backend wait and stop a hung backend from blocking forever. |
| Resend budget and 502 replies | The original only defined fatal reconnect exhaustion. A reachable but broken backend now yields a 502 instead of an endless loop. |
| Graceful `SIGINT`/`SIGTERM` | The original left SIGINT at its default (immediate termination). Now: stop accepting, wake blocked I/O, join every thread, terminate and reap an owned backend, free everything, exit 0. |
| Joinable handler threads plus a reaper | Allows shutdown to prove every thread has finished (the original detached them). |
| Condition-variable slot gate | Equivalent to the semaphore, but shutdown can wake it. |
| `Connection: close` and HTTP/1.0 close semantics | Small HTTP correctness addition |
| Client exit 14 for local output failure | New status; the original did not specify this case |
| `LLMGW_DEBUG` diagnostics | Off by default, so default output is unchanged |
| Empty `LLMGW_STATS_PORT` is a listen failure (exit 4) | `getaddrinfo` would otherwise quietly pick a random port |
| When the stats port fails, the main port is not printed | The port line is printed only once both listeners succeed |
| Initialisation failure (pipe, mutex or thread creation at startup) | Exits 17 with `llm-server: unable to initialise`. Not part of the original. |

Not claimed: byte-exact compatibility with the original demo programs,
passing the course's hidden tests, compliance with the course's
implementation restrictions (banned functions, library restrictions,
50-line functions, no non-course libraries), or full HTTP/1.1 compliance.

## 4. Not applicable to the portfolio

Grades and marking formulas, Subversion commit counts and message rules,
the submission zip and its checksum, automated style marks from the course
style checker, Gradescope runs, interview procedures, and comparison with
demo binaries on the course server. None of these are simulated. They are
replaced by the public checks in [verification.md](verification.md):
warnings-clean builds, unit and integration tests, sanitizers and Valgrind.
