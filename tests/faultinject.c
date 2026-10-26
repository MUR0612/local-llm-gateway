/** @file faultinject.c
 * @brief LD_PRELOAD shim that makes selected calls fail (tests only).
 *
 * FI_FAIL="name:N[,name:N...]" makes the N-th call (1-based, per process)
 * of each listed function fail. Supported: pthread_create (EAGAIN),
 * posix_spawnp (ENOMEM), accept4 (EMFILE), pipe2 (EMFILE).
 * Not usable together with ASan/TSan, which must be loaded first.
 */
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { FI_PTHREAD, FI_SPAWN, FI_ACCEPT, FI_PIPE, FI_COUNT };

static const char* const NAMES[FI_COUNT] = {"pthread_create", "posix_spawnp", "accept4", "pipe2"};
static long counters[FI_COUNT];

/** @return true when this call of function @p id must fail. */
static int should_fail(int id)
{
    long call = __atomic_add_fetch(&counters[id], 1, __ATOMIC_SEQ_CST);
    const char* spec = getenv("FI_FAIL");
    if (spec == NULL) {
        return 0;
    }
    size_t nameLen = strlen(NAMES[id]);
    for (const char* p = spec; (p = strstr(p, NAMES[id])) != NULL; p += nameLen) {
        if (p[nameLen] == ':' && (p == spec || p[-1] == ',')
                && strtol(p + nameLen + 1, NULL, 10) == call) {
            return 1;
        }
    }
    return 0;
}

int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
        void* (*start)(void*), void* arg)
{
    static int (*real)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
    if (real == NULL) {
        *(void**)&real = dlsym(RTLD_NEXT, "pthread_create");
    }
    return should_fail(FI_PTHREAD) ? EAGAIN : real(thread, attr, start, arg);
}

int posix_spawnp(pid_t* pid, const char* file, const posix_spawn_file_actions_t* actions,
        const posix_spawnattr_t* attr, char* const argv[], char* const envp[])
{
    static int (*real)(pid_t*, const char*, const posix_spawn_file_actions_t*,
            const posix_spawnattr_t*, char* const[], char* const[]);
    if (real == NULL) {
        *(void**)&real = dlsym(RTLD_NEXT, "posix_spawnp");
    }
    return should_fail(FI_SPAWN) ? ENOMEM : real(pid, file, actions, attr, argv, envp);
}

/* glibc declares the address as the __SOCKADDR_ARG transparent union. */
int accept4(int fd, __SOCKADDR_ARG addr, socklen_t* restrict len, int flags)
{
    static int (*real)(int, __SOCKADDR_ARG, socklen_t* restrict, int);
    if (real == NULL) {
        *(void**)&real = dlsym(RTLD_NEXT, "accept4");
    }
    if (should_fail(FI_ACCEPT)) {
        errno = EMFILE;
        return -1;
    }
    return real(fd, addr, len, flags);
}

int pipe2(int fds[2], int flags)
{
    static int (*real)(int[2], int);
    if (real == NULL) {
        *(void**)&real = dlsym(RTLD_NEXT, "pipe2");
    }
    if (should_fail(FI_PIPE)) {
        errno = EMFILE;
        return -1;
    }
    return real(fds, flags);
}
