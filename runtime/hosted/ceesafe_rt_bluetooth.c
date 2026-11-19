/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * Bluetooth printers (type-system.md §26.19, ADR 0177): the paired printers with a serial port (Classic), the Low
 * Energy printers in range, and a link that writes bytes to one. Linked only into programs that use them (c-backend.md
 * §6).
 *
 * Every function blocks and may be called from any thread. Errors are BluetoothError's numbers, negated:
 * 1 Unsupported, 2 Off, 3 Denied, 4 NotFound, 5 Refused, 6 Lost, 7 Resources.
 *
 * A list is one block of records, each a transport byte ('C' Classic, 'L' Low Energy), the address and the name, each
 * ended by a zero byte; lib/std copies it and gives it back with ceesafe_bluetooth_free. A link is a block of the
 * runtime's, its address the handle (positive).
 *
 * CEESAFE_BLUETOOTH_SCRIPT=<file> selects, on every system, devices that exist only in the file (for tests), one to a
 * line: `paired ADDRESS NAME` (Classic, paired), `nearby ADDRESS NAME` (Low Energy, found by a scan), `off` (the
 * adapter is off), `denied` (the user refused), `refuse ADDRESS` (it will not connect), `lose ADDRESS N` (its link
 * breaks once N bytes are written). CEESAFE_BLUETOOTH_DUMP=<file> then receives `open ADDRESS`, `write HEX` and
 * `close ADDRESS` lines, appended, for what each link does.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* fopen, getenv */
#endif
#include "ceesafe/rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CEE_BT_UNSUPPORTED 1
#define CEE_BT_OFF 2
#define CEE_BT_DENIED 3
#define CEE_BT_NOT_FOUND 4
#define CEE_BT_REFUSED 5
#define CEE_BT_LOST 6
#define CEE_BT_RESOURCES 7

#define CEE_BT_CLASSIC 0
#define CEE_BT_LOW_ENERGY 1

/* ---- Lists ------------------------------------------------------------------------------------------------------ */

struct cee_bt_list {
    uint8_t *bytes;
    size_t length;
    size_t capacity;
    int failed;
};

static void cee_bt_put(struct cee_bt_list *l, const void *bytes, size_t length) {
    if (l->failed) {
        return;
    }
    if (l->length + length > l->capacity) {
        size_t capacity = l->capacity == 0 ? 256 : l->capacity;
        while (capacity < l->length + length) {
            capacity *= 2;
        }
        uint8_t *grown = (uint8_t *)ceesafe_platform_realloc(l->bytes, capacity);
        if (grown == NULL) {
            l->failed = 1;
            return;
        }
        l->bytes = grown;
        l->capacity = capacity;
    }
    memcpy(l->bytes + l->length, bytes, length);
    l->length += length;
}

/* One record: the transport, the address and the name (UTF-8, without zero bytes). */
static void cee_bt_add(struct cee_bt_list *l, int transport, const char *address, size_t address_length,
                       const char *name, size_t name_length) {
    const uint8_t kind = transport == CEE_BT_LOW_ENERGY ? 'L' : 'C';
    const uint8_t zero = 0;
    cee_bt_put(l, &kind, 1);
    cee_bt_put(l, address, address_length);
    cee_bt_put(l, &zero, 1);
    for (size_t k = 0; k < name_length; k++) {
        if (name[k] != '\0') {
            cee_bt_put(l, name + k, 1);
        }
    }
    cee_bt_put(l, &zero, 1);
}

/* The list's length, its bytes given to `out`; or minus an error. */
static int64_t cee_bt_finish(struct cee_bt_list *l, uint8_t **out) {
    if (l->failed) {
        ceesafe_platform_free(l->bytes);
        return -CEE_BT_RESOURCES;
    }
    *out = l->bytes;
    return (int64_t)l->length;
}

/* ---- Links ------------------------------------------------------------------------------------------------------ */

struct cee_bt_link {
    int scripted;
    char address[64];
    size_t written;
    size_t lose_after; /* scripted: SIZE_MAX, or the bytes after which the link breaks */
    void *native;      /* the system's connection */
};

/* ---- The script (CEESAFE_BLUETOOTH_SCRIPT) ---------------------------------------------------------------------- */

static const char *cee_bt_script(void) {
    const char *path = getenv("CEESAFE_BLUETOOTH_SCRIPT");
    return path != NULL && path[0] != '\0' ? path : NULL;
}

/* The script's lines, one at a time, into `line` (without its end). */
static int cee_bt_line(FILE *f, char *line, size_t size) {
    if (fgets(line, (int)size, f) == NULL) {
        return 0;
    }
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        line[--n] = '\0';
    }
    return 1;
}

/* The word at `*at` (up to a space), its length; `*at` moved past it and the spaces after. */
static size_t cee_bt_word(const char **at) {
    const char *start = *at;
    size_t n = 0;
    while (start[n] != '\0' && start[n] != ' ') {
        n++;
    }
    const char *next = start + n;
    while (*next == ' ') {
        next++;
    }
    *at = next;
    return n;
}

static int cee_bt_is(const char *word, size_t length, const char *text) {
    return strlen(text) == length && memcmp(word, text, length) == 0;
}

/* The script's state that applies to every call: 0, or the error (`off`, `denied`). */
static int cee_bt_script_state(FILE *f) {
    char line[512];
    int state = 0;
    while (cee_bt_line(f, line, sizeof line)) {
        const char *at = line;
        const char *word = at;
        size_t n = cee_bt_word(&at);
        if (cee_bt_is(word, n, "off")) {
            state = CEE_BT_OFF;
        } else if (cee_bt_is(word, n, "denied") && state == 0) {
            state = CEE_BT_DENIED;
        }
    }
    rewind(f);
    return state;
}

static int64_t cee_bt_script_list(const char *path, uint32_t scan, uint8_t **out) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -CEE_BT_UNSUPPORTED;
    }
    int state = cee_bt_script_state(f);
    if (state != 0) {
        fclose(f);
        return -state;
    }
    struct cee_bt_list l;
    memset(&l, 0, sizeof l);
    char line[512];
    while (cee_bt_line(f, line, sizeof line)) {
        const char *at = line;
        const char *word = at;
        size_t n = cee_bt_word(&at);
        int paired = cee_bt_is(word, n, "paired");
        int nearby = cee_bt_is(word, n, "nearby");
        if ((paired && !scan) || (nearby && scan)) {
            const char *address = at;
            size_t address_length = cee_bt_word(&at);
            cee_bt_add(&l, nearby ? CEE_BT_LOW_ENERGY : CEE_BT_CLASSIC, address, address_length, at, strlen(at));
        }
    }
    fclose(f);
    return cee_bt_finish(&l, out);
}

static void cee_bt_dump(const char *what, const char *text, const uint8_t *bytes, size_t length) {
    const char *path = getenv("CEESAFE_BLUETOOTH_DUMP");
    if (path == NULL || path[0] == '\0') {
        return;
    }
    FILE *f = fopen(path, "ab");
    if (f == NULL) {
        return;
    }
    fputs(what, f);
    fputc(' ', f);
    if (text != NULL) {
        fputs(text, f);
    }
    for (size_t k = 0; k < length; k++) {
        fprintf(f, "%02x", bytes[k]);
    }
    fputc('\n', f);
    fclose(f);
}

static int64_t cee_bt_script_open(const char *path, uint32_t transport, const char *address, size_t length) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -CEE_BT_UNSUPPORTED;
    }
    int state = cee_bt_script_state(f);
    if (state != 0) {
        fclose(f);
        return -state;
    }
    int found = 0;
    int refused = 0;
    size_t lose_after = SIZE_MAX;
    char line[512];
    while (cee_bt_line(f, line, sizeof line)) {
        const char *at = line;
        const char *word = at;
        size_t n = cee_bt_word(&at);
        const char *named = at;
        size_t named_length = cee_bt_word(&at);
        if (named_length != length || memcmp(named, address, length) != 0) {
            continue;
        }
        if ((cee_bt_is(word, n, "paired") && transport == CEE_BT_CLASSIC) ||
            (cee_bt_is(word, n, "nearby") && transport == CEE_BT_LOW_ENERGY)) {
            found = 1;
        } else if (cee_bt_is(word, n, "refuse")) {
            refused = 1;
        } else if (cee_bt_is(word, n, "lose")) {
            lose_after = (size_t)strtoull(at, NULL, 10);
        }
    }
    fclose(f);
    if (!found) {
        return -CEE_BT_NOT_FOUND;
    }
    if (refused) {
        return -CEE_BT_REFUSED;
    }
    struct cee_bt_link *link = (struct cee_bt_link *)ceesafe_platform_alloc_zeroed(sizeof *link);
    if (link == NULL) {
        return -CEE_BT_RESOURCES;
    }
    link->scripted = 1;
    memcpy(link->address, address, length);
    link->lose_after = lose_after;
    cee_bt_dump("open", link->address, NULL, 0);
    return (int64_t)(intptr_t)link;
}

static int32_t cee_bt_script_write(struct cee_bt_link *link, const uint8_t *bytes, size_t length) {
    if (link->written >= link->lose_after) {
        return -CEE_BT_LOST;
    }
    size_t room = link->lose_after - link->written;
    size_t taken = length < room ? length : room;
    cee_bt_dump("write", NULL, bytes, taken);
    link->written += taken;
    return taken < length ? -CEE_BT_LOST : 0;
}

/* ---- The systems ------------------------------------------------------------------------------------------------ */
/* Each defines cee_bt_native_list (the paired printers, or a scan's), cee_bt_native_open (fills link->native: 0 or an
   error), cee_bt_native_write (0 or minus an error) and cee_bt_native_close. */

/* None yet (ADR 0177 §5): every system answers Unsupported until its step. */
static int64_t cee_bt_native_list(uint32_t scan, uint32_t milliseconds, uint8_t **out) {
    (void)scan;
    (void)milliseconds;
    (void)out;
    return -CEE_BT_UNSUPPORTED;
}

static int cee_bt_native_open(struct cee_bt_link *link, uint32_t transport, const char *address, size_t length) {
    (void)link;
    (void)transport;
    (void)address;
    (void)length;
    return CEE_BT_UNSUPPORTED;
}

static int32_t cee_bt_native_write(struct cee_bt_link *link, const uint8_t *bytes, size_t length) {
    (void)link;
    (void)bytes;
    (void)length;
    return -CEE_BT_LOST;
}

static void cee_bt_native_close(struct cee_bt_link *link) {
    (void)link;
}

/* ---- The interface lib/std calls -------------------------------------------------------------------------------- */

int64_t ceesafe_bluetooth_list(uint32_t scan, uint32_t milliseconds, const uint8_t **out) {
    *out = NULL;
    uint8_t *list = NULL;
    const char *script = cee_bt_script();
    int64_t length = script != NULL ? cee_bt_script_list(script, scan, &list)
                                    : cee_bt_native_list(scan, milliseconds, &list);
    if (length <= 0) {
        ceesafe_platform_free(list);
        return length;
    }
    *out = list;
    return length;
}

void ceesafe_bluetooth_free(const uint8_t *list) {
    ceesafe_platform_free((void *)list);
}

int64_t ceesafe_bluetooth_open(uint32_t transport, const uint8_t *address, uint64_t length) {
    if (length == 0 || length >= sizeof(((struct cee_bt_link *)0)->address)) {
        return -CEE_BT_NOT_FOUND;
    }
    const char *script = cee_bt_script();
    if (script != NULL) {
        return cee_bt_script_open(script, transport, (const char *)address, (size_t)length);
    }
    struct cee_bt_link *link = (struct cee_bt_link *)ceesafe_platform_alloc_zeroed(sizeof *link);
    if (link == NULL) {
        return -CEE_BT_RESOURCES;
    }
    memcpy(link->address, address, (size_t)length);
    int error = cee_bt_native_open(link, transport, link->address, (size_t)length);
    if (error != 0) {
        ceesafe_platform_free(link);
        return -error;
    }
    return (int64_t)(intptr_t)link;
}

int32_t ceesafe_bluetooth_write(int64_t handle, const uint8_t *bytes, uint64_t length) {
    struct cee_bt_link *link = (struct cee_bt_link *)(intptr_t)handle;
    return link->scripted ? cee_bt_script_write(link, bytes, (size_t)length)
                          : cee_bt_native_write(link, bytes, (size_t)length);
}

void ceesafe_bluetooth_close(int64_t handle) {
    struct cee_bt_link *link = (struct cee_bt_link *)(intptr_t)handle;
    if (link->scripted) {
        cee_bt_dump("close", link->address, NULL, 0);
    } else {
        cee_bt_native_close(link);
    }
    ceesafe_platform_free(link);
}
