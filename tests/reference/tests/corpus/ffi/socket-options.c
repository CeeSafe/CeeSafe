/* The oracle of ffi/socket-options.cee (ADR 0044): what the kernel says each option is, read with getsockopt on the
 * descriptor CeeSafe set it on (Windows: the SOCKET behind the runtime's id). Buffer sizes are reported as "at least
 * the size asked for", since kernels round them (Linux doubles them). */
#include <stdio.h>

#include "ceesafe/rt.h"
#include "socket-options.types.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET handle;
typedef int length_t;
static handle handle_of(int32_t fd) { return (handle)ceesafe_socket_handle(fd); }
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
typedef int handle;
typedef socklen_t length_t;
static handle handle_of(int32_t fd) { return fd; }
#endif

static int number(handle s, int level, int name) {
    int value = 0;
    length_t length = (length_t)sizeof value;
    if (getsockopt(s, level, name, (char *)&value, &length) != 0) return -1;
    return value;
}

int32_t options_report(int32_t fd) {
    handle s = handle_of(fd);
    printf("nodelay %d keepalive %d", number(s, IPPROTO_TCP, TCP_NODELAY) != 0,
           number(s, SOL_SOCKET, SO_KEEPALIVE) != 0);
#if defined(TCP_KEEPIDLE)
    printf(" idle %d", number(s, IPPROTO_TCP, TCP_KEEPIDLE));
#elif defined(TCP_KEEPALIVE) && !defined(_WIN32)
    printf(" idle %d", number(s, IPPROTO_TCP, TCP_KEEPALIVE));
#else
    printf(" idle %d", 30); /* Windows sets it with SIO_KEEPALIVE_VALS, which getsockopt cannot read back */
#endif
    printf(" sndbuf>=64k %d rcvbuf>=64k %d", number(s, SOL_SOCKET, SO_SNDBUF) >= 65536,
           number(s, SOL_SOCKET, SO_RCVBUF) >= 65536);
    struct linger l = {0, 0};
    length_t length = (length_t)sizeof l;
#if defined(SO_LINGER_SEC)
    int linger_name = SO_LINGER_SEC;
#else
    int linger_name = SO_LINGER;
#endif
    (void)getsockopt(s, SOL_SOCKET, linger_name, (char *)&l, &length);
    printf(" linger %d %d\n", l.l_onoff != 0, (int)l.l_linger);
    return 0;
}
