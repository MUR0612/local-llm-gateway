/** @file netio.c
 * @brief Loopback TCP helpers and deadline-aware, stop-aware socket I/O.
 *
 * Address resolution and the per-candidate socket()/bind() and socket()/
 * connect() loops follow the examples in the getaddrinfo(3) man page.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "netio.h"

#define LOOPBACK_HOST "localhost"

long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

long long deadline_after(long long timeoutMs)
{
    return timeoutMs < 0 ? -1 : now_ms() + timeoutMs;
}

IoStatus io_wait(int fd, short events, int wakeFd, long long deadlineMs)
{
    for (;;) {
        struct pollfd fds[2];
        nfds_t count = 1;
        fds[0].fd = fd;
        fds[0].events = events;
        fds[0].revents = 0;
        if (wakeFd >= 0) {
            fds[1].fd = wakeFd;
            fds[1].events = POLLIN;
            fds[1].revents = 0;
            count = 2;
        }
        int timeout = -1;
        if (deadlineMs >= 0) {
            long long left = deadlineMs - now_ms();
            if (left <= 0) {
                return IO_TIMEOUT;
            }
            timeout = left > 1000000000LL ? 1000000000 : (int)left;
        }
        int rc = poll(fds, count, timeout);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return IO_ERROR;
        }
        if (count == 2 && fds[1].revents != 0) {
            return IO_STOPPED;
        }
        if (rc == 0) {
            continue; /* Re-evaluates the deadline. */
        }
        if (fds[0].revents & POLLNVAL) {
            return IO_ERROR;
        }
        /* POLLERR/POLLHUP are reported as ready: the next read()/send()
         * returns the precise condition (EOF or error). */
        return IO_OK;
    }
}

/** Resolves localhost:@p port for IPv4 TCP. */
static struct addrinfo* resolve_loopback(const char* port, int passive)
{
    struct addrinfo hints;
    struct addrinfo* result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    if (port == NULL || getaddrinfo(LOOPBACK_HOST, port, &hints, &result) != 0) {
        return NULL;
    }
    return result;
}

int net_listen_loopback(const char* port)
{
    struct addrinfo* result = resolve_loopback(port, 1);
    struct addrinfo* rp;
    int fd = -1;
    /* man getaddrinfo: try each candidate until bind() succeeds. */
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype | SOCK_CLOEXEC, rp->ai_protocol);
        if (fd == -1) {
            continue;
        }
        int reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break; /* Success */
        }
        close(fd);
        fd = -1;
    }
    if (result != NULL) {
        freeaddrinfo(result);
    }
    if (fd != -1 && listen(fd, SOMAXCONN) != 0) {
        close(fd);
        fd = -1;
    }
    return fd;
}

int net_bound_port(int fd)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    if (getsockname(fd, (struct sockaddr*)&address, &length) != 0
            || address.sin_family != AF_INET) {
        return -1;
    }
    return ntohs(address.sin_port);
}

/** Switches O_NONBLOCK on or off. @return 0 or -1. */
static int set_nonblocking(int fd, int enabled)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        return -1;
    }
    flags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, flags);
}

/** Performs one non-blocking connect with a deadline. */
static int connect_one(const struct addrinfo* a, int wakeFd, long long deadline)
{
    int fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
    if (fd < 0) {
        return -1;
    }
    if (set_nonblocking(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    int rc = connect(fd, a->ai_addr, a->ai_addrlen);
    if (rc != 0 && errno != EINPROGRESS && errno != EINTR) {
        close(fd);
        return -1;
    }
    if (rc != 0) {
        IoStatus waited = io_wait(fd, POLLOUT, wakeFd, deadline);
        int soError = 0;
        socklen_t len = sizeof(soError);
        if (waited != IO_OK
                || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len) != 0
                || soError != 0) {
            close(fd);
            return waited == IO_STOPPED ? IO_STOPPED : -1;
        }
    }
    if (set_nonblocking(fd, 0) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int net_connect_loopback(const char* port, int wakeFd, long long timeoutMs)
{
    struct addrinfo* result = resolve_loopback(port, 0);
    struct addrinfo* rp;
    long long deadline = deadline_after(timeoutMs);
    int fd = -1;
    /* man getaddrinfo: try each candidate until connect() succeeds. */
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = connect_one(rp, wakeFd, deadline);
        if (fd >= 0 || fd == IO_STOPPED) {
            break;
        }
    }
    if (result != NULL) {
        freeaddrinfo(result);
    }
    return fd;
}

IoStatus net_send_all(int fd, const void* bytes, size_t len, int wakeFd,
        long long deadlineMs)
{
    const char* cursor = bytes;
    while (len > 0) {
        ssize_t sent = send(fd, cursor, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent > 0) {
            cursor += sent;
            len -= (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            IoStatus waited = io_wait(fd, POLLOUT, wakeFd, deadlineMs);
            if (waited != IO_OK) {
                return waited;
            }
            continue;
        }
        return IO_ERROR;
    }
    return IO_OK;
}

void reader_init(Reader* reader, int fd, int wakeFd)
{
    reader->fd = fd;
    reader->wakeFd = wakeFd;
    reader->deadlineMs = -1;
    reader->pos = 0;
    reader->len = 0;
}

void reader_reset(Reader* reader, int fd)
{
    reader->fd = fd;
    reader->pos = 0;
    reader->len = 0;
}

size_t reader_available(const Reader* reader)
{
    return reader->len - reader->pos;
}

IoStatus reader_fill(Reader* reader)
{
    if (reader->pos > 0) {
        size_t left = reader_available(reader);
        memmove(reader->buf, reader->buf + reader->pos, left);
        reader->pos = 0;
        reader->len = left;
    }
    if (reader->len == sizeof(reader->buf)) {
        return IO_ERROR; /* Caller asked for more than one buffer of lookahead. */
    }
    for (;;) {
        ssize_t got = recv(reader->fd, reader->buf + reader->len,
                sizeof(reader->buf) - reader->len, MSG_DONTWAIT);
        if (got > 0) {
            reader->len += (size_t)got;
            return IO_OK;
        }
        if (got == 0) {
            return IO_EOF;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return IO_ERROR;
        }
        IoStatus waited = io_wait(
                reader->fd, POLLIN, reader->wakeFd, reader->deadlineMs);
        if (waited != IO_OK) {
            return waited;
        }
    }
}

void close_fd(int* fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}
