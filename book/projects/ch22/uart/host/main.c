#include "ceesafe/rt.h"
#include "ceesafe_exports.h"
#include <stdio.h>
#include <stdlib.h>

_Noreturn void ceesafe_panic(cee_panic_kind kind, cee_loc loc) {
    fprintf(stderr, "board: stopped: %s at %s:%u:%u\n", ceesafe_panic_reason(kind), loc.file, (unsigned)loc.line,
        (unsigned)loc.column);
    exit(3);
}

/* The "device": three words of ordinary memory, laid out as the registers are. */
static uint32_t device[3];

int main(void) {
    uart_start((size_t)(uintptr_t)device);
    printf("control: %u\n", (unsigned)device[2]);

    const uint8_t text[] = {'h', 'e', 'l', 'l', 'o'};
    uint32_t sent = uart_write(text, sizeof text);
    printf("sent: %u, data register: '%c'\n", (unsigned)sent, (char)device[0]);

    device[1] = 0x10; /* status: nothing received */
    uart_on_interrupt();
    printf("received: %llu\n", (unsigned long long)uart_received());

    device[0] = 'k'; /* a byte arrives */
    device[1] = 0;
    uart_on_interrupt();
    printf("received: %llu, last: '%c'\n", (unsigned long long)uart_received(), (char)uart_last());
    return 0;
}
