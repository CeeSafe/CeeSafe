/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * CeeSafe runtime: program arguments, files and TCP sockets (type-system.md §26) for hosted C
 * environments (Linux, macOS) and CosmoOS userland. Only public C library functions (L1, OD-15).
 * Windows (MSVC, best effort; spec/c-backend.md §1.1): files and processes through its C runtime;
 * TCP through Winsock 2, its sockets behind a table of ids. Host services (type-system.md §26.7-§26.10, ADR 0028) work on all three.
 */
#if defined(__APPLE__) && !defined(__APPLE_USE_RFC_3542)
#define __APPLE_USE_RFC_3542 1 /* IPV6_RECVPKTINFO and IPV6_RECVTCLASS (ADR 0050) */
#endif
#if defined(__linux__) && !defined(_GNU_SOURCE)
/* pread, for a read at an offset (ADR 0051), under glibc's strict C11 headers, as ceesafe_rt_thread.c does. */
#define _GNU_SOURCE
#endif
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if defined(_WIN32) && !defined(_CRT_RAND_S)
#define _CRT_RAND_S /* rand_s, for HashMap's key (ADR 0052) */
#endif
#include "ceesafe/rt.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <wchar.h>
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <dirent.h>
#include <sys/statvfs.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h> /* struct iovec, for a datagram's receive (ADR 0050) */
#include <sys/wait.h>
#include <unistd.h>
#if defined(__has_include)
#if __has_include(<netinet/tcp.h>)
#include <netinet/tcp.h>
#endif
#if __has_include(<net/if.h>)
#include <net/if.h>
#endif
#endif
#endif

#if !defined(COSMO_AF_INET) && !defined(_WIN32)
extern char **environ;
#endif
#if !defined(_WIN32) && !defined(O_NOCTTY)
#define O_NOCTTY 0 /* a system without controlling terminals (ADR 0078's File.openRegular) */
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
    case EINVAL: case ENAMETOOLONG: case EMSGSIZE: return CEE_IO_INVALID_INPUT; /* EMSGSIZE: a datagram too long */
    case EPIPE: return CEE_IO_BROKEN_PIPE;
    case ECONNREFUSED: return CEE_IO_CONNECTION_REFUSED;
    case EADDRINUSE: return CEE_IO_ADDRESS_IN_USE;
    case ENOMEM: return CEE_IO_OUT_OF_MEMORY;
    case ECONNRESET: return CEE_IO_CONNECTION_RESET;
    case ECONNABORTED: return CEE_IO_CONNECTION_ABORTED;
    case ENOTCONN: return CEE_IO_NOT_CONNECTED;
    case ETIMEDOUT: return CEE_IO_TIMED_OUT;
    case EHOSTUNREACH: return CEE_IO_HOST_UNREACHABLE;
    case ENETUNREACH: return CEE_IO_NETWORK_UNREACHABLE;
    case EADDRNOTAVAIL: return CEE_IO_ADDRESS_NOT_AVAILABLE;
    case EOPNOTSUPP: case ENOPROTOOPT: case EAFNOSUPPORT: case EPROTONOSUPPORT: return CEE_IO_UNSUPPORTED;
#if defined(ENOTSUP) && ENOTSUP != EOPNOTSUPP
    case ENOTSUP: return CEE_IO_UNSUPPORTED;
#endif
    case CEE_ERROR_HOST_NOT_FOUND: return CEE_IO_HOST_NOT_FOUND; /* the resolver's, not an errno (ADR 0060) */
    case ENOSPC: return CEE_IO_STORAGE_FULL; /* ADR 0077 */
#ifdef EDQUOT
    case EDQUOT: return CEE_IO_STORAGE_FULL;
#endif
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

/* An open descriptor's kind (ADR 0078 §3): 1 a regular file, 2 a directory, 3 anything else, or a negated errno. */
static int64_t cee_fd_kind(int fd) {
#ifdef _WIN32
    struct _stat64 st;
    if (_fstat64(fd, &st) != 0) return -(int64_t)errno;
    unsigned type = (unsigned)st.st_mode & _S_IFMT;
    return type == _S_IFDIR ? 2 : type == _S_IFREG ? 1 : 3;
#else
    struct stat st;
    if (fstat(fd, &st) != 0) return -(int64_t)errno;
#ifdef COSMO_AF_INET
    /* CosmoOS's S_ISDIR and S_ISREG test the record's DT_* type, st_type, not st_mode (its sys/stat.h). */
    return S_ISDIR(st.st_type) ? 2 : S_ISREG(st.st_type) ? 1 : 3;
#else
    return S_ISDIR(st.st_mode) ? 2 : S_ISREG(st.st_mode) ? 1 : 3;
#endif
#endif
}

cee_io_result ceesafe_file_open(cee_str path, uint32_t mode) {
    char name[4096];
    if (!ceesafe_c_path(path, name)) return ceesafe_io_fail(EINVAL);
    int fd;
    do {
#ifdef _WIN32
        int flags = mode == 0u ? _O_RDONLY
                  : mode == 1u ? _O_WRONLY | _O_CREAT | _O_TRUNC
                  : mode == 2u ? _O_RDWR
                  : mode == 3u ? _O_RDWR | _O_CREAT
                  : mode == 5u ? _O_WRONLY | _O_CREAT | _O_APPEND
                  : mode == 6u ? _O_RDONLY /* ADR 0078: File.openRegular; opening never waits on Windows */
                               : _O_RDWR | _O_CREAT | _O_EXCL;
        fd = _open(name, flags | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
        /* Modes 2-4 (ADR 0075 §2.1): read and write; 3 creates a missing file; 4 creates and fails if one exists.
           A created file gets 0666 & ~umask. */
        int flags = mode == 0u ? O_RDONLY
                  : mode == 1u ? O_WRONLY | O_CREAT | O_TRUNC
                  : mode == 2u ? O_RDWR
                  : mode == 3u ? O_RDWR | O_CREAT
                  : mode == 5u ? O_WRONLY | O_CREAT | O_APPEND /* ADR 0077: File.openAppend */
                  /* ADR 0078: File.openRegular. O_NONBLOCK so that opening a FIFO never waits for a writer, O_NOCTTY
                     so that a terminal never becomes the controlling one; cleared below for a regular file. */
                  : mode == 6u ? O_RDONLY | O_NONBLOCK | O_NOCTTY
                               : O_RDWR | O_CREAT | O_EXCL;
        fd = open(name, flags | O_CLOEXEC, mode == 1u ? 0644 : 0666);
#endif
    } while (fd < 0 && errno == EINTR);
#ifdef _WIN32
    if (fd < 0 && mode == 6u && errno == EACCES) {
        /* Windows refuses to open a directory (EACCES); File.openRegular reports IsDirectory there, as elsewhere. No
           file is read, so asking the path after the failed open races with nothing. */
        struct _stat64 st;
        if (_stat64(name, &st) == 0 && ((unsigned)st.st_mode & _S_IFMT) == _S_IFDIR) return ceesafe_io_fail(EISDIR);
        errno = EACCES;
    }
#endif
    if (fd < 0) return ceesafe_io_errno();
    if (mode == 6u) {
        /* ADR 0078 §3: only a regular file is kept; the kind is the open file's (fstat), so nothing renamed into the
           path between a check and the open is ever read. A directory is IsDirectory, anything else InvalidInput. */
        int64_t kind = cee_fd_kind(fd);
        if (kind != 1) {
            int error = kind < 0 ? (int)-kind : kind == 2 ? EISDIR : EINVAL;
#ifdef _WIN32
            (void)_close(fd);
#else
            (void)close(fd);
#endif
            return ceesafe_io_fail(error);
        }
#ifndef _WIN32
        int status = fcntl(fd, F_GETFL);
        if (status < 0 || fcntl(fd, F_SETFL, status & ~O_NONBLOCK) < 0) {
            int error = errno;
            (void)close(fd);
            return ceesafe_io_fail(error);
        }
#endif
    }
    return ceesafe_io_ok(fd);
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

/* ---- Durable files (ADR 0075 §2.1) ------------------------------------------------------- */

/* One read of up to `length` bytes at `offset`, not moving the file position; 0 at or past the end. */
cee_io_result ceesafe_fd_read_at(int32_t fd, uint8_t *buffer, uint64_t length, uint64_t offset) {
    size_t chunk = length > (1u << 30) ? (size_t)1u << 30 : (size_t)length;
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return ceesafe_io_fail(EBADF);
    OVERLAPPED at = { 0 };
    at.Offset = (DWORD)offset;
    at.OffsetHigh = (DWORD)(offset >> 32);
    DWORD n = 0;
    if (!ReadFile(h, buffer, (DWORD)chunk, &n, &at)) {
        DWORD error = GetLastError();
        if (error == ERROR_HANDLE_EOF) return ceesafe_io_ok(0);
        return ceesafe_io_fail(error == ERROR_ACCESS_DENIED ? EACCES : EIO);
    }
    return ceesafe_io_ok((int64_t)n);
#else
    if (offset > (uint64_t)INT64_MAX) return ceesafe_io_fail(EINVAL);
    ssize_t n;
    do {
        n = pread(fd, buffer, chunk, (off_t)offset);
    } while (n < 0 && errno == EINTR);
    return n < 0 ? ceesafe_io_errno() : ceesafe_io_ok((int64_t)n);
#endif
}

/* Writes all `length` bytes at `offset`, not moving the file position, or fails. */
cee_io_result ceesafe_fd_write_at(int32_t fd, const uint8_t *bytes, uint64_t length, uint64_t offset) {
    if (offset > (uint64_t)INT64_MAX || length > (uint64_t)INT64_MAX - offset) return ceesafe_io_fail(EINVAL);
    uint64_t done = 0;
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return ceesafe_io_fail(EBADF);
#endif
    while (done < length) {
        uint64_t rest = length - done;
        size_t chunk = rest > (1u << 30) ? (size_t)1u << 30 : (size_t)rest;
        uint64_t at = offset + done;
#ifdef _WIN32
        OVERLAPPED where = { 0 };
        where.Offset = (DWORD)at;
        where.OffsetHigh = (DWORD)(at >> 32);
        DWORD n = 0;
        if (!WriteFile(h, bytes + done, (DWORD)chunk, &n, &where)) {
            DWORD error = GetLastError();
            return ceesafe_io_fail(error == ERROR_DISK_FULL || error == ERROR_HANDLE_DISK_FULL ? ENOSPC
                               : error == ERROR_ACCESS_DENIED                              ? EACCES
                                                                                           : EIO);
        }
#else
        ssize_t n = pwrite(fd, bytes + done, chunk, (off_t)at);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return ceesafe_io_errno();
#endif
        if (n == 0) return ceesafe_io_fail(EIO);
        done += (uint64_t)n;
    }
    return ceesafe_io_ok(0);
}

/* `all` 0: the data and the metadata needed to read it back (fdatasync); 1: all metadata too (fsync). Apple systems
   use F_FULLFSYNC, because their fsync does not flush the drive's cache, and fall back to fsync only where the file
   system refuses it. After a failure the file's durable contents are unknown (type-system.md §26.3). */
cee_io_result ceesafe_fd_sync(int32_t fd, uint32_t all) {
#ifdef _WIN32
    (void)all;
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return ceesafe_io_fail(EBADF);
    return FlushFileBuffers(h) ? ceesafe_io_ok(0) : ceesafe_io_fail(EIO);
#elif defined(__APPLE__)
    (void)all;
    if (fcntl(fd, F_FULLFSYNC) == 0) return ceesafe_io_ok(0);
    if (errno != ENOTSUP && errno != EINVAL) return ceesafe_io_errno();
    int r;
    do {
        r = fsync(fd);
    } while (r < 0 && errno == EINTR);
    return r < 0 ? ceesafe_io_errno() : ceesafe_io_ok(0);
#else
    int r;
    do {
#if defined(__linux__)
        r = all ? fsync(fd) : fdatasync(fd);
#else
        (void)all;
        r = fsync(fd);
#endif
    } while (r < 0 && errno == EINTR);
    return r < 0 ? ceesafe_io_errno() : ceesafe_io_ok(0);
#endif
}

/* The file's size in bytes. */
cee_io_result ceesafe_fd_length(int32_t fd) {
#ifdef _WIN32
    struct _stat64 st;
    if (_fstat64(fd, &st) != 0) return ceesafe_io_errno();
#else
    struct stat st;
    if (fstat(fd, &st) != 0) return ceesafe_io_errno();
#endif
    return ceesafe_io_ok((int64_t)st.st_size);
}

/* Shrinks the file to `length` bytes, or extends it with zeros. */
cee_io_result ceesafe_fd_set_length(int32_t fd, uint64_t length) {
    if (length > (uint64_t)INT64_MAX) return ceesafe_io_fail(EINVAL);
#ifdef _WIN32
    errno_t r = _chsize_s(fd, (__int64)length);
    return r != 0 ? ceesafe_io_fail(r) : ceesafe_io_ok(0);
#else
    int r;
    do {
        r = ftruncate(fd, (off_t)length);
    } while (r < 0 && errno == EINTR);
    return r < 0 ? ceesafe_io_errno() : ceesafe_io_ok(0);
#endif
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

/* ---- Socket addresses (ADR 0044): IPv4 and IPv6 endpoints, in one form for every platform ------------------- */

static int cee_hex_value(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* IPv6 text (RFC 4291 §2.2: eight groups of up to four hex digits, at most one "::", an embedded dotted IPv4 last)
   to its sixteen bytes; false if malformed. No zone: lib/std's SocketAddress.parse reads those. */
static bool cee_ipv6(cee_str text, uint8_t out[16]) {
    uint16_t groups[8];
    int count = 0;
    int gap = -1; /* the group index "::" stands before */
    uint64_t i = 0;
    if (text.len >= 2u && text.ptr[0] == ':' && text.ptr[1] == ':') {
        gap = 0;
        i = 2;
    } else if (text.len == 0u || text.ptr[0] == ':') {
        return false;
    }
    while (i < text.len) {
        uint64_t start = i;
        uint32_t value = 0;
        int digits = 0;
        while (i < text.len && cee_hex_value(text.ptr[i]) >= 0 && digits < 5) {
            value = value * 16u + (uint32_t)cee_hex_value(text.ptr[i]);
            i++;
            digits++;
        }
        if (i < text.len && text.ptr[i] == '.') {
            /* The last 32 bits as dotted IPv4. */
            uint8_t v4[4];
            cee_str rest = {text.ptr + start, text.len - start};
            if (count > 6 || !ceesafe_ipv4(rest, v4)) return false;
            groups[count++] = (uint16_t)((v4[0] << 8) | v4[1]);
            groups[count++] = (uint16_t)((v4[2] << 8) | v4[3]);
            i = text.len;
            break;
        }
        if (digits == 0 || digits > 4 || count == 8) return false;
        groups[count++] = (uint16_t)value;
        if (i == text.len) break;
        if (text.ptr[i] != ':') return false;
        i++;
        if (i < text.len && text.ptr[i] == ':') {
            if (gap >= 0) return false;
            gap = count;
            i++;
        } else if (i == text.len) {
            return false; /* a single trailing colon */
        }
    }
    if (gap < 0 ? count != 8 : count > 7) return false;
    int at = 0;
    for (int k = 0; k < 8; k++) {
        uint16_t g = 0;
        if (gap < 0 || k < gap) {
            g = groups[at++];
        } else if (k >= gap + (8 - count)) {
            g = groups[at++];
        }
        out[2 * k] = (uint8_t)(g >> 8);
        out[2 * k + 1] = (uint8_t)g;
    }
    return true;
}

bool ceesafe_endpoint_make(uint32_t family, const uint8_t *bytes, uint16_t port, uint32_t scope, ceesafe_endpoint *out) {
    ceesafe_mem_set(out, 0, sizeof *out);
    if (family != 4u && family != 6u) return false;
    out->family = family;
    out->port = port;
    out->scope = family == 6u ? scope : 0u;
    for (uint32_t k = 0; k < (family == 6u ? 16u : 4u); k++) out->bytes[k] = bytes[k];
    return true;
}

bool ceesafe_endpoint_parse(cee_str text, uint16_t port, ceesafe_endpoint *out) {
    ceesafe_mem_set(out, 0, sizeof *out);
    out->port = port;
    if (ceesafe_ipv4(text, out->bytes)) {
        out->family = 4u;
        return true;
    }
    if (cee_ipv6(text, out->bytes)) {
        out->family = 6u;
        return true;
    }
    return false;
}

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

/* Winsock started once for the process, for ceesafe_rt_async.c's resolver (ADR 0048). */
bool ceesafe_winsock_ready(void) { return cee_wsa_ready(); }

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
    case WSAENETUNREACH: return ENETUNREACH;
    case WSAEHOSTUNREACH: return EHOSTUNREACH;
    case WSAEOPNOTSUPP: return EOPNOTSUPP;
    case WSAENOPROTOOPT: return ENOPROTOOPT;
    case WSAEAFNOSUPPORT: return EAFNOSUPPORT;
    case WSAEPROTONOSUPPORT: return EPROTONOSUPPORT;
    case WSAEACCES: return EACCES;
    case WSAEINVAL: case WSAEFAULT: case WSAEMSGSIZE: return EINVAL;
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

typedef union {
    struct sockaddr sa;
    struct sockaddr_in v4;
    struct sockaddr_in6 v6;
} cee_sockaddr;

uint32_t ceesafe_endpoint_sockaddr(const ceesafe_endpoint *e, void *out, uint32_t room) {
    if (room < sizeof(cee_sockaddr)) return 0;
    cee_sockaddr *a = (cee_sockaddr *)out;
    ceesafe_mem_set(a, 0, sizeof *a);
    if (e->family == 4u) {
        a->v4.sin_family = AF_INET;
        a->v4.sin_port = htons(e->port);
        memcpy(&a->v4.sin_addr, e->bytes, 4);
        return (uint32_t)sizeof a->v4;
    }
    if (e->family == 6u) {
        a->v6.sin6_family = AF_INET6;
        a->v6.sin6_port = htons(e->port);
        a->v6.sin6_scope_id = e->scope;
        memcpy(&a->v6.sin6_addr, e->bytes, 16);
        return (uint32_t)sizeof a->v6;
    }
    return 0;
}

/* A sockaddr as an endpoint; an IPv4-mapped IPv6 address (::ffff:a.b.c.d) as IPv4 (ADR 0044 §3.2). */
static bool cee_endpoint_from(const cee_sockaddr *a, ceesafe_endpoint *e) {
    ceesafe_mem_set(e, 0, sizeof *e);
    if (a->sa.sa_family == AF_INET) {
        e->family = 4u;
        e->port = ntohs(a->v4.sin_port);
        memcpy(e->bytes, &a->v4.sin_addr, 4);
        return true;
    }
    if (a->sa.sa_family != AF_INET6) return false;
    const uint8_t *b = (const uint8_t *)&a->v6.sin6_addr;
    e->port = ntohs(a->v6.sin6_port);
    bool mapped = b[10] == 0xffu && b[11] == 0xffu;
    for (int k = 0; k < 10 && mapped; k++) mapped = b[k] == 0u;
    if (mapped) {
        e->family = 4u;
        memcpy(e->bytes, b + 12, 4);
        return true;
    }
    e->family = 6u;
    e->scope = (uint32_t)a->v6.sin6_scope_id;
    memcpy(e->bytes, b, 16);
    return true;
}

/* A new socket, bound and listening (with `backlog` and the CEE_LISTEN_* `flags`), or connected; nonblocking first
   if asked. Winsock's SO_REUSEADDR lets another socket take a bound port over, unlike POSIX's, so reuseAddress is
   not applied (ADR 0044 §3.2); SO_REUSEPORT does not exist: refused. */
static cee_io_result cee_socket_open(const ceesafe_endpoint *e, uint32_t backlog, uint32_t flags, bool listening,
                                     bool nonblocking) {
    cee_sockaddr a;
    int length = (int)ceesafe_endpoint_sockaddr(e, &a, (uint32_t)sizeof a);
    if (length == 0) return ceesafe_io_fail(EAFNOSUPPORT);
    if (listening && (flags & CEE_LISTEN_REUSE_PORT) != 0u) return ceesafe_io_fail(EOPNOTSUPP);
    if (!cee_wsa_ready()) return ceesafe_io_fail(ENOSYS);
    SOCKET s = socket(e->family == 6u ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return cee_wsa_fail();
    u_long on = 1;
    /* FIONBIO is an unsigned long expression and ioctlsocket takes a long: the cast keeps -Wsign-conversion quiet
       (CeeSafe.Web T3); the value is the SDK's own. */
    if (nonblocking && ioctlsocket(s, (long)FIONBIO, &on) != 0) {
        cee_io_result failed = cee_wsa_fail();
        (void)closesocket(s);
        return failed;
    }
    if (listening && e->family == 6u) {
        DWORD only = (flags & CEE_LISTEN_V6_ONLY) != 0u ? 1u : 0u;
        if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&only, (int)sizeof only) != 0) {
            cee_io_result failed = cee_wsa_fail();
            (void)closesocket(s);
            return failed;
        }
    }
    int status = listening ? bind(s, &a.sa, length) : connect(s, &a.sa, length);
    if (status == 0 && listening) status = listen(s, backlog == 0u || backlog > (uint32_t)INT32_MAX ? SOMAXCONN : (int)backlog);
    if (status != 0 && !(nonblocking && !listening && WSAGetLastError() == WSAEWOULDBLOCK)) {
        cee_io_result failed = cee_wsa_fail();
        (void)closesocket(s);
        return failed;
    }
    int32_t id = cee_socket_add(s);
    return id < 0 ? ceesafe_io_fail(ENOMEM) : ceesafe_io_ok(id);
}

static cee_io_result cee_socket_open_text(cee_str address, uint16_t port, bool listening, bool nonblocking) {
    ceesafe_endpoint e;
    if (!ceesafe_endpoint_parse(address, port, &e)) return ceesafe_io_fail(EINVAL);
    return cee_socket_open(&e, 0u, CEE_LISTEN_REUSE_ADDRESS, listening, nonblocking);
}

cee_io_result ceesafe_tcp_listen(cee_str address, uint16_t port) { return cee_socket_open_text(address, port, true, false); }

cee_io_result ceesafe_tcp_port(int32_t fd) {
    SOCKET s = (SOCKET)ceesafe_socket_handle(fd);
    if (s == INVALID_SOCKET) return ceesafe_io_fail(EBADF);
    cee_sockaddr a;
    int length = (int)sizeof a;
    ceesafe_endpoint e;
    if (getsockname(s, &a.sa, &length) != 0) return cee_wsa_fail();
    return cee_endpoint_from(&a, &e) ? ceesafe_io_ok(e.port) : ceesafe_io_fail(EAFNOSUPPORT);
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
    if (ioctlsocket(peer, (long)FIONBIO, &mode) != 0) {
        cee_io_result failed = cee_wsa_fail();
        (void)closesocket(peer);
        return failed;
    }
    int32_t id = cee_socket_add(peer);
    return id < 0 ? ceesafe_io_fail(ENOMEM) : ceesafe_io_ok(id);
}

cee_io_result ceesafe_tcp_accept(int32_t fd) { return cee_socket_accept(fd, false); }

cee_io_result ceesafe_tcp_connect(cee_str address, uint16_t port) { return cee_socket_open_text(address, port, false, false); }

/* ADR 0044's options: setsockopt, and WSAIoctl for the keep-alive's idle time. */
static int32_t cee_option_set(SOCKET s, uint32_t option, int64_t value) {
    switch (option) {
    case CEE_SOCKET_OPTION_NO_DELAY: {
        BOOL on = value != 0;
        return setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, (int)sizeof on) != 0 ? cee_wsa_errno(WSAGetLastError()) : 0;
    }
    case CEE_SOCKET_OPTION_KEEP_ALIVE: {
        if (value > (int64_t)(UINT32_MAX / 1000u)) return EINVAL;
        BOOL on = value >= 0;
        if (setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&on, (int)sizeof on) != 0) return cee_wsa_errno(WSAGetLastError());
        if (value > 0) {
            struct tcp_keepalive k = {1u, (ULONG)value * 1000u, 1000u};
            DWORD returned = 0;
            if (WSAIoctl(s, SIO_KEEPALIVE_VALS, &k, sizeof k, NULL, 0, &returned, NULL, NULL) != 0) {
                return cee_wsa_errno(WSAGetLastError());
            }
        }
        return 0;
    }
    case CEE_SOCKET_OPTION_SEND_BUFFER:
    case CEE_SOCKET_OPTION_RECEIVE_BUFFER: {
        if (value < 0 || value > INT32_MAX) return EINVAL;
        int size = (int)value;
        int name = option == CEE_SOCKET_OPTION_SEND_BUFFER ? SO_SNDBUF : SO_RCVBUF;
        return setsockopt(s, SOL_SOCKET, name, (const char *)&size, (int)sizeof size) != 0 ? cee_wsa_errno(WSAGetLastError()) : 0;
    }
    case CEE_SOCKET_OPTION_LINGER: {
        if (value > 65535) return EINVAL; /* Winsock's l_linger is a u_short */
        struct linger l;
        l.l_onoff = value >= 0 ? 1 : 0;
        l.l_linger = (u_short)(value >= 0 ? value : 0);
        return setsockopt(s, SOL_SOCKET, SO_LINGER, (const char *)&l, (int)sizeof l) != 0 ? cee_wsa_errno(WSAGetLastError()) : 0;
    }
    default:
        return EINVAL;
    }
}

cee_io_result ceesafe_tcp_option(int32_t fd, uint32_t option, int64_t value) {
    SOCKET s = (SOCKET)ceesafe_socket_handle(fd);
    if (s == INVALID_SOCKET) return ceesafe_io_fail(EBADF);
    int32_t error = cee_option_set(s, option, value);
    return error != 0 ? ceesafe_io_fail(error) : ceesafe_io_ok(0);
}

cee_io_result ceesafe_tcp_shutdown_read(int32_t fd) {
    SOCKET s = (SOCKET)ceesafe_socket_handle(fd);
    if (s == INVALID_SOCKET) return ceesafe_io_fail(EBADF);
    return shutdown(s, SD_RECEIVE) != 0 ? cee_wsa_fail() : ceesafe_io_ok(0);
}

cee_io_result ceesafe_tcp_shutdown_write(int32_t fd) {
    SOCKET s = (SOCKET)ceesafe_socket_handle(fd);
    if (s == INVALID_SOCKET) return ceesafe_io_fail(EBADF);
    return shutdown(s, SD_SEND) != 0 ? cee_wsa_fail() : ceesafe_io_ok(0);
}
#else
#ifdef COSMO_AF_INET
/* CosmoOS's own address layout: host-order port, address bytes in network order (sys/socket.h), 16 of them for
   AF_INET6. It has no setsockopt or getpeername: the options and the peer's address are EOPNOTSUPP there. */
typedef union {
    struct sockaddr sa;
} cee_sockaddr;

uint32_t ceesafe_endpoint_sockaddr(const ceesafe_endpoint *e, void *out, uint32_t room) {
    if (room < sizeof(cee_sockaddr) || (e->family != 4u && e->family != 6u)) return 0;
    cee_sockaddr *a = (cee_sockaddr *)out;
    ceesafe_mem_set(a, 0, sizeof *a);
    a->sa.sa_family = e->family == 6u ? AF_INET6 : AF_INET;
    a->sa.sa_port = e->port;
    memcpy(a->sa.sa_addr, e->bytes, e->family == 6u ? 16u : 4u);
    return (uint32_t)sizeof a->sa;
}

static bool cee_endpoint_from(const cee_sockaddr *a, ceesafe_endpoint *e) {
    ceesafe_mem_set(e, 0, sizeof *e);
    if (a->sa.sa_family != AF_INET && a->sa.sa_family != AF_INET6) return false;
    e->family = a->sa.sa_family == AF_INET6 ? 6u : 4u;
    e->port = a->sa.sa_port;
    memcpy(e->bytes, a->sa.sa_addr, e->family == 6u ? 16u : 4u);
    return true;
}
#define CEE_NO_SETSOCKOPT 1
#ifndef SOMAXCONN
#define SOMAXCONN 128
#endif
#else
typedef union {
    struct sockaddr sa;
    struct sockaddr_in v4;
    struct sockaddr_in6 v6;
} cee_sockaddr;

uint32_t ceesafe_endpoint_sockaddr(const ceesafe_endpoint *e, void *out, uint32_t room) {
    if (room < sizeof(cee_sockaddr)) return 0;
    cee_sockaddr *a = (cee_sockaddr *)out;
    ceesafe_mem_set(a, 0, sizeof *a);
    if (e->family == 4u) {
#ifdef __APPLE__
        a->v4.sin_len = (uint8_t)sizeof a->v4;
#endif
        a->v4.sin_family = AF_INET;
        a->v4.sin_port = htons(e->port);
        memcpy(&a->v4.sin_addr, e->bytes, 4);
        return (uint32_t)sizeof a->v4;
    }
    if (e->family == 6u) {
#ifdef __APPLE__
        a->v6.sin6_len = (uint8_t)sizeof a->v6;
#endif
        a->v6.sin6_family = AF_INET6;
        a->v6.sin6_port = htons(e->port);
        a->v6.sin6_scope_id = e->scope;
        memcpy(&a->v6.sin6_addr, e->bytes, 16);
        return (uint32_t)sizeof a->v6;
    }
    return 0;
}

/* A sockaddr as an endpoint; an IPv4-mapped IPv6 address (::ffff:a.b.c.d) as IPv4 (ADR 0044 §3.2). */
static bool cee_endpoint_from(const cee_sockaddr *a, ceesafe_endpoint *e) {
    ceesafe_mem_set(e, 0, sizeof *e);
    if (a->sa.sa_family == AF_INET) {
        e->family = 4u;
        e->port = ntohs(a->v4.sin_port);
        memcpy(e->bytes, &a->v4.sin_addr, 4);
        return true;
    }
    if (a->sa.sa_family != AF_INET6) return false;
    const uint8_t *b = (const uint8_t *)&a->v6.sin6_addr;
    e->port = ntohs(a->v6.sin6_port);
    bool mapped = b[10] == 0xffu && b[11] == 0xffu;
    for (int k = 0; k < 10 && mapped; k++) mapped = b[k] == 0u;
    if (mapped) {
        e->family = 4u;
        memcpy(e->bytes, b + 12, 4);
        return true;
    }
    e->family = 6u;
    e->scope = (uint32_t)a->v6.sin6_scope_id;
    memcpy(e->bytes, b, 16);
    return true;
}
#endif

/* A new TCP socket of the endpoint's family; on macOS, writes to a closed peer report EPIPE instead of raising
   SIGPIPE. */
int64_t ceesafe_tcp_socket_for(const ceesafe_endpoint *e) {
    int fd = socket(e->family == 6u ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -(int64_t)errno;
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return fd;
}

static bool cee_nonblocking(int fd);

/* A socket bound to `e` and listening (`backlog`, 0: SOMAXCONN; the CEE_LISTEN_* `flags`), nonblocking if asked: the
   descriptor, or a negated errno. V6 sockets always get IPV6_V6ONLY set explicitly, so a program behaves alike where
   the system's default differs (ADR 0044 §3.2). */
static int64_t cee_listen_endpoint(const ceesafe_endpoint *e, uint32_t backlog, uint32_t flags, bool nonblocking) {
    cee_sockaddr a;
    uint32_t length = ceesafe_endpoint_sockaddr(e, &a, (uint32_t)sizeof a);
    if (length == 0) return -(int64_t)EAFNOSUPPORT;
#if defined(CEE_NO_SETSOCKOPT) || !defined(SO_REUSEPORT)
    if ((flags & CEE_LISTEN_REUSE_PORT) != 0u) return -(int64_t)EOPNOTSUPP;
#endif
#if defined(CEE_NO_SETSOCKOPT) || !defined(IPV6_V6ONLY)
    if (e->family == 6u && (flags & CEE_LISTEN_V6_ONLY) != 0u) return -(int64_t)EOPNOTSUPP;
#endif
    int64_t made = ceesafe_tcp_socket_for(e);
    if (made < 0) return made;
    int fd = (int)made;
    int failure = 0;
#if !defined(CEE_NO_SETSOCKOPT)
    int one = 1;
    if ((flags & CEE_LISTEN_REUSE_ADDRESS) != 0u) (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    if ((flags & CEE_LISTEN_REUSE_PORT) != 0u && setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) < 0) failure = errno;
#endif
#ifdef IPV6_V6ONLY
    int only = (flags & CEE_LISTEN_V6_ONLY) != 0u ? 1 : 0;
    if (failure == 0 && e->family == 6u && setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &only, sizeof only) < 0) failure = errno;
#endif
#endif
    if (failure == 0 && nonblocking && !cee_nonblocking(fd)) failure = errno;
    int depth = backlog == 0u || backlog > (uint32_t)SOMAXCONN ? SOMAXCONN : (int)backlog;
    if (failure == 0 && (bind(fd, &a.sa, (socklen_t)length) < 0 || listen(fd, depth) < 0)) failure = errno;
    if (failure != 0) {
        (void)close(fd);
        return -(int64_t)failure;
    }
    return fd;
}

/* A socket connecting to `e`: the descriptor once connected (blocking), or at once with the connection in progress
   (nonblocking; ceesafe_async_tcp_connected says when it is made); or a negated errno. */
static int64_t cee_connect_endpoint(const ceesafe_endpoint *e, bool nonblocking) {
    cee_sockaddr a;
    uint32_t length = ceesafe_endpoint_sockaddr(e, &a, (uint32_t)sizeof a);
    if (length == 0) return -(int64_t)EAFNOSUPPORT;
    int64_t made = ceesafe_tcp_socket_for(e);
    if (made < 0) return made;
    int fd = (int)made;
    if (nonblocking && !cee_nonblocking(fd)) {
        int64_t failed = -(int64_t)errno;
        (void)close(fd);
        return failed;
    }
    if (connect(fd, &a.sa, (socklen_t)length) < 0 && !(nonblocking && (errno == EINPROGRESS || errno == EINTR))) {
        int64_t failed = -(int64_t)errno;
        (void)close(fd);
        return failed;
    }
    return fd;
}

static cee_io_result cee_io_of(int64_t r) { return r < 0 ? ceesafe_io_fail((int32_t)-r) : ceesafe_io_ok(r); }

cee_io_result ceesafe_tcp_listen(cee_str address, uint16_t port) {
    ceesafe_endpoint e;
    if (!ceesafe_endpoint_parse(address, port, &e)) return ceesafe_io_fail(EINVAL);
    return cee_io_of(cee_listen_endpoint(&e, 0u, CEE_LISTEN_REUSE_ADDRESS, false));
}

cee_io_result ceesafe_tcp_port(int32_t fd) {
    cee_sockaddr a;
    socklen_t length = sizeof a;
    ceesafe_endpoint e;
    if (getsockname(fd, &a.sa, &length) < 0) return ceesafe_io_errno();
    return cee_endpoint_from(&a, &e) ? ceesafe_io_ok(e.port) : ceesafe_io_fail(EAFNOSUPPORT);
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
    ceesafe_endpoint e;
    if (!ceesafe_endpoint_parse(address, port, &e)) return ceesafe_io_fail(EINVAL);
    return cee_io_of(cee_connect_endpoint(&e, false));
}

cee_io_result ceesafe_tcp_shutdown_write(int32_t fd) {
    return shutdown(fd, SHUT_WR) < 0 ? ceesafe_io_errno() : ceesafe_io_ok(0);
}

cee_io_result ceesafe_tcp_shutdown_read(int32_t fd) {
    return shutdown(fd, SHUT_RD) < 0 ? ceesafe_io_errno() : ceesafe_io_ok(0);
}

/* ADR 0044's options: 0, or an errno. */
static int32_t cee_option_set(int fd, uint32_t option, int64_t value) {
#if defined(CEE_NO_SETSOCKOPT)
    (void)fd;
    (void)option;
    (void)value;
    return EOPNOTSUPP;
#else
    switch (option) {
    case CEE_SOCKET_OPTION_NO_DELAY: {
#ifdef TCP_NODELAY
        int on = value != 0 ? 1 : 0;
        return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on) < 0 ? errno : 0;
#else
        return EOPNOTSUPP;
#endif
    }
    case CEE_SOCKET_OPTION_KEEP_ALIVE: {
        if (value > INT32_MAX) return EINVAL;
        int on = value >= 0 ? 1 : 0;
        if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on) < 0) return errno;
        if (value > 0) {
            int idle = (int)value;
#if defined(TCP_KEEPIDLE)
            if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle) < 0) return errno;
#elif defined(TCP_KEEPALIVE)
            if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof idle) < 0) return errno; /* macOS: seconds */
#else
            (void)idle;
            return EOPNOTSUPP;
#endif
        }
        return 0;
    }
    case CEE_SOCKET_OPTION_SEND_BUFFER:
    case CEE_SOCKET_OPTION_RECEIVE_BUFFER: {
        if (value < 0 || value > INT32_MAX) return EINVAL;
        int size = (int)value;
        int name = option == CEE_SOCKET_OPTION_SEND_BUFFER ? SO_SNDBUF : SO_RCVBUF;
        return setsockopt(fd, SOL_SOCKET, name, &size, sizeof size) < 0 ? errno : 0;
    }
    case CEE_SOCKET_OPTION_LINGER: {
        if (value > INT32_MAX) return EINVAL;
        struct linger l;
        l.l_onoff = value >= 0 ? 1 : 0;
        l.l_linger = value >= 0 ? (int)value : 0;
#ifdef SO_LINGER_SEC
        int name = SO_LINGER_SEC; /* macOS: SO_LINGER counts clock ticks */
#else
        int name = SO_LINGER;
#endif
        return setsockopt(fd, SOL_SOCKET, name, &l, sizeof l) < 0 ? errno : 0;
    }
    default:
        return EINVAL;
    }
#endif
}

cee_io_result ceesafe_tcp_option(int32_t fd, uint32_t option, int64_t value) {
    int32_t error = cee_option_set(fd, option, value);
    return error != 0 ? ceesafe_io_fail(error) : ceesafe_io_ok(0);
}

/* ---- Async sockets (type-system.md §40.4; lib/std/async.cee): non-blocking descriptors. Each call is a value >= 0,
 * CEE_ASYNC_WOULD_BLOCK (the task waits for readiness, then calls again), or a negated errno.
 *
 * CosmoOS userland (CEE_COSMOOS) has no fcntl: its descriptors wait or not per call (MSG_DONTWAIT) or through its
 * own asynchronous I/O, never by a flag set on the descriptor. A socket cannot be made non-blocking there, so opening
 * one for async use (listen, accept, connect) is ENOSYS, which the library reports as AsyncError.Io; everything else
 * a program does, and Async.run itself, works as elsewhere. ---- */

static int64_t cee_async_errno(void) {
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    if (errno == EWOULDBLOCK) return CEE_ASYNC_WOULD_BLOCK;
#endif
    return errno == EAGAIN || errno == EINPROGRESS ? CEE_ASYNC_WOULD_BLOCK : -(int64_t)errno;
}

static bool cee_nonblocking(int fd) {
#if defined(CEE_COSMOOS)
    (void)fd;
    errno = ENOSYS;
    return false;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

int64_t ceesafe_async_tcp_listen(const uint8_t *address, uint64_t length, uint16_t port) {
    cee_str text = {address, length};
    ceesafe_endpoint e;
    if (!ceesafe_endpoint_parse(text, port, &e)) {
        return -(int64_t)EINVAL;
    }
    return cee_listen_endpoint(&e, 0u, CEE_LISTEN_REUSE_ADDRESS, true);
}

int64_t ceesafe_async_tcp_listen_at(uint32_t family, const uint8_t *bytes, uint16_t port, uint32_t scope,
                                    uint32_t backlog, uint32_t flags) {
    ceesafe_endpoint e;
    if (!ceesafe_endpoint_make(family, bytes, port, scope, &e)) {
        return -(int64_t)EINVAL;
    }
    return cee_listen_endpoint(&e, backlog, flags, true);
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
    ceesafe_endpoint e;
    if (!ceesafe_endpoint_parse(text, port, &e)) {
        return -(int64_t)EINVAL;
    }
    return cee_connect_endpoint(&e, true);
}

int64_t ceesafe_async_tcp_connect_endpoint(const ceesafe_endpoint *e) { return cee_connect_endpoint(e, true); }

int64_t ceesafe_async_tcp_connect_at(uint32_t family, const uint8_t *bytes, uint16_t port, uint32_t scope) {
    ceesafe_endpoint e;
    if (!ceesafe_endpoint_make(family, bytes, port, scope, &e)) {
        return -(int64_t)EINVAL;
    }
    return cee_connect_endpoint(&e, true);
}

int64_t ceesafe_async_tcp_connected(int32_t fd) {
#if defined(CEE_COSMOOS)
    (void)fd;
    return -(int64_t)ENOSYS;   /* no async connection can be started (cee_nonblocking), and CosmoOS has no getpeername */
#else
    int error = 0;
    socklen_t size = sizeof error;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0) {
        return -(int64_t)errno;
    }
    if (error != 0) {
        return error == EINPROGRESS || error == EALREADY ? CEE_ASYNC_WOULD_BLOCK : -(int64_t)error;
    }
    /* No error yet: connected once it has a peer (SO_ERROR is also 0 while the connection is still in progress). */
    cee_sockaddr peer;
    socklen_t length = sizeof peer;
    if (getpeername(fd, &peer.sa, &length) == 0) {
        return 0;
    }
    return errno == ENOTCONN ? CEE_ASYNC_WOULD_BLOCK : -(int64_t)errno;
#endif
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

int64_t ceesafe_async_shutdown_read(int32_t fd) {
    return shutdown(fd, SHUT_RD) < 0 ? -(int64_t)errno : 0;
}

int64_t ceesafe_async_socket_option(int32_t fd, uint32_t option, int64_t value) {
    int32_t error = cee_option_set(fd, option, value);
    return error != 0 ? -(int64_t)error : 0;
}

int64_t ceesafe_async_socket_name(int32_t fd, uint32_t peer, uint8_t *bytes, uint32_t *scope) {
    cee_sockaddr a;
    socklen_t length = sizeof a;
    ceesafe_mem_set(&a, 0, sizeof a);
#if defined(CEE_COSMOOS) || defined(COSMO_AF_INET)
    if (peer != 0u) {
        return -(int64_t)EOPNOTSUPP; /* CosmoOS has no getpeername */
    }
    if (getsockname(fd, &a.sa, &length) < 0) {
        return -(int64_t)errno;
    }
#else
    if ((peer != 0u ? getpeername(fd, &a.sa, &length) : getsockname(fd, &a.sa, &length)) < 0) {
        return -(int64_t)errno;
    }
#endif
    ceesafe_endpoint e;
    if (!cee_endpoint_from(&a, &e)) {
        return -(int64_t)EAFNOSUPPORT;
    }
    for (int k = 0; k < 16; k++) bytes[k] = e.bytes[k];
    *scope = e.scope;
    return ((int64_t)e.family << 16) | (int64_t)e.port;
}

uint32_t ceesafe_interface_index(const uint8_t *name, uint64_t length) {
#if defined(IF_NAMESIZE) && !defined(COSMO_AF_INET)
    char text[IF_NAMESIZE];
    if (length == 0u || length >= IF_NAMESIZE) return 0;
    for (uint64_t k = 0; k < length; k++) {
        if (name[k] == 0) return 0;
        text[k] = (char)name[k];
    }
    text[length] = 0;
    return (uint32_t)if_nametoindex(text);
#else
    (void)name;
    (void)length;
    return 0;
#endif
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
    return cee_async_result(cee_socket_open_text(text, port, true, true));
}

int64_t ceesafe_async_tcp_listen_at(uint32_t family, const uint8_t *bytes, uint16_t port, uint32_t scope,
                                    uint32_t backlog, uint32_t flags) {
    ceesafe_endpoint e;
    if (!ceesafe_endpoint_make(family, bytes, port, scope, &e)) return -(int64_t)EINVAL;
    return cee_async_result(cee_socket_open(&e, backlog, flags, true, true));
}

int64_t ceesafe_async_tcp_port(int32_t fd) { return cee_async_result(ceesafe_tcp_port(fd)); }

int64_t ceesafe_async_tcp_accept(int32_t fd) { return cee_async_result(cee_socket_accept(fd, true)); }

/* Starts a connection: the id, which is connected once it is writable and ceesafe_async_tcp_connected is 0. */
int64_t ceesafe_async_tcp_connect(const uint8_t *address, uint64_t length, uint16_t port) {
    cee_str text = {address, length};
    return cee_async_result(cee_socket_open_text(text, port, false, true));
}

int64_t ceesafe_async_tcp_connect_endpoint(const ceesafe_endpoint *e) {
    return cee_async_result(cee_socket_open(e, 0u, 0u, false, true));
}

int64_t ceesafe_async_tcp_connect_at(uint32_t family, const uint8_t *bytes, uint16_t port, uint32_t scope) {
    ceesafe_endpoint e;
    if (!ceesafe_endpoint_make(family, bytes, port, scope, &e)) return -(int64_t)EINVAL;
    return cee_async_result(cee_socket_open(&e, 0u, 0u, false, true));
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
    cee_sockaddr peer;
    int length = (int)sizeof peer;
    if (getpeername(s, &peer.sa, &length) == 0) return 0;
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

int64_t ceesafe_async_shutdown_read(int32_t fd) { return cee_async_result(ceesafe_tcp_shutdown_read(fd)); }

int64_t ceesafe_async_socket_option(int32_t fd, uint32_t option, int64_t value) {
    return cee_async_result(ceesafe_tcp_option(fd, option, value));
}

int64_t ceesafe_async_socket_name(int32_t fd, uint32_t peer, uint8_t *bytes, uint32_t *scope) {
    SOCKET s = (SOCKET)ceesafe_socket_handle(fd);
    if (s == INVALID_SOCKET) return -(int64_t)EBADF;
    cee_sockaddr a;
    int length = (int)sizeof a;
    ceesafe_mem_set(&a, 0, sizeof a);
    if ((peer != 0u ? getpeername(s, &a.sa, &length) : getsockname(s, &a.sa, &length)) != 0) {
        return -(int64_t)cee_wsa_errno(WSAGetLastError());
    }
    ceesafe_endpoint e;
    if (!cee_endpoint_from(&a, &e)) return -(int64_t)EAFNOSUPPORT;
    for (int k = 0; k < 16; k++) bytes[k] = e.bytes[k];
    *scope = e.scope;
    return ((int64_t)e.family << 16) | (int64_t)e.port;
}

/* Windows' zones are numbers in practice; a name would need iphlpapi, which no program links today (ADR 0044). */
uint32_t ceesafe_interface_index(const uint8_t *name, uint64_t length) {
    (void)name;
    (void)length;
    return 0;
}

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

#ifdef _WIN32
/* ceesafec's `embed` (ADR 0087 §2.8) on Windows: the POSIX calls its sources make, which rt.h names here. Paths are
 * UTF-8, as every CeeSafe string, so they go through the wide calls (CreateFileW, GetFinalPathNameByHandleW), never
 * the ANSI code page's. */

/* UTF-8 `text`, ending in NUL, as UTF-16 into `wide` (`size` units, its NUL included); false if it is not UTF-8 or
 * does not fit. */
static bool cee_win_wide(const char *text, wchar_t *wide, int size) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, size) > 0;
}

/* The final path of the open handle `h` (every symbolic link and junction resolved) as UTF-8 into `resolved`, which
 * holds 4,096 bytes: without the `\\?\` prefix the call adds, and `\\?\UNC\server\share` as `\\server\share`. NULL
 * if it does not fit or is not valid UTF-16. */
static char *cee_win_final_path(HANDLE h, char *resolved) {
    wchar_t final[4104];
    DWORD n = GetFinalPathNameByHandleW(h, final, 4104, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0 || n >= 4104) return NULL;
    const wchar_t *start = final;
    int prefix = 0;
    if (wcsncmp(final, L"\\\\?\\UNC\\", 8) == 0) {
        start = final + 8;
        resolved[0] = '\\';
        resolved[1] = '\\';
        prefix = 2;
    } else if (wcsncmp(final, L"\\\\?\\", 4) == 0) {
        start = final + 4;
    }
    int m = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, start, -1, resolved + prefix, 4096 - prefix, NULL, NULL);
    return m > 0 ? resolved : NULL;
}

/* realpath(3): the final path of the file or directory at `path` (FILE_FLAG_BACKUP_SEMANTICS opens a directory too,
 * and no access is asked), as `C:\dir\name` or `\\server\share\name`. */
char *ceesafe_win_realpath(const char *path, char *resolved) {
    wchar_t wide[4104];
    if (!cee_win_wide(path, wide, 4104)) return NULL;
    HANDLE h = CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    char *result = cee_win_final_path(h, resolved);
    CloseHandle(h);
    return result;
}

/* uname(3): only the system's name, the first member of a struct utsname, which is "Windows". */
int32_t ceesafe_win_uname(uint8_t *name) {
    memcpy(name, "Windows", 8);
    return 0;
}

/* openat(2) of the absolute `path`, for reading: a descriptor only for a regular file on a disk (no directory, pipe,
 * console or device), so the caller need not ask what it opened. `directory` and `flags` are not used. */
int32_t ceesafe_win_openat(int32_t directory, char const *path, int32_t flags, ...) {
    (void)directory;
    (void)flags;
    wchar_t wide[4104];
    if (!cee_win_wide(path, wide, 4104)) {
        errno = EINVAL;
        return -1;
    }
    HANDLE h = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        errno = ENOENT;
        return -1;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileType(h) != FILE_TYPE_DISK || !GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        CloseHandle(h);
        errno = EINVAL;
        return -1;
    }
    int fd = _open_osfhandle((intptr_t)h, _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(h);
        return -1;
    }
    return fd;
}

/* fcntl(2) with F_GETPATH (50) only, as macOS has it: the descriptor's final path, as UTF-8, into the `char *`
 * argument, which holds 4,096 bytes. */
int32_t ceesafe_win_fcntl(int32_t fd, int32_t command, ...) {
    if (command != 50) {
        errno = EINVAL;
        return -1;
    }
    va_list arguments;
    va_start(arguments, command);
    char *buffer = va_arg(arguments, char *);
    va_end(arguments);
    intptr_t h = _get_osfhandle(fd);
    if (h == -1 || cee_win_final_path((HANDLE)h, buffer) == NULL) {
        errno = EBADF;
        return -1;
    }
    return 0;
}

/* readlink(2), which ceesafec calls on Linux only: Windows has no /proc. */
ptrdiff_t ceesafe_win_readlink(char const *path, char *buffer, size_t size) {
    (void)path;
    (void)buffer;
    (void)size;
    errno = ENOSYS;
    return -1;
}

/* pread(2): up to `size` bytes at `offset`, without moving the descriptor's position. */
ptrdiff_t ceesafe_win_pread(int32_t fd, uint8_t *buffer, size_t size, int64_t offset) {
    intptr_t h = _get_osfhandle(fd);
    if (h == -1) {
        errno = EBADF;
        return -1;
    }
    OVERLAPPED at;
    memset(&at, 0, sizeof at);
    at.Offset = (DWORD)((uint64_t)offset & 0xFFFFFFFFu);
    at.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    DWORD wanted = size > 0x40000000u ? 0x40000000u : (DWORD)size;
    DWORD got = 0;
    if (!ReadFile((HANDLE)h, buffer, wanted, &got, &at)) {
        if (GetLastError() == ERROR_HANDLE_EOF) return 0;
        errno = EIO;
        return -1;
    }
    return (ptrdiff_t)got;
}
#endif

/* `File.rename(from, to)` (ADR 0075 §2.1): replaces `to` atomically where the system can. */
int64_t ceesafe_host_rename(uint8_t const *from, uint64_t fromLength, uint8_t const *to, uint64_t toLength) {
    char source[4096];
    char target[4096];
    if (!ceesafe_c_path(ceesafe_host_text(from, fromLength), source)) return -(int64_t)EINVAL;
    if (!ceesafe_c_path(ceesafe_host_text(to, toLength), target)) return -(int64_t)EINVAL;
#ifdef _WIN32
    if (MoveFileExA(source, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 0;
    DWORD error = GetLastError();
    return -(int64_t)(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ENOENT
                      : error == ERROR_ACCESS_DENIED                                   ? EACCES
                                                                                       : EIO);
#else
    return rename(source, target) == 0 ? 0 : -(int64_t)errno;
#endif
}

/* `Directory.space(path)` (ADR 0077): the bytes of the volume holding `path`: total, free, and available to an
   unprivileged process. */
int64_t ceesafe_host_space(uint8_t const *path, uint64_t length, uint64_t *total, uint64_t *free, uint64_t *available) {
    char name[4096];
    *total = 0;
    *free = 0;
    *available = 0;
    if (!ceesafe_c_path(ceesafe_host_text(path, length), name)) return -(int64_t)EINVAL;
#ifdef _WIN32
    ULARGE_INTEGER avail, size, unused;
    DWORD attributes = GetFileAttributesA(name);
    if (attributes == INVALID_FILE_ATTRIBUTES) return -(int64_t)ENOENT;
    if (!GetDiskFreeSpaceExA(name, &avail, &size, &unused)) return -(int64_t)EIO;
    *total = size.QuadPart;
    *free = unused.QuadPart;
    *available = avail.QuadPart;
    return 0;
#else
    struct statvfs st;
    int r;
    do {
        r = statvfs(name, &st);
    } while (r < 0 && errno == EINTR);
    if (r < 0) return -(int64_t)errno;
    uint64_t unit = st.f_frsize != 0 ? (uint64_t)st.f_frsize : (uint64_t)st.f_bsize;
    *total = (uint64_t)st.f_blocks * unit;
    *free = (uint64_t)st.f_bfree * unit;
    *available = (uint64_t)st.f_bavail * unit;
    return 0;
#endif
}

/* `File.modified(path)` (ADR 0077): the last modification time, as seconds and nanoseconds since 1970. */
int64_t ceesafe_host_modified(uint8_t const *path, uint64_t length, int64_t *seconds, uint32_t *nanoseconds) {
    char name[4096];
    *seconds = 0;
    *nanoseconds = 0;
    if (!ceesafe_c_path(ceesafe_host_text(path, length), name)) return -(int64_t)EINVAL;
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(name, &st) != 0) return -(int64_t)errno;
    *seconds = (int64_t)st.st_mtime;
#else
    struct stat st;
    if (stat(name, &st) != 0) return -(int64_t)errno;
#if defined(__APPLE__)
    *seconds = (int64_t)st.st_mtimespec.tv_sec;
    *nanoseconds = (uint32_t)st.st_mtimespec.tv_nsec;
#else
    *seconds = (int64_t)st.st_mtim.tv_sec;
    *nanoseconds = (uint32_t)st.st_mtim.tv_nsec;
#endif
#endif
    return 0;
}

/* `f.modified()` (ADR 0078 §3): the open file's last modification time (fstat), so it describes the file that is
   read, whatever the path names now. `file` is a File, whose first field is its descriptor. */
int64_t ceesafe_host_file_modified(void const *file, int64_t *seconds, uint32_t *nanoseconds) {
    int fd = *(const int32_t *)file;
    *seconds = 0;
    *nanoseconds = 0;
#ifdef _WIN32
    struct _stat64 st;
    if (_fstat64(fd, &st) != 0) return -(int64_t)errno;
    *seconds = (int64_t)st.st_mtime;
#else
    struct stat st;
    if (fstat(fd, &st) != 0) return -(int64_t)errno;
#if defined(__APPLE__)
    *seconds = (int64_t)st.st_mtimespec.tv_sec;
    *nanoseconds = (uint32_t)st.st_mtimespec.tv_nsec;
#else
    *seconds = (int64_t)st.st_mtim.tv_sec;
    *nanoseconds = (uint32_t)st.st_mtim.tv_nsec;
#endif
#endif
    return 0;
}

/* `f.kind()` (ADR 0078 §3): 1 a regular file, 2 a directory, 3 anything else, or a negated errno. */
int64_t ceesafe_host_file_kind(void const *file) {
    return cee_fd_kind(*(const int32_t *)file);
}

/* `File.setPermissions(path, mode)` (ADR 0077): chmod. Windows has only the read-only attribute: it is set when the
   owner may not write (mode & 0200 clear). */
int64_t ceesafe_host_set_permissions(uint8_t const *path, uint64_t length, uint32_t mode) {
    char name[4096];
    if (!ceesafe_c_path(ceesafe_host_text(path, length), name)) return -(int64_t)EINVAL;
#ifdef _WIN32
    return _chmod(name, (mode & 0200u) ? (_S_IREAD | _S_IWRITE) : _S_IREAD) == 0 ? 0 : -(int64_t)errno;
#else
    return chmod(name, (mode_t)(mode & 07777u)) == 0 ? 0 : -(int64_t)errno;
#endif
}

/* `Directory.sync(path)` (ADR 0075 §2.1): makes the directory's entries durable. Windows journals them, and a
   directory cannot be flushed there: 0 once the path is checked to be a directory. */
int64_t ceesafe_host_dir_sync(uint8_t const *path, uint64_t length) {
    char name[4096];
    if (!ceesafe_c_path(ceesafe_host_text(path, length), name)) return -(int64_t)EINVAL;
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(name);
    if (attributes == INVALID_FILE_ATTRIBUTES) return -(int64_t)ENOENT;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? 0 : -(int64_t)ENOTDIR;
#else
    int fd;
    do {
        fd = open(name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return -(int64_t)errno;
    cee_io_result r = ceesafe_fd_sync(fd, 1u);
    (void)close(fd);
    return r.error != 0 ? -(int64_t)r.error : 0;
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
 * without mkdtemp, which CosmoOS's libc lacks). The counter is atomic: a split build's compiles call this from several
 * threads at once (ADR 0082 §2.5). */
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
        uint32_t k = ceesafe_atomic_add_u32(&counter, 1u);
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

/* ---- Random bytes (ADR 0060) and HashMap's per-process key (ADR 0052 §3.2), from the system's source. ---- */

#if !defined(_WIN32) && !defined(CEE_COSMOOS) && defined(__has_include)
#if __has_include(<sys/random.h>) && defined(__linux__)
#include <sys/random.h>
#define CEE_HAVE_GETRANDOM 1
#endif
#endif
#include <limits.h> /* SSIZE_MAX, ULONG_MAX */
#if defined(_WIN32)
#include <bcrypt.h>
#if defined(_MSC_VER)
#pragma comment(lib, "bcrypt.lib")
#endif
#endif

static uint64_t cee_random_words[2];
static uint32_t cee_random_state; /* 0 not read, 1 being read, 2 read (ceesafe_atomic_*) */

/* Fills `out` from the system's cryptographic random source: 0, or a negated errno with `out` partly written. */
int64_t ceesafe_random_fill(uint8_t *out, uint64_t length) {
#if defined(_WIN32)
    while (length > 0u) {
        ULONG piece = length > (uint64_t)ULONG_MAX ? ULONG_MAX : (ULONG)length;
        if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, out, piece, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            return -(int64_t)EIO;
        }
        out += piece;
        length -= piece;
    }
    return 0;
#elif defined(CEE_COSMOOS)
    (void)out; (void)length;
    return -(int64_t)EOPNOTSUPP; /* no random source in CosmoOS userland yet */
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    arc4random_buf(out, (size_t)length);
    return 0;
#else
    uint64_t done = 0;
#if defined(CEE_HAVE_GETRANDOM)
    while (done < length) {
        size_t want = length - done > (uint64_t)SSIZE_MAX ? (size_t)SSIZE_MAX : (size_t)(length - done);
        ssize_t n = getrandom(out + done, want, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOSYS) break; /* before Linux 3.17: /dev/urandom below */
            return -(int64_t)errno;
        }
        done += (uint64_t)n;
    }
#endif
    if (done < length) {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0) {
            return -(int64_t)(errno != 0 ? errno : EIO);
        }
        while (done < length) {
            size_t want = length - done > (uint64_t)SSIZE_MAX ? (size_t)SSIZE_MAX : (size_t)(length - done);
            ssize_t n = read(fd, out + done, want);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                int error = n < 0 ? errno : EIO;
                (void)close(fd);
                return -(int64_t)error;
            }
            done += (uint64_t)n;
        }
        (void)close(fd);
    }
    return 0;
#endif
}

void ceesafe_random_key(uint64_t *key) {
    if (ceesafe_atomic_load_u32(&cee_random_state) != 2u) {
        if (ceesafe_atomic_cas_u32(&cee_random_state, 0u, 1u) == 0u) {
            uint8_t bytes[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                                 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c}; /* kept where there is no source */
            uint8_t fresh[16];
            if (ceesafe_random_fill(fresh, sizeof fresh) == 0) {
                ceesafe_mem_copy(bytes, fresh, sizeof bytes);
            }
            uint64_t a = 0, b = 0;
            for (unsigned k = 0; k < 8u; k++) {
                a |= (uint64_t)bytes[k] << (8u * k);
                b |= (uint64_t)bytes[8u + k] << (8u * k);
            }
            cee_random_words[0] = a;
            cee_random_words[1] = b;
            ceesafe_atomic_store_u32(&cee_random_state, 2u);
        } else {
            while (ceesafe_atomic_load_u32(&cee_random_state) != 2u) {
            }
        }
    }
    key[0] = cee_random_words[0];
    key[1] = cee_random_words[1];
}

/* ---- Wall-clock time (ADR 0055). The monotonic clock, for waits, is ceesafe_async_now. ---- */
#if !defined(_WIN32)
#include <time.h>
#endif

/* `Clock.monotonicNanos()` (ADR 0075 §2.6): nanoseconds from an arbitrary start, never decreasing in a process. */
uint64_t ceesafe_clock_monotonic(void) {
#if defined(_WIN32)
    static LARGE_INTEGER frequency;
    if (frequency.QuadPart == 0) (void)QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    (void)QueryPerformanceCounter(&now);
    uint64_t ticks = (uint64_t)now.QuadPart;
    uint64_t per = (uint64_t)frequency.QuadPart;
    return (ticks / per) * UINT64_C(1000000000) + (ticks % per) * UINT64_C(1000000000) / per;
#elif defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
#endif
}

void ceesafe_clock_realtime(int64_t *seconds, uint32_t *nanoseconds) {
    *seconds = 0;
    *nanoseconds = 0;
#if defined(_WIN32)
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    /* 100-nanosecond steps since 1601-01-01, which is 11644473600 seconds before 1970. */
    uint64_t steps = ((uint64_t)now.dwHighDateTime << 32) | (uint64_t)now.dwLowDateTime;
    const uint64_t epoch = UINT64_C(116444736000000000);
    if (steps >= epoch) {
        uint64_t since = steps - epoch;
        *seconds = (int64_t)(since / 10000000u);
        *nanoseconds = (uint32_t)(since % 10000000u) * 100u;
    }
#elif defined(CEE_COSMOOS) && !defined(CLOCK_REALTIME)
    /* no real-time clock in this userland: 1970 (ADR 0055 §3) */
#else
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
        *seconds = (int64_t)now.tv_sec;
        *nanoseconds = (uint32_t)now.tv_nsec;
    }
#endif
}

/* ---- UDP (ADR 0050): binding with the options, and one nonblocking send or receive for code that runs its own wait
 * loop (the interpreter). The operations of tasks are ceesafe_rt_async.c's. ---- */

bool ceesafe_endpoint_from_sockaddr(const void *address, ceesafe_endpoint *out) {
    return cee_endpoint_from((const cee_sockaddr *)address, out);
}

uint32_t ceesafe_endpoint_sockaddr_for(const ceesafe_endpoint *e, uint32_t socket_family, void *out, uint32_t room) {
    if (e->family == 4u && socket_family == 6u) {
        ceesafe_endpoint mapped = *e; /* ::ffff:a.b.c.d */
        mapped.family = 6u;
        mapped.scope = 0u;
        ceesafe_mem_set(mapped.bytes, 0, sizeof mapped.bytes);
        mapped.bytes[10] = 0xffu;
        mapped.bytes[11] = 0xffu;
        memcpy(mapped.bytes + 12, e->bytes, 4);
        return ceesafe_endpoint_sockaddr(&mapped, out, room);
    }
    return ceesafe_endpoint_sockaddr(e, out, room);
}

#if defined(CEE_COSMOOS) || defined(COSMO_AF_INET)
/* CosmoOS userland's socket layer has no UDP yet (ADR 0050 §3.2). */
int64_t ceesafe_async_udp_bind(uint32_t family, const uint8_t *bytes, uint16_t port, uint32_t scope, uint32_t flags,
                               uint32_t send_buffer, uint32_t receive_buffer) {
    (void)family; (void)bytes; (void)port; (void)scope; (void)flags; (void)send_buffer; (void)receive_buffer;
    return -(int64_t)EOPNOTSUPP;
}

int64_t ceesafe_async_udp_send(int32_t fd, const uint8_t *data, uint64_t length, uint32_t socket_family, uint32_t family,
                               const uint8_t *bytes, uint16_t port, uint32_t scope) {
    (void)fd; (void)data; (void)length; (void)socket_family; (void)family; (void)bytes; (void)port; (void)scope;
    return -(int64_t)EOPNOTSUPP;
}

int64_t ceesafe_async_udp_recv(int32_t fd, uint8_t *buffer, uint64_t length, uint8_t *bytes, uint32_t *scope,
                               uint32_t *info) {
    (void)fd; (void)buffer; (void)length; (void)bytes; (void)scope; (void)info;
    return -(int64_t)EOPNOTSUPP;
}
#else
#if defined(_WIN32)
typedef SOCKET cee_udp_socket;
#define CEE_UDP_INVALID INVALID_SOCKET
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
static int cee_udp_error(void) { return cee_wsa_errno(WSAGetLastError()); }
static void cee_udp_close(SOCKET s) { (void)closesocket(s); }
#else
typedef int cee_udp_socket;
#define CEE_UDP_INVALID (-1)
static int cee_udp_error(void) { return errno; }
static void cee_udp_close(int s) { (void)close(s); }
#endif

static int cee_udp_flag(cee_udp_socket s, int level, int name, int value) {
    return setsockopt(s, level, name, (const char *)&value, (int)sizeof value) != 0 ? cee_udp_error() : 0;
}

/* The local address of each datagram (IP_PKTINFO and its kin). On an IPv6 socket the IPv4 option is best effort: it
   covers the IPv4-mapped traffic of a dual-stack socket where the system takes it. */
static int cee_udp_receive_local(cee_udp_socket s, bool v6) {
    int failure = 0;
    if (v6) {
#if defined(IPV6_RECVPKTINFO)
        failure = cee_udp_flag(s, IPPROTO_IPV6, IPV6_RECVPKTINFO, 1);
#elif defined(IPV6_PKTINFO)
        failure = cee_udp_flag(s, IPPROTO_IPV6, IPV6_PKTINFO, 1); /* Windows */
#else
        failure = EOPNOTSUPP;
#endif
    }
#if defined(IP_RECVPKTINFO)
    int v4 = cee_udp_flag(s, IPPROTO_IP, IP_RECVPKTINFO, 1); /* macOS */
#elif defined(IP_PKTINFO)
    int v4 = cee_udp_flag(s, IPPROTO_IP, IP_PKTINFO, 1);
#elif defined(IP_RECVDSTADDR)
    int v4 = cee_udp_flag(s, IPPROTO_IP, IP_RECVDSTADDR, 1);
#else
    int v4 = EOPNOTSUPP;
#endif
    return v6 ? failure : v4;
}

/* The ECN bits of each datagram: the traffic class's or the type of service's low two bits. */
static int cee_udp_receive_ecn(cee_udp_socket s, bool v6) {
    int failure = 0;
#if defined(_WIN32)
#if defined(IP_RECVECN) && defined(IPV6_RECVECN)
    if (v6) failure = cee_udp_flag(s, IPPROTO_IPV6, IPV6_RECVECN, 1);
    int v4 = cee_udp_flag(s, IPPROTO_IP, IP_RECVECN, 1);
#else
    if (v6) failure = EOPNOTSUPP;
    int v4 = EOPNOTSUPP;
#endif
#else
#if defined(IPV6_RECVTCLASS)
    if (v6) failure = cee_udp_flag(s, IPPROTO_IPV6, IPV6_RECVTCLASS, 1);
#else
    if (v6) failure = EOPNOTSUPP;
#endif
#if defined(IP_RECVTOS)
    int v4 = cee_udp_flag(s, IPPROTO_IP, IP_RECVTOS, 1);
#else
    int v4 = EOPNOTSUPP;
#endif
#endif
    return v6 ? failure : v4;
}

int64_t ceesafe_async_udp_bind(uint32_t family, const uint8_t *bytes, uint16_t port, uint32_t scope, uint32_t flags,
                               uint32_t send_buffer, uint32_t receive_buffer) {
    ceesafe_endpoint e;
    if (!ceesafe_endpoint_make(family, bytes, port, scope, &e)) return -(int64_t)EINVAL;
    cee_sockaddr a;
    uint32_t length = ceesafe_endpoint_sockaddr(&e, &a, (uint32_t)sizeof a);
    if (length == 0) return -(int64_t)EAFNOSUPPORT;
#if defined(_WIN32)
    if ((flags & CEE_UDP_REUSE_PORT) != 0u) return -(int64_t)EOPNOTSUPP;
    if (!cee_wsa_ready()) return -(int64_t)ENOSYS;
#elif !defined(SO_REUSEPORT)
    if ((flags & CEE_UDP_REUSE_PORT) != 0u) return -(int64_t)EOPNOTSUPP;
#endif
    bool v6 = e.family == 6u;
    cee_udp_socket s = socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == CEE_UDP_INVALID) return -(int64_t)cee_udp_error();
    int failure = 0;
#if !defined(_WIN32)
    if ((flags & CEE_UDP_REUSE_ADDRESS) != 0u) failure = cee_udp_flag(s, SOL_SOCKET, SO_REUSEADDR, 1);
#if defined(SO_REUSEPORT)
    if (failure == 0 && (flags & CEE_UDP_REUSE_PORT) != 0u) failure = cee_udp_flag(s, SOL_SOCKET, SO_REUSEPORT, 1);
#endif
#endif
    if (failure == 0 && v6) failure = cee_udp_flag(s, IPPROTO_IPV6, IPV6_V6ONLY, (flags & CEE_UDP_V6_ONLY) != 0u ? 1 : 0);
    if (failure == 0 && send_buffer > 0u) {
        failure = cee_udp_flag(s, SOL_SOCKET, SO_SNDBUF, send_buffer > (uint32_t)INT32_MAX ? INT32_MAX : (int)send_buffer);
    }
    if (failure == 0 && receive_buffer > 0u) {
        int size = receive_buffer > (uint32_t)INT32_MAX ? INT32_MAX : (int)receive_buffer;
        failure = cee_udp_flag(s, SOL_SOCKET, SO_RCVBUF, size);
    }
    if (failure == 0 && (flags & CEE_UDP_RECEIVE_LOCAL) != 0u) failure = cee_udp_receive_local(s, v6);
    if (failure == 0 && (flags & CEE_UDP_RECEIVE_ECN) != 0u) failure = cee_udp_receive_ecn(s, v6);
#if defined(_WIN32)
    if (failure == 0) {
        /* A port unreachable would otherwise fail a later receive with WSAECONNRESET (ADR 0050 §3.1). */
        BOOL report = FALSE;
        DWORD returned = 0;
        (void)WSAIoctl(s, SIO_UDP_CONNRESET, &report, (DWORD)sizeof report, NULL, 0, &returned, NULL, NULL);
    }
    u_long on = 1;
    if (failure == 0 && ioctlsocket(s, (long)FIONBIO, &on) != 0) failure = cee_udp_error();
    if (failure == 0 && bind(s, &a.sa, (int)length) != 0) failure = cee_udp_error();
#else
    if (failure == 0 && !cee_nonblocking(s)) failure = errno;
    if (failure == 0 && bind(s, &a.sa, (socklen_t)length) != 0) failure = errno;
#endif
    if (failure != 0) {
        cee_udp_close(s);
        return -(int64_t)failure;
    }
#if defined(_WIN32)
    int32_t id = cee_socket_add(s);
    return id < 0 ? -(int64_t)ENOMEM : (int64_t)id;
#else
    return (int64_t)s;
#endif
}

int64_t ceesafe_async_udp_send(int32_t fd, const uint8_t *data, uint64_t length, uint32_t socket_family, uint32_t family,
                               const uint8_t *bytes, uint16_t port, uint32_t scope) {
    ceesafe_endpoint e;
    if (!ceesafe_endpoint_make(family, bytes, port, scope, &e)) return -(int64_t)EINVAL;
    cee_sockaddr a;
    uint32_t to = ceesafe_endpoint_sockaddr_for(&e, socket_family, &a, (uint32_t)sizeof a);
    if (to == 0) return -(int64_t)EAFNOSUPPORT;
    if (length > 65535u) return -(int64_t)EINVAL;
#if defined(_WIN32)
    SOCKET s = (SOCKET)ceesafe_socket_handle(fd);
    if (s == INVALID_SOCKET) return -(int64_t)EBADF;
    int n = sendto(s, (const char *)data, (int)length, 0, &a.sa, (int)to);
    if (n == SOCKET_ERROR) {
        int failed = WSAGetLastError();
        return failed == WSAEWOULDBLOCK ? CEE_ASYNC_WOULD_BLOCK : -(int64_t)cee_wsa_errno(failed);
    }
    return (int64_t)n;
#else
    ssize_t n;
    do {
        n = sendto(fd, data, (size_t)length, 0, &a.sa, (socklen_t)to);
    } while (n < 0 && errno == EINTR);
    if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? CEE_ASYNC_WOULD_BLOCK : -(int64_t)errno;
    return (int64_t)n;
#endif
}

int64_t ceesafe_async_udp_recv(int32_t fd, uint8_t *buffer, uint64_t length, uint8_t *bytes, uint32_t *scope,
                               uint32_t *info) {
    cee_sockaddr a;
    ceesafe_mem_set(&a, 0, sizeof a);
    uint64_t room = length > 65536u ? 65536u : length; /* a datagram is at most 65507 bytes */
    bool truncated = false;
#if defined(_WIN32)
    SOCKET s = (SOCKET)ceesafe_socket_handle(fd);
    if (s == INVALID_SOCKET) return -(int64_t)EBADF;
    int from = (int)sizeof a;
    int n = recvfrom(s, (char *)buffer, (int)room, 0, &a.sa, &from);
    if (n == SOCKET_ERROR) {
        int failed = WSAGetLastError();
        if (failed != WSAEMSGSIZE) {
            return failed == WSAEWOULDBLOCK ? CEE_ASYNC_WOULD_BLOCK : -(int64_t)cee_wsa_errno(failed);
        }
        truncated = true; /* the buffer is full, the rest is gone */
        n = (int)room;
    }
#else
    struct iovec part;
    part.iov_base = buffer;
    part.iov_len = (size_t)room;
    struct msghdr m;
    ceesafe_mem_set(&m, 0, sizeof m);
    m.msg_name = &a;
    m.msg_namelen = (socklen_t)sizeof a;
    m.msg_iov = &part;
    m.msg_iovlen = 1;
    ssize_t n;
    do {
        n = recvmsg(fd, &m, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? CEE_ASYNC_WOULD_BLOCK : -(int64_t)errno;
    truncated = (m.msg_flags & MSG_TRUNC) != 0;
#endif
    ceesafe_endpoint e;
    if (!cee_endpoint_from(&a, &e)) return -(int64_t)EAFNOSUPPORT;
    for (int k = 0; k < 16; k++) bytes[k] = e.bytes[k];
    *scope = e.scope;
    *info = (e.family << 24) | (truncated ? 1u << 16 : 0u) | (uint32_t)e.port;
    return (int64_t)n;
}
#endif

/* ---- A read at an offset, the file's position untouched (ADR 0051's copy where sendfile cannot). ---- */
int64_t ceesafe_file_read_at(const void *file, uint8_t *buffer, uint64_t length, uint64_t offset) {
    int32_t fd = *(const int32_t *)file;
    uint64_t count = length > ((uint64_t)1 << 30) ? ((uint64_t)1 << 30) : length;
#if defined(_WIN32)
    HANDLE base = (HANDLE)(intptr_t)_get_osfhandle(fd);
    if (base == INVALID_HANDLE_VALUE) {
        return -(int64_t)EBADF;
    }
    /* A ReadFile at an offset still moves a synchronous handle's file pointer: read through a handle of its own. */
    HANDLE h = ReOpenFile(base, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return -(int64_t)EIO;
    }
    OVERLAPPED at;
    ceesafe_mem_set(&at, 0, sizeof at);
    at.Offset = (DWORD)(offset & 0xFFFFFFFFu);
    at.OffsetHigh = (DWORD)(offset >> 32);
    DWORD got = 0;
    BOOL ok = ReadFile(h, buffer, (DWORD)count, &got, &at);
    DWORD error = ok ? 0 : GetLastError();
    (void)CloseHandle(h);
    if (!ok) {
        return error == ERROR_HANDLE_EOF ? 0 : -(int64_t)EIO;
    }
    return (int64_t)got;
#elif defined(CEE_COSMOOS)
    (void)fd; (void)buffer; (void)count; (void)offset;
    return -(int64_t)EOPNOTSUPP;
#else
    ssize_t n;
    do {
        n = pread(fd, buffer, (size_t)count, (off_t)offset);
    } while (n < 0 && errno == EINTR);
    return n < 0 ? -(int64_t)errno : (int64_t)n;
#endif
}
