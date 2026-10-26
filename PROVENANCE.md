# Provenance

This project started as the author's own solution to a networking and
multithreading assignment in a university systems-programming course. That
assignment asked for a client and a multithreaded HTTP server that forwarded
prompts to a small locally run language model. This repository is a
separate, later extension of that work. It is **not** a course submission,
and it is not presented as having been written independently of the course.

## What came from the coursework version

- The overall design: one client per terminal, one handler thread per
  connection, a single shared backend connection protected by a mutex,
  per-client rolling chat history, a managed model child process whose port
  is read from a pipe, SIGHUP statistics through a dedicated `sigwait`
  thread, and connection limiting before `accept()`.
- The client and server command-line semantics, the request dispatch order
  (405 → 404 → 413), the five statistics counters, and the exit-status
  numbers.
- Parts of the code structure of the client loop, the chat-template JSON
  builder and argument validation. The original socket and threading
  patterns were themselves adapted from course lecture examples.

## What is new in this version

Written after the course:

- An HTTP/1.1 subset parser and serialiser and a buffered reader, replacing
  the course-provided helper library, which is neither included nor needed.
- A validating JSON parser and UTF-8-aware encoder (replacing `strstr`-based
  extraction).
- A portable backend layer: managed mode via `posix_spawnp` with a
  ready-port contract, an external mode, I/O deadlines, a reconnect/resend
  policy with a resend budget, child-death detection with process-group
  cleanup, and a llama.cpp adapter.
- An HTTP statistics listener, graceful SIGINT/SIGTERM shutdown, joinable
  handler threads with a reaper, and a condition-variable slot gate.
- Fixes for defects found in the coursework version (history capacity,
  JSON escapes, unchecked allocations, server SIGPIPE, `accept` result check,
  EINTR).
- A new home page, Makefile, unit tests, mock backend, integration suite,
  sanitizer/Valgrind targets and documentation.

## Not included

The course specification, course-provided headers, libraries, home page,
test scripts and demo programs, submission archives, version-control
history from the course repository, and the author's private course notes.
None of these are needed to build or test this project.
