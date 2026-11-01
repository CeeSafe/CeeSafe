#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* A bump allocator over 64 KiB chunks, as the CeeSafe runtime's Arena is (runtime/core: chunks from the heap, freed on reset). */
typedef struct Chunk { struct Chunk *next; size_t used; unsigned char data[65536 - 2 * sizeof(void *)]; } Chunk;
typedef struct { Chunk *head; } Arena;

static void *arena_alloc(Arena *a, size_t size) {
    size = (size + 15) & ~(size_t)15;
    if (!a->head || a->head->used + size > sizeof a->head->data) {
        Chunk *c = malloc(sizeof *c);
        if (!c) exit(1);
        c->next = a->head;
        c->used = 0;
        a->head = c;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += size;
    return p;
}

static void arena_reset(Arena *a) {
    while (a->head) { Chunk *n = a->head->next; free(a->head); a->head = n; }
}

typedef struct Node { uint64_t value; struct Node *previous; } Node;

int main(void) {
    Arena arena = { 0 };
    uint64_t sum = 0;
    for (int round = 0; round < 400; round++) {
        Node *last = NULL;
        for (uint64_t i = 0; i < 200000; i++) {
            Node *n = arena_alloc(&arena, sizeof *n);
            n->value = (i + (uint64_t)round) % 1000;
            n->previous = last;
            last = n;
        }
        for (Node *n = last; n; n = n->previous) sum += n->value;
        arena_reset(&arena);
    }
    printf("%llu\n", (unsigned long long)sum);
    return 0;
}
