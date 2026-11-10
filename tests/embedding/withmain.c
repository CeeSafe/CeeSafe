/* ADR 0088 §2.1: a host with its own `main`, linking the archive of withmain.cee, which has a CeeSafe `main`. */
#include <stdint.h>
#include <stdio.h>

uint64_t withmain_add(uint64_t a, uint64_t b);

int main(void) {
    printf("host main %llu\n", (unsigned long long)withmain_add(40, 2));
    return 0;
}
