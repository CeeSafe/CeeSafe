#include "ceesafe_exports.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int32_t gauge_peak(const int32_t *values, size_t count, struct Range range);
size_t gauge_label(struct Range range, Unit unit, char *out, size_t capacity);
void gauge_each(const int32_t *values, size_t count, void (*visit)(void *, int32_t), void *context);
char *gauge_title(const char *name);
void gauge_release(char *title);

/* The largest of `count` values, each first clamped by CeeSafe's gauge_clamp. */
int32_t gauge_peak(const int32_t *values, size_t count, struct Range range) {
    int32_t peak = range.low;
    for (size_t i = 0; i < count; i++) {
        int32_t value = gauge_clamp(values[i], range);
        if (value > peak) {
            peak = value;
        }
    }
    return peak;
}

/* Writes "low..high unit" to `out`, at most `capacity` bytes with the zero byte. Returns the text's length. */
size_t gauge_label(struct Range range, Unit unit, char *out, size_t capacity) {
    int length = snprintf(out, capacity, "%d..%d %s", range.low, range.high, unit == Unit_Kelvin ? "K" : "C");
    if (length < 0) {
        return 0;
    }
    return (size_t)length < capacity ? (size_t)length : capacity - 1;
}

/* Calls `visit(context, value)` for each value. */
void gauge_each(const int32_t *values, size_t count, void (*visit)(void *, int32_t), void *context) {
    for (size_t i = 0; i < count; i++) {
        visit(context, values[i]);
    }
}

/* A new string "gauge: name", or NULL. The caller gives it back with gauge_release. */
char *gauge_title(const char *name) {
    size_t size = strlen(name) + 8;
    char *title = malloc(size);
    if (title != NULL) {
        snprintf(title, size, "gauge: %s", name);
    }
    return title;
}

void gauge_release(char *title) {
    free(title);
}
