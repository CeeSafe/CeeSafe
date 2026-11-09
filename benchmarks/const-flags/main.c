/* const-flags: C has no const parameters, so the flags are run-time arguments (the translation rule ADR 0083 §3
   calls C): one function, every test kept unless the C compiler clones it. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { size_t pos; uint64_t types; uint64_t jsx; } Scan;

static uint64_t scan(Scan *s, const uint8_t *text, size_t length, int ts, int jsx, uint32_t depth) {
    uint64_t sum = 0;
    while (s->pos < length) {
        uint8_t c = text[s->pos];
        s->pos += 1;
        if (c == '(') {
            if (depth < 40) sum += scan(s, text, length, ts, jsx, depth + 1);
        } else if (c == ')' && depth > 0) {
            return sum;
        } else if (c == ':' && ts) {
            s->types += 1;
        } else if (c == '<' && jsx) {
            s->jsx += 1;
        } else if (ts && c == '?') {
            s->types += 2;
        } else {
            sum += c;
        }
    }
    return sum;
}

int main(void) {
    size_t length = 10000000;
    uint8_t *text = malloc(length);
    if (text == NULL) return 1;
    uint64_t seed = 12345;
    for (size_t i = 0; i < length; i++) {
        seed = (seed * 1103515245u + 12345u) % 2147483648u;
        uint64_t r = (seed >> 16) % 16;
        text[i] = r == 0 ? '(' : r == 1 ? ')' : r == 2 ? ':' : r == 3 ? '<' : r == 4 ? '?' : 'a';
    }
    uint64_t total = 0;
    for (int k = 0; k < 100; k++) {
        Scan s = {0, 0, 0};
        uint64_t found = scan(&s, text, length, k % 2 == 0, 1, 0);
        total += found + s.types + s.jsx;
    }
    printf("%llu\n", (unsigned long long)total);
    free(text);
    return 0;
}
