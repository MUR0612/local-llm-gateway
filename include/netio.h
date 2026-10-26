/** @file netio.h
 * @brief Loopback TCP helpers and deadline-aware, stop-aware socket I/O.
 *
 * Every blocking wait goes through poll() on the target descriptor plus an
 * optional "wake" descriptor (the read end of a stop pipe). Writing one byte
 * to the stop pipe therefore interrupts every waiter without signals,
 * pthread_cancel() or closing descriptors that other threads may still use.
 */
#ifndef LLMGW_NETIO_H
#define LLMGW_NETIO_H

#include <stddef.h>
#include <sys/types.h>

/** Result codes shared by the I/O helpers. */
typedef enum {
    IO_OK = 0,
    IO_EOF = 1,
    IO_ERROR = -1,
    IO_TIMEOUT = -2,
    IO_STOPPED = -3
} IoStatus;

/** Size of the per-connection read buffer. */
#define READER_BUF_SIZE 8192

/** Buffered reader over a socket descriptor. */
typedef struct {
    /** Socket to read from. */
    int fd;
    /** Stop-pipe read end, or -1 for none. */
    int wakeFd;
    /** Absolute CLOCK_MONOTONIC deadline in ms, or -1 for no deadline. */
    long long deadlineMs;
    /** Index of the first unread byte in buf. */
    size_t pos;
    /** Number of valid bytes in buf. */
    size_t len;
    /** Buffered bytes. */
    char buf[READER_BUF_SIZE];
} Reader;

/** Milliseconds from CLOCK_MONOTONIC. */
long long now_ms(void);

/** Converts a relative timeout (<0 = none) into an absolute deadline. */
long long deadline_after(long long timeoutMs);

/**
 * Waits until @p fd reports @p events, the wake descriptor becomes readable,
 * or the deadline passes. Retries EINTR.
 * @return IO_OK, IO_TIMEOUT, IO_STOPPED or IO_ERROR.
 */
IoStatus io_wait(int fd, short events, int wakeFd, long long deadlineMs);

/**
 * Opens a CLOEXEC TCP listener on localhost (IPv4 loopback) for a numeric
 * port or service name. "0" selects an ephemeral port.
 * @return listening descriptor, or -1.
 */
int net_listen_loopback(const char* port);

/** @return the bound port of a socket, or -1. */
int net_bound_port(int fd);

/**
 * Connects to localhost:@p port with a bounded wait. The returned socket is
 * CLOEXEC and in blocking mode.
 * @return descriptor >= 0, -1 on failure, or IO_STOPPED (-3) when woken.
 */
int net_connect_loopback(const char* port, int wakeFd, long long timeoutMs);

/**
 * Sends all bytes, retrying partial writes and EINTR, without raising
 * SIGPIPE. @return IO_OK, IO_ERROR, IO_TIMEOUT or IO_STOPPED.
 */
IoStatus net_send_all(int fd, const void* bytes, size_t len, int wakeFd,
        long long deadlineMs);

/** Prepares a reader for @p fd. */
void reader_init(Reader* reader, int fd, int wakeFd);

/** Drops buffered data (used after a connection is replaced). */
void reader_reset(Reader* reader, int fd);

/**
 * Reads more bytes into the buffer (compacting first).
 * @return IO_OK when at least one byte arrived, IO_EOF on orderly close,
 * otherwise IO_ERROR, IO_TIMEOUT or IO_STOPPED.
 */
IoStatus reader_fill(Reader* reader);

/** @return number of buffered, unread bytes. */
size_t reader_available(const Reader* reader);

/** Closes a descriptor if it is valid (>= 0), retrying nothing. */
void close_fd(int* fd);

#endif
