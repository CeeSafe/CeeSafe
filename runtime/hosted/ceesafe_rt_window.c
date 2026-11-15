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
 *
 * A window opened with the external surface (ADR 0137) is drawn by the program, through the handles that
 * ceesafe_window_native gives it; the runtime never paints its content, and present is Unsupported.
 *
 * Text input and the clipboard (ADR 0141): with text input on, the system's input method composes in the window, and
 * the composition (UTF-8 and a caret's byte offset) is kept in the window: the backend's copy (`composing`) changes
 * when the input method says so and queues COMPOSITION; the program's copy (`composition`) is replaced only when
 * ceesafe_window_poll hands that event out, so a view of it stays valid until the next poll. Committed text is TEXT
 * events, as typed text always was. The clipboard holds UTF-8 text; a read lands in the window (`pasted`) for
 * lib/std to copy at once.
 *
 * Waiting and waking (ADR 0142): ceesafe_window_wait_event sleeps until an event, a wake or a timeout, taking the
 * system's events meanwhile. Every open window is in a list under a spin lock, with an id that names it to
 * ceesafe_window_wake, which any thread may call: it sets the window's `woken` flag and its signal (a pipe, or on
 * Windows an event), which every wait includes, and on macOS signals a run loop source whose callback, on the main
 * thread, posts an application-defined event that ends AppKit's wait.
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
#define CEE_EVENT_COMPOSITION 9 /* ADR 0141 */
/* A queued event taken back (a key the input method consumed, ADR 0141): ceesafe_window_poll skips it. */
#define CEE_EVENT_DROPPED 0
/* A scripted scale change (`scale:S`, ADR 0137): never queued, it updates the window and queues RESIZED. */
#define CEE_SCRIPT_SCALE 100
/* A scripted clipboard (`clipboard[:TEXT]`, ADR 0141): never queued, it replaces the scripted clipboard. */
#define CEE_SCRIPT_CLIPBOARD 101

/* WindowSurface's numbers (ADR 0137): who draws the content. */
#define CEE_SURFACE_FRAMES 0
#define CEE_SURFACE_EXTERNAL 1

/* WindowHandle's kinds (ceesafe_window_native, ADR 0137). */
#define CEE_HANDLE_SCRIPTED 0
#define CEE_HANDLE_APPKIT 1
#define CEE_HANDLE_WIN32 2
#define CEE_HANDLE_XLIB 3

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

/* A changed part of a frame, in the image's pixels (lib/std's WindowBox). */
struct cee_window_box {
    uint32_t x, y, w, h;
};

/* One scripted event and the frame it belongs to. */
struct cee_window_scripted {
    uint64_t frame;
    struct cee_window_event event;
};

struct cee_window {
    int scripted;
    int external; /* WindowSurface.External: the program draws the content, the runtime never does (ADR 0137) */
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
    /* The last frame presented: its size (present_changed's promise is about it), and for a scripted window a copy
       of it, which present_changed updates and the dump writes. */
    int shown;
    uint32_t shown_width, shown_height;
    uint32_t *kept;
    /* Text input (ADR 0141): -1 until the program first sets it (each backend then behaves as before ADR 0141), 0 off,
       1 on; the area in points from the content's top left. */
    int text_input;
    struct cee_window_box text_area;
    /* The composition: the backend's copy and the program's (the file's comment); each caret is a byte offset. */
    char *composing;
    size_t composing_length, composing_capacity, composing_caret;
    char *composition;
    size_t composition_length, composition_capacity, composition_caret;
    /* The clipboard text last read, for lib/std to copy. */
    char *pasted;
    size_t pasted_length;
    /* The scripted window's text: the script's compose and clipboard texts, and its clipboard (`scripted_clipboard`
       is 0 when it holds no text). */
    char *script_text;
    size_t script_text_length, script_text_capacity;
    char *clipboard;
    size_t clipboard_length;
    int scripted_clipboard;
    /* Waking (ADR 0142): the id ceesafe_window_wake knows the window by (from 1, never reused), whether a wake came
       since a wait last returned, the signal every wait includes (a non-blocking pipe's two ends; on Windows a
       manual-reset event), and the link in the list of open windows. Another thread touches `woken`, the signal's
       writing side and nothing else, and only under cee_wake_lock, as does this one. */
    uint64_t wake_id;
    int woken;
    int wake_read, wake_write;
    void *wake_event;
    struct cee_window *wake_link;
    /* The system's window. */
    void *native;
};

void *ceesafe_window_open(const uint8_t *title, uint64_t length, uint32_t width, uint32_t height, uint32_t surface,
                          int32_t *error);
void ceesafe_window_close(void *window);
int32_t ceesafe_window_native(void *window, uint64_t *first, uint64_t *second);
int32_t ceesafe_window_poll(void *window, int32_t *fields, float *position);
int32_t ceesafe_window_present(void *window, const uint32_t *pixels, uint32_t width, uint32_t height);
int32_t ceesafe_window_present_changed(void *window, const uint32_t *pixels, uint32_t width, uint32_t height,
                                       const uint32_t *boxes, uint64_t count);
void ceesafe_window_size(void *window, uint32_t *width, uint32_t *height);
void ceesafe_window_scale(void *window, float *scale);
void ceesafe_window_set_cursor(void *window, int32_t cursor);
void ceesafe_window_set_title(void *window, const uint8_t *title, uint64_t length);
void ceesafe_window_wait(void *window);
void ceesafe_window_set_text_input(void *window, int32_t on);
void ceesafe_window_set_text_area(void *window, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
uint64_t ceesafe_window_composition(void *window, const uint8_t **text, uint64_t *caret);
int32_t ceesafe_window_set_clipboard(void *window, const uint8_t *text, uint64_t length);
int64_t ceesafe_window_clipboard(void *window, const uint8_t **text);
void ceesafe_window_wait_event(void *window, uint32_t timeout_ms);
uint64_t ceesafe_window_waker(void *window);
void ceesafe_window_wake(uint64_t id);

/* Queues an event; the queued copy, which a backend may take back (CEE_EVENT_DROPPED) before the next poll, or NULL. */
static struct cee_window_event *cee_window_push(struct cee_window *w, int32_t kind, int32_t a, int32_t b, int32_t c,
                                                int32_t d, float x, float y) {
    if (w->count == CEE_WINDOW_QUEUE) {
        return NULL; /* a program that never polls loses the newest events, not the memory */
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
    return e;
}

static void cee_window_resized(struct cee_window *w, uint32_t width, uint32_t height) {
    if (width == w->width && height == w->height) {
        return;
    }
    w->width = width;
    w->height = height;
    cee_window_push(w, CEE_EVENT_RESIZED, (int32_t)width, (int32_t)height, 0, 0, 0.0f, 0.0f);
}

/* A new scale (pixels per point): the content's pixel size changed, so RESIZED with the size in points, even when that
   did not change (ADR 0137). */
static void cee_window_rescaled(struct cee_window *w, float scale) {
    if (!(scale > 0.0f) || scale == w->scale) {
        return;
    }
    w->scale = scale;
    cee_window_push(w, CEE_EVENT_RESIZED, (int32_t)w->width, (int32_t)w->height, 0, 0, 0.0f, 0.0f);
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

/* Grows a byte buffer to hold `length` bytes; 0 when memory ran out (the buffer is then unchanged). */
static int cee_window_reserve(char **buffer, size_t *capacity, size_t length) {
    if (length <= *capacity && *buffer != NULL) {
        return 1;
    }
    size_t next = *capacity < 64 ? 64 : *capacity;
    while (next < length) {
        next *= 2;
    }
    char *grown = (char *)ceesafe_platform_realloc(*buffer, next);
    if (grown == NULL) {
        return 0;
    }
    *buffer = grown;
    *capacity = next;
    return 1;
}

/* The input method's composition is now `text` (UTF-8) with the caret at byte `caret`, which is clamped to the text
   and moved back to a character's start. Queues COMPOSITION when that changes it (ADR 0141). */
static void cee_window_compose(struct cee_window *w, const char *text, size_t length, size_t caret) {
    if (caret > length) {
        caret = length;
    }
    while (caret > 0 && caret < length && ((unsigned char)text[caret] & 0xc0u) == 0x80u) {
        caret--;
    }
    if (length == w->composing_length && caret == w->composing_caret &&
        (length == 0 || memcmp(text, w->composing, length) == 0)) {
        return;
    }
    if (!cee_window_reserve(&w->composing, &w->composing_capacity, length + 1u)) {
        length = 0; /* out of memory: the composition is lost, not the window */
        caret = 0;
        if (w->composing_length == 0) {
            return;
        }
    } else if (length > 0) {
        memcpy(w->composing, text, length);
    }
    w->composing_length = length;
    w->composing_caret = caret;
    cee_window_push(w, CEE_EVENT_COMPOSITION, 0, 0, 0, 0, 0.0f, 0.0f);
}

/* The program's copy of the composition becomes the backend's (at a COMPOSITION handed out). */
static void cee_window_show_composition(struct cee_window *w) {
    size_t length = w->composing_length;
    if (!cee_window_reserve(&w->composition, &w->composition_capacity, length + 1u)) {
        w->composition_length = 0;
        w->composition_caret = 0;
        return;
    }
    if (length > 0) {
        memcpy(w->composition, w->composing, length);
    }
    w->composition_length = length;
    w->composition_caret = w->composing_caret;
}

/* Keeps a copy of clipboard text for lib/std to read; 0 when memory ran out. */
static int cee_window_paste(struct cee_window *w, const char *text, size_t length) {
    char *copy = (char *)ceesafe_platform_alloc(length + 1u);
    if (copy == NULL) {
        return 0;
    }
    if (length > 0) {
        memcpy(copy, text, length);
    }
    ceesafe_platform_free(w->pasted);
    w->pasted = copy;
    w->pasted_length = length;
    return 1;
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

#if defined(__APPLE__) || defined(_WIN32)
/* An RGBA pixel (bytes R, G, B, A) as BGRA with an opaque alpha (bytes B, G, R, 255): GDI's and the IOSurface's. */
static uint32_t cee_window_bgra(uint32_t p) {
    return ((p & 0xffu) << 16) | (p & 0xff00u) | ((p >> 16) & 0xffu) | 0xff000000u;
}
#endif

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

/* ---- Waking a window from any thread (ADR 0142) ------------------------------------------------------------------ */
/* The open windows, linked through `wake_link`, and the next id, under a spin lock (the channels' kind, ADR 0032: a
   wake never sleeps). A window is added once it opened and taken out before close frees anything, so a wake finds
   only windows that are open. */

#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <unistd.h>
#endif

static uint32_t cee_wake_lock;
static uint64_t cee_wake_next_id = 1;
static struct cee_window *cee_wake_windows;

static void cee_wake_acquire(void) {
    while (ceesafe_atomic_swap_ord_u32(&cee_wake_lock, 1u, 1) != 0u) {
        while (ceesafe_atomic_load_ord_u32(&cee_wake_lock, 0) != 0u) {
#if defined(_WIN32)
            YieldProcessor();
#else
            sched_yield();
#endif
        }
    }
}

static void cee_wake_release(void) { ceesafe_atomic_store_ord_u32(&cee_wake_lock, 0u, 2); }

/* Makes the window's signal; 0 if the system refused. */
static int cee_wake_signal_open(struct cee_window *w) {
    w->wake_read = -1;
    w->wake_write = -1;
#if defined(_WIN32)
    w->wake_event = (void *)CreateEventW(NULL, TRUE, FALSE, NULL); /* manual reset, not signalled */
    return w->wake_event != NULL;
#else
    int ends[2];
    if (pipe(ends) != 0) {
        return 0;
    }
    for (int k = 0; k < 2; k++) {
        int flags = fcntl(ends[k], F_GETFL);
        if (flags < 0 || fcntl(ends[k], F_SETFL, flags | O_NONBLOCK) != 0 ||
            fcntl(ends[k], F_SETFD, FD_CLOEXEC) != 0) {
            close(ends[0]);
            close(ends[1]);
            return 0;
        }
    }
    w->wake_read = ends[0];
    w->wake_write = ends[1];
    return 1;
#endif
}

static void cee_wake_signal_close(struct cee_window *w) {
#if defined(_WIN32)
    if (w->wake_event != NULL) {
        CloseHandle((HANDLE)w->wake_event);
        w->wake_event = NULL;
    }
#else
    if (w->wake_read >= 0) {
        close(w->wake_read);
        close(w->wake_write);
        w->wake_read = -1;
        w->wake_write = -1;
    }
#endif
}

/* Adds an open window to the list, with the next id. */
static void cee_wake_register(struct cee_window *w) {
    cee_wake_acquire();
    w->wake_id = cee_wake_next_id++;
    w->wake_link = cee_wake_windows;
    cee_wake_windows = w;
    cee_wake_release();
}

/* Takes a window out of the list, before it is closed; a later wake does not find it. */
static void cee_wake_unregister(struct cee_window *w) {
    cee_wake_acquire();
    for (struct cee_window **p = &cee_wake_windows; *p != NULL; p = &(*p)->wake_link) {
        if (*p == w) {
            *p = w->wake_link;
            break;
        }
    }
    w->wake_link = NULL;
    cee_wake_release();
}

/* Whether the window was woken since a wait last returned. */
static int cee_wake_pending(struct cee_window *w) {
    cee_wake_acquire();
    int woken = w->woken;
    cee_wake_release();
    return woken;
}

/* Takes the wake when a wait returns: clears the flag and the signal together, so they agree. */
static void cee_wake_take(struct cee_window *w) {
    cee_wake_acquire();
    if (w->woken) {
        w->woken = 0;
#if defined(_WIN32)
        ResetEvent((HANDLE)w->wake_event);
#else
        char drain[64];
        while (read(w->wake_read, drain, sizeof drain) > 0) {
        }
#endif
    }
    cee_wake_release();
}

/* Waits up to `seconds` for the signal alone (the scripted window's wait). */
static void cee_wake_signal_wait(struct cee_window *w, double seconds) {
    double ms = seconds * 1000.0 + 0.999;
    if (ms <= 0.0) {
        return;
    }
#if defined(_WIN32)
    WaitForSingleObject((HANDLE)w->wake_event, ms >= 4294967294.0 ? 4294967294u : (DWORD)ms);
#else
    struct pollfd p = {w->wake_read, POLLIN, 0};
    poll(&p, 1, ms >= 2147483647.0 ? 2147483647 : (int)ms);
#endif
}

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
        struct cee_window_scripted *grown = (struct cee_window_scripted *)ceesafe_platform_realloc(w->script, next * sizeof *grown);
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

/* Keeps a script item's text in the window; its offset there, or -1 when memory ran out or the script is too long. */
static int32_t cee_window_script_text(struct cee_window *w, const char *text, size_t length) {
    size_t offset = w->script_text_length;
    if (offset + length > 0x7fffffffu ||
        !cee_window_reserve(&w->script_text, &w->script_text_capacity, offset + length + 1u)) {
        return -1;
    }
    if (length > 0) {
        memcpy(w->script_text + offset, text, length);
    }
    w->script_text_length = offset + length;
    return (int32_t)offset;
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
    if (CEE_IS("scale")) {
        if (cee_window_numbers(args, args_length, v, 3) != 1 || !(v[0] > 0.0) || v[0] > 16.0) {
            return 0;
        }
        return cee_window_script_add(w, capacity, frame, CEE_SCRIPT_SCALE, 0, 0, 0, 0, v[0], 0.0);
    }
    if (CEE_IS("close")) {
        return args_length == 0 && cee_window_script_add(w, capacity, frame, CEE_EVENT_CLOSED, 0, 0, 0, 0, 0.0, 0.0);
    }
    if (CEE_IS("compose")) { /* compose:C:TEXT, C the caret's byte offset in TEXT (ADR 0141) */
        size_t k = 0;
        uint64_t caret = 0;
        while (k < args_length && args[k] >= '0' && args[k] <= '9' && caret <= 1000000u) {
            caret = caret * 10u + (uint64_t)(args[k] - '0');
            k++;
        }
        if (k == 0 || k == args_length || args[k] != ':') {
            return 0;
        }
        const char *text = args + k + 1;
        size_t length = args_length - k - 1;
        if (caret > length || (caret < length && ((unsigned char)text[caret] & 0xc0u) == 0x80u)) {
            return 0;
        }
        int32_t offset = cee_window_script_text(w, text, length);
        return offset >= 0 && cee_window_script_add(w, capacity, frame, CEE_EVENT_COMPOSITION, offset,
                                                    (int32_t)length, (int32_t)caret, 0, 0.0, 0.0);
    }
    if (CEE_IS("clipboard")) { /* clipboard:TEXT puts TEXT on the clipboard, clipboard (no colon) no text (ADR 0141) */
        int has = name_length < rest;
        int32_t offset = has ? cee_window_script_text(w, args, args_length) : 0;
        return offset >= 0 && cee_window_script_add(w, capacity, frame, CEE_SCRIPT_CLIPBOARD, offset,
                                                    (int32_t)args_length, 0, has, 0.0, 0.0);
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
            char *grown = (char *)ceesafe_platform_realloc(text, next);
            if (grown == NULL) {
                ceesafe_platform_free(text);
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
    ceesafe_platform_free(text);
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

/* Moves the script's events for the current frame into the queue; a resize, a scale and focus also update the
   window. */
static void cee_window_script_pump(struct cee_window *w) {
    while (w->script_next < w->script_count && w->script[w->script_next].frame <= w->frames) {
        struct cee_window_event e = w->script[w->script_next++].event;
        if (e.kind == CEE_EVENT_RESIZED) {
            cee_window_resized(w, (uint32_t)e.fields[0], (uint32_t)e.fields[1]);
        } else if (e.kind == CEE_SCRIPT_SCALE) {
            cee_window_rescaled(w, e.x);
        } else if (e.kind == CEE_EVENT_FOCUS) {
            cee_window_focus(w, e.fields[0]);
        } else if (e.kind == CEE_EVENT_COMPOSITION) {
            if (w->text_input == 1) { /* with text input off, there is no input method to compose */
                cee_window_compose(w, w->script_text + e.fields[0], (size_t)e.fields[1], (size_t)e.fields[2]);
            }
        } else if (e.kind == CEE_SCRIPT_CLIPBOARD) {
            char *copy = (char *)ceesafe_platform_alloc((size_t)e.fields[1] + 1u);
            if (copy != NULL) {
                if (e.fields[1] > 0) {
                    memcpy(copy, w->script_text + e.fields[0], (size_t)e.fields[1]);
                }
                ceesafe_platform_free(w->clipboard);
                w->clipboard = copy;
                w->clipboard_length = (size_t)e.fields[1];
                w->scripted_clipboard = e.fields[3];
            }
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
   event or `seconds`), cee_native_handles (the system's handles and their CEE_HANDLE_* kind, ADR 0137),
   cee_native_text_input and cee_native_text_area (the input method, ADR 0141), cee_native_clipboard_set (0 or minus
   a CEE_WINDOW_* error) and cee_native_clipboard_get (1 with the text kept by cee_window_paste, 0 without text). With
   `w->external` set before cee_native_open, the backend never paints the content: cee_native_present is not called. */

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
typedef struct {
    unsigned long location, length;
} cee_range; /* NSRange */
#define CEE_NOT_FOUND 0x7ffffffffffffffful /* NSNotFound */

static struct {
    int state; /* 0 not tried, 1 loaded, -1 failed */
    cee_id (*get_class)(const char *);
    cee_sel (*sel)(const char *);
    cee_fn send;
    cee_fn send_stret;
    cee_id (*allocate_class)(cee_id, const char *, size_t);
    signed char (*add_method)(cee_id, cee_sel, cee_fn, const char *);
    void (*register_class)(cee_id);
    void *(*get_protocol)(const char *);
    signed char (*add_protocol)(cee_id, void *);
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
    /* IOSurface and the CoreFoundation it needs (all or none: `surfaces`). */
    int surfaces;
    void *(*surface_create)(void *);
    int32_t (*surface_lock)(void *, uint32_t, uint32_t *);
    int32_t (*surface_unlock)(void *, uint32_t, uint32_t *);
    void *(*surface_base)(void *);
    size_t (*surface_stride)(void *);
    void (*surface_set_value)(void *, const void *, const void *);
    void **key_width;
    void **key_height;
    void **key_bytes_per_element;
    void **key_pixel_format;
    void **key_color_space;
    void *(*dictionary_create)(void *, long, const void *, const void *);
    void (*dictionary_set)(void *, const void *, const void *);
    void *(*number_create)(void *, long, const void *);
    void (*cf_release)(const void *);
    const void *key_callbacks;
    const void *value_callbacks;
    void *(*color_space_plist)(void *);
    cee_id app;
    cee_id delegate_class;
    cee_id view_class; /* CeeSafeContentView, an NSTextInputClient (ADR 0141) */
    cee_id *pasteboard_string; /* NSPasteboardTypeString */
    void *rgb;
    /* Waking (ADR 0142): a run loop source on the main run loop, which another thread signals; NULL without
       CoreFoundation's run loop calls, and then a wake ends AppKit's wait only at its timeout. */
    void *wake_source;
    void *main_loop;
    void (*source_signal)(void *);
    void (*loop_wake_up)(void *);
} cee_objc;

/* CFRunLoopSourceContext, version 0 (CoreFoundation's CFRunLoop.h). */
typedef struct {
    long version;
    void *info;
    const void *(*retain)(const void *);
    void (*release)(const void *);
    void *(*copy_description)(const void *);
    unsigned char (*equal)(const void *, const void *);
    unsigned long (*hash)(const void *);
    void (*schedule)(void *, void *, void *);
    void (*cancel)(void *, void *, void *);
    void (*perform)(void *);
} cee_cf_source_context;

/* The application-defined event that ends a wait (ADR 0142): its subtype and data1, which the pump drops. */
#define CEE_MAC_WAKE_SUBTYPE 0x4345
#define CEE_MAC_WAKE_DATA 0x0142l

/* The windows the delegate's callbacks report to (one delegate per window). */
#define CEE_MAC_WINDOWS 16
static struct {
    cee_id nswindow;
    struct cee_window *window;
} cee_mac_windows[CEE_MAC_WINDOWS];

/* A window's frame: two IOSurfaces shown in turn (the layer's contents), and the frame itself as BGRA. A surface
   that is not shown still lacks the boxes presented since it last was: `pending` (or all of it, `pending_full`). */
#define CEE_MAC_PENDING 16
struct cee_mac {
    cee_id window;
    cee_id view;
    cee_id delegate;
    void *surface[2];
    int current;
    uint32_t surface_width, surface_height;
    uint32_t *frame;
    struct cee_window_box pending[2][CEE_MAC_PENDING];
    int pending_count[2];
    int pending_full[2];
    /* Text input (ADR 0141): the marked text's length and selection in UTF-16 units; during interpretKeyEvents:,
       whether the input method changed the composition; the key codes whose down the input method took. */
    int marked;
    unsigned long marked_utf16;
    cee_range marked_selection;
    int in_key, key_composed;
    unsigned char ime_keys[16];
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
static short cee_send_short(cee_id o, const char *s) { return ((short (*)(cee_id, cee_sel))cee_objc.send)(o, cee_mac_sel(s)); }
static long cee_send_long(cee_id o, const char *s) { return ((long (*)(cee_id, cee_sel))cee_objc.send)(o, cee_mac_sel(s)); }
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
static cee_id cee_send_id_id(cee_id o, const char *s, cee_id a) {
    return ((cee_id(*)(cee_id, cee_sel, cee_id))cee_objc.send)(o, cee_mac_sel(s), a);
}
static signed char cee_send_bool_id(cee_id o, const char *s, cee_id a) {
    return ((signed char (*)(cee_id, cee_sel, cee_id))cee_objc.send)(o, cee_mac_sel(s), a);
}
static signed char cee_send_bool_id_id(cee_id o, const char *s, cee_id a, cee_id b) {
    return ((signed char (*)(cee_id, cee_sel, cee_id, cee_id))cee_objc.send)(o, cee_mac_sel(s), a, b);
}
static cee_rect cee_send_rect_rect(cee_id o, const char *s, cee_rect a) {
#if defined(__x86_64__)
    cee_rect r;
    ((void (*)(cee_rect *, cee_id, cee_sel, cee_rect))cee_objc.send_stret)(&r, o, cee_mac_sel(s), a);
    return r;
#else
    return ((cee_rect(*)(cee_id, cee_sel, cee_rect))cee_objc.send)(o, cee_mac_sel(s), a);
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

/* The content view's methods (ADR 0141): an NSTextInputClient. The marked text is the composition; insertText: is
   committed text, TEXT events. During interpretKeyEvents: (cee_mac_translate), `in_key` is set, and a change to the
   composition marks the key as the input method's. */
static struct cee_window *cee_mac_view_owner(cee_id view) { return cee_mac_owner(cee_send(view, "window")); }

/* An NSString or NSAttributedString's string, as UTF-8 (autoreleased), or NULL. */
static const char *cee_mac_text_of(cee_id string, cee_id *plain) {
    if (string == NULL) {
        return NULL;
    }
    if (cee_send_bool_id(string, "isKindOfClass:", cee_mac_cls("NSAttributedString"))) {
        string = cee_send(string, "string");
    }
    *plain = string;
    return string != NULL ? cee_send_get_cstr(string, "UTF8String") : NULL;
}

static void cee_mac_insert_text(cee_id self, cee_sel cmd, cee_id string, cee_range replacement) {
    (void)cmd;
    (void)replacement;
    struct cee_window *w = cee_mac_view_owner(self);
    if (w == NULL) {
        return;
    }
    struct cee_mac *m = (struct cee_mac *)w->native;
    cee_id plain = NULL;
    const char *utf8 = cee_mac_text_of(string, &plain);
    if (utf8 != NULL) {
        size_t n = strlen(utf8), i = 0;
        while (i < n) {
            cee_window_text(w, cee_window_utf8((const unsigned char *)utf8, n, &i));
        }
    }
    if (m->marked || w->composing_length > 0) {
        m->key_composed = m->in_key;
    }
    m->marked = 0;
    m->marked_utf16 = 0;
    cee_window_compose(w, "", 0, 0); /* after the TEXT events: the composition was committed */
}

static void cee_mac_set_marked_text(cee_id self, cee_sel cmd, cee_id string, cee_range selected, cee_range replacement) {
    (void)cmd;
    (void)replacement;
    struct cee_window *w = cee_mac_view_owner(self);
    if (w == NULL) {
        return;
    }
    struct cee_mac *m = (struct cee_mac *)w->native;
    cee_id plain = NULL;
    const char *utf8 = cee_mac_text_of(string, &plain);
    size_t n = utf8 != NULL ? strlen(utf8) : 0;
    m->key_composed = m->in_key;
    if (n == 0) {
        m->marked = 0;
        m->marked_utf16 = 0;
        cee_window_compose(w, "", 0, 0);
        return;
    }
    unsigned long units = cee_send_ulong(plain, "length");
    unsigned long at = selected.location > units ? units : selected.location;
    const char *before = cee_send_get_cstr(cee_send_id_ulong(plain, "substringToIndex:", at), "UTF8String");
    m->marked = 1;
    m->marked_utf16 = units;
    m->marked_selection.location = at;
    m->marked_selection.length = selected.length > units - at ? units - at : selected.length;
    cee_window_compose(w, utf8, n, before != NULL ? strlen(before) : 0);
}

static void cee_mac_unmark_text(cee_id self, cee_sel cmd) {
    (void)cmd;
    struct cee_window *w = cee_mac_view_owner(self);
    if (w == NULL) {
        return;
    }
    struct cee_mac *m = (struct cee_mac *)w->native;
    m->key_composed = m->in_key;
    m->marked = 0;
    m->marked_utf16 = 0;
    cee_window_compose(w, "", 0, 0); /* discarded, as winit and SDL do: the input method commits with insertText: */
}

static signed char cee_mac_has_marked_text(cee_id self, cee_sel cmd) {
    (void)cmd;
    struct cee_window *w = cee_mac_view_owner(self);
    return w != NULL && ((struct cee_mac *)w->native)->marked;
}

static cee_range cee_mac_marked_range(cee_id self, cee_sel cmd) {
    (void)cmd;
    struct cee_window *w = cee_mac_view_owner(self);
    cee_range r = {CEE_NOT_FOUND, 0};
    if (w != NULL && ((struct cee_mac *)w->native)->marked) {
        r.location = 0;
        r.length = ((struct cee_mac *)w->native)->marked_utf16;
    }
    return r;
}

static cee_range cee_mac_selected_range(cee_id self, cee_sel cmd) {
    (void)cmd;
    struct cee_window *w = cee_mac_view_owner(self);
    cee_range r = {0, 0};
    if (w != NULL && ((struct cee_mac *)w->native)->marked) {
        r = ((struct cee_mac *)w->native)->marked_selection;
    }
    return r;
}

/* The text input area (setTextInputArea), in screen coordinates: where the input method puts its candidates. */
static cee_rect cee_mac_first_rect(cee_id self, cee_sel cmd, cee_range range, cee_range *actual) {
    (void)cmd;
    if (actual != NULL) {
        *actual = range;
    }
    struct cee_window *w = cee_mac_view_owner(self);
    cee_rect r = {{0.0, 0.0}, {0.0, 0.0}};
    if (w == NULL) {
        return r;
    }
    struct cee_mac *m = (struct cee_mac *)w->native;
    /* The content view fills the window's content, whose coordinates are the window's, origin bottom left. */
    r.origin.x = (double)w->text_area.x;
    r.origin.y = (double)w->height - (double)w->text_area.y - (double)w->text_area.h;
    r.size.width = (double)w->text_area.w;
    r.size.height = (double)w->text_area.h;
    return cee_send_rect_rect(m->window, "convertRectToScreen:", r);
}

static cee_id cee_mac_valid_attributes(cee_id self, cee_sel cmd) {
    (void)self;
    (void)cmd;
    return cee_send(cee_mac_cls("NSArray"), "array");
}

static cee_id cee_mac_attributed_substring(cee_id self, cee_sel cmd, cee_range range, cee_range *actual) {
    (void)self;
    (void)cmd;
    (void)range;
    if (actual != NULL) {
        actual->location = CEE_NOT_FOUND;
        actual->length = 0;
    }
    return NULL; /* the document's text is the program's: the input method gets none */
}

static unsigned long cee_mac_character_index(cee_id self, cee_sel cmd, cee_point point) {
    (void)self;
    (void)cmd;
    (void)point;
    return CEE_NOT_FOUND;
}

/* A key the input method did not take (insertNewline:, deleteBackward:, moveLeft:, …): its KEY event stays. */
static void cee_mac_do_command(cee_id self, cee_sel cmd, cee_sel command) {
    (void)self;
    (void)cmd;
    (void)command;
}

static signed char cee_mac_accepts_first_responder(cee_id self, cee_sel cmd) {
    (void)self;
    (void)cmd;
    return 1;
}

/* The wake source's callback (ADR 0142), on the main thread inside the run loop AppKit's wait runs: an
   application-defined event, which ends nextEventMatchingMask: and which the pump drops. */
static void cee_mac_wake_perform(void *info) {
    (void)info;
    void *pool = cee_objc.pool_push();
    cee_point origin = {0.0, 0.0};
    cee_id event = ((cee_id(*)(cee_id, cee_sel, unsigned long, cee_point, unsigned long, double, long, cee_id, short, long,
                               long))cee_objc.send)(
        cee_mac_cls("NSEvent"),
        cee_mac_sel("otherEventWithType:location:modifierFlags:timestamp:windowNumber:context:subtype:data1:data2:"), 15ul,
        origin, 0ul, 0.0, 0l, NULL, (short)CEE_MAC_WAKE_SUBTYPE, CEE_MAC_WAKE_DATA, 0l);
    if (event != NULL) {
        ((void (*)(cee_id, cee_sel, cee_id, signed char))cee_objc.send)(cee_objc.app, cee_mac_sel("postEvent:atStart:"),
                                                                         event, 0);
    }
    cee_objc.pool_pop(pool);
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
    cee_objc.get_protocol = (void *(*)(const char *))cee_mac_symbol(objc, "objc_getProtocol");
    cee_objc.add_protocol = (signed char (*)(cee_id, void *))cee_mac_symbol(objc, "class_addProtocol");
    cee_objc.pasteboard_string = (cee_id *)cee_mac_data_symbol(appkit, "NSPasteboardTypeString");
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
    /* IOSurface, for frames the window server shares (ADR 0070); without it, frames are CGImages as before. */
    void *iosurface = dlopen("/System/Library/Frameworks/IOSurface.framework/IOSurface", RTLD_NOW | RTLD_GLOBAL);
    void *foundation = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW | RTLD_GLOBAL);
    if (iosurface != NULL && foundation != NULL) {
        cee_objc.surface_create = (void *(*)(void *))cee_mac_symbol(iosurface, "IOSurfaceCreate");
        cee_objc.surface_lock = (int32_t(*)(void *, uint32_t, uint32_t *))cee_mac_symbol(iosurface, "IOSurfaceLock");
        cee_objc.surface_unlock = (int32_t(*)(void *, uint32_t, uint32_t *))cee_mac_symbol(iosurface, "IOSurfaceUnlock");
        cee_objc.surface_base = (void *(*)(void *))cee_mac_symbol(iosurface, "IOSurfaceGetBaseAddress");
        cee_objc.surface_stride = (size_t(*)(void *))cee_mac_symbol(iosurface, "IOSurfaceGetBytesPerRow");
        cee_objc.surface_set_value =
            (void (*)(void *, const void *, const void *))cee_mac_symbol(iosurface, "IOSurfaceSetValue");
        cee_objc.key_width = (void **)cee_mac_data_symbol(iosurface, "kIOSurfaceWidth");
        cee_objc.key_height = (void **)cee_mac_data_symbol(iosurface, "kIOSurfaceHeight");
        cee_objc.key_bytes_per_element = (void **)cee_mac_data_symbol(iosurface, "kIOSurfaceBytesPerElement");
        cee_objc.key_pixel_format = (void **)cee_mac_data_symbol(iosurface, "kIOSurfacePixelFormat");
        cee_objc.key_color_space = (void **)cee_mac_data_symbol(iosurface, "kIOSurfaceColorSpace");
        cee_objc.dictionary_create = (void *(*)(void *, long, const void *, const void *))cee_mac_symbol(
            foundation, "CFDictionaryCreateMutable");
        cee_objc.dictionary_set =
            (void (*)(void *, const void *, const void *))cee_mac_symbol(foundation, "CFDictionarySetValue");
        cee_objc.number_create = (void *(*)(void *, long, const void *))cee_mac_symbol(foundation, "CFNumberCreate");
        cee_objc.cf_release = (void (*)(const void *))cee_mac_symbol(foundation, "CFRelease");
        cee_objc.key_callbacks = cee_mac_data_symbol(foundation, "kCFTypeDictionaryKeyCallBacks");
        cee_objc.value_callbacks = cee_mac_data_symbol(foundation, "kCFTypeDictionaryValueCallBacks");
        cee_objc.color_space_plist = (void *(*)(void *))cee_mac_symbol(graphics, "CGColorSpaceCopyPropertyList");
        cee_objc.surfaces = cee_objc.surface_create != NULL && cee_objc.surface_lock != NULL &&
                            cee_objc.surface_unlock != NULL && cee_objc.surface_base != NULL &&
                            cee_objc.surface_stride != NULL && cee_objc.surface_set_value != NULL &&
                            cee_objc.key_width != NULL && cee_objc.key_height != NULL &&
                            cee_objc.key_bytes_per_element != NULL && cee_objc.key_pixel_format != NULL &&
                            cee_objc.key_color_space != NULL && cee_objc.dictionary_create != NULL &&
                            cee_objc.dictionary_set != NULL && cee_objc.number_create != NULL &&
                            cee_objc.cf_release != NULL && cee_objc.key_callbacks != NULL &&
                            cee_objc.value_callbacks != NULL && cee_objc.color_space_plist != NULL;
    }
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
    /* The content view (ADR 0141): an NSView that is an NSTextInputClient, so interpretKeyEvents: reaches the input
       method and the input method's answers reach the window. */
    cee_id view = cee_objc.allocate_class(cee_mac_cls("NSView"), "CeeSafeContentView", 0);
    if (view == NULL) {
        view = cee_mac_cls("CeeSafeContentView");
    } else {
        cee_objc.add_method(view, cee_mac_sel("insertText:replacementRange:"), (cee_fn)cee_mac_insert_text,
                            "v@:@{_NSRange=QQ}");
        cee_objc.add_method(view, cee_mac_sel("setMarkedText:selectedRange:replacementRange:"),
                            (cee_fn)cee_mac_set_marked_text, "v@:@{_NSRange=QQ}{_NSRange=QQ}");
        cee_objc.add_method(view, cee_mac_sel("unmarkText"), (cee_fn)cee_mac_unmark_text, "v@:");
        cee_objc.add_method(view, cee_mac_sel("hasMarkedText"), (cee_fn)cee_mac_has_marked_text, "c@:");
        cee_objc.add_method(view, cee_mac_sel("markedRange"), (cee_fn)cee_mac_marked_range, "{_NSRange=QQ}@:");
        cee_objc.add_method(view, cee_mac_sel("selectedRange"), (cee_fn)cee_mac_selected_range, "{_NSRange=QQ}@:");
        cee_objc.add_method(view, cee_mac_sel("firstRectForCharacterRange:actualRange:"), (cee_fn)cee_mac_first_rect,
                            "{CGRect={CGPoint=dd}{CGSize=dd}}@:{_NSRange=QQ}^{_NSRange=QQ}");
        cee_objc.add_method(view, cee_mac_sel("validAttributesForMarkedText"), (cee_fn)cee_mac_valid_attributes,
                            "@@:");
        cee_objc.add_method(view, cee_mac_sel("attributedSubstringForProposedRange:actualRange:"),
                            (cee_fn)cee_mac_attributed_substring, "@@:{_NSRange=QQ}^{_NSRange=QQ}");
        cee_objc.add_method(view, cee_mac_sel("characterIndexForPoint:"), (cee_fn)cee_mac_character_index,
                            "Q@:{CGPoint=dd}");
        cee_objc.add_method(view, cee_mac_sel("doCommandBySelector:"), (cee_fn)cee_mac_do_command, "v@::");
        cee_objc.add_method(view, cee_mac_sel("acceptsFirstResponder"), (cee_fn)cee_mac_accepts_first_responder,
                            "c@:");
        void *protocol = cee_objc.get_protocol != NULL ? cee_objc.get_protocol("NSTextInputClient") : NULL;
        if (protocol != NULL && cee_objc.add_protocol != NULL) {
            cee_objc.add_protocol(view, protocol); /* NSView's inputContext asks for it */
        }
        cee_objc.register_class(view);
    }
    cee_objc.view_class = view;
    /* Waking from another thread (ADR 0142): a version 0 run loop source in the main run loop's common modes.
       CFRunLoopSourceSignal and CFRunLoopWakeUp may be called from any thread and allocate nothing; the callback runs
       here, on the main thread. */
    void *core = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW | RTLD_GLOBAL);
    if (core != NULL) {
        void *(*source_create)(void *, long, cee_cf_source_context *) =
            (void *(*)(void *, long, cee_cf_source_context *))cee_mac_symbol(core, "CFRunLoopSourceCreate");
        void *(*get_main)(void) = (void *(*)(void))cee_mac_symbol(core, "CFRunLoopGetMain");
        void (*add_source)(void *, void *, void *) =
            (void (*)(void *, void *, void *))cee_mac_symbol(core, "CFRunLoopAddSource");
        void **common = (void **)cee_mac_data_symbol(core, "kCFRunLoopCommonModes");
        cee_objc.source_signal = (void (*)(void *))cee_mac_symbol(core, "CFRunLoopSourceSignal");
        cee_objc.loop_wake_up = (void (*)(void *))cee_mac_symbol(core, "CFRunLoopWakeUp");
        if (source_create != NULL && get_main != NULL && add_source != NULL && common != NULL &&
            cee_objc.source_signal != NULL && cee_objc.loop_wake_up != NULL) {
            cee_cf_source_context context; /* copied by CFRunLoopSourceCreate */
            memset(&context, 0, sizeof context);
            context.perform = cee_mac_wake_perform;
            void *loop = get_main();
            void *source = loop != NULL ? source_create(NULL, 0, &context) : NULL;
            if (source != NULL) {
                add_source(loop, source, *common);
                cee_objc.main_loop = loop;
                cee_objc.wake_source = source;
            }
        }
    }
    /* The pixels are sRGB values (what ClayWind's rasterizer computes); DeviceRGB would leave them unmanaged. */
    cee_objc.rgb = cee_objc.color_space_named != NULL && cee_objc.srgb_name != NULL
                       ? cee_objc.color_space_named(*cee_objc.srgb_name)
                       : NULL;
    if (cee_objc.rgb == NULL) {
        cee_objc.rgb = cee_objc.color_space();
    }
    cee_objc.pool_pop(pool);
    cee_objc.state = cee_objc.app != NULL && delegate != NULL && view != NULL && cee_objc.rgb != NULL ? 1 : -1;
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
    struct cee_mac *m = (struct cee_mac *)ceesafe_platform_alloc_zeroed(sizeof *m);
    if (slot < 0 || m == NULL) {
        ceesafe_platform_free(m);
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
        ceesafe_platform_free(m);
        return CEE_WINDOW_RESOURCES;
    }
    m->window = window;
    /* Our content view (ADR 0141), retained by the window. */
    cee_id view = cee_send(cee_objc.view_class, "alloc");
    view = ((cee_id(*)(cee_id, cee_sel, cee_rect))cee_objc.send)(view, cee_mac_sel("initWithFrame:"), frame);
    if (view != NULL) {
        cee_send_v_id(window, "setContentView:", view);
        cee_send_v(view, "release");
    }
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
    cee_send_bool_id(window, "makeFirstResponder:", m->view);
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
    cee_send_v_id(cee_send(m->view, "layer"), "setContents:", NULL);
    cee_send_v(m->window, "close");
    cee_send_v(m->window, "release");
    cee_send_v(m->delegate, "release");
    cee_objc.pool_pop(pool);
    for (int k = 0; k < 2; k++) {
        if (m->surface[k] != NULL) {
            cee_objc.cf_release(m->surface[k]);
        }
    }
    ceesafe_platform_free(m->frame);
    ceesafe_platform_free(m);
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
        unsigned char bit = (unsigned char)(1u << (code % 8u));
        if (!down && code < 128u && (m->ime_keys[code / 8u] & bit)) {
            m->ime_keys[code / 8u] = (unsigned char)(m->ime_keys[code / 8u] & ~bit);
            return 0; /* the up of a key the input method took */
        }
        if (down && w->text_input == 1 && (m->marked || !(modifiers & (CEE_MOD_COMMAND | CEE_MOD_CONTROL)))) {
            /* Text input (ADR 0141): the key goes to the input method, which answers through the view's methods
               (insertText: as TEXT, setMarkedText: as COMPOSITION, doCommandBySelector: for a key it leaves). A key
               that composes, or comes while composing, is the input method's: its KEY is taken back. */
            struct cee_window_event *queued = cee_window_push(w, CEE_EVENT_KEY, key, 1,
                                                              (int32_t)cee_send_bool(event, "isARepeat"), modifiers,
                                                              0.0f, 0.0f);
            int had = m->marked;
            m->in_key = 1;
            m->key_composed = 0;
            cee_send_v_id(m->view, "interpretKeyEvents:", cee_send_id_id(cee_mac_cls("NSArray"), "arrayWithObject:", event));
            m->in_key = 0;
            if (had || m->key_composed) {
                if (queued != NULL) {
                    queued->kind = CEE_EVENT_DROPPED;
                }
                if (code < 128u) {
                    m->ime_keys[code / 8u] = (unsigned char)(m->ime_keys[code / 8u] | bit);
                }
            }
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
    case 15: /* NSEventTypeApplicationDefined: the runtime's own only ended a wait (ADR 0142) */
        return !(cee_send_short(event, "subtype") == CEE_MAC_WAKE_SUBTYPE &&
                 cee_send_long(event, "data1") == CEE_MAC_WAKE_DATA);
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
    cee_window_rescaled(w, (float)cee_send_double(((struct cee_mac *)w->native)->window, "backingScaleFactor"));
    cee_objc.pool_pop(pool);
}

static int32_t cee_native_handles(struct cee_window *w, uint64_t *first, uint64_t *second) {
    struct cee_mac *m = (struct cee_mac *)w->native;
    *first = (uint64_t)(uintptr_t)m->window;
    *second = (uint64_t)(uintptr_t)m->view;
    return CEE_HANDLE_APPKIT;
}

static void cee_mac_release_pixels(void *info, const void *data, size_t size) {
    (void)data;
    (void)size;
    ceesafe_platform_free(info);
}

/* A frame as a CGImage on the layer: the way without IOSurface. */
static int32_t cee_mac_present_image(struct cee_window *w, const uint32_t *pixels, uint32_t width, uint32_t height) {
    struct cee_mac *m = (struct cee_mac *)w->native;
    size_t bytes = (size_t)width * (size_t)height * 4u;
    void *copy = ceesafe_platform_alloc(bytes);
    if (copy == NULL) {
        return -CEE_WINDOW_RESOURCES;
    }
    memcpy(copy, pixels, bytes);
    void *provider = cee_objc.provider_create(copy, copy, bytes, cee_mac_release_pixels);
    if (provider == NULL) {
        ceesafe_platform_free(copy);
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

/* A width x height BGRA IOSurface tagged sRGB (the frames' colours), or NULL. */
static void *cee_mac_surface(uint32_t width, uint32_t height) {
    void *properties = cee_objc.dictionary_create(NULL, 0, cee_objc.key_callbacks, cee_objc.value_callbacks);
    if (properties == NULL) {
        return NULL;
    }
    int32_t values[4] = {(int32_t)width, (int32_t)height, 4, 0x42475241 /* 'BGRA' */};
    void **keys[4] = {cee_objc.key_width, cee_objc.key_height, cee_objc.key_bytes_per_element, cee_objc.key_pixel_format};
    for (int k = 0; k < 4; k++) {
        void *number = cee_objc.number_create(NULL, 3 /* kCFNumberSInt32Type */, &values[k]);
        if (number != NULL) {
            cee_objc.dictionary_set(properties, *keys[k], number);
            cee_objc.cf_release(number);
        }
    }
    void *surface = cee_objc.surface_create(properties);
    cee_objc.cf_release(properties);
    if (surface != NULL) {
        void *space = cee_objc.color_space_plist(cee_objc.rgb);
        if (space != NULL) {
            cee_objc.surface_set_value(surface, *cee_objc.key_color_space, space);
            cee_objc.cf_release(space);
        }
    }
    return surface;
}

/* Remembers that both surfaces lack `b` (the shown one is replaced before it is shown again). */
static void cee_mac_pend(struct cee_mac *m, struct cee_window_box b) {
    for (int k = 0; k < 2; k++) {
        if (m->pending_count[k] < CEE_MAC_PENDING) {
            m->pending[k][m->pending_count[k]++] = b;
        } else {
            m->pending_full[k] = 1;
        }
    }
}

/* Brings the surface not shown up to date with the frame and shows it. */
static int32_t cee_mac_flip(struct cee_mac *m) {
    int back = 1 - m->current;
    void *surface = m->surface[back];
    if (cee_objc.surface_lock(surface, 0, NULL) != 0) {
        return -CEE_WINDOW_RESOURCES;
    }
    unsigned char *base = (unsigned char *)cee_objc.surface_base(surface);
    size_t stride = cee_objc.surface_stride(surface);
    size_t width = m->surface_width;
    if (m->pending_full[back]) {
        for (size_t y = 0; y < m->surface_height; y++) {
            memcpy(base + y * stride, m->frame + y * width, width * 4u);
        }
    } else {
        for (int k = 0; k < m->pending_count[back]; k++) {
            struct cee_window_box b = m->pending[back][k];
            for (size_t y = b.y; y < (size_t)b.y + b.h; y++) {
                memcpy(base + y * stride + (size_t)b.x * 4u, m->frame + y * width + b.x, (size_t)b.w * 4u);
            }
        }
    }
    cee_objc.surface_unlock(surface, 0, NULL);
    m->pending_full[back] = 0;
    m->pending_count[back] = 0;
    void *pool = cee_objc.pool_push();
    cee_send_v_id(cee_send(m->view, "layer"), "setContents:", surface);
    cee_objc.pool_pop(pool);
    m->current = back;
    return 0;
}

static int32_t cee_native_present(struct cee_window *w, const uint32_t *pixels, uint32_t width, uint32_t height) {
    struct cee_mac *m = (struct cee_mac *)w->native;
    if (!cee_objc.surfaces) {
        return cee_mac_present_image(w, pixels, width, height);
    }
    if (m->frame == NULL || m->surface_width != width || m->surface_height != height) {
        for (int k = 0; k < 2; k++) {
            if (m->surface[k] != NULL) {
                cee_objc.cf_release(m->surface[k]);
            }
            m->surface[k] = cee_mac_surface(width, height);
        }
        ceesafe_platform_free(m->frame);
        m->frame = (uint32_t *)ceesafe_platform_alloc((size_t)width * (size_t)height * 4u);
        if (m->frame == NULL || m->surface[0] == NULL || m->surface[1] == NULL) {
            ceesafe_platform_free(m->frame);
            m->frame = NULL;
            return cee_mac_present_image(w, pixels, width, height);
        }
        m->surface_width = width;
        m->surface_height = height;
    }
    size_t count = (size_t)width * (size_t)height;
    for (size_t k = 0; k < count; k++) {
        m->frame[k] = cee_window_bgra(pixels[k]);
    }
    m->pending_full[0] = 1;
    m->pending_full[1] = 1;
    return cee_mac_flip(m);
}

/* Only `boxes` changed since the last present, which had this size (ceesafe_window_present_changed checks it). */
static int32_t cee_native_present_boxes(struct cee_window *w, const uint32_t *pixels, uint32_t width, uint32_t height,
                                        const struct cee_window_box *boxes, size_t count) {
    struct cee_mac *m = (struct cee_mac *)w->native;
    if (!cee_objc.surfaces || m->frame == NULL || m->surface_width != width || m->surface_height != height) {
        return cee_native_present(w, pixels, width, height);
    }
    for (size_t k = 0; k < count; k++) {
        struct cee_window_box b = boxes[k];
        for (size_t y = b.y; y < (size_t)b.y + b.h; y++) {
            for (size_t x = b.x; x < (size_t)b.x + b.w; x++) {
                m->frame[y * width + x] = cee_window_bgra(pixels[y * width + x]);
            }
        }
        cee_mac_pend(m, b);
    }
    return cee_mac_flip(m);
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

/* Text input on or off (ADR 0141): on, the view is the first responder, so its input context is the active one;
   off, a composition is discarded (the input method then calls unmarkText). */
static void cee_native_text_input(struct cee_window *w, int on) {
    struct cee_mac *m = (struct cee_mac *)w->native;
    void *pool = cee_objc.pool_push();
    if (on) {
        cee_send_bool_id(m->window, "makeFirstResponder:", m->view);
    } else {
        cee_id context = cee_send(m->view, "inputContext");
        if (context != NULL && m->marked) {
            cee_send_v(context, "discardMarkedText");
        }
        m->marked = 0;
        m->marked_utf16 = 0;
    }
    cee_objc.pool_pop(pool);
}

/* A new text input area: the input method asks firstRectForCharacterRange: again. */
static void cee_native_text_area(struct cee_window *w) {
    struct cee_mac *m = (struct cee_mac *)w->native;
    void *pool = cee_objc.pool_push();
    cee_id context = cee_send(m->view, "inputContext");
    if (context != NULL) {
        cee_send_v(context, "invalidateCharacterCoordinates");
    }
    cee_objc.pool_pop(pool);
}

static cee_id cee_mac_pasteboard_type(void) {
    return cee_objc.pasteboard_string != NULL ? *cee_objc.pasteboard_string : cee_nsstring("public.utf8-plain-text");
}

/* The general pasteboard's string (ADR 0141). */
static int32_t cee_native_clipboard_set(struct cee_window *w, const char *text, size_t length) {
    (void)w;
    void *pool = cee_objc.pool_push();
    cee_id string = cee_send(cee_mac_cls("NSString"), "alloc");
    string = ((cee_id(*)(cee_id, cee_sel, const void *, unsigned long, unsigned long))cee_objc.send)(
        string, cee_mac_sel("initWithBytes:length:encoding:"), length > 0 ? (const void *)text : (const void *)"",
        (unsigned long)length, 4ul /* NSUTF8StringEncoding */);
    int32_t result = -CEE_WINDOW_RESOURCES;
    if (string != NULL) {
        cee_id board = cee_send(cee_mac_cls("NSPasteboard"), "generalPasteboard");
        cee_send_ulong(board, "clearContents");
        if (cee_send_bool_id_id(board, "setString:forType:", string, cee_mac_pasteboard_type())) {
            result = 0;
        }
        cee_send_v(string, "release");
    }
    cee_objc.pool_pop(pool);
    return result;
}

static int cee_native_clipboard_get(struct cee_window *w) {
    void *pool = cee_objc.pool_push();
    cee_id board = cee_send(cee_mac_cls("NSPasteboard"), "generalPasteboard");
    cee_id string = cee_send_id_id(board, "stringForType:", cee_mac_pasteboard_type());
    const char *utf8 = string != NULL ? cee_send_get_cstr(string, "UTF8String") : NULL;
    int found = utf8 != NULL && cee_window_paste(w, utf8, strlen(utf8));
    cee_objc.pool_pop(pool);
    return found;
}

/* A wake from another thread (ADR 0142), after its flag is set: the wake source ends AppKit's wait. */
static void cee_native_wake(void) {
    if (cee_objc.wake_source != NULL) {
        cee_objc.source_signal(cee_objc.wake_source);
        cee_objc.loop_wake_up(cee_objc.main_loop);
    }
}

/* Until an event, a wake (the wake source's event) or `seconds`. */
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
#if defined(_MSC_VER)
#pragma comment(lib, "user32.lib") /* clang and cl; MinGW's gcc gets -lgdi32 from ceesafec (tooling.md §8.3) */
#pragma comment(lib, "gdi32.lib")
#endif
#include <imm.h> /* the input method's types and constants; its functions come from imm32.dll, loaded at first use */

struct cee_win32 {
    HWND hwnd;
    uint32_t *frame; /* BGRA, the last presented frame */
    uint32_t frame_width, frame_height;
    int captured;
    unsigned char ime_keys[32]; /* virtual keys whose down the input method took (ADR 0141) */
};

/* imm32.dll (ADR 0141), loaded with LoadLibraryW like GetDpiForWindow's user32, so no program needs a link flag
   (MinGW ignores #pragma comment(lib)). */
static struct {
    int state; /* 0 not tried, 1 loaded, -1 failed */
    HIMC(WINAPI *get_context)(HWND);
    BOOL(WINAPI *release_context)(HWND, HIMC);
    LONG(WINAPI *get_string)(HIMC, DWORD, LPVOID, DWORD);
    BOOL(WINAPI *set_composition_window)(HIMC, LPCOMPOSITIONFORM);
    BOOL(WINAPI *set_candidate_window)(HIMC, LPCANDIDATEFORM);
    BOOL(WINAPI *associate)(HWND, HIMC, DWORD);
    BOOL(WINAPI *notify)(HIMC, DWORD, DWORD, DWORD);
} cee_imm;

static int cee_imm_load(void) {
    if (cee_imm.state != 0) {
        return cee_imm.state == 1;
    }
    cee_imm.state = -1;
    HMODULE imm = LoadLibraryW(L"imm32.dll");
    if (imm == NULL) {
        return 0;
    }
    FARPROC p[7] = {GetProcAddress(imm, "ImmGetContext"),           GetProcAddress(imm, "ImmReleaseContext"),
                    GetProcAddress(imm, "ImmGetCompositionStringW"), GetProcAddress(imm, "ImmSetCompositionWindow"),
                    GetProcAddress(imm, "ImmSetCandidateWindow"),    GetProcAddress(imm, "ImmAssociateContextEx"),
                    GetProcAddress(imm, "ImmNotifyIME")};
    for (int k = 0; k < 7; k++) {
        if (p[k] == NULL) {
            return 0;
        }
    }
    memcpy(&cee_imm.get_context, &p[0], sizeof p[0]);
    memcpy(&cee_imm.release_context, &p[1], sizeof p[1]);
    memcpy(&cee_imm.get_string, &p[2], sizeof p[2]);
    memcpy(&cee_imm.set_composition_window, &p[3], sizeof p[3]);
    memcpy(&cee_imm.set_candidate_window, &p[4], sizeof p[4]);
    memcpy(&cee_imm.associate, &p[5], sizeof p[5]);
    memcpy(&cee_imm.notify, &p[6], sizeof p[6]);
    cee_imm.state = 1;
    return 1;
}

/* One of the input context's strings (GCS_COMPSTR, GCS_RESULTSTR) as UTF-16, allocated; *units its length. */
static wchar_t *cee_imm_string(HIMC context, DWORD which, size_t *units) {
    LONG bytes = cee_imm.get_string(context, which, NULL, 0);
    *units = 0;
    if (bytes <= 0) {
        return NULL;
    }
    wchar_t *text = (wchar_t *)ceesafe_platform_alloc((size_t)bytes + sizeof(wchar_t));
    if (text == NULL) {
        return NULL;
    }
    LONG got = cee_imm.get_string(context, which, text, (DWORD)bytes);
    if (got <= 0) {
        ceesafe_platform_free(text);
        return NULL;
    }
    *units = (size_t)got / sizeof(wchar_t);
    return text;
}

/* UTF-16 as UTF-8, allocated; *length its bytes. An unpaired surrogate becomes U+FFFD. */
static char *cee_win32_utf8(const wchar_t *text, size_t units, size_t *length) {
    *length = 0;
    int count = units == 0 ? 0 : WideCharToMultiByte(CP_UTF8, 0, text, (int)units, NULL, 0, NULL, NULL);
    char *utf8 = (char *)ceesafe_platform_alloc((size_t)(count > 0 ? count : 0) + 1u);
    if (utf8 != NULL && count > 0) {
        *length = (size_t)WideCharToMultiByte(CP_UTF8, 0, text, (int)units, utf8, count, NULL, NULL);
    }
    return utf8;
}

/* Text events for UTF-16 units, surrogate pairs joined. */
static void cee_win32_text_units(struct cee_window *w, const wchar_t *text, size_t units) {
    for (size_t k = 0; k < units; k++) {
        uint32_t unit = (uint32_t)text[k];
        if (unit >= 0xd800u && unit <= 0xdbffu && k + 1 < units && (uint32_t)text[k + 1] >= 0xdc00u &&
            (uint32_t)text[k + 1] <= 0xdfffu) {
            cee_window_text(w, 0x10000u + ((unit - 0xd800u) << 10) + ((uint32_t)text[k + 1] - 0xdc00u));
            k++;
        } else {
            cee_window_text(w, unit);
        }
    }
}

/* Places the input method's windows by the text input area (in pixels): the composition at its top left, the
   candidates below it, never over it. */
static void cee_win32_place_ime(struct cee_window *w) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    if (!cee_imm_load()) {
        return;
    }
    HIMC context = cee_imm.get_context(n->hwnd);
    if (context == NULL) {
        return;
    }
    float scale = w->scale > 0.0f ? w->scale : 1.0f;
    LONG left = (LONG)((float)w->text_area.x * scale), top = (LONG)((float)w->text_area.y * scale);
    LONG right = (LONG)((float)(w->text_area.x + w->text_area.w) * scale);
    LONG bottom = (LONG)((float)(w->text_area.y + w->text_area.h) * scale);
    COMPOSITIONFORM composition;
    memset(&composition, 0, sizeof composition);
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos.x = left;
    composition.ptCurrentPos.y = top;
    cee_imm.set_composition_window(context, &composition);
    CANDIDATEFORM candidates;
    memset(&candidates, 0, sizeof candidates);
    candidates.dwIndex = 0;
    candidates.dwStyle = CFS_EXCLUDE;
    candidates.ptCurrentPos.x = left;
    candidates.ptCurrentPos.y = bottom;
    candidates.rcArea.left = left;
    candidates.rcArea.top = top;
    candidates.rcArea.right = right;
    candidates.rcArea.bottom = bottom;
    cee_imm.set_candidate_window(context, &candidates);
    cee_imm.release_context(n->hwnd, context);
}

/* WM_IME_COMPOSITION with text input on: the result string as TEXT, the composition string and its caret as the
   composition. DefWindowProcW is not called, so no WM_IME_CHAR or WM_CHAR repeats the result. */
static void cee_win32_composition(struct cee_window *w, LPARAM flags) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    HIMC context = cee_imm.get_context(n->hwnd);
    if (context == NULL) {
        return;
    }
    size_t units = 0;
    if (flags & GCS_RESULTSTR) {
        wchar_t *result = cee_imm_string(context, GCS_RESULTSTR, &units);
        if (result != NULL) {
            cee_win32_text_units(w, result, units);
            ceesafe_platform_free(result);
        }
    }
    if (flags & GCS_COMPSTR) {
        wchar_t *composing = cee_imm_string(context, GCS_COMPSTR, &units);
        size_t caret_units = units;
        if (flags & GCS_CURSORPOS) {
            LONG at = cee_imm.get_string(context, GCS_CURSORPOS, NULL, 0);
            caret_units = at < 0 ? 0 : (size_t)at > units ? units : (size_t)at;
        }
        size_t length = 0, caret = 0;
        char *utf8 = composing != NULL ? cee_win32_utf8(composing, units, &length) : NULL;
        if (utf8 != NULL && caret_units > 0) {
            ceesafe_platform_free(cee_win32_utf8(composing, caret_units, &caret));
        }
        cee_window_compose(w, utf8 != NULL ? utf8 : "", length, caret);
        ceesafe_platform_free(utf8);
        ceesafe_platform_free(composing);
    } else if (flags & GCS_RESULTSTR) {
        cee_window_compose(w, "", 0, 0); /* committed: nothing composed now */
    }
    cee_imm.release_context(n->hwnd, context);
}

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
        cee_window_rescaled(w, (float)HIWORD(wparam) / 96.0f);
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
        if (w->text_input == 1) {
            /* A key the input method takes comes as VK_PROCESSKEY: no KEY for it, nor for its up (ADR 0141). */
            UINT vk = (UINT)wparam;
            if (vk == VK_PROCESSKEY) {
                vk = MapVirtualKeyW((UINT)((lparam >> 16) & 0xff), 1 /* MAPVK_VSC_TO_VK */) & 0xffu;
                if (down) {
                    n->ime_keys[vk / 8u] = (unsigned char)(n->ime_keys[vk / 8u] | (1u << (vk % 8u)));
                } else {
                    n->ime_keys[vk / 8u] = (unsigned char)(n->ime_keys[vk / 8u] & ~(1u << (vk % 8u)));
                }
                return 0;
            }
            if (!down && vk < 256u && (n->ime_keys[vk / 8u] & (1u << (vk % 8u)))) {
                n->ime_keys[vk / 8u] = (unsigned char)(n->ime_keys[vk / 8u] & ~(1u << (vk % 8u)));
                return 0;
            }
        }
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
    case WM_IME_SETCONTEXT:
        if (w->text_input == 1) {
            lparam &= ~(LPARAM)ISC_SHOWUICOMPOSITIONWINDOW; /* the program shows the composition (ADR 0141) */
        }
        break;
    case WM_IME_STARTCOMPOSITION:
        if (w->text_input == 1) {
            cee_win32_place_ime(w);
            return 0; /* no system composition window */
        }
        break;
    case WM_IME_COMPOSITION:
        if (w->text_input == 1 && cee_imm_load()) {
            cee_win32_composition(w, lparam);
            return 0;
        }
        break;
    case WM_IME_ENDCOMPOSITION:
        if (w->text_input == 1) {
            cee_window_compose(w, "", 0, 0);
            return 0;
        }
        break;
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
        if (!w->external) { /* an external window's swap chain owns the pixels: only validate (ADR 0137) */
            cee_win32_paint(w, dc);
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static wchar_t *cee_win32_wide(const char *text) {
    int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    wchar_t *wide = count > 0 ? (wchar_t *)ceesafe_platform_alloc((size_t)count * sizeof(wchar_t)) : NULL;
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
    struct cee_win32 *n = (struct cee_win32 *)ceesafe_platform_alloc_zeroed(sizeof *n);
    wchar_t *wide = cee_win32_wide(title);
    if (n == NULL || wide == NULL) {
        ceesafe_platform_free(n);
        ceesafe_platform_free(wide);
        return CEE_WINDOW_RESOURCES;
    }
    DWORD style = WS_OVERLAPPEDWINDOW;
    HWND hwnd = CreateWindowExW(0, cee_win32_class, wide, style, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, NULL, NULL,
                                instance, NULL);
    ceesafe_platform_free(wide);
    if (hwnd == NULL) {
        ceesafe_platform_free(n);
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
    ceesafe_platform_free(n->frame);
    ceesafe_platform_free(n);
    w->native = NULL;
}

static void cee_native_pump(struct cee_window *w) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    MSG message;
    for (int k = 0; k < 256 && PeekMessageW(&message, NULL, 0, 0, PM_REMOVE); k++) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    cee_window_rescaled(w, cee_win32_scale(n->hwnd));
}

static int32_t cee_native_handles(struct cee_window *w, uint64_t *first, uint64_t *second) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    *first = (uint64_t)(uintptr_t)n->hwnd;
    *second = (uint64_t)(uintptr_t)GetModuleHandleW(NULL); /* the instance that registered the class */
    return CEE_HANDLE_WIN32;
}

static int32_t cee_native_present(struct cee_window *w, const uint32_t *pixels, uint32_t width, uint32_t height) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    size_t count = (size_t)width * (size_t)height;
    if (n->frame == NULL || n->frame_width != width || n->frame_height != height) {
        uint32_t *grown = (uint32_t *)ceesafe_platform_realloc(n->frame, count * sizeof(uint32_t));
        if (grown == NULL) {
            return -CEE_WINDOW_RESOURCES;
        }
        n->frame = grown;
        n->frame_width = width;
        n->frame_height = height;
    }
    for (size_t k = 0; k < count; k++) { /* RGBA bytes to GDI's BGRX */
        n->frame[k] = cee_window_bgra(pixels[k]);
    }
    HDC dc = GetDC(n->hwnd);
    if (dc != NULL) {
        cee_win32_paint(w, dc);
        ReleaseDC(n->hwnd, dc);
    }
    return 0;
}

static int32_t cee_native_present_boxes(struct cee_window *w, const uint32_t *pixels, uint32_t width, uint32_t height,
                                        const struct cee_window_box *boxes, size_t count) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    if (n->frame == NULL || n->frame_width != width || n->frame_height != height) {
        return cee_native_present(w, pixels, width, height);
    }
    for (size_t k = 0; k < count; k++) {
        struct cee_window_box b = boxes[k];
        for (size_t y = b.y; y < (size_t)b.y + b.h; y++) {
            for (size_t x = b.x; x < (size_t)b.x + b.w; x++) {
                n->frame[y * width + x] = cee_window_bgra(pixels[y * width + x]);
            }
        }
    }
    HDC dc = GetDC(n->hwnd);
    if (dc == NULL) {
        return 0;
    }
    RECT client;
    GetClientRect(n->hwnd, &client);
    if ((uint32_t)(client.right - client.left) == width && (uint32_t)(client.bottom - client.top) == height) {
        /* One pixel per pixel: only the boxes. StretchDIBits measures a source rectangle's y from the bottom row,
           even for a top-down DIB, so the box's source y is height - (y + h). */
        BITMAPINFO info;
        memset(&info, 0, sizeof info);
        info.bmiHeader.biSize = sizeof info.bmiHeader;
        info.bmiHeader.biWidth = (LONG)width;
        info.bmiHeader.biHeight = -(LONG)height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        for (size_t k = 0; k < count; k++) {
            struct cee_window_box b = boxes[k];
            StretchDIBits(dc, (int)b.x, (int)b.y, (int)b.w, (int)b.h, (int)b.x, (int)(height - b.y - b.h), (int)b.w,
                          (int)b.h, n->frame, &info, DIB_RGB_COLORS, SRCCOPY);
        }
    } else {
        cee_win32_paint(w, dc);
    }
    ReleaseDC(n->hwnd, dc);
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
        ceesafe_platform_free(wide);
    }
}

/* Text input on: the window's default input context, the input method's windows placed by the area; off: a
   composition cancelled and no input context, so keys reach the program as keys (ADR 0141). */
static void cee_native_text_input(struct cee_window *w, int on) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    if (!cee_imm_load()) {
        return;
    }
    if (on) {
        cee_imm.associate(n->hwnd, NULL, IACE_DEFAULT);
        cee_win32_place_ime(w);
        return;
    }
    HIMC context = cee_imm.get_context(n->hwnd);
    if (context != NULL) {
        cee_imm.notify(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        cee_imm.release_context(n->hwnd, context);
    }
    cee_imm.associate(n->hwnd, NULL, 0);
    memset(n->ime_keys, 0, sizeof n->ime_keys);
}

static void cee_native_text_area(struct cee_window *w) {
    if (w->text_input == 1) {
        cee_win32_place_ime(w);
    }
}

/* Another program may hold the clipboard for a moment: a few tries. */
static int cee_win32_open_clipboard(HWND hwnd) {
    for (int k = 0; k < 10; k++) {
        if (OpenClipboard(hwnd)) {
            return 1;
        }
        Sleep(5);
    }
    return 0;
}

/* The clipboard's CF_UNICODETEXT (ADR 0141). */
static int32_t cee_native_clipboard_set(struct cee_window *w, const char *text, size_t length) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    int units = length == 0 ? 0 : MultiByteToWideChar(CP_UTF8, 0, text, (int)length, NULL, 0);
    if (length > 0 && units <= 0) {
        return -CEE_WINDOW_RESOURCES;
    }
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, ((size_t)units + 1u) * sizeof(wchar_t));
    wchar_t *wide = memory != NULL ? (wchar_t *)GlobalLock(memory) : NULL;
    if (wide == NULL) {
        if (memory != NULL) {
            GlobalFree(memory);
        }
        return -CEE_WINDOW_RESOURCES;
    }
    if (units > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text, (int)length, wide, units);
    }
    wide[units] = 0;
    GlobalUnlock(memory);
    if (!cee_win32_open_clipboard(n->hwnd)) {
        GlobalFree(memory);
        return -CEE_WINDOW_RESOURCES;
    }
    EmptyClipboard();
    int32_t result = 0;
    if (SetClipboardData(CF_UNICODETEXT, memory) == NULL) {
        GlobalFree(memory); /* still ours: the clipboard took it only on success */
        result = -CEE_WINDOW_RESOURCES;
    }
    CloseClipboard();
    return result;
}

static int cee_native_clipboard_get(struct cee_window *w) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    if (!cee_win32_open_clipboard(n->hwnd)) {
        return 0;
    }
    int found = 0;
    HANDLE memory = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *wide = memory != NULL ? (const wchar_t *)GlobalLock(memory) : NULL;
    if (wide != NULL) {
        size_t limit = GlobalSize(memory) / sizeof(wchar_t), units = 0;
        while (units < limit && wide[units] != 0) {
            units++;
        }
        size_t length = 0;
        char *utf8 = units <= 0x7fffffffu ? cee_win32_utf8(wide, units, &length) : NULL;
        found = utf8 != NULL && cee_window_paste(w, utf8, length);
        ceesafe_platform_free(utf8);
        GlobalUnlock(memory);
    }
    CloseClipboard();
    return found;
}

static void cee_native_wake(void) {} /* the window's event, which the wait includes, is set (ADR 0142) */

/* Until a message, one already queued included, the window's wake event (ADR 0142) or `seconds`. */
static void cee_native_wait(struct cee_window *w, double seconds) {
    double ms = seconds * 1000.0 + 0.999;
    DWORD milliseconds = ms <= 0.0 ? 0u : ms >= 4294967294.0 ? 4294967294u : (DWORD)ms;
    HANDLE wake = (HANDLE)w->wake_event;
    MsgWaitForMultipleObjectsEx(1, &wake, milliseconds, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}

#elif defined(__linux__)
/* X11 through libX11.so.6, loaded with dlopen: the declarations below are Xlib's (X11/Xlib.h, LP64), so no headers
   are needed to build. A frame is converted to the visual's pixel layout, scaled to the window's size, and drawn
   with XPutImage. */
#include <dlfcn.h>
#include <locale.h>
#include <sys/select.h>

/* KeyPress, KeyRelease, ButtonPress, ButtonRelease, PointerMotion, Exposure, StructureNotify, FocusChange. */
#define CEE_X_EVENTS (1L | 2L | 4L | 8L | 64L | (1L << 15) | (1L << 17) | (1L << 21))

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
typedef struct { /* XSelectionRequestEvent */
    int type;
    unsigned long serial;
    int send_event;
    cee_x_display *display;
    cee_xid owner, requestor;
    unsigned long selection, target, property, time;
} cee_x_selection_request;
typedef struct { /* XSelectionEvent */
    int type;
    unsigned long serial;
    int send_event;
    cee_x_display *display;
    cee_xid requestor;
    unsigned long selection, target, property, time;
} cee_x_selection;
typedef struct { /* XSelectionClearEvent */
    int type;
    unsigned long serial;
    int send_event;
    cee_x_display *display;
    cee_xid window;
    unsigned long selection, time;
} cee_x_selection_clear;
typedef union {
    int type;
    cee_x_any any;
    cee_x_input input;
    cee_x_configure configure;
    cee_x_client client;
    cee_x_selection_request request;
    cee_x_selection selection;
    cee_x_selection_clear clear;
    long pad[24];
} cee_x_event;
typedef struct { /* XIMStyles */
    unsigned short count;
    unsigned long *styles;
} cee_x_styles;
typedef struct { /* XPoint */
    short x, y;
} cee_x_point;
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
    int (*set_background_pixmap)(cee_x_display *, cee_xid, cee_xid); /* for external windows (ADR 0137) */
    /* The input method and the selections (ADR 0141); the variadic ones are called as variadic. */
    char *(*set_locale_modifiers)(const char *);
    void *(*open_im)(cee_x_display *, void *, char *, char *);
    int (*close_im)(void *);
    char *(*get_im_values)(void *, ...);
    void *(*create_ic)(void *, ...);
    void (*destroy_ic)(void *);
    void (*set_ic_focus)(void *);
    void (*unset_ic_focus)(void *);
    char *(*set_ic_values)(void *, ...);
    char *(*get_ic_values)(void *, ...);
    void *(*nested_list)(int, ...);
    int (*filter_event)(cee_x_event *, cee_xid);
    int (*utf8_lookup_string)(void *, cee_x_input *, char *, int, unsigned long *, int *);
    int (*release)(void *); /* XFree */
    char *(*utf8_reset_ic)(void *);
    cee_xid (*get_selection_owner)(cee_x_display *, unsigned long);
    int (*set_selection_owner)(cee_x_display *, unsigned long, cee_xid, unsigned long);
    int (*convert_selection)(cee_x_display *, unsigned long, unsigned long, unsigned long, cee_xid, unsigned long);
    int (*change_property)(cee_x_display *, cee_xid, unsigned long, unsigned long, int, int, const unsigned char *,
                           int);
    int (*get_window_property)(cee_x_display *, cee_xid, unsigned long, long, long, int, unsigned long,
                               unsigned long *, int *, unsigned long *, unsigned long *, unsigned char **);
    int (*send_event)(cee_x_display *, cee_xid, int, long, cee_x_event *);
    int (*check_typed_window_event)(cee_x_display *, cee_xid, int, cee_x_event *);
    long (*max_request_size)(cee_x_display *);
    long (*extended_max_request_size)(cee_x_display *);
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
    /* Text input (ADR 0141): the input method and context, opened at the first setTextInput(true), and whether the
       context draws its pre-edit at a spot (over the spot, XIMPreeditPosition). */
    void *im, *ic;
    int spot;
    unsigned char ime_keys[32]; /* keycodes whose press the input method took */
    /* The CLIPBOARD selection while this window owns it, and the atoms it uses. */
    char *owned;
    size_t owned_length;
    unsigned long clipboard_atom, utf8_atom, targets_atom, property_atom, incr_atom;
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
    CEE_X(set_background_pixmap, int (*)(cee_x_display *, cee_xid, cee_xid), "XSetWindowBackgroundPixmap")
    CEE_X(set_locale_modifiers, char *(*)(const char *), "XSetLocaleModifiers")
    CEE_X(open_im, void *(*)(cee_x_display *, void *, char *, char *), "XOpenIM")
    CEE_X(close_im, int (*)(void *), "XCloseIM")
    CEE_X(get_im_values, char *(*)(void *, ...), "XGetIMValues")
    CEE_X(create_ic, void *(*)(void *, ...), "XCreateIC")
    CEE_X(destroy_ic, void (*)(void *), "XDestroyIC")
    CEE_X(set_ic_focus, void (*)(void *), "XSetICFocus")
    CEE_X(unset_ic_focus, void (*)(void *), "XUnsetICFocus")
    CEE_X(set_ic_values, char *(*)(void *, ...), "XSetICValues")
    CEE_X(get_ic_values, char *(*)(void *, ...), "XGetICValues")
    CEE_X(nested_list, void *(*)(int, ...), "XVaCreateNestedList")
    CEE_X(filter_event, int (*)(cee_x_event *, cee_xid), "XFilterEvent")
    CEE_X(utf8_lookup_string, int (*)(void *, cee_x_input *, char *, int, unsigned long *, int *),
          "Xutf8LookupString")
    CEE_X(release, int (*)(void *), "XFree")
    CEE_X(utf8_reset_ic, char *(*)(void *), "Xutf8ResetIC")
    CEE_X(get_selection_owner, cee_xid(*)(cee_x_display *, unsigned long), "XGetSelectionOwner")
    CEE_X(set_selection_owner, int (*)(cee_x_display *, unsigned long, cee_xid, unsigned long), "XSetSelectionOwner")
    CEE_X(convert_selection,
          int (*)(cee_x_display *, unsigned long, unsigned long, unsigned long, cee_xid, unsigned long),
          "XConvertSelection")
    CEE_X(change_property,
          int (*)(cee_x_display *, cee_xid, unsigned long, unsigned long, int, int, const unsigned char *, int),
          "XChangeProperty")
    CEE_X(get_window_property,
          int (*)(cee_x_display *, cee_xid, unsigned long, long, long, int, unsigned long, unsigned long *, int *,
                  unsigned long *, unsigned long *, unsigned char **),
          "XGetWindowProperty")
    CEE_X(send_event, int (*)(cee_x_display *, cee_xid, int, long, cee_x_event *), "XSendEvent")
    CEE_X(check_typed_window_event, int (*)(cee_x_display *, cee_xid, int, cee_x_event *), "XCheckTypedWindowEvent")
    CEE_X(max_request_size, long (*)(cee_x_display *), "XMaxRequestSize")
    CEE_X(extended_max_request_size, long (*)(cee_x_display *), "XExtendedMaxRequestSize")
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
    struct cee_x11 *n = (struct cee_x11 *)ceesafe_platform_alloc_zeroed(sizeof *n);
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
        ceesafe_platform_free(n);
        return CEE_WINDOW_UNSUPPORTED; /* only TrueColor visuals of 24 or 32 bits */
    }
    n->window = cee_x.create_simple_window(display, cee_x.root_window(display, n->screen), 0, 0, w->width, w->height,
                                           0, 0ul, 0ul);
    if (n->window == 0) {
        cee_x.close_display(display);
        ceesafe_platform_free(n);
        return CEE_WINDOW_RESOURCES;
    }
    cee_x.select_input(display, n->window, CEE_X_EVENTS);
    n->delete_atom = cee_x.intern_atom(display, "WM_DELETE_WINDOW", 0);
    cee_x.set_wm_protocols(display, n->window, &n->delete_atom, 1);
    cee_x.detectable_repeat(display, 1, NULL);
    if (w->external) {
        /* Background None: the X server leaves an exposed part as the GPU drew it, rather than clearing it. */
        cee_x.set_background_pixmap(display, n->window, 0);
    }
    n->clipboard_atom = cee_x.intern_atom(display, "CLIPBOARD", 0);
    n->utf8_atom = cee_x.intern_atom(display, "UTF8_STRING", 0);
    n->targets_atom = cee_x.intern_atom(display, "TARGETS", 0);
    n->property_atom = cee_x.intern_atom(display, "CEESAFE_CLIPBOARD", 0);
    n->incr_atom = cee_x.intern_atom(display, "INCR", 0);
    cee_x.store_name(display, n->window, title);
    cee_x.map_window(display, n->window);
    cee_x.flush(display);
    w->native = n;
    w->scale = 1.0f;
    return 0;
}

static int32_t cee_native_handles(struct cee_window *w, uint64_t *first, uint64_t *second) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    *first = (uint64_t)(uintptr_t)n->display;
    *second = (uint64_t)n->window;
    return CEE_HANDLE_XLIB;
}

static void cee_native_close(struct cee_window *w) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    if (n == NULL) {
        return;
    }
    if (n->ic != NULL) {
        cee_x.destroy_ic(n->ic);
    }
    if (n->im != NULL) {
        cee_x.close_im(n->im);
    }
    cee_x.destroy_window(n->display, n->window);
    cee_x.close_display(n->display);
    ceesafe_platform_free(n->owned);
    ceesafe_platform_free(n->frame);
    ceesafe_platform_free(n);
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

/* A KeyPress with text input on (ADR 0141): the input context's text, as UTF-8. A key code of 0 is text the input
   method committed, not a key: no KEY for it. */
static void cee_x_lookup(struct cee_window *w, cee_x_event *e, unsigned int code, int32_t key, int32_t repeat,
                         int32_t modifiers) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    char buffer[256];
    char *text = buffer;
    unsigned long sym = 0;
    int status = 0;
    int count = cee_x.utf8_lookup_string(n->ic, &e->input, buffer, (int)sizeof buffer, &sym, &status);
    if (status == -1 && count > 0) { /* XBufferOverflow: count is the length needed */
        text = (char *)ceesafe_platform_alloc((size_t)count);
        count = text != NULL ? cee_x.utf8_lookup_string(n->ic, &e->input, text, count, &sym, &status) : 0;
    }
    if (code != 0) {
        cee_window_push(w, CEE_EVENT_KEY, key, 1, repeat, modifiers, 0.0f, 0.0f);
    }
    if ((status == 2 || status == 4) && count > 0 && (code == 0 || !(modifiers & (CEE_MOD_CONTROL | CEE_MOD_COMMAND)))) {
        size_t i = 0;
        while (i < (size_t)count) { /* XLookupChars, XLookupBoth */
            cee_window_text(w, cee_window_utf8((const unsigned char *)text, (size_t)count, &i));
        }
    }
    if (text != buffer) {
        ceesafe_platform_free(text);
    }
}

/* The most bytes a property may hold in one request (no INCR transfers). */
static size_t cee_x_max_property(struct cee_x11 *n) {
    long words = cee_x.extended_max_request_size(n->display);
    if (words <= 0) {
        words = cee_x.max_request_size(n->display);
    }
    return words > 256 ? (size_t)words * 4u - 1024u : 0u;
}

/* Answers another program's request for the CLIPBOARD this window owns: TARGETS or UTF8_STRING (ADR 0141). */
static void cee_x_answer(struct cee_x11 *n, const cee_x_selection_request *r) {
    cee_x_event reply;
    memset(&reply, 0, sizeof reply);
    reply.selection.type = 31; /* SelectionNotify */
    reply.selection.send_event = 1;
    reply.selection.display = n->display;
    reply.selection.requestor = r->requestor;
    reply.selection.selection = r->selection;
    reply.selection.target = r->target;
    reply.selection.time = r->time;
    unsigned long property = r->property != 0 ? r->property : r->target; /* None: an old client */
    if (r->selection == n->clipboard_atom && n->owned != NULL) {
        if (r->target == n->targets_atom) {
            unsigned long targets[2] = {n->targets_atom, n->utf8_atom}; /* format 32 is longs */
            cee_x.change_property(n->display, r->requestor, property, 4 /* XA_ATOM */, 32, 0 /* PropModeReplace */,
                                  (const unsigned char *)targets, 2);
            reply.selection.property = property;
        } else if (r->target == n->utf8_atom && n->owned_length <= cee_x_max_property(n)) {
            cee_x.change_property(n->display, r->requestor, property, n->utf8_atom, 8, 0,
                                  (const unsigned char *)n->owned, (int)n->owned_length);
            reply.selection.property = property;
        }
    }
    cee_x.send_event(n->display, r->requestor, 0, 0, &reply);
    cee_x.flush(n->display);
}

static void cee_native_pump(struct cee_window *w) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    for (int k = 0; k < 256 && cee_x.pending(n->display) > 0; k++) {
        cee_x_event e;
        cee_x.next_event(n->display, &e);
        /* The key code before XFilterEvent, which may rewrite the event it puts back (Xlib's own input method). */
        unsigned int taken = e.input.code & 0xffu;
        int press = e.type == 2;
        if (n->im != NULL && cee_x.filter_event(&e, 0)) {
            if (press) { /* a KeyPress the input method took: its release is no event either (ADR 0141) */
                n->ime_keys[taken / 8u] = (unsigned char)(n->ime_keys[taken / 8u] | (1u << (taken % 8u)));
            }
            continue;
        }
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
            if (n->ime_keys[code / 8u] & (1u << (code % 8u))) {
                n->ime_keys[code / 8u] = (unsigned char)(n->ime_keys[code / 8u] & ~(1u << (code % 8u)));
                if (!down) {
                    break;
                }
            }
            unsigned long unshifted = cee_x.lookup_keysym(&e.input, 0);
            int32_t modifiers = cee_x_modifiers(e.input.state);
            if (down && n->ic != NULL && w->text_input == 1) {
                cee_x_lookup(w, &e, code, cee_x_key(unshifted), repeat, modifiers);
                break;
            }
            char text[32];
            unsigned long sym = 0;
            cee_x.lookup_string(&e.input, text, (int)sizeof text, &sym, NULL);
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
        case 9: /* FocusIn */
            cee_window_focus(w, 1);
            if (n->ic != NULL && w->text_input == 1) {
                cee_x.set_ic_focus(n->ic);
            }
            break;
        case 10: /* FocusOut */
            cee_window_focus(w, 0);
            if (n->ic != NULL) {
                cee_x.unset_ic_focus(n->ic);
            }
            break;
        case 29: /* SelectionClear: another program owns the clipboard now */
            if (e.clear.selection == n->clipboard_atom) {
                ceesafe_platform_free(n->owned);
                n->owned = NULL;
                n->owned_length = 0;
            }
            break;
        case 30: cee_x_answer(n, &e.request); break; /* SelectionRequest */
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
    /* The C library's malloc, not the platform's: the image owns the block, and XDestroyImage frees it
       with free (ADR 0088 §2.5). */
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
        uint32_t *grown = (uint32_t *)ceesafe_platform_realloc(n->frame, count * sizeof(uint32_t));
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

static int32_t cee_native_present_boxes(struct cee_window *w, const uint32_t *pixels, uint32_t width, uint32_t height,
                                        const struct cee_window_box *boxes, size_t count) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    if (n->frame == NULL || n->frame_width != width || n->frame_height != height) {
        return cee_native_present(w, pixels, width, height);
    }
    for (size_t k = 0; k < count; k++) {
        struct cee_window_box b = boxes[k];
        for (size_t y = b.y; y < (size_t)b.y + b.h; y++) {
            memcpy(n->frame + y * width + b.x, pixels + y * width + b.x, (size_t)b.w * sizeof(uint32_t));
        }
    }
    if (w->width != width || w->height != height) {
        cee_x_draw(w); /* scaled: the whole frame */
        return 0;
    }
    /* One pixel per pixel: only the boxes. */
    for (size_t k = 0; k < count; k++) {
        struct cee_window_box b = boxes[k];
        /* The C library's malloc, not the platform's: the image owns the block, and XDestroyImage frees it
           with free (ADR 0088 §2.5). */
        char *data = (char *)malloc((size_t)b.w * (size_t)b.h * 4u);
        if (data == NULL) {
            return -CEE_WINDOW_RESOURCES;
        }
        cee_x_image *image =
            cee_x.create_image(n->display, n->visual, (unsigned int)n->depth, 2 /* ZPixmap */, 0, data, b.w, b.h, 32, 0);
        if (image == NULL) {
            free(data);
            return -CEE_WINDOW_RESOURCES;
        }
        unsigned rs = cee_x_shift(image->red_mask), gs = cee_x_shift(image->green_mask), bs = cee_x_shift(image->blue_mask);
        for (uint32_t y = 0; y < b.h; y++) {
            uint32_t *row = (uint32_t *)(void *)(data + (size_t)y * (size_t)image->bytes_per_line);
            for (uint32_t x = 0; x < b.w; x++) {
                uint32_t p = n->frame[(size_t)(b.y + y) * width + b.x + x];
                row[x] = ((p & 0xffu) << rs) | (((p >> 8) & 0xffu) << gs) | (((p >> 16) & 0xffu) << bs);
            }
        }
        cee_x.put_image(n->display, n->window, cee_x.default_gc(n->display, n->screen), image, 0, 0, (int)b.x,
                        (int)b.y, b.w, b.h);
        image->f.destroy_image(image);
    }
    cee_x.flush(n->display);
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

/* The spot the input method draws its pre-edit at, over the spot: the area's bottom left, in pixels. */
static void cee_x_spot(struct cee_window *w) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    if (n->ic == NULL || !n->spot) {
        return;
    }
    cee_x_point point = {(short)(w->text_area.x > 32767u ? 32767u : w->text_area.x),
                         (short)(w->text_area.y + w->text_area.h > 32767u ? 32767u : w->text_area.y + w->text_area.h)};
    void *list = cee_x.nested_list(0, "spotLocation", &point, (void *)NULL);
    if (list != NULL) {
        cee_x.set_ic_values(n->ic, "preeditAttributes", list, (void *)NULL);
        cee_x.release(list);
    }
}

/* Opens the input method and a context on the window, at the first setTextInput(true). XOpenIM needs a locale
   that is not C to find most input methods: LC_CTYPE is the environment's for the call, then restored. The style
   is over the spot (XIMPreeditPosition) if the input method offers it and the context opens, then XIMPreeditNothing
   (the input method's own window), then XIMPreeditNone. */
static void cee_x_open_im(struct cee_window *w) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    if (n->im == NULL) {
        char saved[128] = "C";
        const char *before = setlocale(LC_CTYPE, NULL);
        if (before != NULL && strlen(before) < sizeof saved) {
            memcpy(saved, before, strlen(before) + 1u);
        }
        int changed = strcmp(saved, "C") == 0 || strcmp(saved, "POSIX") == 0;
        if (changed) {
            changed = setlocale(LC_CTYPE, "") != NULL;
        }
        cee_x.set_locale_modifiers("");
        n->im = cee_x.open_im(n->display, NULL, NULL, NULL);
        if (changed) {
            setlocale(LC_CTYPE, saved);
        }
        if (n->im == NULL) {
            return;
        }
    }
    if (n->ic != NULL) {
        return;
    }
    cee_x_styles *styles = NULL;
    if (cee_x.get_im_values(n->im, "queryInputStyle", &styles, (void *)NULL) != NULL) {
        styles = NULL;
    }
    static const unsigned long wanted[3] = {0x0004ul | 0x0400ul, 0x0008ul | 0x0400ul, 0x0010ul | 0x0800ul};
    for (int k = 0; k < 3 && n->ic == NULL; k++) {
        int offered = styles == NULL;
        for (unsigned j = 0; styles != NULL && j < styles->count; j++) {
            offered = offered || styles->styles[j] == wanted[k];
        }
        if (!offered) {
            continue;
        }
        if (k == 0) {
            cee_x_point point = {0, 0};
            void *list = cee_x.nested_list(0, "spotLocation", &point, (void *)NULL);
            if (list == NULL) {
                continue;
            }
            n->ic = cee_x.create_ic(n->im, "inputStyle", wanted[k], "clientWindow", n->window, "focusWindow",
                                    n->window, "preeditAttributes", list, (void *)NULL);
            cee_x.release(list);
            n->spot = n->ic != NULL;
        } else {
            n->ic = cee_x.create_ic(n->im, "inputStyle", wanted[k], "clientWindow", n->window, "focusWindow",
                                    n->window, (void *)NULL);
        }
    }
    if (styles != NULL) {
        cee_x.release(styles);
    }
    if (n->ic != NULL) {
        unsigned long mask = 0;
        if (cee_x.get_ic_values(n->ic, "filterEvents", &mask, (void *)NULL) == NULL) {
            cee_x.select_input(n->display, n->window, CEE_X_EVENTS | (long)mask);
        }
    }
}

/* Text input on or off (ADR 0141). Off, the context loses focus and its pre-edit is dropped; keys then go through
   XLookupString as before. */
static void cee_native_text_input(struct cee_window *w, int on) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    if (on) {
        cee_x_open_im(w);
        if (n->ic != NULL) {
            cee_x.set_ic_focus(n->ic);
            cee_x_spot(w);
        }
    } else if (n->ic != NULL) {
        char *dropped = cee_x.utf8_reset_ic(n->ic);
        if (dropped != NULL) {
            cee_x.release(dropped);
        }
        cee_x.unset_ic_focus(n->ic);
    }
    cee_x.flush(n->display);
}

static void cee_native_text_area(struct cee_window *w) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    cee_x_spot(w);
    cee_x.flush(n->display);
}

/* The CLIPBOARD selection, as UTF8_STRING (ADR 0141): this window owns it and answers in the pump. */
static int32_t cee_native_clipboard_set(struct cee_window *w, const char *text, size_t length) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    if (length > cee_x_max_property(n)) {
        return -CEE_WINDOW_RESOURCES; /* more than one request carries: no INCR */
    }
    char *copy = (char *)ceesafe_platform_alloc(length + 1u);
    if (copy == NULL) {
        return -CEE_WINDOW_RESOURCES;
    }
    if (length > 0) {
        memcpy(copy, text, length);
    }
    ceesafe_platform_free(n->owned);
    n->owned = copy;
    n->owned_length = length;
    cee_x.set_selection_owner(n->display, n->clipboard_atom, n->window, 0 /* CurrentTime */);
    if (cee_x.get_selection_owner(n->display, n->clipboard_atom) != n->window) {
        ceesafe_platform_free(n->owned);
        n->owned = NULL;
        n->owned_length = 0;
        return -CEE_WINDOW_RESOURCES;
    }
    cee_x.flush(n->display);
    return 0;
}

/* Asks the owner for UTF8_STRING and waits up to a second for its SelectionNotify; other events stay queued. */
static int cee_native_clipboard_get(struct cee_window *w) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    cee_xid owner = cee_x.get_selection_owner(n->display, n->clipboard_atom);
    if (owner == n->window && n->owned != NULL) {
        return cee_window_paste(w, n->owned, n->owned_length);
    }
    if (owner == 0) {
        return 0;
    }
    cee_x.convert_selection(n->display, n->clipboard_atom, n->utf8_atom, n->property_atom, n->window, 0);
    cee_x.flush(n->display);
    double deadline = cee_window_now() + 1.0;
    cee_x_event e;
    for (;;) {
        if (cee_x.check_typed_window_event(n->display, n->window, 31 /* SelectionNotify */, &e)) {
            break;
        }
        double left = deadline - cee_window_now();
        if (left <= 0.0) {
            return 0;
        }
        int fd = cee_x.connection_number(n->display);
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = (suseconds_t)((left < 0.05 ? left : 0.05) * 1e6);
        select(fd + 1, &set, NULL, NULL, &timeout);
    }
    if (e.selection.property == 0) {
        return 0; /* the owner has no UTF8_STRING */
    }
    unsigned long type = 0, items = 0, after = 0;
    int format = 0;
    unsigned char *data = NULL;
    int found = 0;
    if (cee_x.get_window_property(n->display, n->window, n->property_atom, 0, 0x1fffffffL, 1 /* delete */,
                                  0 /* AnyPropertyType */, &type, &format, &items, &after, &data) == 0) {
        if (type != n->incr_atom && format == 8 && data != NULL) {
            found = cee_window_paste(w, (const char *)data, (size_t)items);
        }
        if (data != NULL) {
            cee_x.release(data);
        }
    }
    return found;
}

static void cee_native_wake(void) {} /* the window's pipe, which the wait includes, has its byte (ADR 0142) */

/* Until an event (Xlib's queue, which XPending flushes and fills, or the connection), the window's pipe (ADR 0142) or
   `seconds`. */
static void cee_native_wait(struct cee_window *w, double seconds) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    if (cee_x.pending(n->display) > 0) {
        return;
    }
    double ms = seconds * 1000.0 + 0.999;
    struct pollfd p[2] = {{cee_x.connection_number(n->display), POLLIN, 0}, {w->wake_read, POLLIN, 0}};
    poll(p, 2, ms <= 0.0 ? 0 : ms >= 2147483647.0 ? 2147483647 : (int)ms);
}

#else
#define CEE_WINDOW_NO_NATIVE 1
#endif

/* ---- The calls lib/std/window.cee makes -------------------------------------------------------------------------- */

static char *cee_window_cstring(const uint8_t *text, uint64_t length) {
    char *s = (char *)ceesafe_platform_alloc((size_t)length + 1u);
    if (s != NULL) {
        if (length > 0) {
            memcpy(s, text, (size_t)length);
        }
        s[length] = '\0';
    }
    return s;
}

void *ceesafe_window_open(const uint8_t *title, uint64_t length, uint32_t width, uint32_t height, uint32_t surface,
                          int32_t *error) {
    *error = 0;
    if (width == 0 || height == 0 || width > CEE_WINDOW_MAX_SIDE || height > CEE_WINDOW_MAX_SIDE ||
        (surface != CEE_SURFACE_FRAMES && surface != CEE_SURFACE_EXTERNAL)) {
        *error = CEE_WINDOW_UNSUPPORTED;
        return NULL;
    }
    if (!cee_window_on_main_thread()) {
        *error = CEE_WINDOW_NOT_MAIN_THREAD;
        return NULL;
    }
    struct cee_window *w = (struct cee_window *)ceesafe_platform_alloc_zeroed(sizeof *w);
    char *name = cee_window_cstring(title, length);
    if (w == NULL || name == NULL) {
        ceesafe_platform_free(w);
        ceesafe_platform_free(name);
        *error = CEE_WINDOW_RESOURCES;
        return NULL;
    }
    w->width = width;
    w->height = height;
    w->external = surface == CEE_SURFACE_EXTERNAL;
    w->scale = 1.0f;
    w->focused = 1;
    w->text_input = -1;
    w->last_wait = cee_window_now();
    if (!cee_wake_signal_open(w)) {
        ceesafe_platform_free(w);
        ceesafe_platform_free(name);
        *error = CEE_WINDOW_RESOURCES;
        return NULL;
    }
    const char *script = getenv("CEESAFE_WINDOW_SCRIPT");
    if (script != NULL && script[0] != '\0') {
        w->scripted = 1;
        w->dump = getenv("CEESAFE_WINDOW_DUMP");
        if (w->dump != NULL && w->dump[0] == '\0') {
            w->dump = NULL;
        }
        if (!cee_window_script_load(w, script)) {
            cee_wake_signal_close(w);
            ceesafe_platform_free(w->script);
            ceesafe_platform_free(w->script_text);
            ceesafe_platform_free(w);
            ceesafe_platform_free(name);
            *error = CEE_WINDOW_RESOURCES;
            return NULL;
        }
        ceesafe_platform_free(name);
        cee_window_script_pump(w);
        cee_wake_register(w);
        return w;
    }
#if defined(CEE_WINDOW_NO_NATIVE)
    cee_wake_signal_close(w);
    ceesafe_platform_free(w);
    ceesafe_platform_free(name);
    *error = CEE_WINDOW_UNSUPPORTED;
    return NULL;
#else
    int failed = cee_native_open(w, name);
    ceesafe_platform_free(name);
    if (failed != 0) {
        cee_wake_signal_close(w);
        ceesafe_platform_free(w);
        *error = failed;
        return NULL;
    }
    cee_wake_register(w);
    return w;
#endif
}

void ceesafe_window_close(void *window) {
    struct cee_window *w = (struct cee_window *)window;
    if (w == NULL) {
        return;
    }
    cee_wake_unregister(w); /* first: no wake reaches the window after this (ADR 0142) */
#if !defined(CEE_WINDOW_NO_NATIVE)
    if (!w->scripted) {
        cee_native_close(w);
    }
#endif
    cee_wake_signal_close(w);
    ceesafe_platform_free(w->script);
    ceesafe_platform_free(w->kept);
    ceesafe_platform_free(w->composing);
    ceesafe_platform_free(w->composition);
    ceesafe_platform_free(w->pasted);
    ceesafe_platform_free(w->script_text);
    ceesafe_platform_free(w->clipboard);
    ceesafe_platform_free(w);
}

/* The system's handles for the window, as integers, and their kind (WindowHandle, ADR 0137). */
int32_t ceesafe_window_native(void *window, uint64_t *first, uint64_t *second) {
    struct cee_window *w = (struct cee_window *)window;
    *first = 0;
    *second = 0;
    if (w->scripted) {
        return CEE_HANDLE_SCRIPTED;
    }
#if !defined(CEE_WINDOW_NO_NATIVE)
    return cee_native_handles(w, first, second);
#else
    return CEE_HANDLE_SCRIPTED; /* not reached: without a backend, only scripted windows open */
#endif
}

int32_t ceesafe_window_poll(void *window, int32_t *fields, float *position) {
    struct cee_window *w = (struct cee_window *)window;
    struct cee_window_event e;
    int pumped = 0;
    do {
        if (w->count == 0 && !pumped) {
            pumped = 1;
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
        e = w->queue[w->head];
        w->head = (w->head + 1u) % CEE_WINDOW_QUEUE;
        w->count--;
    } while (e.kind == CEE_EVENT_DROPPED); /* a key the input method took back (ADR 0141) */
    if (e.kind == CEE_EVENT_COMPOSITION) {
        cee_window_show_composition(w); /* the program's copy changes only here (ADR 0141) */
    }
    memcpy(fields, e.fields, sizeof e.fields);
    position[0] = e.x;
    position[1] = e.y;
    return e.kind;
}

int32_t ceesafe_window_present(void *window, const uint32_t *pixels, uint32_t width, uint32_t height) {
    struct cee_window *w = (struct cee_window *)window;
    if (w->external) {
        return -CEE_WINDOW_UNSUPPORTED; /* the program draws an external window's content (ADR 0137) */
    }
    if (width == 0 || height == 0 || width > CEE_WINDOW_MAX_SIDE * 4u || height > CEE_WINDOW_MAX_SIDE * 4u) {
        return -CEE_WINDOW_RESOURCES;
    }
    int32_t result;
    if (w->scripted) {
        size_t count = (size_t)width * (size_t)height;
        if (!w->shown || w->shown_width != width || w->shown_height != height) {
            uint32_t *grown = (uint32_t *)ceesafe_platform_realloc(w->kept, count * sizeof(uint32_t));
            if (grown == NULL) {
                return -CEE_WINDOW_RESOURCES;
            }
            w->kept = grown;
        }
        memcpy(w->kept, pixels, count * sizeof(uint32_t));
        result = cee_window_dump(w, w->kept, width, height);
        w->frames++;
    } else {
#if !defined(CEE_WINDOW_NO_NATIVE)
        result = cee_native_present(w, pixels, width, height);
#else
        result = -CEE_WINDOW_UNSUPPORTED;
#endif
    }
    if (result == 0) {
        w->shown = 1;
        w->shown_width = width;
        w->shown_height = height;
    }
    return result;
}

/* type-system.md §26.13, ADR 0070: only `boxes` (x, y, w, h in the image's pixels) differ from the last frame
   presented. Without a last frame of this size, or with a box outside the image, it is a plain present. */
int32_t ceesafe_window_present_changed(void *window, const uint32_t *pixels, uint32_t width, uint32_t height,
                                       const uint32_t *boxes, uint64_t count) {
    struct cee_window *w = (struct cee_window *)window;
    if (w->external) {
        return -CEE_WINDOW_UNSUPPORTED;
    }
    if (width == 0 || height == 0 || width > CEE_WINDOW_MAX_SIDE * 4u || height > CEE_WINDOW_MAX_SIDE * 4u) {
        return -CEE_WINDOW_RESOURCES;
    }
    if (!w->shown || w->shown_width != width || w->shown_height != height || count > 4096u) {
        return ceesafe_window_present(window, pixels, width, height);
    }
    struct cee_window_box list[64];
    size_t n = 0;
    for (uint64_t k = 0; k < count; k++) {
        struct cee_window_box b = {boxes[4 * k], boxes[4 * k + 1], boxes[4 * k + 2], boxes[4 * k + 3]};
        if (b.w == 0 || b.h == 0) {
            continue;
        }
        if (b.x >= width || b.y >= height || b.w > width - b.x || b.h > height - b.y || n == 64) {
            return ceesafe_window_present(window, pixels, width, height);
        }
        list[n++] = b;
    }
    if (w->scripted) {
        for (size_t k = 0; k < n; k++) {
            for (size_t y = list[k].y; y < (size_t)list[k].y + list[k].h; y++) {
                memcpy(w->kept + y * width + list[k].x, pixels + y * width + list[k].x, (size_t)list[k].w * 4u);
            }
        }
        int32_t result = n > 0 ? cee_window_dump(w, w->kept, width, height) : 0;
        w->frames++;
        return result;
    }
    if (n == 0) {
        return 0;
    }
#if !defined(CEE_WINDOW_NO_NATIVE)
    return cee_native_present_boxes(w, pixels, width, height, list, n);
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
    ceesafe_platform_free(name);
}

void ceesafe_window_wait(void *window) {
    struct cee_window *w = (struct cee_window *)window;
    double target = w->last_wait + 1.0 / 60.0, now = cee_window_now();
    if (w->scripted || now >= target || w->count > 0 || cee_wake_pending(w)) {
        w->last_wait = now > target ? now : target;
        cee_wake_take(w);
        return;
    }
#if !defined(CEE_WINDOW_NO_NATIVE)
    cee_native_wait(w, target - now);
#endif
    now = cee_window_now();
    w->last_wait = now > target ? now : target;
    cee_wake_take(w);
}

/* ---- Waiting for events and waking (ADR 0142) ------------------------------------------------------------------- */

/* Whether nextEvent would return an event: one is queued that was not taken back. */
static int cee_window_has_event(struct cee_window *w) {
    for (unsigned k = 0; k < w->count; k++) {
        if (w->queue[(w->head + k) % CEE_WINDOW_QUEUE].kind != CEE_EVENT_DROPPED) {
            return 1;
        }
    }
    return 0;
}

/* Sleeps until an event is queued, the window is woken, or `timeout_ms` pass, taking the system's events (the pump)
   meanwhile, so that a system event that gives the program none does not end the wait. A scripted window returns at
   once while its script still holds events, which come only with presents; then it waits on its signal alone. */
void ceesafe_window_wait_event(void *window, uint32_t timeout_ms) {
    struct cee_window *w = (struct cee_window *)window;
    double deadline = cee_window_now() + (double)timeout_ms / 1000.0;
    for (;;) {
        if (cee_window_has_event(w) || cee_wake_pending(w)) {
            break;
        }
        if (w->scripted) {
            cee_window_script_pump(w);
            if (cee_window_has_event(w) || w->script_next < w->script_count) {
                break;
            }
        }
#if !defined(CEE_WINDOW_NO_NATIVE)
        else {
            cee_native_pump(w);
            if (cee_window_has_event(w)) {
                break;
            }
        }
#endif
        double left = deadline - cee_window_now();
        if (left <= 0.0 || cee_wake_pending(w)) {
            break;
        }
        if (w->scripted) {
            cee_wake_signal_wait(w, left);
        }
#if !defined(CEE_WINDOW_NO_NATIVE)
        else {
            cee_native_wait(w, left);
        }
#endif
    }
    cee_wake_take(w);
}

/* The id ceesafe_window_wake knows the window by. */
uint64_t ceesafe_window_waker(void *window) { return ((struct cee_window *)window)->wake_id; }

/* From any thread: sets the woken flag and the signal of the open window `id` names, once until a wait takes it; a
   window that was closed is not in the list, and nothing happens. */
void ceesafe_window_wake(uint64_t id) {
    int native = 0;
    cee_wake_acquire();
    for (struct cee_window *w = cee_wake_windows; w != NULL; w = w->wake_link) {
        if (w->wake_id == id) {
            if (!w->woken) {
                w->woken = 1;
#if defined(_WIN32)
                SetEvent((HANDLE)w->wake_event);
#else
                ssize_t written = write(w->wake_write, "", 1); /* the pipe is empty: there is room for the byte */
                (void)written;
#endif
                native = !w->scripted;
            }
            break;
        }
    }
    cee_wake_release();
#if !defined(CEE_WINDOW_NO_NATIVE)
    if (native) {
        cee_native_wake();
    }
#else
    (void)native;
#endif
}

/* ---- Text input and the clipboard (ADR 0141) ------------------------------------------------------------------- */

void ceesafe_window_set_text_input(void *window, int32_t on) {
    struct cee_window *w = (struct cee_window *)window;
    int value = on != 0;
    if (w->text_input == value) {
        return;
    }
    if (!value) {
        cee_window_compose(w, "", 0, 0); /* off ends a composition: COMPOSITION, before the backend discards it */
    }
    w->text_input = value;
#if !defined(CEE_WINDOW_NO_NATIVE)
    if (!w->scripted) {
        cee_native_text_input(w, value);
    }
#endif
}

void ceesafe_window_set_text_area(void *window, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    struct cee_window *w = (struct cee_window *)window;
    struct cee_window_box b = {x, y, width, height};
    w->text_area = b;
#if !defined(CEE_WINDOW_NO_NATIVE)
    if (!w->scripted) {
        cee_native_text_area(w);
    }
#endif
}

/* The program's copy of the composition: its length, the text at *text (valid until the next poll) and the caret. */
uint64_t ceesafe_window_composition(void *window, const uint8_t **text, uint64_t *caret) {
    struct cee_window *w = (struct cee_window *)window;
    *text = (const uint8_t *)w->composition;
    *caret = w->composition_length == 0 ? 0u : (uint64_t)w->composition_caret;
    return w->composition_length == 0 ? 0u : (uint64_t)w->composition_length;
}

/* Puts `length` bytes of UTF-8 on the clipboard: 0, or minus a WindowError's number. */
int32_t ceesafe_window_set_clipboard(void *window, const uint8_t *text, uint64_t length) {
    struct cee_window *w = (struct cee_window *)window;
    if (length > 0x7fffffffu) {
        return -CEE_WINDOW_RESOURCES;
    }
    if (w->scripted) {
        char *copy = (char *)ceesafe_platform_alloc((size_t)length + 1u);
        if (copy == NULL) {
            return -CEE_WINDOW_RESOURCES;
        }
        if (length > 0) {
            memcpy(copy, text, (size_t)length);
        }
        ceesafe_platform_free(w->clipboard);
        w->clipboard = copy;
        w->clipboard_length = (size_t)length;
        w->scripted_clipboard = 1;
        return 0;
    }
#if !defined(CEE_WINDOW_NO_NATIVE)
    return cee_native_clipboard_set(w, (const char *)text, (size_t)length);
#else
    return -CEE_WINDOW_UNSUPPORTED;
#endif
}

/* The clipboard's text: its length, the bytes at *text (valid until the next call), or -1 when it holds no text. */
int64_t ceesafe_window_clipboard(void *window, const uint8_t **text) {
    struct cee_window *w = (struct cee_window *)window;
    int found = 0;
    *text = NULL;
    if (w->scripted) {
        found = w->scripted_clipboard && cee_window_paste(w, w->clipboard, w->clipboard_length);
    }
#if !defined(CEE_WINDOW_NO_NATIVE)
    else {
        found = cee_native_clipboard_get(w);
    }
#endif
    if (!found) {
        return -1;
    }
    *text = (const uint8_t *)w->pasted;
    return (int64_t)w->pasted_length;
}
