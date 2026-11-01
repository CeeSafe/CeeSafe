/* ffi/socket-options.cee's C half: reads a socket's options back with getsockopt. */
#ifndef SOCKET_OPTIONS_TYPES_H
#define SOCKET_OPTIONS_TYPES_H

#include <stdint.h>

int32_t options_report(int32_t fd);

#endif
