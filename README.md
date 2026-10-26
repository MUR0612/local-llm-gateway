# Local LLM Gateway

A small multithreaded HTTP gateway written in C (POSIX threads and sockets).
It lets several local clients chat with one locally hosted language-model
server. Every client has its own rolling chat history, while all clients share
one persistent connection to the model backend.

The gateway can **start and supervise** the backend process itself, or connect
to a backend that you run separately. It detects backend failures, reconnects,
resends the pending prompt and restarts a crashed backend that it owns.

```
 llm-client ──┐
 browser/curl ─┼── HTTP/1.1 ──▶ llm-server ── POST /completion (one shared connection) ──▶ model backend
 stats client ─┘   (loopback)   │  one thread per client,                                  (llama.cpp via adapter,
                                │  per-client history, statistics                           or any compatible server)
```

## Features

- **Per-client history**: the last N completed prompt/response pairs
  (`historysize`, 1..10) are sent with each new prompt.
- **Serialised shared backend**: one persistent connection, one transaction at
  a time, guarded by a mutex. Clients are concurrent; inference is not parallel.
- **Backend supervision** (managed mode): started with `posix_spawnp`, a
  ready-port handshake, startup timeout, child-death detection, process-group
  cleanup, and replacement of a crashed child. External mode never signals or
  restarts your process.
- **Recovery**: connection errors trigger up to 25 immediate reconnect
  retries, and the unanswered prompt is resent. A resend budget stops endless
  loops against a backend that keeps returning bad data.
- **Connection limit** (`maxclients`): extra clients wait in the kernel
  backlog and are not counted until they are accepted.
- **Statistics**: five counters on `SIGHUP` (stderr) and optionally over HTTP
  (`GET /stats`) on a separate port.
- **Graceful shutdown** on `SIGINT`/`SIGTERM`: wakes all blocked I/O, joins all
  threads, stops an owned backend and exits 0.
- **Own HTTP/1.1 subset and JSON parser** with no third-party dependencies.

## Limitations

- **Loopback only, no authentication, no TLS.** Not an internet-facing service.
- **Inference is serialised.** Many clients can connect, but the backend sees
  one request at a time.
- **At-least-once resend.** After a connection failure the same prompt may be
  processed twice by the backend. History and statistics are committed once.
- **HTTP/1.1 subset.** Content-Length framing, keep-alive and pipelining only.
  Chunked encoding and ambiguous framing are rejected. Not a full RFC 9112
  implementation.
- **Linux/glibc only** (uses `accept4`, `pipe2`, `posix_spawnp`, `/proc`). On
  Windows, use WSL2.
- **Not tested with a real model.** All automated tests use a scripted mock
  backend. The llama.cpp adapter follows the llama.cpp server documentation
  and is tested only against a fake stand-in.
- `maxclients` accepts up to 10000, but that is a configuration bound, not a
  tested capacity. No performance or latency numbers are claimed.

## Quickstart

Requirements: GCC (GNU99), GNU make and Python 3 (standard library only, for
tests and the adapter). Valgrind is optional.

```sh
make          # builds ./llm-client and ./llm-server
make test     # unit + integration tests; needs no model, GPU or network
```

### Try it without a model

The test mock echoes your prompt. It is a fixture, not an LLM.

```sh
echo '{}' > /tmp/plan.json
./llm-server --backend-cmd python3 \
    --backend-arg tests/mock_backend.py --backend-arg --plan --backend-arg /tmp/plan.json \
    --backend-arg --log --backend-arg /tmp/mock.jsonl \
    4 5 &                      # prints the listening port on stderr
./llm-client <port>            # type prompts; Ctrl-D to quit
```

### With llama.cpp (not verified against a real model)

[`tools/llamacpp_backend.py`](tools/llamacpp_backend.py) starts `llama-server`
on a free loopback port, waits for `GET /health` to return 200 and reports the
port to the gateway.

```sh
./llm-server --backend-cmd python3 --backend-arg tools/llamacpp_backend.py \
    --backend-arg --model --backend-arg /path/to/model.gguf 4 5
```

The prompt uses the ChatML-style `<|im_start|>` template (suited to
SmolLM2-Instruct). Other models may need a different template.

### Your own backend

Start any compatible server on a loopback port and run
`llm-server --backend-port PORT 4 5`. It must accept `POST /completion` with
`prompt`, `n_predict` and `cache_prompt`, and answer with JSON containing
`content` (string) and `tokens_evaluated` (non-negative integer).

## Command line

```
llm-server [options] maxclients historysize [port]
llm-client port
```

| Argument | Meaning |
|---|---|
| `maxclients` | 0..10000 prompt clients served at once. `0` = no application limit. |
| `historysize` | 1..10 interaction blocks (prompt + reply) kept per connection. |
| `port` | Loopback port or service name. Omitted or `0` = ephemeral; the actual port is printed on stderr. |

Numbers accept an optional leading `+`. Empty strings, whitespace, `-`,
trailing characters and overflow are usage errors.

### Configuration

Precedence: command-line option > environment variable > default.

| Option / variable | Default | Purpose |
|---|---|---|
| `--backend-cmd PROG` / `LLMGW_BACKEND_CMD` | `llmgw-backend` (in `PATH`) | Managed backend executable (run directly, no shell) |
| `--backend-arg ARG` (repeatable) | none | Arguments for the managed backend |
| `--backend-port PORT` / `LLMGW_BACKEND_PORT` | unset | External backend; exclusive with a backend command |
| `--home-page PATH` / `LLMGW_HOME_PAGE` | `<executable dir>/assets/index.html` | File served for `GET /` |
| `LLMGW_STATS_PORT` | unset | Enables the statistics listener |
| `LLMGW_BACKEND_TIMEOUT_MS` | 120000 | Deadline to send a request and read its response |
| `LLMGW_BACKEND_STARTUP_TIMEOUT_MS` | 60000 | Deadline for the managed backend's ready line |
| `LLMGW_BACKEND_CONNECT_TIMEOUT_MS` | 5000 | Deadline per connect attempt |
| `LLMGW_BACKEND_MAX_RESENDS` | 3 | Resends per prompt after transport failures (0..100) |
| `LLMGW_DEBUG` | 0 | `1` prints diagnostics on stderr |
| `LLMGW_CLIENT_TIMEOUT_MS` (client) | 600000 | Client wait for each reply |

**Managed backend contract.** The gateway runs the program in its own process
group with stdin and stderr on `/dev/null` and stdout on a pipe. Within the
startup timeout it must print its loopback port as one decimal line (`8123\n`)
and never write to stdout again. EOF before that line, or a value outside
1..65535, counts as a startup failure.

### Exit statuses

| Program | Status | Meaning |
|---|---|---|
| client | 0 | EOF on stdin |
| client | 8 | Wrong argument count, or invalid `LLMGW_CLIENT_TIMEOUT_MS` |
| client | 9 | Empty port |
| client | 13 | Send failure, timeout, EOF or malformed reply |
| client | 14 | Writing output failed |
| client | 18 | Cannot connect |
| server | 0 | Graceful shutdown (`SIGINT`/`SIGTERM`) |
| server | 1 | Backend unreachable after 1 + 25 connect attempts |
| server | 4 | Cannot listen on `LLMGW_STATS_PORT` |
| server | 9 | Cannot listen on the main port |
| server | 16 | Usage error |
| server | 17 | Managed backend could not be started (including a failed restart) |

## HTTP interface

Prompt port:

| Request | Response |
|---|---|
| `GET /` | `200 text/html` home page (`500` if the file is unreadable) |
| `POST /`, body ≤ 2048 bytes including the trailing newline | `200 text/plain`: backend `content` + `\n` |
| `POST /`, body > 2048 bytes | `413`, `Input is too large: N bytes` |
| Other method | `405` (checked before the path) |
| Other path | `404` (checked before the size) |
| Backend reply unusable / resends exhausted | `502` |
| Malformed request, unsupported framing, early EOF | connection closed, no reply |

Statistics (`LLMGW_STATS_PORT` set): `GET /stats` returns five lines; other
methods get `405`, other paths `404`. `kill -HUP <pid>` writes the same lines
to stderr:

```
Clients connected: 1
Total connected clients: 3
Total sessions completed: 2
Num prompts received: 7
Total tokens evaluated: 412
```

## Testing

| Target | Runs |
|---|---|
| `make test` | 370 C unit checks (HTTP, JSON, history, config, allocation-failure sweep) and 107 integration tests against a scripted mock backend, including LD_PRELOAD fault injection |
| `make sanitize` | The same suites under ASan and UBSan |
| `make tsan` | The integration suite under ThreadSanitizer |
| `make valgrind` | Memcheck on client exit paths and a server workload ending in graceful shutdown |

All of these pass on Ubuntu 26.04 (WSL2), GCC 15.2, glibc 2.43, Python 3.14,
Valgrind 3.26. Other platforms and compilers are untested; the CI workflow in
`.github/workflows/ci.yml` has not been run yet. Details and items not run:
[docs/verification.md](docs/verification.md).

## Documentation

- [Architecture](docs/architecture.md): threads, locks, ownership, recovery state machine, shutdown
- [Requirements](docs/requirements.md): behaviour definitions, statistics and JSON/NUL policy
- [Verification](docs/verification.md): what was run, results, what was not
- [Provenance](PROVENANCE.md): where the project came from

## Project layout

```
src/ include/   C sources and headers
assets/         home page served for GET /
tests/          unit tests (C), integration tests (Python), mock backend, fault injection
tools/          llama.cpp adapter
docs/           documentation
```

## License

MIT. Copyright (c) 2026 Po-Han Wu. See [LICENSE](LICENSE).
