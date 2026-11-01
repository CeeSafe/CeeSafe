/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * CeeSafe runtime: program arguments, files and TCP sockets (type-system.md §26) for hosted C
 * environments (Linux, macOS) and CosmoOS userland. Only public C library functions (L1, OD-15).
 * Windows (MSVC, best effort; spec/c-backend.md §1.1): files and processes through its C runtime;
 * TCP through Winsock 2, its sockets behind a table of ids. Host services (type-system.md §26.7-§26.10, ADR 0028) work on all three.
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "ceesafe/rt.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <dirent.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if !defined(COSMO_AF_INET) && !defined(_WIN32)
extern char **environ;
#endif

/* ---- Program arguments ------------------------------------------------------------------ */

static int32_t ceesafe_argc;
static const uint8_t *const *ceesafe_argv;

void ceesafe_rt_args(int32_t count, const uint8_t *const *arguments) {
    ceesafe_argc = count;
    ceesafe_argv = arguments;
}

uint64_t ceesafe_arg_count(void) {
    return ceesafe_argc > 0 ? (uint64_t)ceesafe_argc : 0u;
}

bool ceesafe_arg(uint64_t index, cee_str *out) {
    if (index >= ceesafe_arg_count()) return false;
    const uint8_t *text = ceesafe_argv[index];
    uint64_t length = 0;
    while (text[length] != 0) length++;
    if (!ceesafe_utf8_valid(text, length)) return false;
    *out = (cee_str){ .ptr = text, .len = length };
    return true;
}

/* ---- Errors ----------------------------------------------------------------------------- */

uint32_t ceesafe_io_kind(int32_t error) {
    switch (error) {
    case ENOENT: return CEE_IO_NOT_FOUND;
    case EACCES: case EPERM: return CEE_IO_PERMISSION_DENIED;
    case EEXIST: return CEE_IO_ALREADY_EXISTS;
    case EISDIR: return CEE_IO_IS_DIRECTORY;
    case ENOTDIR: return CEE_IO_NOT_DIRECTORY;
    case EINVAL: case ENAMETOOLONG: return CEE_IO_INVALID_INPUT;
    case EPIPE: return CEE_IO_BROKEN_PIPE;
    case ECONNREFUSED: return CEE_IO_CONNECTION_REFUSED;
    case EADDRINUSE: return CEE_IO_ADDRESS_IN_USE;
    case ENOMEM: return CEE_IO_OUT_OF_MEMORY;
    default: return CEE_IO_OTHER;
    }
}

static cee_io_result ceesafe_io_ok(int64_t value) { return (cee_io_result){ .value = value, .error = 0 }; }

static cee_io_result ceesafe_io_errno(void) {
    int32_t error = (int32_t)errno;
    return (cee_io_result){ .value = 0, .error = error != 0 ? error : EIO };
}

static cee_io_result ceesafe_io_fail(int32_t error) { return (cee_io_result){ .value = 0, .error = error }; }

/* ---- Files ------------------------------------------------------------------------------ */

/* A NUL-terminated copy of `path` in `buffer`; false if it contains a zero byte or is too long. */
static bool ceesafe_c_path(cee_str path, char buffer[4096]) {
    if (path.len >= 4096u) return false;
    for (uint64_t i = 0; i < path.len; i++) {
        if (path.ptr[i] == 0) return false;
        buffer[i] = (char)path.ptr[i];
    }
    buffer[path.len] = 0;
    return true;
}

cee_io_result ceesafe_file_open(cee_str path, uint32_t mode) {
    char name[4096];
    if (!ceesafe_c_path(path, name)) return ceesafe_io_fail(EINVAL);
    int fd;
    do {
#ifdef _WIN32
        fd = mode == 0u ? _open(name, _O_RDONLY | _O_BINARY)
                        : _open(name, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
        fd = mode == 0u ? open(name, O_RDONLY) : open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
#endif
    } while (fd < 0 && errno == EINTR);
    return fd < 0 ? ceesafe_io_errno() : ceesafe_io_ok(fd);
}

cee_io_result ceesafe_file_remove(cee_str path) {
    char name[4096];
    if (!ceesafe_c_path(path, name)) return ceesafe_io_fail(EINVAL);
#ifdef _WIN32
    return _unlink(name) < 0 ? ceesafe_io_errno() : ceesafe_io_ok(0);
#else
    return unlink(name) < 0 ? ceesafe_io_errno() : ceesafe_io_ok(0);
#endif
}

#ifdef _WIN32
static cee_io_result cee_socket_read(int32_t id, uint8_t *buffer, size_t chunk);
static cee_io_result cee_socket_write(int32_t id, const uint8_t *bytes, uint64_t length);
static void cee_socket_close(int32_t id);
#endif

cee_io_result ceesafe_fd_read(int32_t fd, uint8_t *buffer, uint64_t length) {
    size_t chunk = length > (1u << 30) ? (size_t)1u << 30 : (size_t)length;
#ifdef _WIN32
    if (fd >= CEE_SOCKET_BASE) return cee_socket_read(fd, buffer, chunk);
    int n;
    do {
        n = _read(fd, buffer, (unsigned)chunk);
    } while (n < 0 && errno == EINTR);
#else
    ssize_t n;
    do {
        n = read(fd, buffer, chunk);
    } while (n < 0 && errno == EINTR);
#endif
    return n < 0 ? ceesafe_io_errno() : ceesafe_io_ok((int64_t)n);
}

cee_io_result ceesafe_fd_write(int32_t fd, const uint8_t *bytes, uint64_t length) {
#ifdef _WIN32
    if (fd >= CEE_SOCKET_BASE) return cee_socket_write(fd, bytes, length);
#endif
    uint64_t done = 0;
    while (done < length) {
        uint64_t rest = length - done;
        size_t chunk = rest > (1u << 30) ? (size_t)1u << 30 : (size_t)rest;
#if defined(_WIN32)
        int n = _write(fd, bytes + done, (unsigned)chunk);
#elif defined(MSG_NOSIGNAL)
        /* A socket whose peer is gone reports EPIPE instead of raising SIGPIPE (Linux). */
        ssize_t n = send(fd, bytes + done, chunk, MSG_NOSIGNAL);
        if (n < 0 && errno == ENOTSOCK) n = write(fd, bytes + done, chunk);
#else
        ssize_t n = write(fd, bytes + done, chunk);
#endif
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return ceesafe_io_errno();
        if (n == 0) return ceesafe_io_fail(EIO);
        done += (uint64_t)n;
    }
    return ceesafe_io_ok(0);
}

void ceesafe_fd_drop(int32_t *fd) {
#ifdef _WIN32
    if (*fd >= CEE_SOCKET_BASE) cee_socket_close(*fd);
    else if (*fd >= 0) (void)_close(*fd);
#else
    if (*fd >= 0) (void)close(*fd);
#endif
}

/* ---- TCP over IPv4 ---------------------------------------------------------------------- */

/* Dotted IPv4 text ("127.0.0.1") to its four bytes; false if malformed. */
static bool ceesafe_ipv4(cee_str text, uint8_t out[4]) {
    uint64_t i = 0;
    for (int part = 0; part < 4; part++) {
        if (part > 0) {
            if (i >= text.len || text.ptr[i] != '.') return false;
            i++;
        }
        uint32_t value = 0;
        uint64_t digits = 0;
        while (i < text.len && text.ptr[i] >= '0' && text.ptr[i] <= '9' && digits < 3) {
            value = value * 10u + (uint32_t)(text.ptr[i] - '0');
            i++;
            digits++;
        }
        if (digits == 0 || value > 255u) return false;
        out[part] = (uint8_t)value;
    }
    return i == text.len;
}

bool ceesafe_ipv4_address(cee_str text, uint8_t out[4]) { return ceesafe_ipv4(text, out); }

#ifdef _WIN32
/* ---- Windows: Winsock 2 behind a table of ids (L1 A4a; ceesafe/rt.h's CEE_SOCKET_BASE). The table holds the SOCKET
 * of each id that is open, INVALID_SOCKET in a free slot; a slim reader-writer lock guards it, since any thread may
 * open or close a socket. It comes from the platform's heap: the runtime's own bookkeeping, as the executor's. ---- */

static SRWLOCK cee_socket_lock = SRWLOCK_INIT;
static SOCKET *cee_sockets;
static HANDLE *cee_socket_events; /* by slot: the event the socket signals (ceesafe_socket_select), or NULL */
static HANDLE *cee_socket_ports;  /* by slot: the completion port it is tied to (ceesafe_socket_associate), or NULL */
static int32_t cee_socket_add(SOCKET s);
static uint32_t cee_socket_count;
static uint32_t cee_socket_capacity;
static INIT_ONCE cee_wsa_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK cee_wsa_start(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    WSADATA data;
    (void)once; (void)parameter; (void)context;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

static bool cee_wsa_ready(void) { return InitOnceExecuteOnce(&cee_wsa_once, cee_wsa_start, NULL, NULL) != 0; }

/* A Winsock error as the C runtime's errno, so ceesafe_io_kind and messages read it as on POSIX. */
static int32_t cee_wsa_errno(int error) {
    switch (error) {
    case WSAEWOULDBLOCK: return EWOULDBLOCK;
    case WSAEINPROGRESS: return EINPROGRESS;
    case WSAEALREADY: return EALREADY;
    case WSAECONNREFUSED: return ECONNREFUSED;
    case WSAEADDRINUSE: return EADDRINUSE;
    case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
    case WSAECONNRESET: return ECONNRESET;
    case WSAECONNABORTED: return ECONNABORTED;
    case WSAENOTCONN: return ENOTCONN;
    case WSAEISCONN: return EISCONN;
    case WSAESHUTDOWN: return EPIPE;
    case WSAETIMEDOUT: return ETIMEDOUT;
    case WSAENETUNREACH: case WSAEHOSTUNREACH: return ENETUNREACH;
    case WSAEACCES: return EACCES;
    case WSAEINVAL: case WSAEFAULT: return EINVAL;
    case WSAENOTSOCK: return ENOTSOCK;
    case WSAEMFILE: return EMFILE;
    case WSAENOBUFS: case WSA_NOT_ENOUGH_MEMORY: return ENOMEM;
    case WSAEINTR: return EINTR;
    default: return EIO;
    }
}

static cee_io_result cee_wsa_fail(void) { return ceesafe_io_fail(cee_wsa_errno(WSAGetLastError())); }

uintptr_t ceesafe_socket_handle(int32_t id) {
    SOCKET s = INVALID_SOCKET;
    AcquireSRWLockShared(&cee_socket_lock);
    if (id >= CEE_SOCKET_BASE && (uint32_t)(id - CEE_SOCKET_BASE) < cee_socket_count) {
        s = cee_sockets[id - CEE_SOCKET_BASE];
    }
    ReleaseSRWLockShared(&cee_socket_lock);
    return (uintptr_t)s;
}

void ceesafe_socket_select(int32_t id, void *event) {
    AcquireSRWLockExclusive(&cee_socket_lock);
    if (id >= CEE_SOCKET_BASE && (uint32_t)(id - CEE_SOCKET_BASE) < cee_socket_count) {
        uint32_t k = (uint32_t)(id - CEE_SOCKET_BASE);
        if (cee_sockets[k] != INVALID_SOCKET && cee_socket_events[k] != (HANDLE)event &&
            WSAEventSelect(cee_sockets[k], (HANDLE)event, FD_READ | FD_WRITE | FD_ACCEPT | FD_CONNECT | FD_CLOSE) == 0) {
            cee_socket_events[k] = (HANDLE)event;
        }
    }
    ReleaseSRWLockExclusive(&cee_socket_lock);
}

int32_t ceesafe_socket_associate(int32_t id, void *port) {
    int32_t status = -1;
    AcquireSRWLockExclusive(&cee_socket_lock);
    if (id >= CEE_SOCKET_BASE && (uint32_t)(id - CEE_SOCKET_BASE) < cee_socket_count) {
        uint32_t k = (uint32_t)(id - CEE_SOCKET_BASE);
        if (cee_sockets[k] == INVALID_SOCKET) {
            status = -1;
        } else if (cee_socket_ports[k] == (HANDLE)port) {
            status = 1;
        } else if (cee_socket_ports[k] != NULL) {
            status = 0; /* tied to another thread's port for life */
        } else if (CreateIoCompletionPort((HANDLE)cee_sockets[k], (HANDLE)port, 0, 0) != NULL) {
            cee_socket_ports[k] = (HANDLE)port;
            status = 1;
        }
    }
    ReleaseSRWLockExclusive(&cee_socket_lock);
    return status;
}

int32_t ceesafe_socket_adopt(uintptr_t s, void *port) {
    int32_t id = cee_socket_add((SOCKET)s);
    if (id >= 0 && port != NULL) {
        AcquireSRWLockExclusive(&cee_socket_lock);
        cee_socket_ports[id - CEE_SOCKET_BASE] = (HANDLE)port; /* tied already (ConnectEx's socket): never again */
        ReleaseSRWLockExclusive(&cee_socket_lock);
    }
    return id;
}

int32_t ceesafe_socket_errno(int error) { return cee_wsa_errno(error); }

void ceesafe_socket_unselect(void *event) {
    AcquireSRWLockExclusive(&cee_socket_lock);
    for (uint32_t k = 0; k < cee_socket_count; k++) {
        if (cee_sockets[k] != INVALID_SOCKET && cee_socket_events[k] == (HANDLE)event) {
            (void)WSAEventSelect(cee_sockets[k], NULL, 0); /* the socket stays nonblocking */
            cee_socket_events[k] = NULL;
        }
    }
    ReleaseSRWLockExclusive(&cee_socket_lock);
}

/* An id for `s`, or -1 if the table cannot grow (then `s` is closed). */
static int32_t cee_socket_add(SOCKET s) {
    int32_t id = -1;
    AcquireSRWLockExclusive(&cee_socket_lock);
    for (uint32_t k = 0; k < cee_socket_count && id < 0; k++) {
        if (cee_sockets[k] == INVALID_SOCKET) {
            cee_sockets[k] = s;
            cee_socket_events[k] = NULL;
            cee_socket_ports[k] = NULL;
            id = CEE_SOCKET_BASE + (int32_t)k;
        }
    }
    if (id < 0 && cee_socket_count < (uint32_t)(INT32_MAX - CEE_SOCKET_BASE)) {
        if (cee_socket_count == cee_socket_capacity) {
            uint32_t capacity = cee_socket_capacity == 0u ? 16u : cee_socket_capacity * 2u;
            SOCKET *grown = (SOCKET *)ceesafe_platform_realloc(cee_sockets, (size_t)capacity * sizeof *grown);
            if (grown != NULL) {
                cee_sockets = grown;
                HANDLE *events = (HANDLE *)ceesafe_platform_realloc(cee_socket_events, (size_t)capacity * sizeof *events);
                if (events != NULL) {
                    cee_socket_events = events;
                    HANDLE *ports = (HANDLE *)ceesafe_platform_realloc(cee_socket_ports, (size_t)capacity * sizeof *ports);
                    if (ports != NULL) {
                        cee_socket_ports = ports;
                        cee_socket_capacity = capacity;
                    }
                }
            }
        }
        if (cee_socket_count < cee_socket_capacity) {
            cee_sockets[cee_socket_count] = s;
            cee_socket_events[cee_socket_count] = NULL;
            cee_socket_ports[cee_socket_count] = NULL;
            id = CEE_SOCKET_BASE + (int32_t)cee_socket_count;
            cee_socket_count += 1u;
        }
    }
    ReleaseSRWLockExclusive(&cee_socket_lock);
    if (id < 0) {
        (void)closesocket(s);
    }
    return id;
}

static void cee_socket_close(int32_t id) {
    SOCKET s = INVALID_SOCKET;
    AcquireSRWLockExclusive(&cee_socket_lock);
    if (id >= CEE_SOCKET_BASE && (uint32_t)(id - CEE_SOCKET_BASE) < cee_socket_count) {
        s = cee_sockets[id - CEE_SOCKET_BASE];
        cee_sockets[id - CEE_SOCKET_BASE] = INVALID_SOCKET;
        cee_socket_events[id - CEE_SOCKET_BASE] = NULL;
        cee_socket_ports[id - CEE_SOCKET_BASE] = NULL;
    }
    ReleaseSRWLockExclusive(&cee_socket_lock);
    if (s != INVALID_SOCKET) {
        (void)closesocket(s);
    }
}

static cee_io_result cee_socket_read(int32_t id, uint8_t *buffer, size_t chunk) {
    SOCKET s = (SOCKET)ceesafe_socket_handle(id);
    if (s == INVALID_SOCKET) return ceesafe_io_fail(EBADF);
    int n = recv(s, (char *)buffer, (int)chunk, 0);
    return n == SOCKET_ERROR ? cee_wsa_fail() : ceesafe_io_ok(n);
}

static cee_io_result cee_socket_write(int32_t id, const uint8_t *bytes, uint64_t length) {
    SOCKET s = (SOCKET)ceesafe_socket_handle(id);
    if (s == INVALID_SOCKET) return ceesafe_io_fail(EBADF);
    uint64_t done = 0;
    while (done < length) {
        uint64_t rest = length - done;
        int chunk = rest > (1u << 30) ? 1 << 30 : (int)rest;
        int n = send(s, (const char *)(bytes + done), chunk, 0);
        if (n == SOCKET_ERROR) return cee_wsa_fail();
        if (n == 0) return ceesafe_io_fail(EIO);
        done += (uint64_t)n;
    }
    return ceesafe_io_ok(0);
}

static void cee_address_set(struct sockaddr_in *a, const uint8_t bytes[4], uint16_t port) {
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    memcpy(&a->sin_addr, bytes, 4);
}

/* A new socket, bound and listening, or connected; nonblocking first if asked. */
static cee_io_result cee_socket_open(cee_str address, uint16_t port, bool listening, bool nonblocking) {
    uint8_t bytes[4];
    if (!ceesafe_ipv4(address, bytes)) return ceesafe_io_fail(EINVAL);
    if (!cee_wsa_ready()) return ceesafe_io_fail(ENOSYS);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return cee_wsa_fail();
    u_long on = 1;
    if (nonblocking && ioctlsocket(s, FIONBIO, &on) != 0) {
        cee_io_result failed = cee_wsa_fail();
        (void)closesocket(s);
        return failed;
    }
    struct sockaddr_in a;
    cee_address_set(&a, bytes, port);
    int status = listening ? bind(s, (const struct sockaddr *)&a, (int)sizeof a) : connect(s, (const struct sockaddr *)&a, (int)sizeof a);
    if (status == 0 && listening) status = listen(s, 16);
    if (status != 0 && !(nonblocking && !listening && WSAGetLastError() == WSAEWOULDBLOCK)) {
        cee_io_result failed = cee_wsa_fail();
        (void)closesocket(s);
        return failed;
    }
    int32_t id = cee_socket_add(s);
    return id < 0 ? ceesafe_io_fail(ENOMEM) : ceesafe_io_ok(id);
}

cee_io_result ceesafe_tcp_listen(cee_str address, uint16_t port) { return cee_socket_open(address, port, true, false); }

cee_io_result ceesafe_tcp_port(int32_t fd) {
    SOCKET s = (SOCKET)ceesafe_socket_handle(fd);
    if (s == INVALID_SOCKET) return ceesafe_io_fail(EBADF);
    struct sockaddr_in a;
    int length = (int)sizeof a;
    if (getsockname(s, (struct sockaddr *)&a, &length) != 0) return cee_wsa_fail();
    return ceesafe_io_ok(ntohs(a.sin_port));
}

static cee_io_result cee_socket_accept(int32_t fd, bool nonblocking) {
    SOCKET s = (SOCKET)ceesafe_socket_handle(fd);
    if (s == INVALID_SOCKET) return ceesafe_io_fail(EBADF);
    SOCKET peer = accept(s, NULL, NULL);
    if (peer == INVALID_SOCKET) return cee_wsa_fail();
    /* An accepted socket has the listener's properties, including WSAEventSelect's (which makes it nonblocking):
       undo them for a blocking stream. */
    (void)WSAEventSelect(peer, NULL, 0);
    u_long mode = nonblocking ? 1u : 0u;
    if (ioctlsocket(peer, FIONBIO, &mode) != 0) {
        cee_io_result failed = cee_wsa_fail();
        (void)closesocket(peer);
        return failed;
    }
    int32_t id = cee_socket_add(peer);
    return id < 0 ? ceesafe_io_fail(ENOMEM) : ceesafe_io_ok(id);
}

cee_io_result ceesafe_tcp_accept(int32_t fd) { return cee_socket_accept(fd, false); }

cee_io_result ceesafe_tcp_connect(cee_str address, uint16_t port) { return cee_socket_open(address, port, false, false); }

cee_io_result ceesafe_tcp_shutdown_write(int32_t fd) {
    SOCKET s = (SOCKET)ceesafe_socket_handle(fd);
    if (s == INVALID_SOCKET) return ceesafe_io_fail(EBADF);
    return shutdown(s, SD_SEND) != 0 ? cee_wsa_fail() : ceesafe_io_ok(0);
}
#else
#ifdef COSMO_AF_INET
/* CosmoOS's own address layout: host-order port, address bytes in network order (sys/socket.h). */
typedef struct sockaddr ceesafe_address;
static void ceesafe_address_set(ceesafe_address *a, const uint8_t bytes[4], uint16_t port) {
    memset(a, 0, sizeof *a);
    a->sa_family = AF_INET;
    a->sa_port = port;
    memcpy(a->sa_addr, bytes, 4);
}
static uint16_t ceesafe_address_port(const ceesafe_address *a) { return a->sa_port; }
#else
typedef struct sockaddr_in ceesafe_address;
static void ceesafe_address_set(ceesafe_address *a, const uint8_t bytes[4], uint16_t port) {
    memset(a, 0, sizeof *a);
#ifdef __APPLE__
    a->sin_len = (uint8_t)sizeof *a;
#endif
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    memcpy(&a->sin_addr, bytes, 4);
}
static uint16_t ceesafe_address_port(const ceesafe_address *a) { return ntohs(a->sin_port); }
#endif

/* A new TCP socket; on macOS, writes to a closed peer report EPIPE instead of raising SIGPIPE. */
static int ceesafe_tcp_socket(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
#ifdef SO_NOSIGPIPE
    if (fd >= 0) {
        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    }
#endif
    return fd;
}

cee_io_result ceesafe_tcp_listen(cee_str address, uint16_t port) {
    uint8_t bytes[4];
    if (!ceesafe_ipv4(address, bytes)) return ceesafe_io_fail(EINVAL);
    int fd = ceesafe_tcp_socket();
    if (fd < 0) return ceesafe_io_errno();
#ifdef SO_REUSEADDR
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    ceesafe_address a;
    ceesafe_address_set(&a, bytes, port);
    if (bind(fd, (const struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 16) < 0) {
        cee_io_result failed = ceesafe_io_errno();
        (void)close(fd);
        return failed;
    }
    return ceesafe_io_ok(fd);
}

cee_io_result ceesafe_tcp_port(int32_t fd) {
    ceesafe_address a;
    socklen_t length = sizeof a;
    if (getsockname(fd, (struct sockaddr *)&a, &length) < 0) return ceesafe_io_errno();
    return ceesafe_io_ok(ceesafe_address_port(&a));
}

cee_io_result ceesafe_tcp_accept(int32_t fd) {
    int peer;
    do {
        peer = accept(fd, NULL, NULL);
    } while (peer < 0 && errno == EINTR);
    if (peer < 0) return ceesafe_io_errno();
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(peer, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return ceesafe_io_ok(peer);
}

cee_io_result ceesafe_tcp_connect(cee_str address, uint16_t port) {
    uint8_t bytes[4];
    if (!ceesafe_ipv4(address, bytes)) return ceesafe_io_fail(EINVAL);
    int fd = ceesafe_tcp_socket();
    if (fd < 0) return ceesafe_io_errno();
    ceesafe_address a;
    ceesafe_address_set(&a, bytes, port);
    if (connect(fd, (const struct sockaddr *)&a, sizeof a) < 0) {
        cee_io_result failed = ceesafe_io_errno();
        (void)close(fd);
        return failed;
    }
    return ceesafe_io_ok(fd);
}

cee_io_result ceesafe_tcp_shutdown_write(int32_t fd) {
    return shutdown(fd, SHUT_WR) < 0 ? ceesafe_io_errno() : ceesafe_io_ok(0);
}

/* ---- Async sockets (type-system.md §40.4; lib/std/async.cee): non-blocking descriptors. Each call is a value >= 0,
 * CEE_ASYNC_WOULD_BLOCK (the task waits for readiness, then calls again), or a negated errno. ---- */

static int64_t cee_async_errno(void) {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS ? CEE_ASYNC_WOULD_BLOCK : -(int64_t)errno;
}

static bool cee_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

int64_t ceesafe_async_tcp_listen(const uint8_t *address, uint64_t length, uint16_t port) {
    cee_str text = {address, length};
    cee_io_result r = ceesafe_tcp_listen(text, port);
    if (r.error != 0) {
        return -(int64_t)r.error;
    }
    if (!cee_nonblocking((int)r.value)) {
        int64_t failed = -(int64_t)errno;
        (void)close((int)r.value);
        return failed;
    }
    return r.value;
}

int64_t ceesafe_async_tcp_port(int32_t fd) {
    cee_io_result r = ceesafe_tcp_port(fd);
    return r.error == 0 ? r.value : -(int64_t)r.error;
}

int64_t ceesafe_async_tcp_accept(int32_t fd) {
    int peer;
    do {
        peer = accept(fd, NULL, NULL);
    } while (peer < 0 && errno == EINTR);
    if (peer < 0) {
        return cee_async_errno();
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(peer, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    if (!cee_nonblocking(peer)) {
        int64_t failed = -(int64_t)errno;
        (void)close(peer);
        return failed;
    }
    return peer;
}

/* Starts a connection: the descriptor, which is connected once it is writable and ceesafe_async_tcp_connected is 0. */
int64_t ceesafe_async_tcp_connect(const uint8_t *address, uint64_t length, uint16_t port) {
    cee_str text = {address, length};
    uint8_t bytes[4];
    if (!ceesafe_ipv4(text, bytes)) {
        return -(int64_t)EINVAL;
    }
    int fd = ceesafe_tcp_socket();
    if (fd < 0) {
        return -(int64_t)errno;
    }
    if (!cee_nonblocking(fd)) {
        int64_t failed = -(int64_t)errno;
        (void)close(fd);
        return failed;
    }
    ceesafe_address a;
    ceesafe_address_set(&a, bytes, port);
    if (connect(fd, (const struct sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS && errno != EINTR) {
        int64_t failed = -(int64_t)errno;
        (void)close(fd);
        return failed;
    }
    return fd;
}

int64_t ceesafe_async_tcp_connected(int32_t fd) {
    int error = 0;
    socklen_t size = sizeof error;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0) {
        return -(int64_t)errno;
    }
    if (error != 0) {
        return error == EINPROGRESS || error == EALREADY ? CEE_ASYNC_WOULD_BLOCK : -(int64_t)error;
    }
    /* No error yet: connected once it has a peer (SO_ERROR is also 0 while the connection is still in progress). */
    ceesafe_address peer;
    socklen_t length = sizeof peer;
    if (getpeername(fd, (struct sockaddr *)&peer, &length) == 0) {
        return 0;
    }
    return errno == ENOTCONN ? CEE_ASYNC_WOULD_BLOCK : -(int64_t)errno;
}

int64_t ceesafe_async_read(int32_t fd, uint8_t *buffer, uint64_t length) {
    ssize_t n;
    do {
        n = read(fd, buffer, (size_t)length);
    } while (n < 0 && errno == EINTR);
    return n < 0 ? cee_async_errno() : (int64_t)n;
}

int64_t ceesafe_async_send(int32_t fd, const uint8_t *bytes, uint64_t length) {
    ssize_t n;
    do {
#ifdef MSG_NOSIGNAL
        n = send(fd, bytes, (size_t)length, MSG_NOSIGNAL);
#else
        n = send(fd, bytes, (size_t)length, 0);
#endif
    } while (n < 0 && errno == EINTR);
    return n < 0 ? cee_async_errno() : (int64_t)n;
}

int64_t ceesafe_async_shutdown_write(int32_t fd) {
    return shutdown(fd, SHUT_WR) < 0 ? -(int64_t)errno : 0;
}

void ceesafe_async_close(int32_t fd) {
    (void)close(fd);
}
#endif /* _WIN32 */

#ifdef _WIN32
/* Async sockets on Windows: the same calls over nonblocking Winsock sockets; the executor waits for them with
 * WSAEventSelect and WSAPoll (ceesafe_rt_async.c). */
static int64_t cee_async_result(cee_io_result r) {
    if (r.error == 0) return r.value;
    return r.error == EWOULDBLOCK || r.error == EINPROGRESS ? CEE_ASYNC_WOULD_BLOCK : -(int64_t)r.error;
}

int64_t ceesafe_async_tcp_listen(const uint8_t *address, uint64_t length, uint16_t port) {
    cee_str text = {address, length};
    return cee_async_result(cee_socket_open(text, port, true, true));
}

int64_t ceesafe_async_tcp_port(int32_t fd) { return cee_async_result(ceesafe_tcp_port(fd)); }

int64_t ceesafe_async_tcp_accept(int32_t fd) { return cee_async_result(cee_socket_accept(fd, true)); }

/* Starts a connection: the id, which is connected once it is writable and ceesafe_async_tcp_connected is 0. */
int64_t ceesafe_async_tcp_connect(const uint8_t *address, uint64_t length, uint16_t port) {
    cee_str text = {address, length};
    return cee_async_result(cee_socket_open(text, port, false, true));
}

int64_t ceesafe_async_tcp_connected(int32_t fd) {
    SOCKET s = (SOCKET)ceesafe_socket_handle(fd);
    if (s == INVALID_SOCKET) return -(int64_t)EBADF;
    int error = 0;
    int size = (int)sizeof error;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&error, &size) != 0) return -(int64_t)cee_wsa_errno(WSAGetLastError());
    if (error != 0) {
        return error == WSAEINPROGRESS || error == WSAEALREADY || error == WSAEWOULDBLOCK ? CEE_ASYNC_WOULD_BLOCK
                                                                                         : -(int64_t)cee_wsa_errno(error);
    }
    struct sockaddr_in peer;
    int length = (int)sizeof peer;
    if (getpeername(s, (struct sockaddr *)&peer, &length) == 0) return 0;
    int failed = WSAGetLastError();
    return failed == WSAENOTCONN ? CEE_ASYNC_WOULD_BLOCK : -(int64_t)cee_wsa_errno(failed);
}

int64_t ceesafe_async_read(int32_t fd, uint8_t *buffer, uint64_t length) {
    size_t chunk = length > (1u << 30) ? (size_t)1u << 30 : (size_t)length;
    if (fd < CEE_SOCKET_BASE) return cee_async_result(ceesafe_fd_read(fd, buffer, chunk));
    return cee_async_result(cee_socket_read(fd, buffer, chunk));
}

int64_t ceesafe_async_send(int32_t fd, const uint8_t *bytes, uint64_t length) {
    SOCKET s = (SOCKET)ceesafe_socket_handle(fd);
    if (s == INVALID_SOCKET) return -(int64_t)EBADF;
    int chunk = length > (1u << 30) ? 1 << 30 : (int)length;
    int n = send(s, (const char *)bytes, chunk, 0);
    return n == SOCKET_ERROR ? cee_async_result(cee_wsa_fail()) : (int64_t)n;
}

int64_t ceesafe_async_shutdown_write(int32_t fd) { return cee_async_result(ceesafe_tcp_shutdown_write(fd)); }

void ceesafe_async_close(int32_t fd) {
    if (fd >= CEE_SOCKET_BASE) {
        cee_socket_close(fd);
    } else if (fd >= 0) {
        (void)_close(fd);
    }
}
#endif /* _WIN32 */

/* ---- Processes -------------------------------------------------------------------------- */

/* A NUL-terminated copy from the counted allocator; NULL if the text has a zero byte or memory runs out. */
static char *ceesafe_c_string(cee_str text, int32_t *error) {
    for (uint64_t i = 0; i < text.len; i++) {
        if (text.ptr[i] == 0) { *error = EINVAL; return NULL; }
    }
    char *copy = (char *)ceesafe_alloc((size_t)text.len + 1u, CEE_ALLOC_SYSTEM);
    if (copy == NULL) { *error = ENOMEM; return NULL; }
    for (uint64_t i = 0; i < text.len; i++) copy[i] = (char)text.ptr[i];
    copy[text.len] = 0;
    return copy;
}

cee_io_result ceesafe_process_run(cee_str program, const cee_str *arguments, uint64_t count) {
    if (count > 4096u) return ceesafe_io_fail(EINVAL);
    char **argv = (char **)ceesafe_alloc((size_t)(count + 2u) * sizeof(char *), CEE_ALLOC_SYSTEM);
    if (argv == NULL) return ceesafe_io_fail(ENOMEM);
    int32_t error = 0;
    uint64_t made = 0;
    argv[0] = ceesafe_c_string(program, &error);
    if (argv[0] != NULL) {
        made = 1;
        for (; made <= count; made++) {
            argv[made] = ceesafe_c_string(arguments[made - 1u], &error);
            if (argv[made] == NULL) break;
        }
    }
    cee_io_result result = ceesafe_io_ok(0);
    if (error != 0) {
        result = ceesafe_io_fail(error);
    } else {
        argv[count + 1u] = NULL;
        (void)fflush(stdout);
#ifdef _WIN32
        intptr_t status = _spawnvp(_P_WAIT, argv[0], (const char *const *)argv);
        result = status < 0 ? ceesafe_io_errno() : ceesafe_io_ok((int64_t)status);
#else
        pid_t pid;
#ifdef COSMO_AF_INET
        pid = spawnvp(argv[0], (const char *const *)argv, NULL, 0);
        int started = pid < 0 ? errno : 0;
#else
        int started = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
#endif
        if (started != 0) {
            result = ceesafe_io_fail(started);
        } else {
            int status = 0;
            pid_t waited;
            do {
                waited = waitpid(pid, &status, 0);
            } while (waited < 0 && errno == EINTR);
            if (waited < 0) {
                result = ceesafe_io_errno();
            } else if (WIFEXITED(status)) {
                result = ceesafe_io_ok(WEXITSTATUS(status));
            } else {
                result = ceesafe_io_ok(128 + WTERMSIG(status));
            }
        }
#endif
    }
    for (uint64_t i = 0; i < made; i++) ceesafe_free(argv[i], CEE_ALLOC_SYSTEM);
    ceesafe_free(argv, CEE_ALLOC_SYSTEM);
    return result;
}

/* ---- Host services (type-system.md §26.7-§26.10; ADR 0028), for lib/std/host.cee ------------------------
 * Each returns a value >= 0, or a negated errno. Paths follow ceesafe_c_path: no zero byte, at most 4,095 bytes. */

static cee_str ceesafe_host_text(uint8_t const *bytes, uint64_t length) { return (cee_str){ .ptr = bytes, .len = length }; }

int64_t ceesafe_host_env(uint8_t const *name, uint64_t length, uint8_t const **value, uint64_t *value_length) {
    char key[4096];
    if (!ceesafe_c_path(ceesafe_host_text(name, length), key)) return -(int64_t)EINVAL;
    const char *found = getenv(key);
    if (found == NULL) return 0;
    uint64_t n = (uint64_t)strlen(found);
    if (!ceesafe_utf8_valid((const uint8_t *)found, n)) return 0;
    *value = (const uint8_t *)found;
    *value_length = n;
    return 1;
}

/* 0: nothing there; 1: a file; 2: a directory; 3: something else. Symbolic links are followed. */
int64_t ceesafe_host_path_kind(uint8_t const *path, uint64_t length) {
    char name[4096];
    if (!ceesafe_c_path(ceesafe_host_text(path, length), name)) return -(int64_t)EINVAL;
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(name, &st) != 0) return errno == ENOENT ? 0 : -(int64_t)errno;
    unsigned type = (unsigned)st.st_mode & _S_IFMT;
    return type == _S_IFDIR ? 2 : type == _S_IFREG ? 1 : 3;
#else
    struct stat st;
    if (stat(name, &st) != 0) return errno == ENOENT || errno == ENOTDIR ? 0 : -(int64_t)errno;
#ifdef COSMO_AF_INET
    /* CosmoOS's S_ISDIR and S_ISREG test the record's DT_* type, st_type, not st_mode (its sys/stat.h). */
    return S_ISDIR(st.st_type) ? 2 : S_ISREG(st.st_type) ? 1 : 3;
#else
    return S_ISDIR(st.st_mode) ? 2 : S_ISREG(st.st_mode) ? 1 : 3;
#endif
#endif
}

int64_t ceesafe_host_dir_create(uint8_t const *path, uint64_t length) {
    char name[4096];
    if (!ceesafe_c_path(ceesafe_host_text(path, length), name)) return -(int64_t)EINVAL;
#ifdef _WIN32
    return _mkdir(name) == 0 ? 0 : -(int64_t)errno;
#else
    return mkdir(name, 0777) == 0 ? 0 : -(int64_t)errno;
#endif
}

int64_t ceesafe_host_dir_remove(uint8_t const *path, uint64_t length) {
    char name[4096];
    if (!ceesafe_c_path(ceesafe_host_text(path, length), name)) return -(int64_t)EINVAL;
#ifdef _WIN32
    return _rmdir(name) == 0 ? 0 : -(int64_t)errno;
#else
    return rmdir(name) == 0 ? 0 : -(int64_t)errno;
#endif
}

#ifdef _WIN32
typedef struct ceesafe_host_dir { intptr_t handle; struct _finddata_t entry; int pending; } ceesafe_host_dir;
#endif

/* An open directory listing, or NULL with *error set. Close it with ceesafe_host_dir_close. */
void *ceesafe_host_dir_open(uint8_t const *path, uint64_t length, int32_t *error) {
    char name[4096];
    if (!ceesafe_c_path(ceesafe_host_text(path, length), name)) { *error = EINVAL; return NULL; }
#ifdef _WIN32
    size_t n = strlen(name);
    if (n + 3u > sizeof name) { *error = EINVAL; return NULL; }
    if (n > 0u && name[n - 1u] != '\\' && name[n - 1u] != '/') name[n++] = '\\';
    name[n++] = '*';
    name[n] = 0;
    ceesafe_host_dir *d = (ceesafe_host_dir *)ceesafe_alloc(sizeof *d, CEE_ALLOC_SYSTEM);
    if (d == NULL) { *error = ENOMEM; return NULL; }
    d->handle = _findfirst(name, &d->entry);
    if (d->handle == -1) { *error = errno; ceesafe_free(d, CEE_ALLOC_SYSTEM); return NULL; }
    d->pending = 1;
    return d;
#else
    DIR *d = opendir(name);
    if (d == NULL) *error = errno;
    return d;
#endif
}

/* The next entry's name (valid until the next call), skipping "." and "..": 1, or 0 at the end. */
int64_t ceesafe_host_dir_next(void *dir, uint8_t const **name, uint64_t *length) {
    for (;;) {
        const char *entry;
#ifdef _WIN32
        ceesafe_host_dir *d = (ceesafe_host_dir *)dir;
        if (!d->pending) {
            if (_findnext(d->handle, &d->entry) != 0) return errno == ENOENT ? 0 : -(int64_t)errno;
        }
        d->pending = 0;
        entry = d->entry.name;
#else
        errno = 0;
        struct dirent *e = readdir((DIR *)dir);
        if (e == NULL) return errno == 0 ? 0 : -(int64_t)errno;
        entry = e->d_name;
#endif
        if (strcmp(entry, ".") == 0 || strcmp(entry, "..") == 0) continue;
        *name = (const uint8_t *)entry;
        *length = (uint64_t)strlen(entry);
        return 1;
    }
}

void ceesafe_host_dir_close(void *dir) {
#ifdef _WIN32
    ceesafe_host_dir *d = (ceesafe_host_dir *)dir;
    (void)_findclose(d->handle);
    ceesafe_free(d, CEE_ALLOC_SYSTEM);
#else
    (void)closedir((DIR *)dir);
#endif
}

/* Copies a C string into (buffer, capacity): its length, or -ERANGE if it does not fit. */
static int64_t ceesafe_host_copy_out(const char *text, uint8_t *buffer, uint64_t capacity) {
    uint64_t n = (uint64_t)strlen(text);
    if (n > capacity) return -(int64_t)ERANGE;
    for (uint64_t i = 0; i < n; i++) buffer[i] = (uint8_t)text[i];
    return (int64_t)n;
}

/* A new, empty directory only this user can enter, under the platform's temporary directory; its path goes to
 * (buffer, capacity). A name is tried until mkdir creates it (mkdir fails on an existing name, so this is atomic
 * without mkdtemp, which CosmoOS's libc lacks). */
int64_t ceesafe_host_temporary(uint8_t *buffer, uint64_t capacity) {
    static uint32_t counter;
    char base[4096];
#ifdef _WIN32
    DWORD got = GetTempPathA((DWORD)sizeof base, base);
    if (got == 0u) return -(int64_t)ENOENT;
    if (got >= sizeof base - 64u) return -(int64_t)ENAMETOOLONG;
    unsigned long id = (unsigned long)GetCurrentProcessId();
#else
    const char *tmp = getenv("TMPDIR");
    if (tmp == NULL || tmp[0] == 0) tmp = "/tmp";
    if (strlen(tmp) >= sizeof base - 64u) return -(int64_t)ENAMETOOLONG;
    (void)strcpy(base, tmp);
    unsigned long id = (unsigned long)getpid();
#endif
    size_t n = strlen(base);
    if (n > 0u && base[n - 1u] != '/' && base[n - 1u] != '\\') base[n++] = '/';
    base[n] = 0;
    for (uint32_t attempt = 0; attempt < 10000u; attempt++) {
        /* base, then "ceesafe-", the process id in decimal, '-' and 8 hex digits (built by hand: n < 4096 - 64). */
        char name[4096];
        uint32_t k = counter++;
        uint64_t mix = ((uint64_t)id << 32) ^ ((uint64_t)k * 0x9E3779B97F4A7C15u) ^ (uint64_t)(uintptr_t)&counter;
        size_t at = n;
        memcpy(name, base, n);
        memcpy(name + at, "ceesafe-", 5u);
        at += 5u;
        char digits[24];
        size_t count = 0;
        unsigned long rest = id;
        do {
            digits[count++] = (char)('0' + rest % 10u);
            rest /= 10u;
        } while (rest != 0u && count < sizeof digits);
        while (count > 0u) name[at++] = digits[--count];
        name[at++] = '-';
        uint32_t tag = (uint32_t)(mix >> 24);
        for (int shift = 28; shift >= 0; shift -= 4) name[at++] = "0123456789abcdef"[(tag >> (unsigned)shift) & 0xFu];
        name[at] = 0;
#ifdef _WIN32
        int made = _mkdir(name);
#else
        int made = mkdir(name, 0700);
#endif
        if (made == 0) return ceesafe_host_copy_out(name, buffer, capacity);
        if (errno != EEXIST) return -(int64_t)errno;
    }
    return -(int64_t)EEXIST;
}

int64_t ceesafe_host_current(uint8_t *buffer, uint64_t capacity) {
    char name[4096];
#ifdef _WIN32
    if (_getcwd(name, (int)sizeof name) == NULL) return -(int64_t)errno;
#else
    if (getcwd(name, sizeof name) == NULL) return -(int64_t)errno;
#endif
    return ceesafe_host_copy_out(name, buffer, capacity);
}

#ifdef _WIN32
/* An errno for a Windows error code (enough to tell NotFound and PermissionDenied apart). */
static int ceesafe_host_win_errno(DWORD code) {
    switch (code) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_BAD_PATHNAME: return ENOENT;
    case ERROR_ACCESS_DENIED: return EACCES;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return ENOMEM;
    default: return EINVAL;
    }
}

/* Appends `argument` to a command line under the C runtime's parsing rules (CommandLineToArgvW). */
static size_t ceesafe_host_quote(char *line, size_t at, size_t capacity, const char *argument, int *ok) {
    int plain = argument[0] != 0;
    for (const char *c = argument; *c; c++) {
        if (*c == ' ' || *c == '\t' || *c == '"') plain = 0;
    }
#define CEESAFE_PUT(ch) do { if (at + 1u >= capacity) { *ok = 0; return at; } line[at++] = (ch); } while (0)
    if (plain) {
        for (const char *c = argument; *c; c++) CEESAFE_PUT(*c);
        return at;
    }
    CEESAFE_PUT('"');
    for (const char *c = argument;; c++) {
        size_t slashes = 0;
        while (*c == '\\') { slashes++; c++; }
        if (*c == 0) {
            for (size_t i = 0; i < slashes * 2u; i++) CEESAFE_PUT('\\');
            break;
        }
        if (*c == '"') {
            for (size_t i = 0; i < slashes * 2u + 1u; i++) CEESAFE_PUT('\\');
        } else {
            for (size_t i = 0; i < slashes; i++) CEESAFE_PUT('\\');
        }
        CEESAFE_PUT(*c);
    }
    CEESAFE_PUT('"');
#undef CEESAFE_PUT
    return at;
}
#endif

/* Runs argv[0] (found on PATH) with arguments argv[1..count), its stdin read from the file at `input` and its stdout
 * and stderr written to the files at `output` and `errors` (created or truncated), and waits: the exit status, or
 * 128 plus the signal that ended it, as ceesafe_process_run reports it. Files rather than pipes, so a child that writes
 * much to both streams cannot block on one while the parent reads the other, without poll or threads (ADR 0028). */
int64_t ceesafe_host_spawn(void const *arguments, uint64_t count, uint8_t const *input, uint64_t input_length,
                        uint8_t const *output, uint64_t output_length, uint8_t const *errors, uint64_t errors_length) {
    const cee_str *texts = (const cee_str *)arguments;
    char in_name[4096], out_name[4096], err_name[4096];
    if (count == 0u || count > 4097u || !ceesafe_c_path(ceesafe_host_text(input, input_length), in_name)
        || !ceesafe_c_path(ceesafe_host_text(output, output_length), out_name)
        || !ceesafe_c_path(ceesafe_host_text(errors, errors_length), err_name)) {
        return -(int64_t)EINVAL;
    }
    char **argv = (char **)ceesafe_alloc((size_t)(count + 1u) * sizeof(char *), CEE_ALLOC_SYSTEM);
    if (argv == NULL) return -(int64_t)ENOMEM;
    int32_t error = 0;
    uint64_t made = 0;
    for (; made < count; made++) {
        argv[made] = ceesafe_c_string(texts[made], &error);
        if (argv[made] == NULL) break;
    }
    int64_t result = 0;
    if (error != 0) {
        result = -(int64_t)error;
    } else {
        argv[count] = NULL;
        (void)fflush(stdout);
#ifdef _WIN32
        size_t capacity = 32768u;
        char *line = (char *)ceesafe_alloc(capacity, CEE_ALLOC_SYSTEM);
        int ok = line != NULL;
        size_t at = 0;
        for (uint64_t i = 0; ok && i < count; i++) {
            if (i > 0u && at + 1u < capacity) line[at++] = ' ';
            at = ceesafe_host_quote(line, at, capacity, argv[i], &ok);
        }
        if (!ok) {
            result = -(int64_t)(line == NULL ? ENOMEM : E2BIG);
        } else {
            line[at] = 0;
            SECURITY_ATTRIBUTES inherit = { sizeof inherit, NULL, TRUE };
            HANDLE in = CreateFileA(in_name, GENERIC_READ, FILE_SHARE_READ, &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            HANDLE out = CreateFileA(out_name, GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            HANDLE err = CreateFileA(err_name, GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (in == INVALID_HANDLE_VALUE || out == INVALID_HANDLE_VALUE || err == INVALID_HANDLE_VALUE) {
                result = -(int64_t)ceesafe_host_win_errno(GetLastError());
            } else {
                STARTUPINFOA startup;
                PROCESS_INFORMATION process;
                memset(&startup, 0, sizeof startup);
                startup.cb = sizeof startup;
                startup.dwFlags = STARTF_USESTDHANDLES;
                startup.hStdInput = in;
                startup.hStdOutput = out;
                startup.hStdError = err;
                if (!CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) {
                    result = -(int64_t)ceesafe_host_win_errno(GetLastError());
                } else {
                    DWORD code = 0;
                    (void)WaitForSingleObject(process.hProcess, INFINITE);
                    result = GetExitCodeProcess(process.hProcess, &code) ? (int64_t)code : -(int64_t)EINVAL;
                    (void)CloseHandle(process.hThread);
                    (void)CloseHandle(process.hProcess);
                }
            }
            if (in != INVALID_HANDLE_VALUE) (void)CloseHandle(in);
            if (out != INVALID_HANDLE_VALUE) (void)CloseHandle(out);
            if (err != INVALID_HANDLE_VALUE) (void)CloseHandle(err);
        }
        if (line != NULL) ceesafe_free(line, CEE_ALLOC_SYSTEM);
#else
        pid_t pid = -1;
        int started;
#ifdef COSMO_AF_INET
        int in = open(in_name, O_RDONLY);
        int out = open(out_name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int err = open(err_name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (in < 0 || out < 0 || err < 0) {
            started = errno;
        } else {
            /* The child receives exactly these handles (CosmoOS libc/include/spawn.h). */
            struct spawn_handle handles[3] = { { 0, in, 0u, 0u }, { 1, out, 0u, 0u }, { 2, err, 0u, 0u } };
            pid = spawnvp(argv[0], (const char *const *)argv, handles, 3);
            started = pid < 0 ? errno : 0;
        }
        if (in >= 0) (void)close(in);
        if (out >= 0) (void)close(out);
        if (err >= 0) (void)close(err);
#else
        posix_spawn_file_actions_t actions;
        started = posix_spawn_file_actions_init(&actions);
        if (started == 0) {
            started = posix_spawn_file_actions_addopen(&actions, 0, in_name, O_RDONLY, 0);
            if (started == 0) started = posix_spawn_file_actions_addopen(&actions, 1, out_name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (started == 0) started = posix_spawn_file_actions_addopen(&actions, 2, err_name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (started == 0) started = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
            (void)posix_spawn_file_actions_destroy(&actions);
        }
#endif
        if (started != 0) {
            result = -(int64_t)started;
        } else {
            int status = 0;
            pid_t waited;
            do {
                waited = waitpid(pid, &status, 0);
            } while (waited < 0 && errno == EINTR);
            result = waited < 0 ? -(int64_t)errno : WIFEXITED(status) ? (int64_t)WEXITSTATUS(status) : (int64_t)(128 + WTERMSIG(status));
        }
#endif
    }
    for (uint64_t i = 0; i < made; i++) ceesafe_free(argv[i], CEE_ALLOC_SYSTEM);
    ceesafe_free(argv, CEE_ALLOC_SYSTEM);
    return result;
}

