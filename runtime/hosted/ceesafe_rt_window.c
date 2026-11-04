/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * Native windows (type-system.md §26.13, ADR 0069): one resizable window that shows premultiplied RGBA8 frames and
 * reports the pointer, keys, typed text, the wheel, resizes and close requests. Linked only into programs that open a
 * window (c-backend.md §6).
 *
 * Backends: AppKit on macOS, through the Objective-C runtime loaded with dlopen at the first open, so no program links
 * a framework; Win32 and GDI on Windows; X11 on Linux, libX11.so.6 loaded with dlopen. CEESAFE_WINDOW_SCRIPT=<file>
 * selects, on every system, a window that exists only in memory and whose events come from the file (for tests);
 * CEESAFE_WINDOW_DUMP=<file> then receives each presented frame.
 *
 * Events are queued in the window (a ring of CEE_WINDOW_QUEUE) and handed out one per ceesafe_window_poll, as a kind
 * and four integer and two float fields (lib/std/window.cee decodes them).
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* dlopen, syscall(SYS_gettid), select, clock_gettime under glibc's strict C11 headers */
#endif
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* fopen, getenv */
#endif
#include "ceesafe/rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* WindowError's numbers (lib/std/window.cee). */
#define CEE_WINDOW_UNSUPPORTED 1
#define CEE_WINDOW_NO_DISPLAY 2
#define CEE_WINDOW_NOT_MAIN_THREAD 3
#define CEE_WINDOW_RESOURCES 4

/* Event kinds (lib/std/window.cee's nextEvent). */
#define CEE_EVENT_CLOSED 1
#define CEE_EVENT_RESIZED 2
#define CEE_EVENT_FOCUS 3
#define CEE_EVENT_MOVED 4
#define CEE_EVENT_BUTTON 5
#define CEE_EVENT_WHEEL 6
#define CEE_EVENT_KEY 7
#define CEE_EVENT_TEXT 8

/* Key numbers: 1-14 the named keys, 100 + n F(n), 0x10000 + c a character, 0x20000 + code any other key. */
#define CEE_KEY_TAB 1
#define CEE_KEY_ENTER 2
#define CEE_KEY_ESCAPE 3
#define CEE_KEY_BACKSPACE 4
#define CEE_KEY_DELETE 5
#define CEE_KEY_LEFT 6
#define CEE_KEY_RIGHT 7
#define CEE_KEY_UP 8
#define CEE_KEY_DOWN 9
#define CEE_KEY_HOME 10
#define CEE_KEY_END 11
#define CEE_KEY_PAGE_UP 12
#define CEE_KEY_PAGE_DOWN 13
#define CEE_KEY_SPACE 14
#define CEE_KEY_FUNCTION 100
#define CEE_KEY_CHARACTER 0x10000
#define CEE_KEY_OTHER 0x20000

/* Modifier bits. */
#define CEE_MOD_SHIFT 1
#define CEE_MOD_CONTROL 2
#define CEE_MOD_ALT 4
#define CEE_MOD_COMMAND 8

#define CEE_WINDOW_QUEUE 512
#define CEE_WINDOW_MAX_SIDE 16384u

struct cee_window_event {
    int32_t kind;
    int32_t fields[4];
    float x, y;
};

/* One scripted event and the frame it belongs to. */
struct cee_window_scripted {
    uint64_t frame;
    struct cee_window_event event;
};

struct cee_window {
    int scripted;
    struct cee_window_event queue[CEE_WINDOW_QUEUE];
    unsigned head, count;
    uint32_t width, height; /* points */
    float scale;
    int32_t cursor;
    int focused;
    double last_wait; /* seconds, the monotonic clock */
    /* The scripted window. */
    struct cee_window_scripted *script;
    size_t script_count, script_next;
    uint64_t frames; /* presents so far */
    const char *dump;
    /* The system's window. */
    void *native;
};

void *ceesafe_window_open(const uint8_t *title, uint64_t length, uint32_t width, uint32_t height, int32_t *error);
void ceesafe_window_close(void *window);
int32_t ceesafe_window_poll(void *window, int32_t *fields, float *position);
int32_t ceesafe_window_present(void *window, const uint32_t *pixels, uint32_t width, uint32_t height);
void ceesafe_window_size(void *window, uint32_t *width, uint32_t *height);
void ceesafe_window_scale(void *window, float *scale);
void ceesafe_window_set_cursor(void *window, int32_t cursor);
void ceesafe_window_set_title(void *window, const uint8_t *title, uint64_t length);
void ceesafe_window_wait(void *window);

static void cee_window_push(struct cee_window *w, int32_t kind, int32_t a, int32_t b, int32_t c, int32_t d, float x,
                            float y) {
    if (w->count == CEE_WINDOW_QUEUE) {
        return; /* a program that never polls loses the newest events, not the memory */
    }
    struct cee_window_event *e = &w->queue[(w->head + w->count) % CEE_WINDOW_QUEUE];
    e->kind = kind;
    e->fields[0] = a;
    e->fields[1] = b;
    e->fields[2] = c;
    e->fields[3] = d;
    e->x = x;
    e->y = y;
    w->count++;
}

static void cee_window_resized(struct cee_window *w, uint32_t width, uint32_t height) {
    if (width == w->width && height == w->height) {
        return;
    }
    w->width = width;
    w->height = height;
    cee_window_push(w, CEE_EVENT_RESIZED, (int32_t)width, (int32_t)height, 0, 0, 0.0f, 0.0f);
}

static void cee_window_focus(struct cee_window *w, int focused) {
    if (focused == w->focused) {
        return;
    }
    w->focused = focused;
    cee_window_push(w, CEE_EVENT_FOCUS, focused, 0, 0, 0, 0.0f, 0.0f);
}

/* A typed character: control characters and the private-use function-key range are keys, not text. */
static void cee_window_text(struct cee_window *w, uint32_t scalar) {
    if (scalar < 0x20u || scalar == 0x7fu || (scalar >= 0xf700u && scalar <= 0xf8ffu) || scalar > 0x10ffffu ||
        (scalar >= 0xd800u && scalar <= 0xdfffu)) {
        return;
    }
    cee_window_push(w, CEE_EVENT_TEXT, (int32_t)scalar, 0, 0, 0, 0.0f, 0.0f);
}

/* A letter as lowercase, other printable ASCII as it is: WindowKey.Character. */
static int32_t cee_window_character(uint32_t c) {
    if (c >= 'A' && c <= 'Z') {
        c = c - 'A' + 'a';
    }
    return (int32_t)(CEE_KEY_CHARACTER + c);
}

/* Decodes one UTF-8 scalar from s[*i..n) and moves past it; a malformed sequence is U+FFFD and one byte. */
static uint32_t cee_window_utf8(const unsigned char *s, size_t n, size_t *i) {
    unsigned char b = s[*i];
    size_t extra;
    uint32_t c;
    if (b < 0x80u) {
        *i += 1;
        return b;
    } else if ((b & 0xe0u) == 0xc0u) {
        extra = 1;
        c = b & 0x1fu;
    } else if ((b & 0xf0u) == 0xe0u) {
        extra = 2;
        c = b & 0x0fu;
    } else if ((b & 0xf8u) == 0xf0u) {
        extra = 3;
        c = b & 0x07u;
    } else {
        *i += 1;
        return 0xfffdu;
    }
    for (size_t k = 1; k <= extra; k++) {
        if (*i + k >= n || (s[*i + k] & 0xc0u) != 0x80u) {
            *i += 1;
            return 0xfffdu;
        }
        c = (c << 6) | (uint32_t)(s[*i + k] & 0x3fu);
    }
    *i += extra + 1u;
    return c;
}

/* ---- The monotonic clock and sleeping ---------------------------------------------------------------------------- */

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
static double cee_window_now(void) {
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
}
#else
#include <time.h>
static double cee_window_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
#endif

/* ---- The program's first thread --------------------------------------------------------------------------------- */

#if defined(__APPLE__)
#include <pthread.h>
static int cee_window_on_main_thread(void) { return pthread_main_np() != 0; }
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
static int cee_window_on_main_thread(void) { return (pid_t)syscall(SYS_gettid) == getpid(); }
#elif defined(_WIN32)
/* The first thread's id, recorded by a C runtime initializer, which runs on it before main. */
static DWORD cee_window_main_thread;
static void __cdecl cee_window_record_main(void) { cee_window_main_thread = GetCurrentThreadId(); }
#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) void(__cdecl *ceesafe_window_record_main)(void) = cee_window_record_main;
#if defined(_M_IX86)
#pragma comment(linker, "/include:_ceesafe_window_record_main")
#else
#pragma comment(linker, "/include:ceesafe_window_record_main")
#endif
#else
__attribute__((constructor)) static void cee_window_constructor(void) { cee_window_record_main(); }
#endif
static int cee_window_on_main_thread(void) { return GetCurrentThreadId() == cee_window_main_thread; }
#else
static int cee_window_on_main_thread(void) { return 1; }
#endif

/* ---- The scripted window ----------------------------------------------------------------------------------------- */

static int cee_window_key_name(const char *name, size_t length, int32_t *key) {
    static const struct {
        const char *name;
        int32_t key;
    } names[] = {{"tab", CEE_KEY_TAB},         {"enter", CEE_KEY_ENTER},     {"escape", CEE_KEY_ESCAPE},
                 {"backspace", CEE_KEY_BACKSPACE}, {"delete", CEE_KEY_DELETE}, {"left", CEE_KEY_LEFT},
                 {"right", CEE_KEY_RIGHT},     {"up", CEE_KEY_UP},           {"down", CEE_KEY_DOWN},
                 {"home", CEE_KEY_HOME},       {"end", CEE_KEY_END},         {"pageup", CEE_KEY_PAGE_UP},
                 {"pagedown", CEE_KEY_PAGE_DOWN}, {"space", CEE_KEY_SPACE}};
    for (size_t k = 0; k < sizeof names / sizeof names[0]; k++) {
        if (strlen(names[k].name) == length && memcmp(names[k].name, name, length) == 0) {
            *key = names[k].key;
            return 1;
        }
    }
    if ((length == 2 || length == 3) && name[0] == 'f' && name[1] >= '1' && name[1] <= '9') {
        int n = name[1] - '0';
        if (length == 3) {
            if (name[2] < '0' || name[2] > '9') {
                return 0;
            }
            n = n * 10 + (name[2] - '0');
        }
        if (n >= 1 && n <= 12) {
            *key = CEE_KEY_FUNCTION + n;
            return 1;
        }
        return 0;
    }
    if (length == 1 && (unsigned char)name[0] >= 0x21u && (unsigned char)name[0] <= 0x7eu) {
        *key = cee_window_character((unsigned char)name[0]);
        return 1;
    }
    return 0;
}

/* Parses "a,b[,c]" numbers from s[0..n): at most `max`, returns how many. */
static int cee_window_numbers(const char *s, size_t n, double *out, int max) {
    int count = 0;
    size_t i = 0;
    while (i < n && count < max) {
        char buffer[64];
        size_t j = i;
        while (j < n && s[j] != ',') {
            j++;
        }
        if (j - i == 0 || j - i >= sizeof buffer) {
            return -1;
        }
        memcpy(buffer, s + i, j - i);
        buffer[j - i] = '\0';
        char *end = NULL;
        out[count] = strtod(buffer, &end);
        if (end == buffer || *end != '\0') {
            return -1;
        }
        count++;
        i = j + 1;
    }
    return i < n ? -1 : count;
}

static int cee_window_script_add(struct cee_window *w, size_t *capacity, uint64_t frame, int32_t kind, int32_t a,
                                 int32_t b, int32_t c, int32_t d, double x, double y) {
    if (w->script_count == *capacity) {
        size_t next = *capacity == 0 ? 64 : *capacity * 2;
        struct cee_window_scripted *grown = (struct cee_window_scripted *)realloc(w->script, next * sizeof *grown);
        if (grown == NULL) {
            return 0;
        }
        w->script = grown;
        *capacity = next;
    }
    struct cee_window_scripted *s = &w->script[w->script_count++];
    s->frame = frame;
    s->event.kind = kind;
    s->event.fields[0] = a;
    s->event.fields[1] = b;
    s->event.fields[2] = c;
    s->event.fields[3] = d;
    s->event.x = (float)x;
    s->event.y = (float)y;
    return 1;
}

/* One "FRAME:WHAT[:ARGS]" item (type-system.md §26.13); 0 if it is malformed or memory ran out. */
static int cee_window_script_item(struct cee_window *w, size_t *capacity, const char *s, size_t n) {
    size_t colon = 0;
    while (colon < n && s[colon] != ':') {
        colon++;
    }
    if (colon == 0 || colon == n) {
        return 0;
    }
    uint64_t frame = 0;
    for (size_t k = 0; k < colon; k++) {
        if (s[k] < '0' || s[k] > '9' || frame > 1000000000u) {
            return 0;
        }
        frame = frame * 10u + (uint64_t)(s[k] - '0');
    }
    const char *what = s + colon + 1;
    size_t rest = n - colon - 1;
    size_t name_length = 0;
    while (name_length < rest && what[name_length] != ':') {
        name_length++;
    }
    const char *args = what + name_length + (name_length < rest ? 1 : 0);
    size_t args_length = name_length < rest ? rest - name_length - 1 : 0;
    double v[3];
#define CEE_IS(text) (name_length == sizeof(text) - 1 && memcmp(what, text, sizeof(text) - 1) == 0)
    if (CEE_IS("move")) {
        if (cee_window_numbers(args, args_length, v, 3) != 2) {
            return 0;
        }
        return cee_window_script_add(w, capacity, frame, CEE_EVENT_MOVED, 0, 0, 0, 0, v[0], v[1]);
    }
    if (CEE_IS("down") || CEE_IS("up")) {
        if (cee_window_numbers(args, args_length, v, 3) != 3 || v[0] < 0 || v[0] > 2) {
            return 0;
        }
        return cee_window_script_add(w, capacity, frame, CEE_EVENT_BUTTON, (int32_t)v[0], CEE_IS("down") ? 1 : 0, 0,
                                     0, v[1], v[2]);
    }
    if (CEE_IS("click")) {
        if (cee_window_numbers(args, args_length, v, 3) != 2) {
            return 0;
        }
        return cee_window_script_add(w, capacity, frame, CEE_EVENT_BUTTON, 0, 1, 0, 0, v[0], v[1]) &&
               cee_window_script_add(w, capacity, frame + 1u, CEE_EVENT_BUTTON, 0, 0, 0, 0, v[0], v[1]);
    }
    if (CEE_IS("wheel")) {
        if (cee_window_numbers(args, args_length, v, 3) != 2) {
            return 0;
        }
        return cee_window_script_add(w, capacity, frame, CEE_EVENT_WHEEL, 0, 0, 0, 0, v[0], v[1]);
    }
    if (CEE_IS("key") || CEE_IS("keydown") || CEE_IS("keyup")) {
        int32_t key = 0;
        if (!cee_window_key_name(args, args_length, &key)) {
            return 0;
        }
        int ok = 1;
        if (!CEE_IS("keyup")) {
            ok = cee_window_script_add(w, capacity, frame, CEE_EVENT_KEY, key, 1, 0, 0, 0.0, 0.0);
        }
        if (ok && !CEE_IS("keydown")) {
            ok = cee_window_script_add(w, capacity, frame, CEE_EVENT_KEY, key, 0, 0, 0, 0.0, 0.0);
        }
        return ok;
    }
    if (CEE_IS("type")) {
        size_t i = 0;
        while (i < args_length) {
            uint32_t c = cee_window_utf8((const unsigned char *)args, args_length, &i);
            if (!cee_window_script_add(w, capacity, frame, CEE_EVENT_TEXT, (int32_t)c, 0, 0, 0, 0.0, 0.0)) {
                return 0;
            }
        }
        return 1;
    }
    if (CEE_IS("resize")) {
        if (cee_window_numbers(args, args_length, v, 3) != 2 || v[0] < 1 || v[1] < 1 || v[0] > CEE_WINDOW_MAX_SIDE ||
            v[1] > CEE_WINDOW_MAX_SIDE) {
            return 0;
        }
        return cee_window_script_add(w, capacity, frame, CEE_EVENT_RESIZED, (int32_t)v[0], (int32_t)v[1], 0, 0, 0.0,
                                     0.0);
    }
    if (CEE_IS("focus")) {
        if (args_length != 1 || (args[0] != '0' && args[0] != '1')) {
            return 0;
        }
        return cee_window_script_add(w, capacity, frame, CEE_EVENT_FOCUS, args[0] == '1', 0, 0, 0, 0.0, 0.0);
    }
    if (CEE_IS("close")) {
        return args_length == 0 && cee_window_script_add(w, capacity, frame, CEE_EVENT_CLOSED, 0, 0, 0, 0, 0.0, 0.0);
    }
#undef CEE_IS
    return 0;
}

static int cee_window_script_load(struct cee_window *w, const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    size_t capacity = 0, used = 0;
    char *text = NULL;
    for (;;) {
        if (used == capacity) {
            size_t next = capacity == 0 ? 4096 : capacity * 2;
            char *grown = (char *)realloc(text, next);
            if (grown == NULL) {
                free(text);
                fclose(f);
                return 0;
            }
            text = grown;
            capacity = next;
        }
        size_t got = fread(text + used, 1, capacity - used, f);
        used += got;
        if (got == 0) {
            break;
        }
    }
    fclose(f);
    size_t items = 0, start = 0;
    int ok = 1;
    for (size_t k = 0; k <= used && ok; k++) {
        if (k == used || text[k] == ';' || text[k] == '\n' || text[k] == '\r') {
            size_t a = start, b = k;
            while (a < b && (text[a] == ' ' || text[a] == '\t')) {
                a++;
            }
            while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t')) {
                b--;
            }
            if (b > a) {
                ok = cee_window_script_item(w, &items, text + a, b - a);
            }
            start = k + 1;
        }
    }
    free(text);
    if (!ok) {
        return 0;
    }
    /* Stable by frame: an insertion sort keeps the script's order within a frame (a click's up moves to the next). */
    for (size_t i = 1; i < w->script_count; i++) {
        struct cee_window_scripted s = w->script[i];
        size_t j = i;
        while (j > 0 && w->script[j - 1].frame > s.frame) {
            w->script[j] = w->script[j - 1];
            j--;
        }
        w->script[j] = s;
    }
    return 1;
}

/* Moves the script's events for the current frame into the queue; a resize and focus also update the window. */
static void cee_window_script_pump(struct cee_window *w) {
    while (w->script_next < w->script_count && w->script[w->script_next].frame <= w->frames) {
        struct cee_window_event e = w->script[w->script_next++].event;
        if (e.kind == CEE_EVENT_RESIZED) {
            cee_window_resized(w, (uint32_t)e.fields[0], (uint32_t)e.fields[1]);
        } else if (e.kind == CEE_EVENT_FOCUS) {
            cee_window_focus(w, e.fields[0]);
        } else {
            cee_window_push(w, e.kind, e.fields[0], e.fields[1], e.fields[2], e.fields[3], e.x, e.y);
        }
    }
}

static int32_t cee_window_dump(struct cee_window *w, const uint32_t *pixels, uint32_t width, uint32_t height) {
    if (w->dump == NULL) {
        return 0;
    }
    FILE *f = fopen(w->dump, "wb");
    if (f == NULL) {
        return -CEE_WINDOW_RESOURCES;
    }
    fprintf(f, "ceesafe-window %u %u\n", (unsigned)width, (unsigned)height);
    size_t count = (size_t)width * (size_t)height;
    for (size_t k = 0; k < count; k++) {
        unsigned char rgba[4] = {(unsigned char)(pixels[k] & 0xffu), (unsigned char)((pixels[k] >> 8) & 0xffu),
                                 (unsigned char)((pixels[k] >> 16) & 0xffu), (unsigned char)(pixels[k] >> 24)};
        fwrite(rgba, 1, 4, f);
    }
    return fclose(f) == 0 ? 0 : -CEE_WINDOW_RESOURCES;
}

/* ---- The system's windows ---------------------------------------------------------------------------------------- */
/* Each backend defines: cee_native_open (0 or a CEE_WINDOW_* error), cee_native_close, cee_native_pump (moves pending
   system events into the queue), cee_native_present, cee_native_cursor, cee_native_title, cee_native_wait (until an
   event or `seconds`). */

#if defined(__APPLE__)
/* AppKit through the Objective-C runtime. Every call goes through objc_msgSend cast to the method's C type; on x86_64,
   a method that returns an NSRect goes through objc_msgSend_stret. */
#include <dlfcn.h>

typedef void *cee_id;
typedef void *cee_sel;
typedef void (*cee_fn)(void);
typedef struct {
    double x, y;
} cee_point;
typedef struct {
    double width, height;
} cee_size;
typedef struct {
    cee_point origin;
    cee_size size;
} cee_rect;

static struct {
    int state; /* 0 not tried, 1 loaded, -1 failed */
    cee_id (*get_class)(const char *);
    cee_sel (*sel)(const char *);
    cee_fn send;
    cee_fn send_stret;
    cee_id (*allocate_class)(cee_id, const char *, size_t);
    signed char (*add_method)(cee_id, cee_sel, cee_fn, const char *);
    void (*register_class)(cee_id);
    void *(*pool_push)(void);
    void (*pool_pop)(void *);
    void *(*color_space)(void);
    void *(*color_space_named)(void *);
    void **srgb_name;
    void *(*provider_create)(void *, const void *, size_t, void (*)(void *, const void *, size_t));
    void *(*image_create)(size_t, size_t, size_t, size_t, size_t, void *, uint32_t, void *, const double *, bool,
                          int32_t);
    void (*image_release)(void *);
    void (*provider_release)(void *);
    cee_id *default_mode;
    cee_id *gravity_resize;
    cee_id app;
    cee_id delegate_class;
    void *rgb;
} cee_objc;

/* The windows the delegate's callbacks report to (one delegate per window). */
#define CEE_MAC_WINDOWS 16
static struct {
    cee_id nswindow;
    struct cee_window *window;
} cee_mac_windows[CEE_MAC_WINDOWS];

struct cee_mac {
    cee_id window;
    cee_id view;
    cee_id delegate;
};

static cee_fn cee_mac_symbol(void *handle, const char *name) {
    void *p = dlsym(handle, name);
    cee_fn f;
    memcpy(&f, &p, sizeof f); /* ISO C has no object-to-function pointer conversion; POSIX guarantees this one */
    return f;
}

static void *cee_mac_data_symbol(void *handle, const char *name) { return dlsym(handle, name); }

/* Typed sends. */
static cee_id cee_mac_cls(const char *name) { return cee_objc.get_class(name); }
static cee_sel cee_mac_sel(const char *name) { return cee_objc.sel(name); }
static cee_id cee_send(cee_id o, const char *s) { return ((cee_id(*)(cee_id, cee_sel))cee_objc.send)(o, cee_mac_sel(s)); }
static void cee_send_v(cee_id o, const char *s) { ((void (*)(cee_id, cee_sel))cee_objc.send)(o, cee_mac_sel(s)); }
static void cee_send_v_id(cee_id o, const char *s, cee_id a) {
    ((void (*)(cee_id, cee_sel, cee_id))cee_objc.send)(o, cee_mac_sel(s), a);
}
static void cee_send_v_bool(cee_id o, const char *s, signed char a) {
    ((void (*)(cee_id, cee_sel, signed char))cee_objc.send)(o, cee_mac_sel(s), a);
}
static void cee_send_v_long(cee_id o, const char *s, long a) {
    ((void (*)(cee_id, cee_sel, long))cee_objc.send)(o, cee_mac_sel(s), a);
}
static cee_id cee_send_cstr(cee_id o, const char *s, const char *a) {
    return ((cee_id(*)(cee_id, cee_sel, const char *))cee_objc.send)(o, cee_mac_sel(s), a);
}
static const char *cee_send_get_cstr(cee_id o, const char *s) {
    return ((const char *(*)(cee_id, cee_sel))cee_objc.send)(o, cee_mac_sel(s));
}
static unsigned long cee_send_ulong(cee_id o, const char *s) {
    return ((unsigned long (*)(cee_id, cee_sel))cee_objc.send)(o, cee_mac_sel(s));
}
static unsigned short cee_send_ushort(cee_id o, const char *s) {
    return ((unsigned short (*)(cee_id, cee_sel))cee_objc.send)(o, cee_mac_sel(s));
}
static double cee_send_double(cee_id o, const char *s) {
    return ((double (*)(cee_id, cee_sel))cee_objc.send)(o, cee_mac_sel(s));
}
static signed char cee_send_bool(cee_id o, const char *s) {
    return ((signed char (*)(cee_id, cee_sel))cee_objc.send)(o, cee_mac_sel(s));
}
static signed char cee_send_bool_sel(cee_id o, const char *s, cee_sel a) {
    return ((signed char (*)(cee_id, cee_sel, cee_sel))cee_objc.send)(o, cee_mac_sel(s), a);
}
static cee_id cee_send_id_ulong(cee_id o, const char *s, unsigned long a) {
    return ((cee_id(*)(cee_id, cee_sel, unsigned long))cee_objc.send)(o, cee_mac_sel(s), a);
}
static cee_point cee_send_point(cee_id o, const char *s) {
    return ((cee_point(*)(cee_id, cee_sel))cee_objc.send)(o, cee_mac_sel(s));
}
static cee_rect cee_send_rect(cee_id o, const char *s) {
#if defined(__x86_64__)
    cee_rect r;
    ((void (*)(cee_rect *, cee_id, cee_sel))cee_objc.send_stret)(&r, o, cee_mac_sel(s));
    return r;
#else
    return ((cee_rect(*)(cee_id, cee_sel))cee_objc.send)(o, cee_mac_sel(s));
#endif
}
static cee_id cee_nsstring(const char *text) { return cee_send_cstr(cee_mac_cls("NSString"), "stringWithUTF8String:", text); }

static struct cee_window *cee_mac_owner(cee_id nswindow) {
    for (int k = 0; k < CEE_MAC_WINDOWS; k++) {
        if (cee_mac_windows[k].nswindow == nswindow && nswindow != NULL) {
            return cee_mac_windows[k].window;
        }
    }
    return NULL;
}

static void cee_mac_update_size(struct cee_window *w) {
    struct cee_mac *m = (struct cee_mac *)w->native;
    cee_rect r = cee_send_rect(m->view, "bounds");
    double width = r.size.width < 1.0 ? 1.0 : r.size.width, height = r.size.height < 1.0 ? 1.0 : r.size.height;
    cee_window_resized(w, (uint32_t)(width + 0.5), (uint32_t)(height + 0.5));
}

/* The delegate's methods. */
static signed char cee_mac_should_close(cee_id self, cee_sel cmd, cee_id sender) {
    (void)self;
    (void)cmd;
    struct cee_window *w = cee_mac_owner(sender);
    if (w != NULL) {
        cee_window_push(w, CEE_EVENT_CLOSED, 0, 0, 0, 0, 0.0f, 0.0f);
    }
    return 0; /* the program closes it, by dropping the Window */
}

static void cee_mac_did_resize(cee_id self, cee_sel cmd, cee_id notification) {
    (void)self;
    (void)cmd;
    struct cee_window *w = cee_mac_owner(cee_send(notification, "object"));
    if (w != NULL) {
        cee_mac_update_size(w);
    }
}

static void cee_mac_did_focus(cee_id self, cee_sel cmd, cee_id notification) {
    (void)self;
    (void)cmd;
    struct cee_window *w = cee_mac_owner(cee_send(notification, "object"));
    if (w != NULL) {
        cee_window_focus(w, 1);
    }
}

static void cee_mac_did_unfocus(cee_id self, cee_sel cmd, cee_id notification) {
    (void)self;
    (void)cmd;
    struct cee_window *w = cee_mac_owner(cee_send(notification, "object"));
    if (w != NULL) {
        cee_window_focus(w, 0);
    }
}

static int cee_mac_load(void) {
    if (cee_objc.state != 0) {
        return cee_objc.state == 1;
    }
    cee_objc.state = -1;
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL);
    void *appkit = dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_NOW | RTLD_GLOBAL);
    void *graphics = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_NOW | RTLD_GLOBAL);
    void *quartz = dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW | RTLD_GLOBAL);
    if (objc == NULL || appkit == NULL || graphics == NULL || quartz == NULL) {
        return 0;
    }
    cee_objc.get_class = (cee_id(*)(const char *))cee_mac_symbol(objc, "objc_getClass");
    cee_objc.sel = (cee_sel(*)(const char *))cee_mac_symbol(objc, "sel_registerName");
    cee_objc.send = cee_mac_symbol(objc, "objc_msgSend");
#if defined(__x86_64__)
    cee_objc.send_stret = cee_mac_symbol(objc, "objc_msgSend_stret");
#endif
    cee_objc.allocate_class = (cee_id(*)(cee_id, const char *, size_t))cee_mac_symbol(objc, "objc_allocateClassPair");
    cee_objc.add_method = (signed char (*)(cee_id, cee_sel, cee_fn, const char *))cee_mac_symbol(objc, "class_addMethod");
    cee_objc.register_class = (void (*)(cee_id))cee_mac_symbol(objc, "objc_registerClassPair");
    cee_objc.pool_push = (void *(*)(void))cee_mac_symbol(objc, "objc_autoreleasePoolPush");
    cee_objc.pool_pop = (void (*)(void *))cee_mac_symbol(objc, "objc_autoreleasePoolPop");
    cee_objc.color_space = (void *(*)(void))cee_mac_symbol(graphics, "CGColorSpaceCreateDeviceRGB");
    cee_objc.color_space_named = (void *(*)(void *))cee_mac_symbol(graphics, "CGColorSpaceCreateWithName");
    cee_objc.srgb_name = (void **)cee_mac_data_symbol(graphics, "kCGColorSpaceSRGB");
    cee_objc.provider_create = (void *(*)(void *, const void *, size_t, void (*)(void *, const void *, size_t)))
        cee_mac_symbol(graphics, "CGDataProviderCreateWithData");
    cee_objc.image_create = (void *(*)(size_t, size_t, size_t, size_t, size_t, void *, uint32_t, void *,
                                       const double *, bool, int32_t))cee_mac_symbol(graphics, "CGImageCreate");
    cee_objc.image_release = (void (*)(void *))cee_mac_symbol(graphics, "CGImageRelease");
    cee_objc.provider_release = (void (*)(void *))cee_mac_symbol(graphics, "CGDataProviderRelease");
    cee_objc.default_mode = (cee_id *)cee_mac_data_symbol(appkit, "NSDefaultRunLoopMode");
    cee_objc.gravity_resize = (cee_id *)cee_mac_data_symbol(quartz, "kCAGravityResize");
    if (cee_objc.get_class == NULL || cee_objc.sel == NULL || cee_objc.send == NULL || cee_objc.allocate_class == NULL ||
        cee_objc.add_method == NULL || cee_objc.register_class == NULL || cee_objc.pool_push == NULL ||
        cee_objc.pool_pop == NULL || cee_objc.color_space == NULL || cee_objc.provider_create == NULL ||
        cee_objc.image_create == NULL || cee_objc.image_release == NULL || cee_objc.provider_release == NULL ||
        cee_objc.default_mode == NULL || cee_objc.gravity_resize == NULL
#if defined(__x86_64__)
        || cee_objc.send_stret == NULL
#endif
    ) {
        return 0;
    }
    void *pool = cee_objc.pool_push();
    cee_objc.app = cee_send(cee_mac_cls("NSApplication"), "sharedApplication");
    cee_send_v_long(cee_objc.app, "setActivationPolicy:", 0); /* NSApplicationActivationPolicyRegular */
    cee_send_v(cee_objc.app, "finishLaunching");
    cee_id delegate = cee_objc.allocate_class(cee_mac_cls("NSObject"), "CeeSafeWindowDelegate", 0);
    if (delegate == NULL) {
        delegate = cee_mac_cls("CeeSafeWindowDelegate"); /* a second runtime in the process registered it */
    } else {
        cee_objc.add_method(delegate, cee_mac_sel("windowShouldClose:"), (cee_fn)cee_mac_should_close, "c@:@");
        cee_objc.add_method(delegate, cee_mac_sel("windowDidResize:"), (cee_fn)cee_mac_did_resize, "v@:@");
        cee_objc.add_method(delegate, cee_mac_sel("windowDidBecomeKey:"), (cee_fn)cee_mac_did_focus, "v@:@");
        cee_objc.add_method(delegate, cee_mac_sel("windowDidResignKey:"), (cee_fn)cee_mac_did_unfocus, "v@:@");
        cee_objc.register_class(delegate);
    }
    cee_objc.delegate_class = delegate;
    /* The pixels are sRGB values (what ClayWind's rasterizer computes); DeviceRGB would leave them unmanaged. */
    cee_objc.rgb = cee_objc.color_space_named != NULL && cee_objc.srgb_name != NULL
                       ? cee_objc.color_space_named(*cee_objc.srgb_name)
                       : NULL;
    if (cee_objc.rgb == NULL) {
        cee_objc.rgb = cee_objc.color_space();
    }
    cee_objc.pool_pop(pool);
    cee_objc.state = cee_objc.app != NULL && delegate != NULL && cee_objc.rgb != NULL ? 1 : -1;
    return cee_objc.state == 1;
}

static int cee_native_open(struct cee_window *w, const char *title) {
    if (!cee_mac_load()) {
        return CEE_WINDOW_NO_DISPLAY;
    }
    int slot = -1;
    for (int k = 0; k < CEE_MAC_WINDOWS; k++) {
        if (cee_mac_windows[k].nswindow == NULL) {
            slot = k;
            break;
        }
    }
    struct cee_mac *m = (struct cee_mac *)calloc(1, sizeof *m);
    if (slot < 0 || m == NULL) {
        free(m);
        return CEE_WINDOW_RESOURCES;
    }
    void *pool = cee_objc.pool_push();
    cee_rect frame = {{0.0, 0.0}, {(double)w->width, (double)w->height}};
    cee_id window = cee_send(cee_mac_cls("NSWindow"), "alloc");
    /* Titled, closable, miniaturizable, resizable; buffered backing. */
    window = ((cee_id(*)(cee_id, cee_sel, cee_rect, unsigned long, unsigned long, signed char))cee_objc.send)(
        window, cee_mac_sel("initWithContentRect:styleMask:backing:defer:"), frame, 15ul, 2ul, 0);
    if (window == NULL) {
        cee_objc.pool_pop(pool);
        free(m);
        return CEE_WINDOW_RESOURCES;
    }
    m->window = window;
    m->view = cee_send(window, "contentView");
    m->delegate = cee_send(cee_send(cee_objc.delegate_class, "alloc"), "init");
    w->native = m;
    cee_mac_windows[slot].nswindow = window;
    cee_mac_windows[slot].window = w;
    cee_send_v_bool(window, "setReleasedWhenClosed:", 0);
    cee_send_v_id(window, "setDelegate:", m->delegate);
    cee_send_v_bool(window, "setAcceptsMouseMovedEvents:", 1);
    cee_send_v_id(window, "setTitle:", cee_nsstring(title));
    /* An sRGB window: its backing store matches the frames (sRGB CGImages), so Core Animation shows them without
       converting every frame on the CPU; the window server matches the window to the display. */
    cee_send_v_id(window, "setColorSpace:", cee_send(cee_mac_cls("NSColorSpace"), "sRGBColorSpace"));
    cee_send_v_bool(m->view, "setWantsLayer:", 1);
    cee_id layer = cee_send(m->view, "layer");
    cee_send_v_id(layer, "setContentsGravity:", *cee_objc.gravity_resize);
    cee_send_v_id(layer, "setBackgroundColor:", NULL);
    cee_send_v(window, "center");
    cee_send_v_id(window, "makeKeyAndOrderFront:", NULL);
    cee_send_v_bool(cee_objc.app, "activateIgnoringOtherApps:", 1);
    w->scale = (float)cee_send_double(window, "backingScaleFactor");
    cee_objc.pool_pop(pool);
    return 0;
}

static void cee_native_close(struct cee_window *w) {
    struct cee_mac *m = (struct cee_mac *)w->native;
    if (m == NULL) {
        return;
    }
    void *pool = cee_objc.pool_push();
    for (int k = 0; k < CEE_MAC_WINDOWS; k++) {
        if (cee_mac_windows[k].window == w) {
            cee_mac_windows[k].nswindow = NULL;
            cee_mac_windows[k].window = NULL;
        }
    }
    cee_send_v_id(m->window, "setDelegate:", NULL);
    cee_send_v(m->window, "close");
    cee_send_v(m->window, "release");
    cee_send_v(m->delegate, "release");
    cee_objc.pool_pop(pool);
    free(m);
    w->native = NULL;
}

static int32_t cee_mac_modifiers(unsigned long flags) {
    return ((flags & (1ul << 17)) ? CEE_MOD_SHIFT : 0) | ((flags & (1ul << 18)) ? CEE_MOD_CONTROL : 0) |
           ((flags & (1ul << 19)) ? CEE_MOD_ALT : 0) | ((flags & (1ul << 20)) ? CEE_MOD_COMMAND : 0);
}

static int32_t cee_mac_key(unsigned short code, cee_id event) {
    switch (code) {
    case 48: return CEE_KEY_TAB;
    case 36:
    case 76: return CEE_KEY_ENTER;
    case 53: return CEE_KEY_ESCAPE;
    case 51: return CEE_KEY_BACKSPACE;
    case 117: return CEE_KEY_DELETE;
    case 123: return CEE_KEY_LEFT;
    case 124: return CEE_KEY_RIGHT;
    case 126: return CEE_KEY_UP;
    case 125: return CEE_KEY_DOWN;
    case 115: return CEE_KEY_HOME;
    case 119: return CEE_KEY_END;
    case 116: return CEE_KEY_PAGE_UP;
    case 121: return CEE_KEY_PAGE_DOWN;
    case 49: return CEE_KEY_SPACE;
    case 122: return CEE_KEY_FUNCTION + 1;
    case 120: return CEE_KEY_FUNCTION + 2;
    case 99: return CEE_KEY_FUNCTION + 3;
    case 118: return CEE_KEY_FUNCTION + 4;
    case 96: return CEE_KEY_FUNCTION + 5;
    case 97: return CEE_KEY_FUNCTION + 6;
    case 98: return CEE_KEY_FUNCTION + 7;
    case 100: return CEE_KEY_FUNCTION + 8;
    case 101: return CEE_KEY_FUNCTION + 9;
    case 109: return CEE_KEY_FUNCTION + 10;
    case 103: return CEE_KEY_FUNCTION + 11;
    case 111: return CEE_KEY_FUNCTION + 12;
    default: break;
    }
    /* The unshifted character on the layout in use (charactersByApplyingModifiers:, macOS 10.15), or the one
       ignoring modifiers, with letters lowered. */
    cee_id text = NULL;
    if (cee_send_bool_sel(event, "respondsToSelector:", cee_mac_sel("charactersByApplyingModifiers:"))) {
        text = cee_send_id_ulong(event, "charactersByApplyingModifiers:", 0ul);
    } else {
        text = cee_send(event, "charactersIgnoringModifiers");
    }
    const char *utf8 = text != NULL ? cee_send_get_cstr(text, "UTF8String") : NULL;
    if (utf8 != NULL && utf8[0] != '\0') {
        size_t i = 0;
        uint32_t c = cee_window_utf8((const unsigned char *)utf8, strlen(utf8), &i);
        if (c >= 0x21u && c < 0xf700u) {
            return cee_window_character(c);
        }
    }
    return CEE_KEY_OTHER + code;
}

static void cee_mac_cursor_set(int32_t cursor) {
    static const char *const names[] = {"arrowCursor",       "pointingHandCursor",    "IBeamCursor",
                                        "crosshairCursor",   "openHandCursor",        "resizeLeftRightCursor",
                                        "resizeUpDownCursor"};
    int32_t k = cursor >= 0 && cursor < 7 ? cursor : 0;
    cee_send_v(cee_send(cee_mac_cls("NSCursor"), names[k]), "set");
}

/* One NSEvent into the queue; whether AppKit should still see it. */
static int cee_mac_translate(struct cee_window *w, cee_id event) {
    struct cee_mac *m = (struct cee_mac *)w->native;
    unsigned long type = cee_send_ulong(event, "type");
    cee_id target = cee_send(event, "window");
    double height = (double)w->height;
    switch (type) {
    case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 25: case 26: case 27: {
        if (target != m->window && target != NULL) {
            return 1;
        }
        cee_point p = target == NULL ? cee_send_point(m->window, "mouseLocationOutsideOfEventStream")
                                     : cee_send_point(event, "locationInWindow");
        float x = (float)p.x, y = (float)(height - p.y);
        int inside = p.x >= 0.0 && p.y >= 0.0 && p.x < (double)w->width && p.y < height;
        if (type == 5 || type == 6 || type == 7 || type == 27) {
            cee_window_push(w, CEE_EVENT_MOVED, 0, 0, 0, 0, x, y);
            cee_mac_cursor_set(inside ? w->cursor : 0);
        } else {
            int32_t button = (type == 1 || type == 2) ? 0 : (type == 3 || type == 4) ? 1 : 2;
            int32_t down = type == 1 || type == 3 || type == 25;
            if (!down || inside) {
                cee_window_push(w, CEE_EVENT_BUTTON, button, down, 0, 0, x, y);
            }
        }
        return 1;
    }
    case 22: { /* scroll wheel */
        if (target != m->window) {
            return 1;
        }
        double dx = cee_send_double(event, "scrollingDeltaX"), dy = cee_send_double(event, "scrollingDeltaY");
        if (cee_send_bool(event, "hasPreciseScrollingDeltas")) {
            dx /= 10.0;
            dy /= 10.0;
        }
        if (dx != 0.0 || dy != 0.0) {
            cee_window_push(w, CEE_EVENT_WHEEL, 0, 0, 0, 0, (float)dx, (float)dy);
        }
        return 1;
    }
    case 10: case 11: { /* key down, key up */
        if (target != m->window) {
            return 1;
        }
        unsigned short code = cee_send_ushort(event, "keyCode");
        int32_t modifiers = cee_mac_modifiers(cee_send_ulong(event, "modifierFlags"));
        int32_t key = cee_mac_key(code, event);
        int32_t down = type == 10;
        if (down && (modifiers & CEE_MOD_COMMAND) && (key == CEE_KEY_CHARACTER + 'q' || key == CEE_KEY_CHARACTER + 'w')) {
            cee_window_push(w, CEE_EVENT_CLOSED, 0, 0, 0, 0, 0.0f, 0.0f); /* ⌘Q and ⌘W ask to close */
            return 0;
        }
        cee_window_push(w, CEE_EVENT_KEY, key, down, down ? (int32_t)cee_send_bool(event, "isARepeat") : 0, modifiers,
                        0.0f, 0.0f);
        if (down && !(modifiers & (CEE_MOD_COMMAND | CEE_MOD_CONTROL))) {
            cee_id characters = cee_send(event, "characters");
            const char *utf8 = characters != NULL ? cee_send_get_cstr(characters, "UTF8String") : NULL;
            if (utf8 != NULL) {
                size_t n = strlen(utf8), i = 0;
                while (i < n) {
                    cee_window_text(w, cee_window_utf8((const unsigned char *)utf8, n, &i));
                }
            }
        }
        return 0; /* not to AppKit: an unhandled key beeps */
    }
    default:
        return 1;
    }
}

static void cee_native_pump(struct cee_window *w) {
    void *pool = cee_objc.pool_push();
    cee_id past = cee_send(cee_mac_cls("NSDate"), "distantPast");
    for (int k = 0; k < 256; k++) {
        cee_id event = ((cee_id(*)(cee_id, cee_sel, unsigned long long, cee_id, cee_id, signed char))cee_objc.send)(
            cee_objc.app, cee_mac_sel("nextEventMatchingMask:untilDate:inMode:dequeue:"), ~0ull, past,
            *cee_objc.default_mode, 1);
        if (event == NULL) {
            break;
        }
        if (cee_mac_translate(w, event)) {
            cee_send_v_id(cee_objc.app, "sendEvent:", event);
        }
    }
    cee_send_v(cee_objc.app, "updateWindows");
    w->scale = (float)cee_send_double(((struct cee_mac *)w->native)->window, "backingScaleFactor");
    cee_objc.pool_pop(pool);
}

static void cee_mac_release_pixels(void *info, const void *data, size_t size) {
    (void)data;
    (void)size;
    free(info);
}

static int32_t cee_native_present(struct cee_window *w, const uint32_t *pixels, uint32_t width, uint32_t height) {
    struct cee_mac *m = (struct cee_mac *)w->native;
    size_t bytes = (size_t)width * (size_t)height * 4u;
    void *copy = malloc(bytes);
    if (copy == NULL) {
        return -CEE_WINDOW_RESOURCES;
    }
    memcpy(copy, pixels, bytes);
    void *provider = cee_objc.provider_create(copy, copy, bytes, cee_mac_release_pixels);
    if (provider == NULL) {
        free(copy);
        return -CEE_WINDOW_RESOURCES;
    }
    /* kCGImageAlphaNoneSkipLast, default byte order: bytes R, G, B, then an alpha the opaque window ignores. */
    void *image = cee_objc.image_create(width, height, 8, 32, (size_t)width * 4u, cee_objc.rgb, 5u, provider, NULL,
                                        false, 0);
    cee_objc.provider_release(provider);
    if (image == NULL) {
        return -CEE_WINDOW_RESOURCES;
    }
    void *pool = cee_objc.pool_push();
    cee_send_v_id(cee_send(m->view, "layer"), "setContents:", image);
    cee_objc.pool_pop(pool);
    cee_objc.image_release(image);
    return 0;
}

static void cee_native_cursor(struct cee_window *w) {
    void *pool = cee_objc.pool_push();
    cee_mac_cursor_set(w->cursor);
    cee_objc.pool_pop(pool);
}

static void cee_native_title(struct cee_window *w, const char *title) {
    void *pool = cee_objc.pool_push();
    cee_send_v_id(((struct cee_mac *)w->native)->window, "setTitle:", cee_nsstring(title));
    cee_objc.pool_pop(pool);
}

static void cee_native_wait(struct cee_window *w, double seconds) {
    (void)w;
    void *pool = cee_objc.pool_push();
    cee_id until = ((cee_id(*)(cee_id, cee_sel, double))cee_objc.send)(
        cee_mac_cls("NSDate"), cee_mac_sel("dateWithTimeIntervalSinceNow:"), seconds);
    /* Waits for an event without taking it; the next poll takes it. */
    ((cee_id(*)(cee_id, cee_sel, unsigned long long, cee_id, cee_id, signed char))cee_objc.send)(
        cee_objc.app, cee_mac_sel("nextEventMatchingMask:untilDate:inMode:dequeue:"), ~0ull, until,
        *cee_objc.default_mode, 0);
    cee_objc.pool_pop(pool);
}

#elif defined(_WIN32)
/* Win32 and GDI. The window procedure queues events while PeekMessageW dispatches; a frame is kept as BGRA for
   WM_PAINT and drawn with StretchDIBits. */
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

struct cee_win32 {
    HWND hwnd;
    uint32_t *frame; /* BGRA, the last presented frame */
    uint32_t frame_width, frame_height;
    int captured;
};

static const wchar_t cee_win32_class[] = L"CeeSafeWindow";
static int cee_win32_registered;

static float cee_win32_scale(HWND hwnd) {
    typedef UINT(WINAPI * dpi_for_window)(HWND);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    FARPROC p = user32 != NULL ? GetProcAddress(user32, "GetDpiForWindow") : NULL;
    if (p == NULL) {
        return 1.0f;
    }
    dpi_for_window f;
    memcpy(&f, &p, sizeof f);
    UINT dpi = f(hwnd);
    return dpi == 0 ? 1.0f : (float)dpi / 96.0f;
}

static void cee_win32_paint(struct cee_window *w, HDC dc) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    RECT client;
    GetClientRect(n->hwnd, &client);
    if (n->frame == NULL) {
        FillRect(dc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return;
    }
    BITMAPINFO info;
    memset(&info, 0, sizeof info);
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = (LONG)n->frame_width;
    info.bmiHeader.biHeight = -(LONG)n->frame_height; /* top-down */
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, 0, 0, client.right - client.left, client.bottom - client.top, 0, 0, (int)n->frame_width,
                  (int)n->frame_height, n->frame, &info, DIB_RGB_COLORS, SRCCOPY);
}

static int32_t cee_win32_modifiers(void) {
    return (GetKeyState(VK_SHIFT) < 0 ? CEE_MOD_SHIFT : 0) | (GetKeyState(VK_CONTROL) < 0 ? CEE_MOD_CONTROL : 0) |
           (GetKeyState(VK_MENU) < 0 ? CEE_MOD_ALT : 0) |
           ((GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) ? CEE_MOD_COMMAND : 0);
}

static int32_t cee_win32_key(WPARAM vk) {
    switch (vk) {
    case VK_TAB: return CEE_KEY_TAB;
    case VK_RETURN: return CEE_KEY_ENTER;
    case VK_ESCAPE: return CEE_KEY_ESCAPE;
    case VK_BACK: return CEE_KEY_BACKSPACE;
    case VK_DELETE: return CEE_KEY_DELETE;
    case VK_LEFT: return CEE_KEY_LEFT;
    case VK_RIGHT: return CEE_KEY_RIGHT;
    case VK_UP: return CEE_KEY_UP;
    case VK_DOWN: return CEE_KEY_DOWN;
    case VK_HOME: return CEE_KEY_HOME;
    case VK_END: return CEE_KEY_END;
    case VK_PRIOR: return CEE_KEY_PAGE_UP;
    case VK_NEXT: return CEE_KEY_PAGE_DOWN;
    case VK_SPACE: return CEE_KEY_SPACE;
    default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        return CEE_KEY_FUNCTION + 1 + (int32_t)(vk - VK_F1);
    }
    UINT c = MapVirtualKeyW((UINT)vk, 2 /* MAPVK_VK_TO_CHAR */) & 0x7fffffffu;
    if (c >= 0x21u) {
        return cee_window_character(c);
    }
    return CEE_KEY_OTHER + (int32_t)vk;
}

static HCURSOR cee_win32_cursor(int32_t cursor) {
    /* IDC_ARROW, IDC_HAND, IDC_IBEAM, IDC_CROSS, IDC_SIZEALL, IDC_SIZEWE, IDC_SIZENS, as wide resource names. */
    static const WORD shapes[] = {32512, 32649, 32513, 32515, 32646, 32644, 32645};
    return LoadCursorW(NULL, MAKEINTRESOURCEW(shapes[cursor >= 0 && cursor < 7 ? cursor : 0]));
}

static LRESULT CALLBACK cee_win32_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    struct cee_window *w = (struct cee_window *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (w == NULL) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    float scale = w->scale > 0.0f ? w->scale : 1.0f;
    float x = (float)(short)LOWORD(lparam) / scale, y = (float)(short)HIWORD(lparam) / scale;
    static uint32_t high_surrogate;
    switch (message) {
    case WM_CLOSE:
        cee_window_push(w, CEE_EVENT_CLOSED, 0, 0, 0, 0, 0.0f, 0.0f);
        return 0;
    case WM_SIZE: {
        uint32_t width = (uint32_t)((float)LOWORD(lparam) / scale + 0.5f);
        uint32_t height = (uint32_t)((float)HIWORD(lparam) / scale + 0.5f);
        if (width > 0 && height > 0) {
            cee_window_resized(w, width, height);
        }
        return 0;
    }
    case WM_DPICHANGED: {
        RECT *suggested = (RECT *)lparam;
        w->scale = (float)HIWORD(wparam) / 96.0f;
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SETFOCUS: cee_window_focus(w, 1); return 0;
    case WM_KILLFOCUS: cee_window_focus(w, 0); return 0;
    case WM_MOUSEMOVE: cee_window_push(w, CEE_EVENT_MOVED, 0, 0, 0, 0, x, y); return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: {
        int32_t button = (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)   ? 0
                         : (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) ? 1
                                                                                  : 2;
        int32_t down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN;
        if (down) {
            if (n->captured++ == 0) {
                SetCapture(hwnd);
            }
        } else if (n->captured > 0 && --n->captured == 0) {
            ReleaseCapture();
        }
        cee_window_push(w, CEE_EVENT_BUTTON, button, down, 0, 0, x, y);
        return 0;
    }
    case WM_MOUSEWHEEL:
        cee_window_push(w, CEE_EVENT_WHEEL, 0, 0, 0, 0, 0.0f, (float)(short)HIWORD(wparam) / 120.0f);
        return 0;
    case WM_MOUSEHWHEEL:
        cee_window_push(w, CEE_EVENT_WHEEL, 0, 0, 0, 0, -(float)(short)HIWORD(wparam) / 120.0f, 0.0f);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        int32_t down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        int32_t repeat = down && ((lparam >> 30) & 1) != 0;
        cee_window_push(w, CEE_EVENT_KEY, cee_win32_key(wparam), down, repeat, cee_win32_modifiers(), 0.0f, 0.0f);
        if (message == WM_SYSKEYDOWN && wparam == VK_F4) {
            break; /* Alt+F4 still closes through WM_CLOSE */
        }
        return 0;
    }
    case WM_CHAR: {
        uint32_t unit = (uint32_t)wparam;
        if (unit >= 0xd800u && unit <= 0xdbffu) {
            high_surrogate = unit;
            return 0;
        }
        if (unit >= 0xdc00u && unit <= 0xdfffu) {
            if (high_surrogate != 0) {
                cee_window_text(w, 0x10000u + ((high_surrogate - 0xd800u) << 10) + (unit - 0xdc00u));
            }
            high_surrogate = 0;
            return 0;
        }
        high_surrogate = 0;
        cee_window_text(w, unit);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            SetCursor(cee_win32_cursor(w->cursor));
            return TRUE;
        }
        break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(hwnd, &paint);
        cee_win32_paint(w, dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static wchar_t *cee_win32_wide(const char *text) {
    int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    wchar_t *wide = count > 0 ? (wchar_t *)malloc((size_t)count * sizeof(wchar_t)) : NULL;
    if (wide != NULL) {
        MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, count);
    }
    return wide;
}

static int cee_native_open(struct cee_window *w, const char *title) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    if (!cee_win32_registered) {
        typedef BOOL(WINAPI * set_awareness)(HANDLE);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        FARPROC p = user32 != NULL ? GetProcAddress(user32, "SetProcessDpiAwarenessContext") : NULL;
        if (p != NULL) {
            set_awareness f;
            memcpy(&f, &p, sizeof f);
            f((HANDLE)-4); /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
        }
        WNDCLASSEXW c;
        memset(&c, 0, sizeof c);
        c.cbSize = sizeof c;
        c.style = CS_HREDRAW | CS_VREDRAW;
        c.lpfnWndProc = cee_win32_proc;
        c.hInstance = instance;
        c.lpszClassName = cee_win32_class;
        c.hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(32512)); /* IDI_APPLICATION */
        if (RegisterClassExW(&c) == 0) {
            return CEE_WINDOW_RESOURCES;
        }
        cee_win32_registered = 1;
    }
    struct cee_win32 *n = (struct cee_win32 *)calloc(1, sizeof *n);
    wchar_t *wide = cee_win32_wide(title);
    if (n == NULL || wide == NULL) {
        free(n);
        free(wide);
        return CEE_WINDOW_RESOURCES;
    }
    DWORD style = WS_OVERLAPPEDWINDOW;
    HWND hwnd = CreateWindowExW(0, cee_win32_class, wide, style, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, NULL, NULL,
                                instance, NULL);
    free(wide);
    if (hwnd == NULL) {
        free(n);
        return CEE_WINDOW_RESOURCES;
    }
    n->hwnd = hwnd;
    w->native = n;
    w->scale = cee_win32_scale(hwnd);
    RECT r = {0, 0, (LONG)((float)w->width * w->scale + 0.5f), (LONG)((float)w->height * w->scale + 0.5f)};
    AdjustWindowRectEx(&r, style, FALSE, 0);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)w);
    SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    UpdateWindow(hwnd);
    return 0;
}

static void cee_native_close(struct cee_window *w) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    if (n == NULL) {
        return;
    }
    SetWindowLongPtrW(n->hwnd, GWLP_USERDATA, 0);
    DestroyWindow(n->hwnd);
    free(n->frame);
    free(n);
    w->native = NULL;
}

static void cee_native_pump(struct cee_window *w) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    MSG message;
    for (int k = 0; k < 256 && PeekMessageW(&message, NULL, 0, 0, PM_REMOVE); k++) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    w->scale = cee_win32_scale(n->hwnd);
}

static int32_t cee_native_present(struct cee_window *w, const uint32_t *pixels, uint32_t width, uint32_t height) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    size_t count = (size_t)width * (size_t)height;
    if (n->frame == NULL || n->frame_width != width || n->frame_height != height) {
        uint32_t *grown = (uint32_t *)realloc(n->frame, count * sizeof(uint32_t));
        if (grown == NULL) {
            return -CEE_WINDOW_RESOURCES;
        }
        n->frame = grown;
        n->frame_width = width;
        n->frame_height = height;
    }
    for (size_t k = 0; k < count; k++) { /* RGBA bytes to GDI's BGRX */
        uint32_t p = pixels[k];
        n->frame[k] = ((p & 0xffu) << 16) | (p & 0xff00u) | ((p >> 16) & 0xffu);
    }
    HDC dc = GetDC(n->hwnd);
    if (dc != NULL) {
        cee_win32_paint(w, dc);
        ReleaseDC(n->hwnd, dc);
    }
    return 0;
}

static void cee_native_cursor(struct cee_window *w) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    POINT p;
    RECT client;
    if (GetCursorPos(&p) && ScreenToClient(n->hwnd, &p) && GetClientRect(n->hwnd, &client) && PtInRect(&client, p)) {
        SetCursor(cee_win32_cursor(w->cursor));
    }
}

static void cee_native_title(struct cee_window *w, const char *title) {
    wchar_t *wide = cee_win32_wide(title);
    if (wide != NULL) {
        SetWindowTextW(((struct cee_win32 *)w->native)->hwnd, wide);
        free(wide);
    }
}

static void cee_native_wait(struct cee_window *w, double seconds) {
    (void)w;
    DWORD milliseconds = seconds <= 0.0 ? 0u : (DWORD)(seconds * 1000.0);
    MsgWaitForMultipleObjects(0, NULL, FALSE, milliseconds, QS_ALLINPUT);
}

#elif defined(__linux__)
/* X11 through libX11.so.6, loaded with dlopen: the declarations below are Xlib's (X11/Xlib.h, LP64), so no headers
   are needed to build. A frame is converted to the visual's pixel layout, scaled to the window's size, and drawn
   with XPutImage. */
#include <dlfcn.h>
#include <sys/select.h>

typedef unsigned long cee_xid;
typedef struct cee_x_display cee_x_display;
typedef struct {
    int type;
    unsigned long serial;
    int send_event;
    cee_x_display *display;
    cee_xid window;
} cee_x_any;
typedef struct {
    int type;
    unsigned long serial;
    int send_event;
    cee_x_display *display;
    cee_xid window, root, subwindow;
    unsigned long time;
    int x, y, x_root, y_root;
    unsigned int state, code; /* keycode or button */
    int same_screen;
} cee_x_input;
typedef struct {
    int type;
    unsigned long serial;
    int send_event;
    cee_x_display *display;
    cee_xid event, window;
    int x, y, width, height, border_width;
    cee_xid above;
    int override_redirect;
} cee_x_configure;
typedef struct {
    int type;
    unsigned long serial;
    int send_event;
    cee_x_display *display;
    cee_xid window;
    unsigned long message_type;
    int format;
    long data[5];
} cee_x_client;
typedef union {
    int type;
    cee_x_any any;
    cee_x_input input;
    cee_x_configure configure;
    cee_x_client client;
    long pad[24];
} cee_x_event;
typedef struct cee_x_image {
    int width, height, xoffset, format;
    char *data;
    int byte_order, bitmap_unit, bitmap_bit_order, bitmap_pad, depth, bytes_per_line, bits_per_pixel;
    unsigned long red_mask, green_mask, blue_mask;
    char *obdata;
    struct {
        struct cee_x_image *(*create_image)(void);
        int (*destroy_image)(struct cee_x_image *);
        unsigned long (*get_pixel)(struct cee_x_image *, int, int);
        int (*put_pixel)(struct cee_x_image *, int, int, unsigned long);
        struct cee_x_image *(*sub_image)(struct cee_x_image *, int, int, unsigned int, unsigned int);
        int (*add_pixel)(struct cee_x_image *, long);
    } f;
} cee_x_image;

static struct {
    int state;
    cee_x_display *(*open_display)(const char *);
    int (*close_display)(cee_x_display *);
    int (*default_screen)(cee_x_display *);
    cee_xid (*root_window)(cee_x_display *, int);
    void *(*default_visual)(cee_x_display *, int);
    int (*default_depth)(cee_x_display *, int);
    void *(*default_gc)(cee_x_display *, int);
    cee_xid (*create_simple_window)(cee_x_display *, cee_xid, int, int, unsigned int, unsigned int, unsigned int,
                                    unsigned long, unsigned long);
    int (*select_input)(cee_x_display *, cee_xid, long);
    int (*map_window)(cee_x_display *, cee_xid);
    int (*destroy_window)(cee_x_display *, cee_xid);
    int (*store_name)(cee_x_display *, cee_xid, const char *);
    unsigned long (*intern_atom)(cee_x_display *, const char *, int);
    int (*set_wm_protocols)(cee_x_display *, cee_xid, unsigned long *, int);
    int (*pending)(cee_x_display *);
    int (*next_event)(cee_x_display *, cee_x_event *);
    int (*flush)(cee_x_display *);
    int (*connection_number)(cee_x_display *);
    cee_x_image *(*create_image)(cee_x_display *, void *, unsigned int, int, int, char *, unsigned int, unsigned int,
                                 int, int);
    int (*put_image)(cee_x_display *, cee_xid, void *, cee_x_image *, int, int, int, int, unsigned int, unsigned int);
    int (*lookup_string)(cee_x_input *, char *, int, unsigned long *, void *);
    unsigned long (*lookup_keysym)(cee_x_input *, int);
    cee_xid (*create_font_cursor)(cee_x_display *, unsigned int);
    int (*define_cursor)(cee_x_display *, cee_xid, cee_xid);
    int (*detectable_repeat)(cee_x_display *, int, int *);
} cee_x;

struct cee_x11 {
    cee_x_display *display;
    cee_xid window;
    int screen;
    void *visual;
    int depth;
    unsigned long delete_atom;
    uint32_t *frame; /* the last presented frame, for Expose */
    uint32_t frame_width, frame_height;
    unsigned char keys[32]; /* keycodes held down, for repeats */
    cee_xid cursors[7];
};

typedef void (*cee_x_fn)(void);
static cee_x_fn cee_x_symbol(void *handle, const char *name) {
    void *p = dlsym(handle, name);
    cee_x_fn f;
    memcpy(&f, &p, sizeof f); /* ISO C has no object-to-function pointer conversion; POSIX guarantees this one */
    return f;
}

static int cee_x_load(void) {
    if (cee_x.state != 0) {
        return cee_x.state == 1;
    }
    cee_x.state = -1;
    void *x = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (x == NULL) {
        return 0;
    }
#define CEE_X(field, type, name)                                                                                       \
    cee_x.field = (type)cee_x_symbol(x, name);                                                                         \
    if (cee_x.field == NULL) {                                                                                         \
        return 0;                                                                                                      \
    }
    CEE_X(open_display, cee_x_display * (*)(const char *), "XOpenDisplay")
    CEE_X(close_display, int (*)(cee_x_display *), "XCloseDisplay")
    CEE_X(default_screen, int (*)(cee_x_display *), "XDefaultScreen")
    CEE_X(root_window, cee_xid(*)(cee_x_display *, int), "XRootWindow")
    CEE_X(default_visual, void *(*)(cee_x_display *, int), "XDefaultVisual")
    CEE_X(default_depth, int (*)(cee_x_display *, int), "XDefaultDepth")
    CEE_X(default_gc, void *(*)(cee_x_display *, int), "XDefaultGC")
    CEE_X(create_simple_window,
          cee_xid(*)(cee_x_display *, cee_xid, int, int, unsigned int, unsigned int, unsigned int, unsigned long,
                     unsigned long),
          "XCreateSimpleWindow")
    CEE_X(select_input, int (*)(cee_x_display *, cee_xid, long), "XSelectInput")
    CEE_X(map_window, int (*)(cee_x_display *, cee_xid), "XMapWindow")
    CEE_X(destroy_window, int (*)(cee_x_display *, cee_xid), "XDestroyWindow")
    CEE_X(store_name, int (*)(cee_x_display *, cee_xid, const char *), "XStoreName")
    CEE_X(intern_atom, unsigned long (*)(cee_x_display *, const char *, int), "XInternAtom")
    CEE_X(set_wm_protocols, int (*)(cee_x_display *, cee_xid, unsigned long *, int), "XSetWMProtocols")
    CEE_X(pending, int (*)(cee_x_display *), "XPending")
    CEE_X(next_event, int (*)(cee_x_display *, cee_x_event *), "XNextEvent")
    CEE_X(flush, int (*)(cee_x_display *), "XFlush")
    CEE_X(connection_number, int (*)(cee_x_display *), "XConnectionNumber")
    CEE_X(create_image,
          cee_x_image * (*)(cee_x_display *, void *, unsigned int, int, int, char *, unsigned int, unsigned int, int,
                            int),
          "XCreateImage")
    CEE_X(put_image,
          int (*)(cee_x_display *, cee_xid, void *, cee_x_image *, int, int, int, int, unsigned int, unsigned int),
          "XPutImage")
    CEE_X(lookup_string, int (*)(cee_x_input *, char *, int, unsigned long *, void *), "XLookupString")
    CEE_X(lookup_keysym, unsigned long (*)(cee_x_input *, int), "XLookupKeysym")
    CEE_X(create_font_cursor, cee_xid(*)(cee_x_display *, unsigned int), "XCreateFontCursor")
    CEE_X(define_cursor, int (*)(cee_x_display *, cee_xid, cee_xid), "XDefineCursor")
    CEE_X(detectable_repeat, int (*)(cee_x_display *, int, int *), "XkbSetDetectableAutoRepeat")
#undef CEE_X
    cee_x.state = 1;
    return 1;
}

static int cee_native_open(struct cee_window *w, const char *title) {
    if (!cee_x_load()) {
        return CEE_WINDOW_NO_DISPLAY;
    }
    cee_x_display *display = cee_x.open_display(NULL);
    if (display == NULL) {
        return CEE_WINDOW_NO_DISPLAY;
    }
    struct cee_x11 *n = (struct cee_x11 *)calloc(1, sizeof *n);
    if (n == NULL) {
        cee_x.close_display(display);
        return CEE_WINDOW_RESOURCES;
    }
    n->display = display;
    n->screen = cee_x.default_screen(display);
    n->visual = cee_x.default_visual(display, n->screen);
    n->depth = cee_x.default_depth(display, n->screen);
    if (n->depth < 24) {
        cee_x.close_display(display);
        free(n);
        return CEE_WINDOW_UNSUPPORTED; /* only TrueColor visuals of 24 or 32 bits */
    }
    n->window = cee_x.create_simple_window(display, cee_x.root_window(display, n->screen), 0, 0, w->width, w->height,
                                           0, 0ul, 0ul);
    if (n->window == 0) {
        cee_x.close_display(display);
        free(n);
        return CEE_WINDOW_RESOURCES;
    }
    /* KeyPress, KeyRelease, ButtonPress, ButtonRelease, PointerMotion, Exposure, StructureNotify, FocusChange. */
    cee_x.select_input(display, n->window, 1L | 2L | 4L | 8L | 64L | (1L << 15) | (1L << 17) | (1L << 21));
    n->delete_atom = cee_x.intern_atom(display, "WM_DELETE_WINDOW", 0);
    cee_x.set_wm_protocols(display, n->window, &n->delete_atom, 1);
    cee_x.detectable_repeat(display, 1, NULL);
    cee_x.store_name(display, n->window, title);
    cee_x.map_window(display, n->window);
    cee_x.flush(display);
    w->native = n;
    w->scale = 1.0f;
    return 0;
}

static void cee_native_close(struct cee_window *w) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    if (n == NULL) {
        return;
    }
    cee_x.destroy_window(n->display, n->window);
    cee_x.close_display(n->display);
    free(n->frame);
    free(n);
    w->native = NULL;
}

static int32_t cee_x_key(unsigned long sym) {
    switch (sym) {
    case 0xff09: return CEE_KEY_TAB;
    case 0xff0d:
    case 0xff8d: return CEE_KEY_ENTER;
    case 0xff1b: return CEE_KEY_ESCAPE;
    case 0xff08: return CEE_KEY_BACKSPACE;
    case 0xffff: return CEE_KEY_DELETE;
    case 0xff51: return CEE_KEY_LEFT;
    case 0xff53: return CEE_KEY_RIGHT;
    case 0xff52: return CEE_KEY_UP;
    case 0xff54: return CEE_KEY_DOWN;
    case 0xff50: return CEE_KEY_HOME;
    case 0xff57: return CEE_KEY_END;
    case 0xff55: return CEE_KEY_PAGE_UP;
    case 0xff56: return CEE_KEY_PAGE_DOWN;
    case 0x20: return CEE_KEY_SPACE;
    default: break;
    }
    if (sym >= 0xffbe && sym <= 0xffc9) {
        return CEE_KEY_FUNCTION + 1 + (int32_t)(sym - 0xffbe);
    }
    if (sym >= 0x21 && sym <= 0x7e) {
        return cee_window_character((uint32_t)sym);
    }
    return CEE_KEY_OTHER + (int32_t)(sym & 0xffffu);
}

static int32_t cee_x_modifiers(unsigned int state) {
    return ((state & 1u) ? CEE_MOD_SHIFT : 0) | ((state & 4u) ? CEE_MOD_CONTROL : 0) |
           ((state & 8u) ? CEE_MOD_ALT : 0) | ((state & 64u) ? CEE_MOD_COMMAND : 0);
}

/* A keysym's Unicode scalar: Latin-1 keysyms are their code point, 0x01000000 + c is c. */
static uint32_t cee_x_scalar(unsigned long sym) {
    if ((sym >= 0x20 && sym <= 0x7e) || (sym >= 0xa0 && sym <= 0xff)) {
        return (uint32_t)sym;
    }
    if ((sym & 0xff000000ul) == 0x01000000ul) {
        return (uint32_t)(sym & 0x00fffffful);
    }
    return 0;
}

static void cee_x_draw(struct cee_window *w);

static void cee_native_pump(struct cee_window *w) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    for (int k = 0; k < 256 && cee_x.pending(n->display) > 0; k++) {
        cee_x_event e;
        cee_x.next_event(n->display, &e);
        switch (e.type) {
        case 2: /* KeyPress */
        case 3: { /* KeyRelease */
            unsigned int code = e.input.code & 0xffu;
            int32_t down = e.type == 2;
            int32_t repeat = down && (n->keys[code / 8u] & (1u << (code % 8u))) != 0;
            if (down) {
                n->keys[code / 8u] = (unsigned char)(n->keys[code / 8u] | (1u << (code % 8u)));
            } else {
                n->keys[code / 8u] = (unsigned char)(n->keys[code / 8u] & ~(1u << (code % 8u)));
            }
            unsigned long unshifted = cee_x.lookup_keysym(&e.input, 0);
            char text[32];
            unsigned long sym = 0;
            cee_x.lookup_string(&e.input, text, (int)sizeof text, &sym, NULL);
            int32_t modifiers = cee_x_modifiers(e.input.state);
            cee_window_push(w, CEE_EVENT_KEY, cee_x_key(unshifted), down, repeat, modifiers, 0.0f, 0.0f);
            if (down && !(modifiers & (CEE_MOD_CONTROL | CEE_MOD_COMMAND))) {
                uint32_t c = cee_x_scalar(sym);
                if (c != 0) {
                    cee_window_text(w, c);
                }
            }
            break;
        }
        case 4: /* ButtonPress */
        case 5: { /* ButtonRelease */
            unsigned int b = e.input.code;
            float x = (float)e.input.x, y = (float)e.input.y;
            if (b >= 4 && b <= 7) {
                if (e.type == 4) {
                    float dx = b == 6 ? -1.0f : b == 7 ? 1.0f : 0.0f, dy = b == 4 ? 1.0f : b == 5 ? -1.0f : 0.0f;
                    cee_window_push(w, CEE_EVENT_WHEEL, 0, 0, 0, 0, dx, dy);
                }
            } else if (b >= 1 && b <= 3) {
                int32_t button = b == 1 ? 0 : b == 3 ? 1 : 2;
                cee_window_push(w, CEE_EVENT_BUTTON, button, e.type == 4, 0, 0, x, y);
            }
            break;
        }
        case 6: /* MotionNotify */
            cee_window_push(w, CEE_EVENT_MOVED, 0, 0, 0, 0, (float)e.input.x, (float)e.input.y);
            break;
        case 9: cee_window_focus(w, 1); break;  /* FocusIn */
        case 10: cee_window_focus(w, 0); break; /* FocusOut */
        case 12: cee_x_draw(w); break;          /* Expose */
        case 22: /* ConfigureNotify */
            if (e.configure.width > 0 && e.configure.height > 0) {
                cee_window_resized(w, (uint32_t)e.configure.width, (uint32_t)e.configure.height);
            }
            break;
        case 33: /* ClientMessage */
            if ((unsigned long)e.client.data[0] == n->delete_atom) {
                cee_window_push(w, CEE_EVENT_CLOSED, 0, 0, 0, 0, 0.0f, 0.0f);
            }
            break;
        default: break;
        }
    }
}

static unsigned cee_x_shift(unsigned long mask) {
    unsigned s = 0;
    while (s < 32u && mask != 0 && (mask & 1ul) == 0) {
        mask >>= 1;
        s++;
    }
    return s;
}

/* Draws the last frame, scaled (nearest) to the window. */
static void cee_x_draw(struct cee_window *w) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    if (n->frame == NULL) {
        return;
    }
    uint32_t width = w->width, height = w->height;
    char *data = (char *)malloc((size_t)width * (size_t)height * 4u);
    if (data == NULL) {
        return;
    }
    cee_x_image *image = cee_x.create_image(n->display, n->visual, (unsigned int)n->depth, 2 /* ZPixmap */, 0, data,
                                            width, height, 32, 0);
    if (image == NULL) {
        free(data);
        return;
    }
    unsigned rs = cee_x_shift(image->red_mask), gs = cee_x_shift(image->green_mask), bs = cee_x_shift(image->blue_mask);
    for (uint32_t y = 0; y < height; y++) {
        uint32_t sy = (uint32_t)(((uint64_t)y * n->frame_height) / height);
        uint32_t *row = (uint32_t *)(void *)(data + (size_t)y * (size_t)image->bytes_per_line);
        for (uint32_t x = 0; x < width; x++) {
            uint32_t sx = (uint32_t)(((uint64_t)x * n->frame_width) / width);
            uint32_t p = n->frame[(size_t)sy * n->frame_width + sx];
            row[x] = ((p & 0xffu) << rs) | (((p >> 8) & 0xffu) << gs) | (((p >> 16) & 0xffu) << bs);
        }
    }
    cee_x.put_image(n->display, n->window, cee_x.default_gc(n->display, n->screen), image, 0, 0, 0, 0, width, height);
    image->f.destroy_image(image); /* frees data too, with Xlib's free */
    cee_x.flush(n->display);
}

static int32_t cee_native_present(struct cee_window *w, const uint32_t *pixels, uint32_t width, uint32_t height) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    size_t count = (size_t)width * (size_t)height;
    if (n->frame == NULL || n->frame_width != width || n->frame_height != height) {
        uint32_t *grown = (uint32_t *)realloc(n->frame, count * sizeof(uint32_t));
        if (grown == NULL) {
            return -CEE_WINDOW_RESOURCES;
        }
        n->frame = grown;
        n->frame_width = width;
        n->frame_height = height;
    }
    memcpy(n->frame, pixels, count * sizeof(uint32_t));
    cee_x_draw(w);
    return 0;
}

static void cee_native_cursor(struct cee_window *w) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    static const unsigned int shapes[] = {68, 60, 152, 34, 52, 108, 116}; /* XC_left_ptr, hand2, xterm, ... */
    int32_t k = w->cursor >= 0 && w->cursor < 7 ? w->cursor : 0;
    if (n->cursors[k] == 0) {
        n->cursors[k] = cee_x.create_font_cursor(n->display, shapes[k]);
    }
    cee_x.define_cursor(n->display, n->window, n->cursors[k]);
    cee_x.flush(n->display);
}

static void cee_native_title(struct cee_window *w, const char *title) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    cee_x.store_name(n->display, n->window, title);
    cee_x.flush(n->display);
}

static void cee_native_wait(struct cee_window *w, double seconds) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    if (cee_x.pending(n->display) > 0) {
        return;
    }
    int fd = cee_x.connection_number(n->display);
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval timeout;
    timeout.tv_sec = (time_t)seconds;
    timeout.tv_usec = (suseconds_t)((seconds - (double)timeout.tv_sec) * 1e6);
    select(fd + 1, &set, NULL, NULL, &timeout);
}

#else
#define CEE_WINDOW_NO_NATIVE 1
#endif

/* ---- The calls lib/std/window.cee makes -------------------------------------------------------------------------- */

static char *cee_window_cstring(const uint8_t *text, uint64_t length) {
    char *s = (char *)malloc((size_t)length + 1u);
    if (s != NULL) {
        if (length > 0) {
            memcpy(s, text, (size_t)length);
        }
        s[length] = '\0';
    }
    return s;
}

void *ceesafe_window_open(const uint8_t *title, uint64_t length, uint32_t width, uint32_t height, int32_t *error) {
    *error = 0;
    if (width == 0 || height == 0 || width > CEE_WINDOW_MAX_SIDE || height > CEE_WINDOW_MAX_SIDE) {
        *error = CEE_WINDOW_UNSUPPORTED;
        return NULL;
    }
    if (!cee_window_on_main_thread()) {
        *error = CEE_WINDOW_NOT_MAIN_THREAD;
        return NULL;
    }
    struct cee_window *w = (struct cee_window *)calloc(1, sizeof *w);
    char *name = cee_window_cstring(title, length);
    if (w == NULL || name == NULL) {
        free(w);
        free(name);
        *error = CEE_WINDOW_RESOURCES;
        return NULL;
    }
    w->width = width;
    w->height = height;
    w->scale = 1.0f;
    w->focused = 1;
    w->last_wait = cee_window_now();
    const char *script = getenv("CEESAFE_WINDOW_SCRIPT");
    if (script != NULL && script[0] != '\0') {
        w->scripted = 1;
        w->dump = getenv("CEESAFE_WINDOW_DUMP");
        if (w->dump != NULL && w->dump[0] == '\0') {
            w->dump = NULL;
        }
        if (!cee_window_script_load(w, script)) {
            free(w->script);
            free(w);
            free(name);
            *error = CEE_WINDOW_RESOURCES;
            return NULL;
        }
        free(name);
        cee_window_script_pump(w);
        return w;
    }
#if defined(CEE_WINDOW_NO_NATIVE)
    free(w);
    free(name);
    *error = CEE_WINDOW_UNSUPPORTED;
    return NULL;
#else
    int failed = cee_native_open(w, name);
    free(name);
    if (failed != 0) {
        free(w);
        *error = failed;
        return NULL;
    }
    return w;
#endif
}

void ceesafe_window_close(void *window) {
    struct cee_window *w = (struct cee_window *)window;
    if (w == NULL) {
        return;
    }
#if !defined(CEE_WINDOW_NO_NATIVE)
    if (!w->scripted) {
        cee_native_close(w);
    }
#endif
    free(w->script);
    free(w);
}

int32_t ceesafe_window_poll(void *window, int32_t *fields, float *position) {
    struct cee_window *w = (struct cee_window *)window;
    if (w->count == 0) {
        if (w->scripted) {
            cee_window_script_pump(w);
        }
#if !defined(CEE_WINDOW_NO_NATIVE)
        else {
            cee_native_pump(w);
        }
#endif
    }
    if (w->count == 0) {
        return 0;
    }
    struct cee_window_event e = w->queue[w->head];
    w->head = (w->head + 1u) % CEE_WINDOW_QUEUE;
    w->count--;
    memcpy(fields, e.fields, sizeof e.fields);
    position[0] = e.x;
    position[1] = e.y;
    return e.kind;
}

int32_t ceesafe_window_present(void *window, const uint32_t *pixels, uint32_t width, uint32_t height) {
    struct cee_window *w = (struct cee_window *)window;
    if (width == 0 || height == 0 || width > CEE_WINDOW_MAX_SIDE * 4u || height > CEE_WINDOW_MAX_SIDE * 4u) {
        return -CEE_WINDOW_RESOURCES;
    }
    if (w->scripted) {
        int32_t result = cee_window_dump(w, pixels, width, height);
        w->frames++;
        return result;
    }
#if !defined(CEE_WINDOW_NO_NATIVE)
    return cee_native_present(w, pixels, width, height);
#else
    return -CEE_WINDOW_UNSUPPORTED;
#endif
}

void ceesafe_window_size(void *window, uint32_t *width, uint32_t *height) {
    struct cee_window *w = (struct cee_window *)window;
    *width = w->width;
    *height = w->height;
}

void ceesafe_window_scale(void *window, float *scale) { *scale = ((struct cee_window *)window)->scale; }

void ceesafe_window_set_cursor(void *window, int32_t cursor) {
    struct cee_window *w = (struct cee_window *)window;
    if (cursor == w->cursor) {
        return;
    }
    w->cursor = cursor;
#if !defined(CEE_WINDOW_NO_NATIVE)
    if (!w->scripted) {
        cee_native_cursor(w);
    }
#endif
}

void ceesafe_window_set_title(void *window, const uint8_t *title, uint64_t length) {
    struct cee_window *w = (struct cee_window *)window;
    char *name = cee_window_cstring(title, length);
    if (name == NULL) {
        return;
    }
#if !defined(CEE_WINDOW_NO_NATIVE)
    if (!w->scripted) {
        cee_native_title(w, name);
    }
#else
    (void)w;
#endif
    free(name);
}

void ceesafe_window_wait(void *window) {
    struct cee_window *w = (struct cee_window *)window;
    double target = w->last_wait + 1.0 / 60.0, now = cee_window_now();
    if (w->scripted || now >= target || w->count > 0) {
        w->last_wait = now > target ? now : target;
        return;
    }
#if !defined(CEE_WINDOW_NO_NATIVE)
    cee_native_wait(w, target - now);
#endif
    now = cee_window_now();
    w->last_wait = now > target ? now : target;
}
