/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * Native windows (type-system.md §26.13, ADR 0069): one resizable window that shows premultiplied RGBA8 frames and
 * reports the pointer, keys, typed text, the wheel, resizes and close requests. Linked only into programs that open a
 * window (c-backend.md §6).
 *
 * Backends: AppKit on macOS, through the Objective-C runtime loaded with dlopen at the first open, so no program links
 * a framework; Win32 and GDI on Windows; X11 on Linux and FreeBSD, libX11.so.6 loaded with dlopen. CEESAFE_WINDOW_SCRIPT=<file>
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
 *
 * Waiting for any window (ADR 0145): ceesafe_window_wait_any sleeps until any open window has an event or was woken,
 * pumping every window. The process has one more signal, which every wake sets too, so that wait has one handle to
 * wait on however many windows are open. On macOS the pump queues each event in the window it is for, whichever
 * window's pump took it from AppKit's one queue.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* dlopen, syscall(SYS_gettid), select, clock_gettime under glibc's strict C11 headers */
#endif
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* fopen, getenv */
#endif
#include "ceesafe/rt.h"
#if defined(__APPLE__)
#include <TargetConditionals.h> /* TARGET_OS_IPHONE: UIKit rather than AppKit (ADR 0154) */
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
#define CEE_WINDOW_UIKIT 1
#endif

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
/* A scripted menu choice (`menu:ID`, ADR 0146): never queued as an event, it queues the choice. */
#define CEE_SCRIPT_MENU 102
/* A scripted dialog answer (`dialog[:PATHS]`, ADR 0146): never queued as an event, it is the next dialog's answer. */
#define CEE_SCRIPT_DIALOG 103
/* A scripted drag of files (`dragover:X,Y:N`, or `dragleave` with N 0) and drop (`drop:X,Y:PATHS`) (ADR 0150): never
   queued as events, they change the window's drag and queue the drop. */
#define CEE_SCRIPT_DRAG 104
#define CEE_SCRIPT_DROP 105
/* A scripted touch (`touch:ID,PHASE,X,Y`, ADR 0155): never queued as an event, it is a finger on the screen. */
#define CEE_SCRIPT_TOUCH 106
/* A scripted safe area (`safe:LEFT,TOP,RIGHT,BOTTOM`, ADR 0156): never queued, it updates the window's insets (its
   fields are thousandths of a point) and queues RESIZED. */
#define CEE_SCRIPT_SAFE 107

/* Touches (ADR 0155): WindowTouchPhase's numbers, the touches a window keeps that the program has not taken, the
   fingers it follows at once, and a scripted touch's time: its frame's, at 60 frames a second. */
#define CEE_TOUCH_BEGAN 0
#define CEE_TOUCH_MOVED 1
#define CEE_TOUCH_ENDED 2
#define CEE_TOUCH_CANCELLED 3
#define CEE_TOUCH_QUEUE 256u
#define CEE_TOUCH_DOWN 32u
#define CEE_TOUCH_FRAME_NS 16666667u

/* Drops (ADR 0150): the drops a window keeps that the program has not taken. */
#define CEE_DROP_QUEUE 64u

/* Menus (ADR 0146): lib/std's encoding is, per entry, a header of CEE_MENU_HEADER bytes (the kind, the flags, the
   shortcut's modifier bits, 0, then the id, the shortcut's key number and the title's length as little-endian u32s)
   and the title's UTF-8 bytes. */
#define CEE_MENU_TITLE 1
#define CEE_MENU_ITEM 2
#define CEE_MENU_SEPARATOR 3
#define CEE_MENU_ENABLED 1
#define CEE_MENU_CHECKED 2
#define CEE_MENU_HEADER 16u
#define CEE_MENU_MAX_ENTRIES 4096u
#define CEE_MENU_CHOICES 64u

/* File dialogs' kinds (lib/std's FileDialog.run) and the scripted window's ring of due answers (ADR 0146). */
#define CEE_DIALOG_FILE 0
#define CEE_DIALOG_FILES 1
#define CEE_DIALOG_SAVE 2
#define CEE_DIALOG_FOLDER 3
#define CEE_DIALOG_ANSWERS 64u

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

/* A scripted dialog's answer (`dialog[:PATHS]`, ADR 0146): PATHS at script_text + offset, or a cancel. */
struct cee_window_answer {
    int32_t offset, length;
    int cancel;
};

/* Paths being collected, each followed by a zero byte (a drop's, ADR 0150). */
struct cee_window_paths {
    char *text;
    size_t length, capacity;
};

/* A touch the program has not taken (ADR 0155). */
struct cee_window_touch {
    uint32_t id;
    int32_t phase;
    float x, y;
    uint64_t time;
};

/* A finger that is down (ADR 0155): the system's key for it (a UITouch's address, an Android pointer id + 1, a script's
   ID + 1), the runtime's number for it, and where it was last. */
struct cee_window_finger {
    uintptr_t key;
    uint32_t id;
    float x, y;
};

/* A drop the program has not taken (ADR 0150): where, and its paths, which the queue owns. */
struct cee_window_drop {
    float x, y;
    char *paths;
    size_t length;
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
    /* Menus (ADR 0146): the ids of the items chosen and not yet handed out (a ring); the copy of lib/std's encoding of
       the menu last set, which a scripted choice is checked against, and its items' ids in order (Windows' command
       k + 1 is item k). */
    uint32_t choices[CEE_MENU_CHOICES];
    unsigned choice_head, choice_count;
    uint8_t *menu;
    size_t menu_length;
    uint32_t *menu_ids;
    size_t menu_items;
    /* File dialogs (ADR 0146): the last answer, each path followed by a zero byte, for lib/std to copy; the scripted
       window's answers that are due (offsets into script_text, a ring). */
    char *answer;
    size_t answer_length, answer_capacity;
    struct cee_window_answer answers[CEE_DIALOG_ANSWERS];
    unsigned answer_head, answer_count;
    /* Drops (ADR 0150): the drops not yet taken (a ring), and the paths of the one last handed out, for lib/std to
       copy; the drag of files over the window (`drag_count` 0 for none) and whether it changed since a wait last
       returned, which ends a wait as a wake does. Only the program's first thread touches them. */
    struct cee_window_drop drops[CEE_DROP_QUEUE];
    unsigned drop_head, drop_count;
    char *dropped;
    uint32_t drag_count;
    float drag_x, drag_y;
    int drag_changed;
    /* Touches (ADR 0155): whether the program turned them on; the touches not yet taken (a ring); the fingers down,
       which the touch queue follows while on and the pointer follows while off (the one finger that is the pointer,
       `pointer_finger`, with its key and last place); the last number given and the last touch's time. Only the program's first
       thread touches them. */
    int touches_on;
    struct cee_window_touch touches[CEE_TOUCH_QUEUE];
    unsigned touch_head, touch_count;
    struct cee_window_finger fingers[CEE_TOUCH_DOWN];
    unsigned finger_count;
    int pointer_finger;
    uintptr_t pointer_key;
    float pointer_x, pointer_y;
    uint32_t touch_next;
    uint64_t touch_time;
    /* The safe area (ADR 0156): how far the system covers the content from its left, top, right and bottom edges, in
       points; zero where the system covers nothing. */
    float safe[4];
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
void ceesafe_window_wait_any(uint32_t timeout_ms);
uint64_t ceesafe_window_waker(void *window);
void ceesafe_window_wake(uint64_t id);
int32_t ceesafe_window_set_menu(void *window, const uint8_t *entries, uint64_t length);
int64_t ceesafe_window_menu_choice(void *window);
int64_t ceesafe_window_dialog(void *window, int32_t kind, const uint8_t *title, uint64_t title_length,
                              const uint8_t *name, uint64_t name_length, const uint8_t *extensions,
                              uint64_t extensions_length, const uint8_t **answer);
int64_t ceesafe_window_drop(void *window, float *position, const uint8_t **paths);
uint32_t ceesafe_window_drag_over(void *window, float *position);
int32_t ceesafe_window_set_touches(void *window, int32_t on);
int32_t ceesafe_window_touch(void *window, uint32_t *id, int32_t *phase, float *position, uint64_t *time);
void ceesafe_window_safe_area(void *window, float *insets);

static void cee_touch_cancel_all(struct cee_window *w);

/* Queues an event; the queued copy, which a backend may take back (CEE_EVENT_DROPPED) before the next poll, or NULL. */
static struct cee_window_event *cee_window_push(struct cee_window *w, int32_t kind, int32_t a, int32_t b, int32_t c,
                                                int32_t d, float x, float y) {
    if (kind == CEE_EVENT_CLOSED) {
        cee_touch_cancel_all(w); /* the user asked to close: no finger stays down (ADR 0155) */
    }
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

/* A new safe area (ADR 0156), in points from the left, top, right and bottom edges: where the program may draw what
   must be seen changed, so RESIZED with the size in points, even when that did not change, as for a scale. An inset
   is never negative, and never a number that is none. */
static void cee_window_safe_area(struct cee_window *w, float left, float top, float right, float bottom) {
    float next[4] = {left, top, right, bottom};
    int changed = 0;
    for (int k = 0; k < 4; k++) {
        if (!(next[k] > 0.0f)) {
            next[k] = 0.0f;
        }
        if (next[k] != w->safe[k]) {
            changed = 1;
        }
    }
    if (!changed) {
        return;
    }
    memcpy(w->safe, next, sizeof next);
    cee_window_push(w, CEE_EVENT_RESIZED, (int32_t)w->width, (int32_t)w->height, 0, 0, 0.0f, 0.0f);
}

static void cee_window_focus(struct cee_window *w, int focused) {
    if (focused == w->focused) {
        return;
    }
    w->focused = focused;
    if (!focused) {
        cee_touch_cancel_all(w); /* the system took the screen (ADR 0155) */
    }
    cee_window_push(w, CEE_EVENT_FOCUS, focused, 0, 0, 0, 0.0f, 0.0f);
}

/* ---- Touches (ADR 0155) ------------------------------------------------------------------------------------------ */
/* A backend gives each finger on a touch screen to cee_touch_began, cee_touch_moved and cee_touch_ended, under a key
   of its own (never 0) and in points. With touches on they are queued for nextTouch; with them off, the first finger
   down is the pointer (ADR 0154) and the others are ignored. */

/* Takes the queued touch at ring offset k out, keeping the others' order. */
static void cee_touch_remove(struct cee_window *w, unsigned k) {
    for (unsigned j = k; j + 1u < w->touch_count; j++) {
        w->touches[(w->touch_head + j) % CEE_TOUCH_QUEUE] = w->touches[(w->touch_head + j + 1u) % CEE_TOUCH_QUEUE];
    }
    w->touch_count--;
}

/* Makes room in a full queue (ADR 0155 §2.2): the oldest Moved goes, or failing that the oldest finger whose Began and
   end are both queued goes whole, so no finger is left half given. Whether there is room. */
static int cee_touch_room(struct cee_window *w) {
    for (unsigned k = 0; k < w->touch_count; k++) {
        if (w->touches[(w->touch_head + k) % CEE_TOUCH_QUEUE].phase == CEE_TOUCH_MOVED) {
            cee_touch_remove(w, k);
            return 1;
        }
    }
    for (unsigned k = 0; k < w->touch_count; k++) {
        struct cee_window_touch *t = &w->touches[(w->touch_head + k) % CEE_TOUCH_QUEUE];
        if (t->phase != CEE_TOUCH_BEGAN) {
            continue;
        }
        uint32_t id = t->id;
        unsigned end = k + 1u;
        while (end < w->touch_count) {
            struct cee_window_touch *u = &w->touches[(w->touch_head + end) % CEE_TOUCH_QUEUE];
            if (u->id == id && (u->phase == CEE_TOUCH_ENDED || u->phase == CEE_TOUCH_CANCELLED)) {
                break;
            }
            end++;
        }
        if (end < w->touch_count) {
            for (unsigned j = w->touch_count; j-- > k;) {
                if (w->touches[(w->touch_head + j) % CEE_TOUCH_QUEUE].id == id) {
                    cee_touch_remove(w, j);
                }
            }
            return 1;
        }
    }
    return 0; /* not reached: at most CEE_TOUCH_DOWN fingers are down, and as many were down at the last read */
}

static void cee_touch_queue(struct cee_window *w, uint32_t id, int32_t phase, float x, float y, uint64_t time) {
    if (w->touch_count == CEE_TOUCH_QUEUE && phase == CEE_TOUCH_MOVED) {
        for (unsigned k = w->touch_count; k-- > 0;) { /* the finger's last queued touch, if a Moved, takes the place */
            struct cee_window_touch *t = &w->touches[(w->touch_head + k) % CEE_TOUCH_QUEUE];
            if (t->id == id) {
                if (t->phase == CEE_TOUCH_MOVED) {
                    t->x = x;
                    t->y = y;
                    t->time = time;
                    return;
                }
                break;
            }
        }
    }
    if (w->touch_count == CEE_TOUCH_QUEUE && !cee_touch_room(w)) {
        return;
    }
    struct cee_window_touch *t = &w->touches[(w->touch_head + w->touch_count) % CEE_TOUCH_QUEUE];
    t->id = id;
    t->phase = phase;
    t->x = x;
    t->y = y;
    t->time = time;
    w->touch_count++;
}

static struct cee_window_finger *cee_touch_finger(struct cee_window *w, uintptr_t key) {
    for (unsigned k = 0; k < w->finger_count; k++) {
        if (w->fingers[k].key == key) {
            return &w->fingers[k];
        }
    }
    return NULL;
}

/* A finger's end: Ended or Cancelled queued, and the finger forgotten. */
static void cee_touch_finish(struct cee_window *w, struct cee_window_finger *f, int32_t phase, uint64_t time) {
    cee_touch_queue(w, f->id, phase, f->x, f->y, time);
    *f = w->fingers[--w->finger_count];
}

static void cee_touch_began(struct cee_window *w, uintptr_t key, float x, float y, uint64_t time) {
    w->touch_time = time;
    if (!w->touches_on) {
        if (!w->pointer_finger) {
            w->pointer_finger = 1;
            w->pointer_key = key;
            w->pointer_x = x;
            w->pointer_y = y;
            cee_window_push(w, CEE_EVENT_MOVED, 0, 0, 0, 0, x, y);
            cee_window_push(w, CEE_EVENT_BUTTON, 0, 1, 0, 0, x, y);
        }
        return;
    }
    struct cee_window_finger *f = cee_touch_finger(w, key);
    if (f != NULL) {
        cee_touch_finish(w, f, CEE_TOUCH_CANCELLED, time); /* the system began the key again without ending it */
    }
    if (w->finger_count == CEE_TOUCH_DOWN) {
        return; /* more fingers than any screen has: the newest is not followed */
    }
    if (++w->touch_next == 0) {
        w->touch_next = 1; /* after 2^32 fingers in one window */
    }
    f = &w->fingers[w->finger_count++];
    f->key = key;
    f->id = w->touch_next;
    f->x = x;
    f->y = y;
    cee_touch_queue(w, f->id, CEE_TOUCH_BEGAN, x, y, time);
}

static void cee_touch_moved(struct cee_window *w, uintptr_t key, float x, float y, uint64_t time) {
    w->touch_time = time;
    if (!w->touches_on) {
        if (w->pointer_finger && w->pointer_key == key) {
            w->pointer_x = x;
            w->pointer_y = y;
            cee_window_push(w, CEE_EVENT_MOVED, 0, 0, 0, 0, x, y);
        }
        return;
    }
    struct cee_window_finger *f = cee_touch_finger(w, key);
    if (f == NULL || (f->x == x && f->y == y)) {
        return;
    }
    f->x = x;
    f->y = y;
    cee_touch_queue(w, f->id, CEE_TOUCH_MOVED, x, y, time);
}

/* A finger lifted (`cancelled` 0) at (x, y), or taken away by the system. */
static void cee_touch_ended(struct cee_window *w, uintptr_t key, float x, float y, uint64_t time, int cancelled) {
    w->touch_time = time;
    if (!w->touches_on) {
        if (w->pointer_finger && w->pointer_key == key) {
            w->pointer_finger = 0;
            cee_window_push(w, CEE_EVENT_BUTTON, 0, 0, 0, 0, x, y);
        }
        return;
    }
    struct cee_window_finger *f = cee_touch_finger(w, key);
    if (f == NULL) {
        return;
    }
    if (!cancelled && (f->x != x || f->y != y)) { /* a last move reported with the lift: a Moved, then the end */
        f->x = x;
        f->y = y;
        cee_touch_queue(w, f->id, CEE_TOUCH_MOVED, x, y, time);
    }
    cee_touch_finish(w, f, cancelled ? CEE_TOUCH_CANCELLED : CEE_TOUCH_ENDED, time);
}

/* Every finger down is Cancelled, at its last place and the last touch's time. */
static void cee_touch_cancel_all(struct cee_window *w) {
    while (w->finger_count > 0) {
        cee_touch_finish(w, &w->fingers[w->finger_count - 1u], CEE_TOUCH_CANCELLED, w->touch_time);
    }
}

#if !defined(CEE_WINDOW_UIKIT) && !defined(__ANDROID__) /* mobile text comes with the input method (P4 step 5) */
/* A typed character: control characters and the private-use function-key range are keys, not text. */
static void cee_window_text(struct cee_window *w, uint32_t scalar) {
    if (scalar < 0x20u || scalar == 0x7fu || (scalar >= 0xf700u && scalar <= 0xf8ffu) || scalar > 0x10ffffu ||
        (scalar >= 0xd800u && scalar <= 0xdfffu)) {
        return;
    }
    cee_window_push(w, CEE_EVENT_TEXT, (int32_t)scalar, 0, 0, 0, 0.0f, 0.0f);
}
#endif

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

/* ---- Menus and file dialogs: what every backend shares (ADR 0146) ------------------------------------------------ */

/* Queues a chosen item's id; a program that never takes its choices loses the newest, as with events. */
static void cee_window_choose(struct cee_window *w, uint32_t id) {
    if (w->choice_count == CEE_MENU_CHOICES) {
        return;
    }
    w->choices[(w->choice_head + w->choice_count) % CEE_MENU_CHOICES] = id;
    w->choice_count++;
}

static uint32_t cee_menu_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* One entry of lib/std's encoding, read from bytes[*at..length) and moved past; 0 at the end or when malformed. */
struct cee_menu_entry {
    int kind, flags;
    int32_t modifiers;
    uint32_t id, key;
    const char *title;
    size_t title_length;
};

static int cee_menu_next(const uint8_t *bytes, size_t length, size_t *at, struct cee_menu_entry *e) {
    if (*at >= length || length - *at < CEE_MENU_HEADER) {
        return 0;
    }
    const uint8_t *p = bytes + *at;
    size_t title = cee_menu_u32(p + 12);
    if (title > length - *at - CEE_MENU_HEADER) {
        return 0;
    }
    e->kind = p[0];
    e->flags = p[1];
    e->modifiers = p[2] & 15;
    e->id = cee_menu_u32(p + 4);
    e->key = cee_menu_u32(p + 8);
    e->title = (const char *)(p + CEE_MENU_HEADER);
    e->title_length = title;
    *at += CEE_MENU_HEADER + title;
    return 1;
}

/* Checks lib/std's encoding: every entry whole and of a known kind, a menu's title before any item or separator, at
   most CEE_MENU_MAX_ENTRIES entries. 0 with the number of items, or CEE_WINDOW_UNSUPPORTED. */
static int cee_menu_check(const uint8_t *bytes, size_t length, size_t *items) {
    size_t at = 0, entries = 0;
    int menus = 0;
    struct cee_menu_entry e;
    *items = 0;
    while (cee_menu_next(bytes, length, &at, &e)) {
        if (++entries > CEE_MENU_MAX_ENTRIES || e.kind < CEE_MENU_TITLE || e.kind > CEE_MENU_SEPARATOR ||
            (e.kind != CEE_MENU_TITLE && menus == 0)) {
            return CEE_WINDOW_UNSUPPORTED;
        }
        menus += e.kind == CEE_MENU_TITLE;
        *items += e.kind == CEE_MENU_ITEM;
    }
    return at == length ? 0 : CEE_WINDOW_UNSUPPORTED;
}

/* Whether the window's menu has an enabled item `id` (a scripted choice is the user's choice of one). */
static int cee_menu_enabled(struct cee_window *w, uint32_t id) {
    size_t at = 0;
    struct cee_menu_entry e;
    while (cee_menu_next(w->menu, w->menu_length, &at, &e)) {
        if (e.kind == CEE_MENU_ITEM && e.id == id && (e.flags & CEE_MENU_ENABLED)) {
            return 1;
        }
    }
    return 0;
}

/* Adds one path, and its zero byte, to the window's answer; 0 when memory ran out. */
static int cee_window_answer_add(struct cee_window *w, const char *path, size_t length) {
    if (length == 0) {
        return 1; /* no path is empty: a dialog never answers one */
    }
    if (!cee_window_reserve(&w->answer, &w->answer_capacity, w->answer_length + length + 1u)) {
        return 0;
    }
    memcpy(w->answer + w->answer_length, path, length);
    w->answer[w->answer_length + length] = '\0';
    w->answer_length += length + 1u;
    return 1;
}

/* Adds the paths of `text`, separated by `separator` (a scripted answer's '|', a dialog program's lines); 0 when
   memory ran out. With `first`, only the first path. */
static int cee_window_answer_split(struct cee_window *w, const char *text, size_t length, char separator, int first) {
    size_t start = 0;
    for (size_t k = 0; k <= length; k++) {
        if (k == length || text[k] == separator) {
            if (k > start) {
                if (!cee_window_answer_add(w, text + start, k - start)) {
                    return 0;
                }
                if (first) {
                    return 1;
                }
            }
            start = k + 1;
        }
    }
    return 1;
}

#if (defined(__APPLE__) && !defined(CEE_WINDOW_UIKIT)) || defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__)) || \
    defined(__FreeBSD__)
/* Calls f(extension, length, context) for each extension of a comma-separated list, trimmed, without a leading dot,
   skipping empty ones; the number of them (the backends' dialogs). */
static size_t cee_dialog_extensions(const char *list, size_t length,
                                    void (*f)(const char *extension, size_t length, void *context), void *context) {
    size_t count = 0, start = 0;
    for (size_t k = 0; k <= length; k++) {
        if (k == length || list[k] == ',') {
            size_t a = start, b = k;
            while (a < b && (list[a] == ' ' || list[a] == '\t' || list[a] == '.')) {
                a++;
            }
            while (b > a && (list[b - 1] == ' ' || list[b - 1] == '\t')) {
                b--;
            }
            if (b > a) {
                if (f != NULL) {
                    f(list + a, b - a, context);
                }
                count++;
            }
            start = k + 1;
        }
    }
    return count;
}
#endif

/* ---- Drops: what every backend shares (ADR 0150) ---------------------------------------------------------------- */

/* Adds one path, and its zero byte, to `p`; 0 when memory ran out. An empty path, or one holding a zero byte, which no
   file system path does, is left out. */
static int cee_paths_add(struct cee_window_paths *p, const char *path, size_t length) {
    if (length == 0 || memchr(path, '\0', length) != NULL) {
        return 1;
    }
    if (!cee_window_reserve(&p->text, &p->capacity, p->length + length + 1u)) {
        return 0;
    }
    memcpy(p->text + p->length, path, length);
    p->text[p->length + length] = '\0';
    p->length += length + 1u;
    return 1;
}

/* The drag of files over the window is now `count` files at (x, y), or none for 0; a change ends a wait. Whether it
   changed. */
static int cee_window_drag(struct cee_window *w, uint32_t count, float x, float y) {
    if (count == 0) {
        x = 0.0f;
        y = 0.0f;
    }
    if (count == w->drag_count && x == w->drag_x && y == w->drag_y) {
        return 0;
    }
    w->drag_count = count;
    w->drag_x = x;
    w->drag_y = y;
    w->drag_changed = 1;
    return 1;
}

/* Files dropped at (x, y): the drag ends, and the drop is queued with the paths, which it takes from `p`. With no path,
   nothing is queued; with CEE_DROP_QUEUE drops not taken, the newest is lost, as with events. */
static void cee_window_dropped(struct cee_window *w, float x, float y, struct cee_window_paths *p) {
    cee_window_drag(w, 0, 0.0f, 0.0f);
    if (p->length == 0 || w->drop_count == CEE_DROP_QUEUE) {
        ceesafe_platform_free(p->text);
    } else {
        struct cee_window_drop *d = &w->drops[(w->drop_head + w->drop_count) % CEE_DROP_QUEUE];
        d->x = x;
        d->y = y;
        d->paths = p->text;
        d->length = p->length;
        w->drop_count++;
    }
    p->text = NULL;
    p->length = 0;
    p->capacity = 0;
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

#if (defined(__APPLE__) && !defined(CEE_WINDOW_UIKIT)) || defined(_WIN32)
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
#elif defined(__ANDROID__)
#include <sys/syscall.h>
#include <unistd.h>
/* Android (ADR 0154 §2.1): the program's first thread is the one the runtime's activity glue starts main on. */
static pid_t cee_android_program_tid;
static int cee_window_on_main_thread(void) {
    return cee_android_program_tid != 0 && (pid_t)syscall(SYS_gettid) == cee_android_program_tid;
}
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
static int cee_window_on_main_thread(void) { return (pid_t)syscall(SYS_gettid) == getpid(); }
#elif defined(__FreeBSD__)
#include <pthread.h>
#include <pthread_np.h>
static int cee_window_on_main_thread(void) { return pthread_main_np() == 1; }
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

/* The process's signal (ADR 0145): every wake sets it with the window's, under the lock, and ceesafe_window_wait_any
   waits on it. Made at the first open, on the main thread, and kept until the process exits. */
static int cee_wake_any_made;
#if defined(_WIN32)
static HANDLE cee_wake_any_event;
#else
static int cee_wake_any_read = -1, cee_wake_any_write = -1;
#endif

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

#if !defined(_WIN32)
/* A pipe whose two ends are non-blocking and close-on-exec; 0 if the system refused. */
static int cee_wake_pipe(int ends[2]) {
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
    return 1;
}
#endif

/* Makes the process's signal if it is not made yet (on the main thread, at an open); 0 if the system refused. */
static int cee_wake_any_open(void) {
    if (cee_wake_any_made) {
        return 1;
    }
#if defined(_WIN32)
    cee_wake_any_event = CreateEventW(NULL, TRUE, FALSE, NULL); /* manual reset, not signalled */
    if (cee_wake_any_event == NULL) {
        return 0;
    }
#else
    int ends[2];
    if (!cee_wake_pipe(ends)) {
        return 0;
    }
    cee_wake_any_read = ends[0];
    cee_wake_any_write = ends[1];
#endif
    cee_wake_any_made = 1;
    return 1;
}

/* Makes the window's signal, and the process's at the first open; 0 if the system refused. */
static int cee_wake_signal_open(struct cee_window *w) {
    w->wake_read = -1;
    w->wake_write = -1;
    if (!cee_wake_any_open()) {
        return 0;
    }
#if defined(_WIN32)
    w->wake_event = (void *)CreateEventW(NULL, TRUE, FALSE, NULL); /* manual reset, not signalled */
    return w->wake_event != NULL;
#else
    int ends[2];
    if (!cee_wake_pipe(ends)) {
        return 0;
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

/* Under the lock: clears the window's flag and its signal together, so they agree. */
static void cee_wake_clear(struct cee_window *w) {
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
}

/* Under the lock: clears the process's signal (ADR 0145). */
static void cee_wake_any_clear(void) {
#if defined(_WIN32)
    ResetEvent(cee_wake_any_event);
#else
    char drain[64];
    while (read(cee_wake_any_read, drain, sizeof drain) > 0) {
    }
#endif
}

/* Takes the wake when a wait returns, and a change of the drag of files over the window, which ends a wait as a wake
   does (ADR 0150). The process's signal stays set: waitAny clears it when no window is woken. */
static void cee_wake_take(struct cee_window *w) {
    w->drag_changed = 0;
    cee_wake_acquire();
    cee_wake_clear(w);
    cee_wake_release();
}

/* Whether any open window was woken since a wait on it last returned (ADR 0145). */
static int cee_wake_any_pending(void) {
    int woken = 0;
    cee_wake_acquire();
    for (struct cee_window *w = cee_wake_windows; w != NULL && !woken; w = w->wake_link) {
        woken = w->woken;
    }
    cee_wake_release();
    return woken;
}

/* Before waitAny waits: 0 if a window was woken, else 1 with the process's signal cleared, under the lock, so a wake
   after this sets it again and ends the wait. */
static int cee_wake_any_arm(void) {
    int woken = 0;
    cee_wake_acquire();
    for (struct cee_window *w = cee_wake_windows; w != NULL && !woken; w = w->wake_link) {
        woken = w->woken;
    }
    if (!woken) {
        cee_wake_any_clear();
    }
    cee_wake_release();
    return !woken;
}

/* Takes every open window's wake and drag change, and clears the process's signal, when waitAny returns. */
static void cee_wake_take_all(void) {
    cee_wake_acquire();
    for (struct cee_window *w = cee_wake_windows; w != NULL; w = w->wake_link) {
        cee_wake_clear(w);
        w->drag_changed = 0;
    }
    cee_wake_any_clear();
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

/* Waits up to `seconds` for the process's signal alone (waitAny with only scripted windows, ADR 0145). */
static void cee_wake_any_wait(double seconds) {
    double ms = seconds * 1000.0 + 0.999;
    if (ms <= 0.0) {
        return;
    }
#if defined(_WIN32)
    WaitForSingleObject(cee_wake_any_event, ms >= 4294967294.0 ? 4294967294u : (DWORD)ms);
#else
    struct pollfd p = {cee_wake_any_read, POLLIN, 0};
    poll(&p, 1, ms >= 2147483647.0 ? 2147483647 : (int)ms);
#endif
}

/* Sleeps until the monotonic clock reaches `deadline` (waitAny with no window, ADR 0145). */
static void cee_window_sleep_until(double deadline) {
    for (;;) {
        double left = deadline - cee_window_now();
        if (left <= 0.0) {
            return;
        }
#if defined(_WIN32)
        double ms = left * 1000.0 + 0.999;
        Sleep(ms >= 4294967294.0 ? 4294967294u : (DWORD)ms);
#else
        struct timespec t;
        t.tv_sec = (time_t)left;
        t.tv_nsec = (long)((left - (double)t.tv_sec) * 1e9);
        nanosleep(&t, NULL);
#endif
    }
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
    if (CEE_IS("safe")) { /* safe:LEFT,TOP,RIGHT,BOTTOM, in points (ADR 0156) */
        double insets[5];
        if (cee_window_numbers(args, args_length, insets, 5) != 4) {
            return 0;
        }
        for (int k = 0; k < 4; k++) {
            if (!(insets[k] >= 0.0) || insets[k] > CEE_WINDOW_MAX_SIDE) {
                return 0;
            }
        }
        return cee_window_script_add(w, capacity, frame, CEE_SCRIPT_SAFE, (int32_t)(insets[0] * 1000.0 + 0.5),
                                     (int32_t)(insets[1] * 1000.0 + 0.5), (int32_t)(insets[2] * 1000.0 + 0.5),
                                     (int32_t)(insets[3] * 1000.0 + 0.5), 0.0, 0.0);
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
    if (CEE_IS("menu")) { /* menu:ID chooses the menu item ID (ADR 0146) */
        uint64_t id = 0;
        for (size_t k = 0; k < args_length; k++) {
            if (args[k] < '0' || args[k] > '9' || id > 0xffffffffu / 10u) {
                return 0;
            }
            id = id * 10u + (uint64_t)(args[k] - '0');
        }
        if (args_length == 0 || id > 0xffffffffu) {
            return 0;
        }
        return cee_window_script_add(w, capacity, frame, CEE_SCRIPT_MENU, (int32_t)(uint32_t)id, 0, 0, 0, 0.0, 0.0);
    }
    if (CEE_IS("dialog")) { /* dialog:PATH[|PATH…] answers the next dialog, dialog (no colon) cancels it (ADR 0146) */
        int has = name_length < rest;
        if (has && args_length == 0) {
            return 0;
        }
        int32_t offset = has ? cee_window_script_text(w, args, args_length) : 0;
        return offset >= 0 && cee_window_script_add(w, capacity, frame, CEE_SCRIPT_DIALOG, offset,
                                                    (int32_t)args_length, 0, has, 0.0, 0.0);
    }
    if (CEE_IS("dragover") || CEE_IS("drop")) { /* dragover:X,Y:N and drop:X,Y:PATH[|PATH…] (ADR 0150) */
        size_t at = 0;
        while (at < args_length && args[at] != ':') {
            at++;
        }
        if (at == args_length || at + 1 == args_length || cee_window_numbers(args, at, v, 3) != 2) {
            return 0;
        }
        const char *tail = args + at + 1;
        size_t tail_length = args_length - at - 1;
        if (CEE_IS("dragover")) {
            uint64_t count = 0;
            for (size_t k = 0; k < tail_length; k++) {
                if (tail[k] < '0' || tail[k] > '9' || count > 100000000u) {
                    return 0;
                }
                count = count * 10u + (uint64_t)(tail[k] - '0');
            }
            return count > 0 && cee_window_script_add(w, capacity, frame, CEE_SCRIPT_DRAG, (int32_t)count, 0, 0, 0,
                                                      v[0], v[1]);
        }
        int32_t offset = cee_window_script_text(w, tail, tail_length);
        return offset >= 0 && cee_window_script_add(w, capacity, frame, CEE_SCRIPT_DROP, offset,
                                                    (int32_t)tail_length, 0, 0, v[0], v[1]);
    }
    if (CEE_IS("touch")) { /* touch:ID,PHASE,X,Y (ADR 0155) */
        static const char *const phases[4] = {"began", "moved", "ended", "cancelled"};
        uint64_t id = 0;
        size_t at = 0;
        while (at < args_length && args[at] >= '0' && args[at] <= '9' && id <= 100000000u) {
            id = id * 10u + (uint64_t)(args[at++] - '0');
        }
        if (at == 0 || at == args_length || args[at] != ',') {
            return 0;
        }
        size_t start = ++at;
        while (at < args_length && args[at] != ',') {
            at++;
        }
        int32_t phase = -1;
        for (int32_t k = 0; k < 4; k++) {
            if (at - start == strlen(phases[k]) && memcmp(args + start, phases[k], at - start) == 0) {
                phase = k;
            }
        }
        if (phase < 0 || at == args_length || cee_window_numbers(args + at + 1, args_length - at - 1, v, 3) != 2) {
            return 0;
        }
        return cee_window_script_add(w, capacity, frame, CEE_SCRIPT_TOUCH, (int32_t)id, phase, 0, 0, v[0], v[1]);
    }
    if (CEE_IS("dragleave")) {
        return name_length == rest && cee_window_script_add(w, capacity, frame, CEE_SCRIPT_DRAG, 0, 0, 0, 0, 0.0, 0.0);
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
        } else if (e.kind == CEE_SCRIPT_SAFE) {
            cee_window_safe_area(w, (float)e.fields[0] / 1000.0f, (float)e.fields[1] / 1000.0f,
                                 (float)e.fields[2] / 1000.0f, (float)e.fields[3] / 1000.0f);
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
        } else if (e.kind == CEE_SCRIPT_MENU) {
            cee_window_choose(w, (uint32_t)e.fields[0]);
        } else if (e.kind == CEE_SCRIPT_DIALOG) {
            if (w->answer_count < CEE_DIALOG_ANSWERS) {
                struct cee_window_answer *a = &w->answers[(w->answer_head + w->answer_count) % CEE_DIALOG_ANSWERS];
                a->offset = e.fields[0];
                a->length = e.fields[1];
                a->cancel = !e.fields[3];
                w->answer_count++;
            }
        } else if (e.kind == CEE_SCRIPT_DRAG) {
            cee_window_drag(w, (uint32_t)e.fields[0], e.x, e.y);
        } else if (e.kind == CEE_SCRIPT_DROP) {
            struct cee_window_paths p = {NULL, 0, 0};
            const char *text = w->script_text + e.fields[0];
            size_t length = (size_t)e.fields[1], start = 0;
            int ok = 1;
            for (size_t k = 0; k <= length && ok; k++) {
                if (k == length || text[k] == '|') {
                    ok = cee_paths_add(&p, text + start, k - start);
                    start = k + 1;
                }
            }
            cee_window_dropped(w, e.x, e.y, &p); /* out of memory: the paths kept so far */
        } else if (e.kind == CEE_SCRIPT_TOUCH) {
            uintptr_t key = (uintptr_t)e.fields[0] + 1u;
            uint64_t time = w->frames * (uint64_t)CEE_TOUCH_FRAME_NS;
            if (e.fields[1] == CEE_TOUCH_BEGAN) {
                cee_touch_began(w, key, e.x, e.y, time);
            } else if (e.fields[1] == CEE_TOUCH_MOVED) {
                cee_touch_moved(w, key, e.x, e.y, time);
            } else {
                cee_touch_ended(w, key, e.x, e.y, time, e.fields[1] == CEE_TOUCH_CANCELLED);
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
   a CEE_WINDOW_* error) and cee_native_clipboard_get (1 with the text kept by cee_window_paste, 0 without text),
   cee_native_menu (shows w->menu: 0 or minus an error) and cee_native_dialog (runs a file dialog and adds its paths
   with cee_window_answer_add: 0, none added for a cancel, or minus an error) (ADR 0146). With `w->external` set
   before cee_native_open, the backend never paints the content: cee_native_present is not called. */

#if defined(CEE_WINDOW_UIKIT)
/* UIKit (ADR 0154) through the Objective-C runtime, loaded with dlopen as macOS's AppKit is, so no program links a
   framework. The program runs inside UIApplicationMain (ceesafe_window_app_start, called by a windowed build's start):
   on the main thread, from the run loop, which runs only while the program polls or waits (CFRunLoopRunInMode). One
   window, which fills the screen; the first touch is the primary pointer (until P4 step 3). On x86_64 (the simulator
   on Intel Macs) a method that returns a CGRect goes through objc_msgSend_stret. */
#include <crt_externs.h>
#include <dlfcn.h>

#define CEE_HANDLE_UIKIT 4

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
} cee_ios_source_context;

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
    int (*application_main)(int, char **, cee_id, cee_id);
    void *(*color_space_named)(const void *);
    const void *const *srgb_name;
    void *(*provider_create)(void *, const void *, size_t, void (*)(void *, const void *, size_t));
    void *(*image_create)(size_t, size_t, size_t, size_t, size_t, void *, uint32_t, void *, const double *, bool,
                          int32_t);
    void (*image_release)(void *);
    void (*provider_release)(void *);
    void *rgb;
    int32_t (*run_in_mode)(const void *, double, unsigned char);
    const void *const *default_mode;
    void (*source_signal)(void *);
    void (*loop_wake_up)(void *);
    void *wake_source;
    void *main_loop;
    cee_id *gravity_resize;
    cee_id view_class;
} cee_uikit;

/* The one window and its view. */
struct cee_ios {
    cee_id window;
    cee_id view;
};
static struct cee_window *cee_ios_shown;
/* The application's window scene (iOS 13 onward; required from iOS 27, whose UIKit stops an app without one): the
   scene delegate records it when it connects, and the window is made in it. */
static cee_id cee_ios_scene;

static cee_fn cee_ios_symbol(void *handle, const char *name) {
    void *p = dlsym(handle, name);
    cee_fn f;
    memcpy(&f, &p, sizeof f); /* ISO C has no object-to-function pointer conversion; POSIX guarantees this one */
    return f;
}

static cee_id cee_ios_cls(const char *name) { return cee_uikit.get_class(name); }
static cee_sel cee_ios_sel(const char *name) { return cee_uikit.sel(name); }
static cee_id cee_ios_send(cee_id o, const char *s) { return ((cee_id(*)(cee_id, cee_sel))cee_uikit.send)(o, cee_ios_sel(s)); }
static void cee_ios_send_v(cee_id o, const char *s) { ((void (*)(cee_id, cee_sel))cee_uikit.send)(o, cee_ios_sel(s)); }
static void cee_ios_send_v_id(cee_id o, const char *s, cee_id a) {
    ((void (*)(cee_id, cee_sel, cee_id))cee_uikit.send)(o, cee_ios_sel(s), a);
}
static long cee_ios_send_long(cee_id o, const char *s) { return ((long (*)(cee_id, cee_sel))cee_uikit.send)(o, cee_ios_sel(s)); }
static unsigned long cee_ios_send_ulong(cee_id o, const char *s) {
    return ((unsigned long (*)(cee_id, cee_sel))cee_uikit.send)(o, cee_ios_sel(s));
}
static double cee_ios_send_double(cee_id o, const char *s) {
    return ((double (*)(cee_id, cee_sel))cee_uikit.send)(o, cee_ios_sel(s));
}
static const char *cee_ios_send_cstr(cee_id o, const char *s) {
    return ((const char *(*)(cee_id, cee_sel))cee_uikit.send)(o, cee_ios_sel(s));
}
static cee_rect cee_ios_send_rect(cee_id o, const char *s) {
#if defined(__x86_64__)
    cee_rect r;
    ((void (*)(cee_rect *, cee_id, cee_sel))cee_uikit.send_stret)(&r, o, cee_ios_sel(s));
    return r;
#else
    return ((cee_rect(*)(cee_id, cee_sel))cee_uikit.send)(o, cee_ios_sel(s));
#endif
}
/* A view's safeAreaInsets, a UIEdgeInsets: top, left, bottom, right (four doubles, returned as a CGRect is). */
typedef struct {
    double top, left, bottom, right;
} cee_insets;
static cee_insets cee_ios_send_insets(cee_id o, const char *s) {
#if defined(__x86_64__)
    cee_insets r;
    ((void (*)(cee_insets *, cee_id, cee_sel))cee_uikit.send_stret)(&r, o, cee_ios_sel(s));
    return r;
#else
    return ((cee_insets(*)(cee_id, cee_sel))cee_uikit.send)(o, cee_ios_sel(s));
#endif
}
static cee_id cee_ios_send_id_window(cee_id window, cee_id scene) {
    return ((cee_id(*)(cee_id, cee_sel, cee_id))cee_uikit.send)(window, cee_ios_sel("initWithWindowScene:"), scene);
}
static cee_point cee_ios_location(cee_id touch, cee_id view) {
    return ((cee_point(*)(cee_id, cee_sel, cee_id))cee_uikit.send)(touch, cee_ios_sel("locationInView:"), view);
}
static cee_id cee_ios_string(const char *text) {
    return ((cee_id(*)(cee_id, cee_sel, const char *))cee_uikit.send)(cee_ios_cls("NSString"),
                                                                     cee_ios_sel("stringWithUTF8String:"), text);
}

/* The view's touches, each a finger under its UITouch's address (a touch keeps its object from beginning to end), at
   its place in the view in points and its time (seconds since the system started) in nanoseconds (ADR 0154 §2.2,
   ADR 0155). Phase 0 began, 1 moved, 2 ended, 3 cancelled. */
static void cee_ios_touches(cee_id view, cee_id touches, int phase) {
    struct cee_window *w = cee_ios_shown;
    if (w == NULL) {
        return;
    }
    cee_id all = cee_ios_send(touches, "allObjects");
    unsigned long count = all != NULL ? cee_ios_send_ulong(all, "count") : 0ul;
    for (unsigned long k = 0; k < count; k++) {
        cee_id touch = ((cee_id(*)(cee_id, cee_sel, unsigned long))cee_uikit.send)(all, cee_ios_sel("objectAtIndex:"), k);
        cee_point p = cee_ios_location(touch, view);
        double seconds = cee_ios_send_double(touch, "timestamp");
        uint64_t time = seconds > 0.0 ? (uint64_t)(seconds * 1e9) : 0u;
        uintptr_t key = (uintptr_t)touch;
        if (phase == 0) {
            cee_touch_began(w, key, (float)p.x, (float)p.y, time);
        } else if (phase == 1) {
            cee_touch_moved(w, key, (float)p.x, (float)p.y, time);
        } else {
            cee_touch_ended(w, key, (float)p.x, (float)p.y, time, phase == 3);
        }
    }
}

static void cee_ios_touches_began(cee_id self, cee_sel cmd, cee_id touches, cee_id event) {
    (void)cmd;
    (void)event;
    cee_ios_touches(self, touches, 0);
}
static void cee_ios_touches_moved(cee_id self, cee_sel cmd, cee_id touches, cee_id event) {
    (void)cmd;
    (void)event;
    cee_ios_touches(self, touches, 1);
}
static void cee_ios_touches_ended(cee_id self, cee_sel cmd, cee_id touches, cee_id event) {
    (void)cmd;
    (void)event;
    cee_ios_touches(self, touches, 2);
}
static void cee_ios_touches_cancelled(cee_id self, cee_sel cmd, cee_id touches, cee_id event) {
    (void)cmd;
    (void)event;
    cee_ios_touches(self, touches, 3);
}

/* The wake source's callback: nothing to do; that a source was handled ends the run loop's wait (ADR 0142). */
static void cee_ios_wake_perform(void *info) { (void)info; }

static int cee_uikit_load(void) {
    if (cee_uikit.state != 0) {
        return cee_uikit.state == 1;
    }
    cee_uikit.state = -1;
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL);
    void *uikit = dlopen("/System/Library/Frameworks/UIKit.framework/UIKit", RTLD_NOW | RTLD_GLOBAL);
    void *graphics = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_NOW | RTLD_GLOBAL);
    void *quartz = dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW | RTLD_GLOBAL);
    void *core = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW | RTLD_GLOBAL);
    if (objc == NULL || uikit == NULL || graphics == NULL || quartz == NULL || core == NULL) {
        return 0;
    }
    cee_uikit.get_class = (cee_id(*)(const char *))cee_ios_symbol(objc, "objc_getClass");
    cee_uikit.sel = (cee_sel(*)(const char *))cee_ios_symbol(objc, "sel_registerName");
    cee_uikit.send = cee_ios_symbol(objc, "objc_msgSend");
#if defined(__x86_64__)
    cee_uikit.send_stret = cee_ios_symbol(objc, "objc_msgSend_stret");
#endif
    cee_uikit.allocate_class = (cee_id(*)(cee_id, const char *, size_t))cee_ios_symbol(objc, "objc_allocateClassPair");
    cee_uikit.add_method = (signed char (*)(cee_id, cee_sel, cee_fn, const char *))cee_ios_symbol(objc,
                                                                                                  "class_addMethod");
    cee_uikit.register_class = (void (*)(cee_id))cee_ios_symbol(objc, "objc_registerClassPair");
    cee_uikit.get_protocol = (void *(*)(const char *))cee_ios_symbol(objc, "objc_getProtocol");
    cee_uikit.add_protocol = (signed char (*)(cee_id, void *))cee_ios_symbol(objc, "class_addProtocol");
    cee_uikit.pool_push = (void *(*)(void))cee_ios_symbol(objc, "objc_autoreleasePoolPush");
    cee_uikit.pool_pop = (void (*)(void *))cee_ios_symbol(objc, "objc_autoreleasePoolPop");
    cee_uikit.application_main = (int (*)(int, char **, cee_id, cee_id))cee_ios_symbol(uikit, "UIApplicationMain");
    cee_uikit.color_space_named = (void *(*)(const void *))cee_ios_symbol(graphics, "CGColorSpaceCreateWithName");
    cee_uikit.srgb_name = (const void *const *)dlsym(graphics, "kCGColorSpaceSRGB");
    cee_uikit.provider_create = (void *(*)(void *, const void *, size_t, void (*)(void *, const void *, size_t)))
        cee_ios_symbol(graphics, "CGDataProviderCreateWithData");
    cee_uikit.image_create = (void *(*)(size_t, size_t, size_t, size_t, size_t, void *, uint32_t, void *,
                                        const double *, bool, int32_t))cee_ios_symbol(graphics, "CGImageCreate");
    cee_uikit.image_release = (void (*)(void *))cee_ios_symbol(graphics, "CGImageRelease");
    cee_uikit.provider_release = (void (*)(void *))cee_ios_symbol(graphics, "CGDataProviderRelease");
    cee_uikit.run_in_mode = (int32_t(*)(const void *, double, unsigned char))cee_ios_symbol(core, "CFRunLoopRunInMode");
    cee_uikit.default_mode = (const void *const *)dlsym(core, "kCFRunLoopDefaultMode");
    cee_uikit.source_signal = (void (*)(void *))cee_ios_symbol(core, "CFRunLoopSourceSignal");
    cee_uikit.loop_wake_up = (void (*)(void *))cee_ios_symbol(core, "CFRunLoopWakeUp");
    cee_uikit.gravity_resize = (cee_id *)dlsym(quartz, "kCAGravityResize");
    void *(*source_create)(void *, long, cee_ios_source_context *) =
        (void *(*)(void *, long, cee_ios_source_context *))cee_ios_symbol(core, "CFRunLoopSourceCreate");
    void *(*get_main)(void) = (void *(*)(void))cee_ios_symbol(core, "CFRunLoopGetMain");
    void (*add_source)(void *, void *, const void *) =
        (void (*)(void *, void *, const void *))cee_ios_symbol(core, "CFRunLoopAddSource");
    const void *const *common = (const void *const *)dlsym(core, "kCFRunLoopCommonModes");
    if (cee_uikit.get_class == NULL || cee_uikit.sel == NULL || cee_uikit.send == NULL ||
        cee_uikit.allocate_class == NULL || cee_uikit.add_method == NULL || cee_uikit.register_class == NULL ||
        cee_uikit.pool_push == NULL || cee_uikit.pool_pop == NULL || cee_uikit.application_main == NULL ||
        cee_uikit.color_space_named == NULL || cee_uikit.srgb_name == NULL || cee_uikit.provider_create == NULL ||
        cee_uikit.image_create == NULL || cee_uikit.image_release == NULL || cee_uikit.provider_release == NULL ||
        cee_uikit.run_in_mode == NULL || cee_uikit.default_mode == NULL || cee_uikit.source_signal == NULL ||
        cee_uikit.loop_wake_up == NULL || cee_uikit.gravity_resize == NULL || source_create == NULL ||
        get_main == NULL || add_source == NULL || common == NULL
#if defined(__x86_64__)
        || cee_uikit.send_stret == NULL
#endif
    ) {
        return 0;
    }
    cee_uikit.rgb = cee_uikit.color_space_named(*cee_uikit.srgb_name);
    cee_ios_source_context context; /* copied by CFRunLoopSourceCreate */
    memset(&context, 0, sizeof context);
    context.perform = cee_ios_wake_perform;
    cee_uikit.main_loop = get_main();
    cee_uikit.wake_source = cee_uikit.main_loop != NULL ? source_create(NULL, 0, &context) : NULL;
    if (cee_uikit.wake_source != NULL) {
        add_source(cee_uikit.main_loop, cee_uikit.wake_source, *common);
    }
    cee_id view = cee_uikit.allocate_class(cee_ios_cls("UIView"), "CeeSafeTouchView", 0);
    if (view == NULL) {
        view = cee_ios_cls("CeeSafeTouchView"); /* a second runtime in the process registered it */
    } else {
        cee_uikit.add_method(view, cee_ios_sel("touchesBegan:withEvent:"), (cee_fn)cee_ios_touches_began, "v@:@@");
        cee_uikit.add_method(view, cee_ios_sel("touchesMoved:withEvent:"), (cee_fn)cee_ios_touches_moved, "v@:@@");
        cee_uikit.add_method(view, cee_ios_sel("touchesEnded:withEvent:"), (cee_fn)cee_ios_touches_ended, "v@:@@");
        cee_uikit.add_method(view, cee_ios_sel("touchesCancelled:withEvent:"), (cee_fn)cee_ios_touches_cancelled, "v@:@@");
        cee_uikit.register_class(view);
    }
    cee_uikit.view_class = view;
    cee_uikit.state = cee_uikit.rgb != NULL && view != NULL ? 1 : -1;
    return cee_uikit.state == 1;
}

/* The application's start (ADR 0154 §2.1): a windowed build's ceesafe_rt_windowed calls this first. UIApplicationMain
   never returns; once the application has launched, the delegate calls the program's `main` again from the run loop,
   and that call's ceesafe_rt_windowed returns at once. When `main` returns, the process exits with its code. */
int main(int argc, char **argv);

static void cee_ios_run(cee_id self, cee_sel cmd, cee_id argument) {
    (void)self;
    (void)cmd;
    (void)argument;
    exit(main(*_NSGetArgc(), *_NSGetArgv()));
}

/* The scene life cycle (ADR 0154 §2.1): the application's scenes are configured from code, so an app's Info.plist
   needs no scene configuration, with CeeSafeSceneDelegate as their delegate. */
static cee_id cee_ios_configuration(cee_id self, cee_sel cmd, cee_id application, cee_id session, cee_id options) {
    (void)self;
    (void)cmd;
    (void)application;
    (void)options;
    cee_id configuration = ((cee_id(*)(cee_id, cee_sel, cee_id, cee_id))cee_uikit.send)(
        cee_ios_cls("UISceneConfiguration"), cee_ios_sel("configurationWithName:sessionRole:"),
        cee_ios_string("CeeSafe"), cee_ios_send(session, "role"));
    if (configuration != NULL) {
        cee_ios_send_v_id(configuration, "setDelegateClass:", cee_ios_cls("CeeSafeSceneDelegate"));
    }
    return configuration; /* autoreleased, as the method's result is not owned */
}

static void cee_ios_scene_connect(cee_id self, cee_sel cmd, cee_id scene, cee_id session, cee_id options) {
    (void)self;
    (void)cmd;
    (void)session;
    (void)options;
    if (cee_ios_scene == NULL) {
        cee_ios_scene = cee_ios_send(scene, "retain");
    }
}

static void cee_ios_scene_disconnect(cee_id self, cee_sel cmd, cee_id scene) {
    (void)self;
    (void)cmd;
    if (scene == cee_ios_scene) {
        cee_ios_scene = NULL;
        cee_ios_send_v(scene, "release");
    }
}

static signed char cee_ios_did_finish(cee_id self, cee_sel cmd, cee_id application, cee_id options) {
    (void)cmd;
    (void)application;
    (void)options;
    ((void (*)(cee_id, cee_sel, cee_sel, cee_id, double))cee_uikit.send)(
        self, cee_ios_sel("performSelector:withObject:afterDelay:"), cee_ios_sel("ceesafeRun:"), NULL, 0.0);
    return 1;
}

void ceesafe_window_app_start(void) {
    static int started;
    if (started) {
        return;
    }
    started = 1;
    if (!cee_uikit_load()) {
        return; /* no UIKit: the program runs, and Window.open is NoDisplay */
    }
    cee_id delegate = cee_uikit.allocate_class(cee_ios_cls("UIResponder"), "CeeSafeAppDelegate", 0);
    if (delegate != NULL) {
        cee_uikit.add_method(delegate, cee_ios_sel("application:didFinishLaunchingWithOptions:"),
                             (cee_fn)cee_ios_did_finish, "c@:@@");
        cee_uikit.add_method(delegate, cee_ios_sel("ceesafeRun:"), (cee_fn)cee_ios_run, "v@:@");
        cee_uikit.add_method(delegate, cee_ios_sel("application:configurationForConnectingSceneSession:options:"),
                             (cee_fn)cee_ios_configuration, "@@:@@@");
        void *app_protocol = cee_uikit.get_protocol != NULL ? cee_uikit.get_protocol("UIApplicationDelegate") : NULL;
        if (app_protocol != NULL && cee_uikit.add_protocol != NULL) {
            cee_uikit.add_protocol(delegate, app_protocol);
        }
        cee_uikit.register_class(delegate);
    }
    cee_id scene_delegate = cee_uikit.allocate_class(cee_ios_cls("UIResponder"), "CeeSafeSceneDelegate", 0);
    if (scene_delegate != NULL) {
        cee_uikit.add_method(scene_delegate, cee_ios_sel("scene:willConnectToSession:options:"),
                             (cee_fn)cee_ios_scene_connect, "v@:@@@");
        cee_uikit.add_method(scene_delegate, cee_ios_sel("sceneDidDisconnect:"), (cee_fn)cee_ios_scene_disconnect,
                             "v@:@");
        void *scene_protocol =
            cee_uikit.get_protocol != NULL ? cee_uikit.get_protocol("UIWindowSceneDelegate") : NULL;
        if (scene_protocol != NULL && cee_uikit.add_protocol != NULL) {
            cee_uikit.add_protocol(scene_delegate, scene_protocol);
        }
        cee_uikit.register_class(scene_delegate);
    }
    void *pool = cee_uikit.pool_push();
    cee_id name = cee_ios_string("CeeSafeAppDelegate");
    (void)cee_uikit.application_main(*_NSGetArgc(), *_NSGetArgv(), NULL, name);
    cee_uikit.pool_pop(pool); /* not reached */
}

/* Whether the window's scene is in the foreground and active (UISceneActivationStateForegroundActive, 0). */
static int cee_ios_active(void) {
    return cee_ios_scene != NULL && cee_ios_send_long(cee_ios_scene, "activationState") == 0;
}

static int cee_native_open(struct cee_window *w, const char *title) {
    (void)title; /* an iOS window has no title */
    if (cee_uikit.state != 1 || cee_ios_send(cee_ios_cls("UIApplication"), "sharedApplication") == NULL) {
        return CEE_WINDOW_NO_DISPLAY; /* not a windowed build, so no application (ADR 0154 §2.1) */
    }
    if (cee_ios_shown != NULL) {
        return CEE_WINDOW_RESOURCES; /* one window per program on iOS */
    }
    struct cee_ios *m = (struct cee_ios *)ceesafe_platform_alloc_zeroed(sizeof *m);
    if (m == NULL) {
        return CEE_WINDOW_RESOURCES;
    }
    /* The scene connects after the application has launched: wait for it, ten seconds at most. */
    for (int k = 0; k < 100 && cee_ios_scene == NULL; k++) {
        (void)cee_uikit.run_in_mode(*cee_uikit.default_mode, 0.1, 1);
    }
    if (cee_ios_scene == NULL) {
        ceesafe_platform_free(m);
        return CEE_WINDOW_NO_DISPLAY;
    }
    void *pool = cee_uikit.pool_push();
    cee_id screen = cee_ios_send(cee_ios_scene, "screen");
    cee_rect bounds = cee_ios_send_rect(screen, "bounds");
    cee_id window = cee_ios_send_id_window(cee_ios_send(cee_ios_cls("UIWindow"), "alloc"), cee_ios_scene);
    cee_id controller = cee_ios_send(cee_ios_send(cee_ios_cls("UIViewController"), "alloc"), "init");
    cee_id view = ((cee_id(*)(cee_id, cee_sel, cee_rect))cee_uikit.send)(cee_ios_send(cee_uikit.view_class, "alloc"),
                                                                         cee_ios_sel("initWithFrame:"), bounds);
    if (window == NULL || controller == NULL || view == NULL) {
        cee_uikit.pool_pop(pool);
        ceesafe_platform_free(m);
        return CEE_WINDOW_RESOURCES;
    }
    cee_ios_send_v_id(window, "setBackgroundColor:", cee_ios_send(cee_ios_cls("UIColor"), "blackColor"));
    ((void (*)(cee_id, cee_sel, signed char))cee_uikit.send)(view, cee_ios_sel("setMultipleTouchEnabled:"), 1);
    cee_ios_send_v_id(controller, "setView:", view);
    cee_ios_send_v(view, "release");
    cee_ios_send_v_id(window, "setRootViewController:", controller);
    cee_ios_send_v(controller, "release");
    cee_ios_send_v_id(cee_ios_send(view, "layer"), "setContentsGravity:", *cee_uikit.gravity_resize);
    cee_ios_send_v(window, "makeKeyAndVisible");
    m->window = window;
    m->view = view;
    w->native = m;
    w->width = (uint32_t)bounds.size.width;
    w->height = (uint32_t)bounds.size.height;
    w->scale = (float)cee_ios_send_double(screen, "scale");
    cee_ios_send_v(window, "layoutIfNeeded"); /* the view's safe area is known once it is laid out (ADR 0156) */
    cee_insets safe = cee_ios_send_insets(view, "safeAreaInsets");
    w->safe[0] = safe.left > 0.0 ? (float)safe.left : 0.0f;
    w->safe[1] = safe.top > 0.0 ? (float)safe.top : 0.0f;
    w->safe[2] = safe.right > 0.0 ? (float)safe.right : 0.0f;
    w->safe[3] = safe.bottom > 0.0 ? (float)safe.bottom : 0.0f;
    cee_ios_shown = w;
    cee_uikit.pool_pop(pool);
    return 0;
}

static void cee_native_close(struct cee_window *w) {
    struct cee_ios *m = (struct cee_ios *)w->native;
    if (m == NULL) {
        return;
    }
    void *pool = cee_uikit.pool_push();
    cee_ios_send_v_id(m->window, "setRootViewController:", NULL);
    ((void (*)(cee_id, cee_sel, signed char))cee_uikit.send)(m->window, cee_ios_sel("setHidden:"), 1);
    cee_ios_send_v(m->window, "release");
    cee_uikit.pool_pop(pool);
    if (cee_ios_shown == w) {
        cee_ios_shown = NULL;
    }
    ceesafe_platform_free(m);
    w->native = NULL;
}

/* Runs the run loop's pending sources (touches among them), then reads the size, the scale, the safe area and the
   application's state, which give Resized and Focus when they change. */
static void cee_native_pump(struct cee_window *w) {
    void *pool = cee_uikit.pool_push();
    for (int k = 0; k < 256 && cee_uikit.run_in_mode(*cee_uikit.default_mode, 0.0, 1) == 4; k++) {
        /* kCFRunLoopRunHandledSource: another may be pending */
    }
    struct cee_ios *m = (struct cee_ios *)w->native;
    if (m != NULL) {
        cee_rect bounds = cee_ios_send_rect(m->view, "bounds");
        if (bounds.size.width >= 1.0 && bounds.size.height >= 1.0) {
            cee_window_resized(w, (uint32_t)bounds.size.width, (uint32_t)bounds.size.height);
        }
        if (cee_ios_scene != NULL) {
            cee_window_rescaled(w, (float)cee_ios_send_double(cee_ios_send(cee_ios_scene, "screen"), "scale"));
        }
        /* The safe area (ADR 0156): what the status bar, the sensor housing, the home indicator and the screen's
           rounded corners leave of the view. */
        cee_insets safe = cee_ios_send_insets(m->view, "safeAreaInsets");
        cee_window_safe_area(w, (float)safe.left, (float)safe.top, (float)safe.right, (float)safe.bottom);
        int active = cee_ios_active();
        if (active != w->focused) {
            cee_window_focus(w, active);
        }
    }
    cee_uikit.pool_pop(pool);
}

static int32_t cee_native_handles(struct cee_window *w, uint64_t *first, uint64_t *second) {
    struct cee_ios *m = (struct cee_ios *)w->native;
    *first = (uint64_t)(uintptr_t)m->window;
    *second = (uint64_t)(uintptr_t)m->view;
    return CEE_HANDLE_UIKIT;
}

static void cee_ios_release_pixels(void *info, const void *data, size_t size) {
    (void)data;
    (void)size;
    ceesafe_platform_free(info);
}

/* The frame as a CGImage in the view's layer: the pixels are copied, premultiplied RGBA shown as opaque sRGB. */
static int32_t cee_native_present(struct cee_window *w, const uint32_t *pixels, uint32_t width, uint32_t height) {
    struct cee_ios *m = (struct cee_ios *)w->native;
    size_t bytes = (size_t)width * (size_t)height * 4u;
    uint32_t *copy = (uint32_t *)ceesafe_platform_alloc(bytes);
    if (copy == NULL) {
        return -CEE_WINDOW_RESOURCES;
    }
    memcpy(copy, pixels, bytes);
    void *pool = cee_uikit.pool_push();
    void *provider = cee_uikit.provider_create(copy, copy, bytes, cee_ios_release_pixels);
    if (provider == NULL) {
        ceesafe_platform_free(copy);
        cee_uikit.pool_pop(pool);
        return -CEE_WINDOW_RESOURCES;
    }
    /* kCGImageAlphaNoneSkipLast: bytes R, G, B and an alpha that is not shown, as the window is opaque. */
    void *image = cee_uikit.image_create(width, height, 8, 32, (size_t)width * 4u, cee_uikit.rgb, 5u, provider, NULL,
                                         false, 0);
    cee_uikit.provider_release(provider);
    if (image == NULL) {
        cee_uikit.pool_pop(pool);
        return -CEE_WINDOW_RESOURCES;
    }
    cee_ios_send_v_id(cee_ios_send(m->view, "layer"), "setContents:", (cee_id)image);
    cee_uikit.image_release(image);
    cee_uikit.pool_pop(pool);
    return 0;
}

static int32_t cee_native_present_boxes(struct cee_window *w, const uint32_t *pixels, uint32_t width, uint32_t height,
                                        const struct cee_window_box *boxes, size_t count) {
    (void)boxes;
    (void)count;
    return cee_native_present(w, pixels, width, height); /* the layer takes whole images */
}

static void cee_native_cursor(struct cee_window *w) { (void)w; }               /* no pointer on a touch screen */
static void cee_native_title(struct cee_window *w, const char *title) { (void)w; (void)title; } /* no title bar */
static void cee_native_text_input(struct cee_window *w, int on) { (void)w; (void)on; } /* P4 step 5 */
static void cee_native_text_area(struct cee_window *w) { (void)w; }

static int32_t cee_native_clipboard_set(struct cee_window *w, const char *text, size_t length) {
    (void)w;
    char *copy = (char *)ceesafe_platform_alloc(length + 1u);
    if (copy == NULL) {
        return -CEE_WINDOW_RESOURCES;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    void *pool = cee_uikit.pool_push();
    cee_id string = cee_ios_string(copy);
    ceesafe_platform_free(copy);
    if (string == NULL) {
        cee_uikit.pool_pop(pool);
        return -CEE_WINDOW_RESOURCES;
    }
    cee_ios_send_v_id(cee_ios_send(cee_ios_cls("UIPasteboard"), "generalPasteboard"), "setString:", string);
    cee_uikit.pool_pop(pool);
    return 0;
}

static int cee_native_clipboard_get(struct cee_window *w) {
    void *pool = cee_uikit.pool_push();
    cee_id string = cee_ios_send(cee_ios_send(cee_ios_cls("UIPasteboard"), "generalPasteboard"), "string");
    const char *utf8 = string != NULL ? cee_ios_send_cstr(string, "UTF8String") : NULL;
    int found = utf8 != NULL && cee_window_paste(w, utf8, strlen(utf8));
    cee_uikit.pool_pop(pool);
    return found;
}

/* A wake from another thread (ADR 0142): the wake source ends the run loop's wait. */
static void cee_native_wake(void) {
    if (cee_uikit.wake_source != NULL) {
        cee_uikit.source_signal(cee_uikit.wake_source);
        cee_uikit.loop_wake_up(cee_uikit.main_loop);
    }
}

/* Until a source is handled (a touch, a wake, UIKit's own) or `seconds`. */
static void cee_native_wait(struct cee_window *w, double seconds) {
    (void)w;
    void *pool = cee_uikit.pool_push();
    (void)cee_uikit.run_in_mode(*cee_uikit.default_mode, seconds > 0.0 ? seconds : 0.0, 1);
    cee_uikit.pool_pop(pool);
}

static void cee_native_wait_any(double seconds) { cee_native_wait(NULL, seconds); }

static int32_t cee_native_menu(struct cee_window *w) {
    (void)w;
    return -CEE_WINDOW_UNSUPPORTED; /* no menu bar on iOS */
}

static int32_t cee_native_dialog(struct cee_window *w, int kind, const char *title, const char *name,
                                 const char *extensions, size_t extensions_length) {
    (void)w;
    (void)kind;
    (void)title;
    (void)name;
    (void)extensions;
    (void)extensions_length;
    return -CEE_WINDOW_UNSUPPORTED; /* file dialogs on iOS are not covered (ADR 0154 §2.2) */
}

#elif defined(__APPLE__)
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
    cee_id *pasteboard_file_url; /* NSPasteboardTypeFileURL (ADR 0150) */
    cee_id *file_urls_only; /* NSPasteboardURLReadingFileURLsOnlyKey */
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

/* Menus (ADR 0146): whether the runtime set the application's main menu, the window whose menus it shows (NULL when
   that window was dropped: only the application's menu is left), and the object their items send their action to. */
static int cee_mac_menu_set;
static struct cee_window *cee_mac_menu_owner;
static cee_id cee_mac_menu_target;
static int cee_mac_build_menu(const uint8_t *bytes, size_t length);

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
       whether the input method changed the composition; the key codes whose down the input method, or a menu's
       shortcut (ADR 0146), took. */
    int marked;
    unsigned long marked_utf16;
    cee_range marked_selection;
    int in_key, key_composed;
    unsigned char ime_keys[16];
    /* A drag over the view (ADR 0150): how many files its pasteboard holds, read when it entered. */
    uint32_t drag_files;
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

/* The content view as an NSDraggingDestination (ADR 0150). It takes file URLs only, which it reads from the drag's
   pasteboard as NSURLs with NSPasteboardURLReadingFileURLsOnlyKey; a drag with none is refused (NSDragOperationNone),
   so the system shows no drop. Each method runs on the main thread inside AppKit's run loop, that of a pump or of a
   wait, and posts the wake event (ADR 0142) after a change, so that a wait it ran in returns. */

/* The drag's file URLs, an autoreleased NSArray, or NULL. */
static cee_id cee_mac_drag_urls(cee_id info) {
    cee_id board = cee_send(info, "draggingPasteboard");
    if (board == NULL) {
        return NULL;
    }
    cee_id classes = cee_send_id_id(cee_mac_cls("NSArray"), "arrayWithObject:", cee_mac_cls("NSURL"));
    cee_id yes = ((cee_id(*)(cee_id, cee_sel, signed char))cee_objc.send)(cee_mac_cls("NSNumber"),
                                                                         cee_mac_sel("numberWithBool:"), 1);
    cee_id key = cee_objc.file_urls_only != NULL ? *cee_objc.file_urls_only
                                                 : cee_nsstring("NSPasteboardURLReadingFileURLsOnlyKey");
    cee_id options = ((cee_id(*)(cee_id, cee_sel, cee_id, cee_id))cee_objc.send)(
        cee_mac_cls("NSDictionary"), cee_mac_sel("dictionaryWithObject:forKey:"), yes, key);
    return ((cee_id(*)(cee_id, cee_sel, cee_id, cee_id))cee_objc.send)(
        board, cee_mac_sel("readObjectsForClasses:options:"), classes, options);
}

/* The drag's position in points from the content's top left (the content view fills the window's content). */
static void cee_mac_drag_at(struct cee_window *w, cee_id info, float *x, float *y) {
    cee_point p = cee_send_point(info, "draggingLocation");
    *x = (float)p.x;
    *y = (float)((double)w->height - p.y);
}

/* The window's answer: copy where the source allows it, else generic or link (the program only reads the files);
   none without files, or for a source that allows only a move. */
static unsigned long cee_mac_drag_operation(cee_id info, uint32_t files) {
    unsigned long mask = cee_send_ulong(info, "draggingSourceOperationMask");
    if (files == 0) {
        return 0ul;
    }
    return (mask & 1ul) ? 1ul : (mask & 4ul) ? 4ul : (mask & 2ul) ? 2ul : 0ul; /* Copy, Generic, Link */
}

/* draggingEntered: (which counts the files) and draggingUpdated:. */
static unsigned long cee_mac_dragging(cee_id self, cee_id info, int entered) {
    struct cee_window *w = cee_mac_view_owner(self);
    if (w == NULL) {
        return 0ul;
    }
    struct cee_mac *m = (struct cee_mac *)w->native;
    void *pool = cee_objc.pool_push();
    if (entered) {
        cee_id urls = cee_mac_drag_urls(info);
        unsigned long count = urls != NULL ? cee_send_ulong(urls, "count") : 0ul;
        m->drag_files = count > 0xfffffffful ? 0xffffffffu : (uint32_t)count;
    }
    unsigned long operation = cee_mac_drag_operation(info, m->drag_files);
    float x = 0.0f, y = 0.0f;
    cee_mac_drag_at(w, info, &x, &y);
    if (cee_window_drag(w, operation != 0ul ? m->drag_files : 0u, x, y)) {
        cee_mac_wake_perform(NULL);
    }
    cee_objc.pool_pop(pool);
    return operation;
}

static unsigned long cee_mac_dragging_entered(cee_id self, cee_sel cmd, cee_id info) {
    (void)cmd;
    return cee_mac_dragging(self, info, 1);
}

static unsigned long cee_mac_dragging_updated(cee_id self, cee_sel cmd, cee_id info) {
    (void)cmd;
    return cee_mac_dragging(self, info, 0);
}

/* draggingExited: and draggingEnded:: the drag left, or ended without a drop here. */
static void cee_mac_dragging_exited(cee_id self, cee_sel cmd, cee_id info) {
    (void)cmd;
    (void)info;
    struct cee_window *w = cee_mac_view_owner(self);
    if (w == NULL) {
        return;
    }
    ((struct cee_mac *)w->native)->drag_files = 0;
    if (cee_window_drag(w, 0, 0.0f, 0.0f)) {
        cee_mac_wake_perform(NULL);
    }
}

static signed char cee_mac_prepare_drag(cee_id self, cee_sel cmd, cee_id info) {
    (void)cmd;
    (void)info;
    struct cee_window *w = cee_mac_view_owner(self);
    return w != NULL && ((struct cee_mac *)w->native)->drag_files > 0;
}

/* The drop: the files' paths (UTF-8 from NSURL's path, of the file path URL a file reference URL stands for). */
static signed char cee_mac_perform_drag(cee_id self, cee_sel cmd, cee_id info) {
    (void)cmd;
    struct cee_window *w = cee_mac_view_owner(self);
    if (w == NULL) {
        return 0;
    }
    void *pool = cee_objc.pool_push();
    cee_id urls = cee_mac_drag_urls(info);
    unsigned long count = urls != NULL ? cee_send_ulong(urls, "count") : 0ul;
    struct cee_window_paths p = {NULL, 0, 0};
    for (unsigned long k = 0; k < count; k++) {
        cee_id url = cee_send(cee_send_id_ulong(urls, "objectAtIndex:", k), "filePathURL");
        cee_id path = url != NULL ? cee_send(url, "path") : NULL;
        const char *utf8 = path != NULL ? cee_send_get_cstr(path, "UTF8String") : NULL;
        if (utf8 != NULL && !cee_paths_add(&p, utf8, strlen(utf8))) {
            break; /* out of memory: the paths kept so far */
        }
    }
    float x = 0.0f, y = 0.0f;
    cee_mac_drag_at(w, info, &x, &y);
    signed char taken = p.length > 0;
    cee_window_dropped(w, x, y, &p);
    ((struct cee_mac *)w->native)->drag_files = 0;
    cee_mac_wake_perform(NULL);
    cee_objc.pool_pop(pool);
    return taken;
}

/* draggingUpdated: only when the pointer moves. */
static signed char cee_mac_periodic_drags(cee_id self, cee_sel cmd) {
    (void)self;
    (void)cmd;
    return 0;
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
    cee_objc.pasteboard_file_url = (cee_id *)cee_mac_data_symbol(appkit, "NSPasteboardTypeFileURL");
    cee_objc.file_urls_only = (cee_id *)cee_mac_data_symbol(appkit, "NSPasteboardURLReadingFileURLsOnlyKey");
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
        /* An NSDraggingDestination for file URLs (ADR 0150). */
        cee_objc.add_method(view, cee_mac_sel("draggingEntered:"), (cee_fn)cee_mac_dragging_entered, "Q@:@");
        cee_objc.add_method(view, cee_mac_sel("draggingUpdated:"), (cee_fn)cee_mac_dragging_updated, "Q@:@");
        cee_objc.add_method(view, cee_mac_sel("draggingExited:"), (cee_fn)cee_mac_dragging_exited, "v@:@");
        cee_objc.add_method(view, cee_mac_sel("draggingEnded:"), (cee_fn)cee_mac_dragging_exited, "v@:@");
        cee_objc.add_method(view, cee_mac_sel("prepareForDragOperation:"), (cee_fn)cee_mac_prepare_drag, "c@:@");
        cee_objc.add_method(view, cee_mac_sel("performDragOperation:"), (cee_fn)cee_mac_perform_drag, "c@:@");
        cee_objc.add_method(view, cee_mac_sel("wantsPeriodicDraggingUpdates"), (cee_fn)cee_mac_periodic_drags,
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
        /* A destination for dragged files (ADR 0150). */
        cee_id type =
            cee_objc.pasteboard_file_url != NULL ? *cee_objc.pasteboard_file_url : cee_nsstring("public.file-url");
        cee_send_v_id(view, "registerForDraggedTypes:",
                      cee_send_id_id(cee_mac_cls("NSArray"), "arrayWithObject:", type));
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
    if (cee_mac_menu_owner == w) { /* its menus leave the menu bar; the application's menu stays (ADR 0146) */
        cee_mac_menu_owner = NULL;
        cee_mac_build_menu(NULL, 0);
    }
    cee_send_v_id(m->window, "setDelegate:", NULL);
    cee_send_v(m->view, "unregisterDraggedTypes");
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
        unsigned char bit = (unsigned char)(1u << (code % 8u));
        if (down && cee_mac_menu_set && !m->marked &&
            cee_send_bool_id(cee_send(cee_objc.app, "mainMenu"), "performKeyEquivalent:", event)) {
            /* A shortcut of an enabled menu item (ADR 0146): its action queued the choice; no KEY for the key, nor
               for its up. */
            if (code < 128u) {
                m->ime_keys[code / 8u] = (unsigned char)(m->ime_keys[code / 8u] | bit);
            }
            return 0;
        }
        if (down && (modifiers & CEE_MOD_COMMAND) && (key == CEE_KEY_CHARACTER + 'q' || key == CEE_KEY_CHARACTER + 'w')) {
            cee_window_push(w, CEE_EVENT_CLOSED, 0, 0, 0, 0, 0.0f, 0.0f); /* ⌘Q and ⌘W ask to close */
            return 0;
        }
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

/* Takes AppKit's pending events, which are the application's, not `w`'s: each goes to the queue of the program's
   window it is for (ADR 0145), an event without a window to `w`'s, one of a window not the program's to AppKit. */
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
        struct cee_window *owner = cee_mac_owner(cee_send(event, "window"));
        if (cee_mac_translate(owner != NULL ? owner : w, event)) {
            cee_send_v_id(cee_objc.app, "sendEvent:", event);
        }
    }
    cee_send_v(cee_objc.app, "updateWindows");
    for (int k = 0; k < CEE_MAC_WINDOWS; k++) {
        struct cee_window *shown = cee_mac_windows[k].window;
        if (shown != NULL) {
            cee_window_rescaled(shown, (float)cee_send_double(((struct cee_mac *)shown->native)->window,
                                                             "backingScaleFactor"));
        }
    }
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

/* waitAny's wait (ADR 0145): AppKit's queue is the application's, and every wake signals the wake source. */
static void cee_native_wait_any(double seconds) { cee_native_wait(NULL, seconds); }

/* Menus (ADR 0146): the application's main menu, the application menu first, then the program's; each item's tag is
   its id, and its action, on the main thread (in a pump's sendEvent:, AppKit's run loop, or performKeyEquivalent:),
   queues it in the window whose menus the bar shows. */

/* `length` bytes of UTF-8 as an autoreleased NSString (an empty one if AppKit refused). */
static cee_id cee_mac_string(const char *text, size_t length) {
    cee_id string = cee_send(cee_mac_cls("NSString"), "alloc");
    string = ((cee_id(*)(cee_id, cee_sel, const void *, unsigned long, unsigned long))cee_objc.send)(
        string, cee_mac_sel("initWithBytes:length:encoding:"), length > 0 ? (const void *)text : (const void *)"",
        (unsigned long)length, 4ul /* NSUTF8StringEncoding */);
    return string != NULL ? cee_send(string, "autorelease") : cee_nsstring("");
}

/* An item's action: its tag is the program's id. The wake event then ends a wait whose run loop ran the action. */
static void cee_mac_menu_choose(cee_id self, cee_sel cmd, cee_id sender) {
    (void)self;
    (void)cmd;
    if (cee_mac_menu_owner != NULL) {
        cee_window_choose(cee_mac_menu_owner, (uint32_t)(unsigned long)cee_send_long(sender, "tag"));
        cee_mac_wake_perform(NULL);
    }
}

/* The application menu's Quit (⌘Q): every window of the program is asked to close. */
static void cee_mac_menu_quit(cee_id self, cee_sel cmd, cee_id sender) {
    (void)self;
    (void)cmd;
    (void)sender;
    for (int k = 0; k < CEE_MAC_WINDOWS; k++) {
        if (cee_mac_windows[k].window != NULL) {
            cee_window_push(cee_mac_windows[k].window, CEE_EVENT_CLOSED, 0, 0, 0, 0, 0.0f, 0.0f);
        }
    }
    cee_mac_wake_perform(NULL);
}

/* A new item, retained: `action` (none if NULL) goes to `target`. */
static cee_id cee_mac_item(cee_id title, const char *action, cee_id target, cee_id key, unsigned long mask) {
    cee_id item = cee_send(cee_mac_cls("NSMenuItem"), "alloc");
    item = ((cee_id(*)(cee_id, cee_sel, cee_id, cee_sel, cee_id))cee_objc.send)(
        item, cee_mac_sel("initWithTitle:action:keyEquivalent:"), title, action != NULL ? cee_mac_sel(action) : NULL,
        key);
    if (item != NULL) {
        if (target != NULL) {
            cee_send_v_id(item, "setTarget:", target);
        }
        ((void (*)(cee_id, cee_sel, unsigned long))cee_objc.send)(item, cee_mac_sel("setKeyEquivalentModifierMask:"),
                                                                   mask);
    }
    return item;
}

/* Adds a retained item to a menu, which retains it. */
static void cee_mac_add(cee_id menu, cee_id item) {
    if (item != NULL) {
        cee_send_v_id(menu, "addItem:", item);
        cee_send_v(item, "release");
    }
}

/* A shortcut's key as a key equivalent's character (AppKit's function-key characters for the named keys); 0 for none.
   A letter with ⇧ is uppercase, as AppKit expects. */
static uint32_t cee_mac_key_equivalent(uint32_t key, int32_t modifiers) {
    switch (key) {
    case CEE_KEY_TAB: return 0x09u;
    case CEE_KEY_ENTER: return 0x0du;
    case CEE_KEY_ESCAPE: return 0x1bu;
    case CEE_KEY_BACKSPACE: return 0x08u;
    case CEE_KEY_DELETE: return 0xf728u;
    case CEE_KEY_LEFT: return 0xf702u;
    case CEE_KEY_RIGHT: return 0xf703u;
    case CEE_KEY_UP: return 0xf700u;
    case CEE_KEY_DOWN: return 0xf701u;
    case CEE_KEY_HOME: return 0xf729u;
    case CEE_KEY_END: return 0xf72bu;
    case CEE_KEY_PAGE_UP: return 0xf72cu;
    case CEE_KEY_PAGE_DOWN: return 0xf72du;
    case CEE_KEY_SPACE: return 0x20u;
    default: break;
    }
    if (key > CEE_KEY_FUNCTION && key <= CEE_KEY_FUNCTION + 12u) {
        return 0xf704u + (key - CEE_KEY_FUNCTION - 1u);
    }
    if (key > CEE_KEY_CHARACTER + 0x20u && key <= CEE_KEY_CHARACTER + 0x10ffffu) {
        uint32_t c = key - CEE_KEY_CHARACTER;
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
        if ((modifiers & CEE_MOD_SHIFT) && c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        }
        return c;
    }
    return 0;
}

/* A scalar as an autoreleased NSString; empty for 0. */
static cee_id cee_mac_scalar(uint32_t c) {
    char utf8[4];
    size_t n = 0;
    if (c == 0) {
        return cee_nsstring("");
    } else if (c < 0x80u) {
        utf8[n++] = (char)c;
    } else if (c < 0x800u) {
        utf8[n++] = (char)(0xc0u | (c >> 6));
        utf8[n++] = (char)(0x80u | (c & 0x3fu));
    } else if (c < 0x10000u) {
        utf8[n++] = (char)(0xe0u | (c >> 12));
        utf8[n++] = (char)(0x80u | ((c >> 6) & 0x3fu));
        utf8[n++] = (char)(0x80u | (c & 0x3fu));
    } else {
        utf8[n++] = (char)(0xf0u | (c >> 18));
        utf8[n++] = (char)(0x80u | ((c >> 12) & 0x3fu));
        utf8[n++] = (char)(0x80u | ((c >> 6) & 0x3fu));
        utf8[n++] = (char)(0x80u | (c & 0x3fu));
    }
    return cee_mac_string(utf8, n);
}

static unsigned long cee_mac_mask(int32_t modifiers) {
    return ((modifiers & CEE_MOD_SHIFT) ? 1ul << 17 : 0ul) | ((modifiers & CEE_MOD_CONTROL) ? 1ul << 18 : 0ul) |
           ((modifiers & CEE_MOD_ALT) ? 1ul << 19 : 0ul) | ((modifiers & CEE_MOD_COMMAND) ? 1ul << 20 : 0ul);
}

/* Sets the main menu: the application menu, then the menus of lib/std's encoding (none for NULL); 0 if AppKit
   refused. The items' target, CeeSafeMenuTarget, is made at the first call and kept for the process. */
static int cee_mac_build_menu(const uint8_t *bytes, size_t length) {
    if (cee_mac_menu_target == NULL) {
        cee_id target = cee_objc.allocate_class(cee_mac_cls("NSObject"), "CeeSafeMenuTarget", 0);
        if (target == NULL) {
            target = cee_mac_cls("CeeSafeMenuTarget"); /* a second runtime in the process registered it */
        } else {
            cee_objc.add_method(target, cee_mac_sel("choose:"), (cee_fn)cee_mac_menu_choose, "v@:@");
            cee_objc.add_method(target, cee_mac_sel("quit:"), (cee_fn)cee_mac_menu_quit, "v@:@");
            cee_objc.register_class(target);
        }
        cee_mac_menu_target = target != NULL ? cee_send(cee_send(target, "alloc"), "init") : NULL;
        if (cee_mac_menu_target == NULL) {
            return 0;
        }
    }
    void *pool = cee_objc.pool_push();
    cee_id none = cee_nsstring("");
    cee_id bar = cee_send_id_id(cee_send(cee_mac_cls("NSMenu"), "alloc"), "initWithTitle:", none);
    if (bar == NULL) {
        cee_objc.pool_pop(pool);
        return 0;
    }
    cee_id name = cee_send(cee_send(cee_mac_cls("NSProcessInfo"), "processInfo"), "processName");
    if (name == NULL) {
        name = none;
    }
    cee_id app = cee_objc.app;
    cee_id app_menu = cee_send_id_id(cee_send(cee_mac_cls("NSMenu"), "alloc"), "initWithTitle:", name);
    if (app_menu != NULL) {
        cee_mac_add(app_menu, cee_mac_item(cee_send_id_id(cee_nsstring("About "), "stringByAppendingString:", name),
                                           "orderFrontStandardAboutPanel:", app, none, 0ul));
        cee_send_v_id(app_menu, "addItem:", cee_send(cee_mac_cls("NSMenuItem"), "separatorItem"));
        cee_mac_add(app_menu, cee_mac_item(cee_send_id_id(cee_nsstring("Hide "), "stringByAppendingString:", name),
                                           "hide:", app, cee_nsstring("h"), 1ul << 20));
        cee_mac_add(app_menu, cee_mac_item(cee_nsstring("Hide Others"), "hideOtherApplications:", app,
                                           cee_nsstring("h"), (1ul << 20) | (1ul << 19)));
        cee_mac_add(app_menu, cee_mac_item(cee_nsstring("Show All"), "unhideAllApplications:", app, none, 0ul));
        cee_send_v_id(app_menu, "addItem:", cee_send(cee_mac_cls("NSMenuItem"), "separatorItem"));
        cee_mac_add(app_menu, cee_mac_item(cee_send_id_id(cee_nsstring("Quit "), "stringByAppendingString:", name),
                                           "quit:", cee_mac_menu_target, cee_nsstring("q"), 1ul << 20));
        cee_id app_item = cee_mac_item(name, NULL, NULL, none, 0ul);
        if (app_item != NULL) {
            cee_send_v_id(app_item, "setSubmenu:", app_menu);
        }
        cee_mac_add(bar, app_item);
        cee_send_v(app_menu, "release");
    }
    cee_id current = NULL; /* the menu being filled, retained by its item in the bar */
    size_t at = 0;
    struct cee_menu_entry e;
    while (bytes != NULL && cee_menu_next(bytes, length, &at, &e)) {
        cee_id title = cee_mac_string(e.title, e.title_length);
        if (e.kind == CEE_MENU_TITLE) {
            cee_id menu = cee_send_id_id(cee_send(cee_mac_cls("NSMenu"), "alloc"), "initWithTitle:", title);
            cee_id item = cee_mac_item(title, NULL, NULL, none, 0ul);
            current = NULL;
            if (menu != NULL && item != NULL) {
                cee_send_v_bool(menu, "setAutoenablesItems:", 0); /* the program's flags, not AppKit's validation */
                cee_send_v_id(item, "setSubmenu:", menu);
                current = menu;
            }
            cee_mac_add(bar, item);
            if (menu != NULL) {
                cee_send_v(menu, "release");
            }
        } else if (current != NULL && e.kind == CEE_MENU_ITEM) {
            cee_id item = cee_mac_item(title, "choose:", cee_mac_menu_target,
                                       cee_mac_scalar(cee_mac_key_equivalent(e.key, e.modifiers)),
                                       cee_mac_mask(e.modifiers));
            if (item != NULL) {
                cee_send_v_long(item, "setTag:", (long)e.id);
                cee_send_v_bool(item, "setEnabled:", (e.flags & CEE_MENU_ENABLED) ? 1 : 0);
                cee_send_v_long(item, "setState:", (e.flags & CEE_MENU_CHECKED) ? 1l : 0l); /* NSControlStateValueOn */
            }
            cee_mac_add(current, item);
        } else if (current != NULL) {
            cee_send_v_id(current, "addItem:", cee_send(cee_mac_cls("NSMenuItem"), "separatorItem"));
        }
    }
    cee_send_v_id(app, "setMainMenu:", bar);
    cee_send_v(bar, "release");
    cee_mac_menu_set = 1;
    cee_objc.pool_pop(pool);
    return 1;
}

static int32_t cee_native_menu(struct cee_window *w) {
    if (!cee_mac_build_menu(w->menu, w->menu_length)) {
        return -CEE_WINDOW_RESOURCES;
    }
    cee_mac_menu_owner = w; /* the last window to set its menus has the menu bar */
    return 0;
}

/* File dialogs (ADR 0146): NSOpenPanel and NSSavePanel, run app-modally. The allowed types are UTTypes where AppKit
   takes them (macOS 11), UniformTypeIdentifiers loaded with dlopen like AppKit; else the extensions themselves. */
static int cee_mac_uttypes(void) {
    static int state; /* 0 not tried, 1 loaded, -1 not there */
    if (state == 0) {
        void *uti = dlopen("/System/Library/Frameworks/UniformTypeIdentifiers.framework/UniformTypeIdentifiers",
                           RTLD_NOW | RTLD_GLOBAL);
        state = uti != NULL && cee_mac_cls("UTType") != NULL ? 1 : -1;
    }
    return state == 1;
}

/* One extension into the lists: [0] its NSString, [1] (if there) its UTType. */
static void cee_mac_extension(const char *extension, size_t length, void *context) {
    cee_id *lists = (cee_id *)context;
    cee_id text = cee_mac_string(extension, length);
    cee_send_v_id(lists[0], "addObject:", text);
    if (lists[1] != NULL) {
        cee_id type = cee_send_id_id(cee_mac_cls("UTType"), "typeWithFilenameExtension:", text);
        if (type != NULL) {
            cee_send_v_id(lists[1], "addObject:", type);
        }
    }
}

/* An NSURL's path into the window's answer; 0 when memory ran out. */
static int cee_mac_answer_url(struct cee_window *w, cee_id url) {
    cee_id path = url != NULL ? cee_send(url, "path") : NULL;
    const char *utf8 = path != NULL ? cee_send_get_cstr(path, "UTF8String") : NULL;
    return utf8 == NULL || cee_window_answer_add(w, utf8, strlen(utf8));
}

static int32_t cee_native_dialog(struct cee_window *w, int kind, const char *title, const char *name,
                                 const char *extensions, size_t extensions_length) {
    struct cee_mac *m = (struct cee_mac *)w->native;
    void *pool = cee_objc.pool_push();
    cee_id panel = kind == CEE_DIALOG_SAVE ? cee_send(cee_mac_cls("NSSavePanel"), "savePanel")
                                           : cee_send(cee_mac_cls("NSOpenPanel"), "openPanel");
    if (panel == NULL) {
        cee_objc.pool_pop(pool);
        return -CEE_WINDOW_RESOURCES;
    }
    if (title[0] != '\0') {
        cee_send_v_id(panel, "setTitle:", cee_nsstring(title));
        cee_send_v_id(panel, "setMessage:", cee_nsstring(title)); /* panels without a title bar show the message */
    }
    if (kind != CEE_DIALOG_SAVE) {
        cee_send_v_bool(panel, "setCanChooseFiles:", kind != CEE_DIALOG_FOLDER);
        cee_send_v_bool(panel, "setCanChooseDirectories:", kind == CEE_DIALOG_FOLDER);
        cee_send_v_bool(panel, "setAllowsMultipleSelection:", kind == CEE_DIALOG_FILES);
    }
    cee_send_v_bool(panel, "setCanCreateDirectories:", kind == CEE_DIALOG_SAVE || kind == CEE_DIALOG_FOLDER);
    if (kind == CEE_DIALOG_SAVE && name[0] != '\0') {
        cee_send_v_id(panel, "setNameFieldStringValue:", cee_nsstring(name));
    }
    if (kind != CEE_DIALOG_FOLDER && extensions_length > 0) {
        int types = cee_mac_uttypes() &&
                    cee_send_bool_sel(panel, "respondsToSelector:", cee_mac_sel("setAllowedContentTypes:"));
        cee_id lists[2] = {cee_send(cee_mac_cls("NSMutableArray"), "array"),
                           types ? cee_send(cee_mac_cls("NSMutableArray"), "array") : NULL};
        if (lists[0] != NULL && cee_dialog_extensions(extensions, extensions_length, cee_mac_extension, lists) > 0) {
            if (lists[1] != NULL && cee_send_ulong(lists[1], "count") > 0) {
                cee_send_v_id(panel, "setAllowedContentTypes:", lists[1]);
            } else {
                cee_send_v_id(panel, "setAllowedFileTypes:", lists[0]); /* before macOS 11 */
            }
        }
    }
    cee_send_v_bool(cee_objc.app, "activateIgnoringOtherApps:", 1);
    long response = cee_send_long(panel, "runModal");
    int32_t result = 0;
    if (response == 1) { /* NSModalResponseOK */
        if (kind == CEE_DIALOG_SAVE) {
            result = cee_mac_answer_url(w, cee_send(panel, "URL")) ? 0 : -CEE_WINDOW_RESOURCES;
        } else {
            cee_id urls = cee_send(panel, "URLs");
            unsigned long count = urls != NULL ? cee_send_ulong(urls, "count") : 0ul;
            for (unsigned long k = 0; k < count && result == 0; k++) {
                if (!cee_mac_answer_url(w, cee_send_id_ulong(urls, "objectAtIndex:", k))) {
                    result = -CEE_WINDOW_RESOURCES;
                }
            }
        }
    }
    cee_send_v_id(m->window, "makeKeyAndOrderFront:", NULL);
    cee_objc.pool_pop(pool);
    return result;
}

#elif defined(_WIN32)
/* Win32 and GDI. The window procedure queues events while PeekMessageW dispatches; a frame is kept as BGRA for
   WM_PAINT and drawn with StretchDIBits. */
#if defined(_MSC_VER)
#pragma comment(lib, "user32.lib") /* clang and cl; MinGW's gcc gets -lgdi32 from ceesafec (tooling.md §8.3) */
#pragma comment(lib, "gdi32.lib")
#endif
#include <imm.h> /* the input method's types and constants; its functions come from imm32.dll, loaded at first use */
#define COBJMACROS    /* the COM interfaces' C calls, IFileDialog_Show and the like (ADR 0146) */
#include <shobjidl.h> /* the file dialogs' interfaces; their functions come from ole32.dll, loaded at first use */

/* The window's IDropTarget (ADR 0150), which the window owns: OLE calls through its vtable, first. */
struct cee_win32_drop_target {
    IDropTarget target;
    struct cee_window *window;
    uint32_t files; /* the files of the drag over the window, counted at DragEnter */
};

struct cee_win32 {
    HWND hwnd;
    uint32_t *frame; /* BGRA, the last presented frame */
    uint32_t frame_width, frame_height;
    int captured;
    unsigned char ime_keys[32]; /* virtual keys whose down the input method took (ADR 0141) */
    int menu_sizing;            /* while setMenu keeps the content's size, WM_SIZE is not the user's (ADR 0146) */
    /* Drops (ADR 0150): whether OLE was initialized for the window, and whether its drop target is registered. */
    int ole, drop_registered;
    struct cee_win32_drop_target drop;
};

static void cee_win32_drop_open(struct cee_window *w);
static void cee_win32_drop_close(struct cee_window *w);
static void cee_win32_ole_done(void);

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
        if (n->menu_sizing) {
            return 0; /* setMenu reports the size it ends with (ADR 0146) */
        }
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
        if ((message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) && (wparam == VK_MENU || wparam == VK_F10) &&
            GetMenu(hwnd) != NULL) {
            break; /* Alt alone, or F10, opens the menu bar, as in every Windows program with one (ADR 0146) */
        }
        return 0;
    }
    case WM_COMMAND: /* a menu item (ADR 0146): its command is its index in the menu plus 1 */
        if (HIWORD(wparam) == 0 && lparam == 0) {
            size_t command = (size_t)LOWORD(wparam);
            if (command >= 1 && command <= w->menu_items) {
                cee_window_choose(w, w->menu_ids[command - 1]);
            }
            return 0;
        }
        break;
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
    cee_win32_drop_open(w);
    return 0;
}

static void cee_native_close(struct cee_window *w) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    if (n == NULL) {
        return;
    }
    cee_win32_drop_close(w);
    SetWindowLongPtrW(n->hwnd, GWLP_USERDATA, 0);
    DestroyWindow(n->hwnd);
    if (n->ole) {
        cee_win32_ole_done();
    }
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
    /* The thread's messages are every window's, and the window procedure queues each in its own window (ADR 0145). */
    cee_window_rescaled(w, cee_win32_scale(n->hwnd));
    for (struct cee_window *other = cee_wake_windows; other != NULL; other = other->wake_link) {
        if (other != w && !other->scripted && other->native != NULL) {
            cee_window_rescaled(other, cee_win32_scale(((struct cee_win32 *)other->native)->hwnd));
        }
    }
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

/* waitAny's wait (ADR 0145): the thread's message queue holds every window's messages, and every wake sets the
   process's event, so one handle serves any number of windows. */
static void cee_native_wait_any(double seconds) {
    double ms = seconds * 1000.0 + 0.999;
    DWORD milliseconds = ms <= 0.0 ? 0u : ms >= 4294967294.0 ? 4294967294u : (DWORD)ms;
    HANDLE wake = cee_wake_any_event;
    MsgWaitForMultipleObjectsEx(1, &wake, milliseconds, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}

/* Menus (ADR 0146): the window's menu bar, an HMENU of popups, which DestroyWindow destroys with the window. An item's
   command is its index plus 1 (WM_COMMAND carries 16 bits, an id 32); its shortcut is the text after a tab, which
   Windows shows right-aligned and does not act on. */

/* A shortcut as Windows writes it, "Ctrl+Shift+S", into `out` (at least 64 bytes), NUL-terminated; its length, 0 for
   a key no text names. */
static size_t cee_win32_shortcut(uint32_t key, int32_t modifiers, char *out) {
    static const char *const named[] = {"",     "Tab",  "Enter", "Esc",  "Backspace", "Del",  "Left", "Right",
                                        "Up",   "Down", "Home",  "End",  "PgUp",      "PgDn", "Space"};
    char name[16];
    if (key >= 1 && key <= CEE_KEY_SPACE) {
        snprintf(name, sizeof name, "%s", named[key]);
    } else if (key > CEE_KEY_FUNCTION && key <= CEE_KEY_FUNCTION + 12u) {
        snprintf(name, sizeof name, "F%u", (unsigned)(key - CEE_KEY_FUNCTION));
    } else if (key > CEE_KEY_CHARACTER + 0x20u && key < CEE_KEY_CHARACTER + 0x7fu) {
        uint32_t c = key - CEE_KEY_CHARACTER;
        name[0] = (char)(c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c);
        name[1] = '\0';
    } else {
        return 0; /* a key beyond ASCII has no name on every layout */
    }
    int n = snprintf(out, 64, "%s%s%s%s%s", (modifiers & CEE_MOD_CONTROL) ? "Ctrl+" : "",
                     (modifiers & CEE_MOD_ALT) ? "Alt+" : "", (modifiers & CEE_MOD_SHIFT) ? "Shift+" : "",
                     (modifiers & CEE_MOD_COMMAND) ? "Win+" : "", name);
    return n > 0 ? (size_t)n : 0;
}

/* An entry's text as UTF-16, allocated: the title with each '&' doubled (no mnemonics), then a tab and the shortcut. */
static wchar_t *cee_win32_menu_text(const struct cee_menu_entry *e) {
    char shortcut[64];
    size_t extra = e->kind == CEE_MENU_ITEM ? cee_win32_shortcut(e->key, e->modifiers, shortcut) : 0;
    char *text = (char *)ceesafe_platform_alloc(e->title_length * 2u + extra + 2u);
    if (text == NULL) {
        return NULL;
    }
    size_t n = 0;
    for (size_t k = 0; k < e->title_length; k++) {
        if (e->title[k] == '&') {
            text[n++] = '&';
        }
        text[n++] = e->title[k] == '\0' ? ' ' : e->title[k];
    }
    if (extra > 0) {
        text[n++] = '\t';
        memcpy(text + n, shortcut, extra);
        n += extra;
    }
    text[n] = '\0';
    wchar_t *wide = cee_win32_wide(text);
    ceesafe_platform_free(text);
    return wide;
}

/* The menu bar of lib/std's encoding, or NULL with *failed set when the system refused; NULL for no menus. */
static HMENU cee_win32_build_menu(const uint8_t *bytes, size_t length, int *failed) {
    HMENU bar = NULL, current = NULL;
    size_t at = 0, command = 0;
    struct cee_menu_entry e;
    *failed = 0;
    while (!*failed && cee_menu_next(bytes, length, &at, &e)) {
        if (e.kind == CEE_MENU_SEPARATOR) {
            *failed = !AppendMenuW(current, MF_SEPARATOR, 0, NULL);
            continue;
        }
        wchar_t *text = cee_win32_menu_text(&e);
        if (text == NULL) {
            *failed = 1;
            break;
        }
        if (e.kind == CEE_MENU_TITLE) {
            if (bar == NULL) {
                bar = CreateMenu();
            }
            current = CreatePopupMenu();
            if (bar == NULL || current == NULL || !AppendMenuW(bar, MF_POPUP | MF_STRING, (UINT_PTR)current, text)) {
                if (current != NULL) {
                    DestroyMenu(current); /* not yet the bar's */
                }
                *failed = 1;
            }
        } else {
            UINT flags = (UINT)(MF_STRING | ((e.flags & CEE_MENU_ENABLED) ? MF_ENABLED : MF_GRAYED) |
                                ((e.flags & CEE_MENU_CHECKED) ? MF_CHECKED : MF_UNCHECKED));
            *failed = !AppendMenuW(current, flags, (UINT_PTR)++command, text);
        }
        ceesafe_platform_free(text);
    }
    if (*failed && bar != NULL) {
        DestroyMenu(bar); /* with its popups */
        bar = NULL;
    }
    return bar;
}

/* Sets the window's menu bar; the content keeps its size (the window grows by the bar), unless it is maximized. */
static int32_t cee_native_menu(struct cee_window *w) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    int failed = 0;
    HMENU bar = cee_win32_build_menu(w->menu, w->menu_length, &failed);
    if (failed) {
        return -CEE_WINDOW_RESOURCES;
    }
    HMENU old = GetMenu(n->hwnd);
    RECT client;
    GetClientRect(n->hwnd, &client);
    n->menu_sizing = 1;
    if (!SetMenu(n->hwnd, bar)) {
        n->menu_sizing = 0;
        if (bar != NULL) {
            DestroyMenu(bar);
        }
        return -CEE_WINDOW_RESOURCES;
    }
    if (!IsZoomed(n->hwnd)) {
        RECT r = client;
        AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrW(n->hwnd, GWL_STYLE), bar != NULL,
                           (DWORD)GetWindowLongPtrW(n->hwnd, GWL_EXSTYLE));
        SetWindowPos(n->hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    n->menu_sizing = 0;
    if (old != NULL) {
        DestroyMenu(old);
    }
    DrawMenuBar(n->hwnd);
    GetClientRect(n->hwnd, &client);
    float scale = w->scale > 0.0f ? w->scale : 1.0f;
    uint32_t width = (uint32_t)((float)(client.right - client.left) / scale + 0.5f);
    uint32_t height = (uint32_t)((float)(client.bottom - client.top) / scale + 0.5f);
    if (width > 0 && height > 0) {
        cee_window_resized(w, width, height);
    }
    return 0;
}

/* File dialogs (ADR 0146): the common item dialogs, through COM. ole32.dll is loaded with LoadLibraryW like imm32.dll,
   so no program needs a link flag; the class and interface ids are written here, so no uuid library is needed. */
static const CLSID cee_clsid_open = {0xdc1c5a9c, 0xe88a, 0x4dde, {0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7}};
static const CLSID cee_clsid_save = {0xc0b4e2f3, 0xba21, 0x4773, {0x8d, 0xba, 0x33, 0x5e, 0xc9, 0x46, 0xeb, 0x8b}};
static const IID cee_iid_open = {0xd57c7288, 0xd4ad, 0x4768, {0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60}};
static const IID cee_iid_save = {0x84bccd23, 0x5fde, 0x4cdb, {0xae, 0xa4, 0xaf, 0x64, 0xb8, 0x3d, 0x78, 0xab}};

static struct {
    int state; /* 0 not tried, 1 loaded, -1 failed */
    HRESULT(WINAPI *initialize)(LPVOID, DWORD);
    void(WINAPI *uninitialize)(void);
    HRESULT(WINAPI *create)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
    void(WINAPI *free)(LPVOID);
} cee_ole;

static int cee_ole_load(void) {
    if (cee_ole.state != 0) {
        return cee_ole.state == 1;
    }
    cee_ole.state = -1;
    HMODULE ole = LoadLibraryW(L"ole32.dll");
    if (ole == NULL) {
        return 0;
    }
    FARPROC p[4] = {GetProcAddress(ole, "CoInitializeEx"), GetProcAddress(ole, "CoUninitialize"),
                    GetProcAddress(ole, "CoCreateInstance"), GetProcAddress(ole, "CoTaskMemFree")};
    for (int k = 0; k < 4; k++) {
        if (p[k] == NULL) {
            return 0;
        }
    }
    memcpy(&cee_ole.initialize, &p[0], sizeof p[0]);
    memcpy(&cee_ole.uninitialize, &p[1], sizeof p[1]);
    memcpy(&cee_ole.create, &p[2], sizeof p[2]);
    memcpy(&cee_ole.free, &p[3], sizeof p[3]);
    cee_ole.state = 1;
    return 1;
}

/* The filter's pattern, "*.png;*.jpg", being built. */
struct cee_win32_patterns {
    char *text;
    size_t length;
};

static void cee_win32_pattern(const char *extension, size_t length, void *context) {
    struct cee_win32_patterns *p = (struct cee_win32_patterns *)context;
    if (p->length > 0) {
        p->text[p->length++] = ';';
    }
    p->text[p->length++] = '*';
    p->text[p->length++] = '.';
    memcpy(p->text + p->length, extension, length);
    p->length += length;
    p->text[p->length] = '\0';
}

/* A shell item's file system path into the window's answer: 0 done, or minus a WindowError's number (a path that is
   not valid UTF-16, so not UTF-8 either, is Unsupported). */
static int32_t cee_win32_answer_item(struct cee_window *w, IShellItem *item) {
    LPWSTR path = NULL;
    if (FAILED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path)) || path == NULL) {
        return -CEE_WINDOW_RESOURCES;
    }
    int32_t result = -CEE_WINDOW_UNSUPPORTED;
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, NULL, 0, NULL, NULL);
    char *utf8 = count > 0 ? (char *)ceesafe_platform_alloc((size_t)count) : NULL;
    if (count > 0 && utf8 == NULL) {
        result = -CEE_WINDOW_RESOURCES;
    } else if (utf8 != NULL &&
               WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, utf8, count, NULL, NULL) == count) {
        result = cee_window_answer_add(w, utf8, (size_t)count - 1u) ? 0 : -CEE_WINDOW_RESOURCES;
    }
    ceesafe_platform_free(utf8);
    cee_ole.free(path);
    return result;
}

/* The dialog, once COM is initialized on this thread: 0 (with the answer, none if cancelled) or minus an error. */
static int32_t cee_win32_dialog(struct cee_window *w, int kind, const char *title, const char *name,
                                const char *extensions, size_t extensions_length) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    IFileDialog *dialog = NULL;
    HRESULT made = kind == CEE_DIALOG_SAVE
                       ? cee_ole.create(&cee_clsid_save, NULL, CLSCTX_INPROC_SERVER, &cee_iid_save, (void **)&dialog)
                       : cee_ole.create(&cee_clsid_open, NULL, CLSCTX_INPROC_SERVER, &cee_iid_open, (void **)&dialog);
    if (FAILED(made) || dialog == NULL) {
        return -CEE_WINDOW_UNSUPPORTED;
    }
    int32_t result = 0;
    FILEOPENDIALOGOPTIONS options = 0;
    IFileDialog_GetOptions(dialog, &options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (kind == CEE_DIALOG_FILE || kind == CEE_DIALOG_FILES) {
        options |= FOS_FILEMUSTEXIST;
    }
    if (kind == CEE_DIALOG_FILES) {
        options |= FOS_ALLOWMULTISELECT;
    }
    if (kind == CEE_DIALOG_FOLDER) {
        options |= FOS_PICKFOLDERS;
    }
    if (kind == CEE_DIALOG_SAVE) {
        options |= FOS_OVERWRITEPROMPT;
    }
    IFileDialog_SetOptions(dialog, options);
    wchar_t *wide = title[0] != '\0' ? cee_win32_wide(title) : NULL;
    if (wide != NULL) {
        IFileDialog_SetTitle(dialog, wide);
        ceesafe_platform_free(wide);
    }
    wide = kind == CEE_DIALOG_SAVE && name[0] != '\0' ? cee_win32_wide(name) : NULL;
    if (wide != NULL) {
        IFileDialog_SetFileName(dialog, wide);
        ceesafe_platform_free(wide);
    }
    wchar_t *filter_text = NULL; /* the dialog keeps a pointer to it until it is released */
    if (kind != CEE_DIALOG_FOLDER && extensions_length > 0) {
        struct cee_win32_patterns patterns = {(char *)ceesafe_platform_alloc(extensions_length * 3u + 4u), 0};
        if (patterns.text != NULL &&
            cee_dialog_extensions(extensions, extensions_length, cee_win32_pattern, &patterns) > 0) {
            filter_text = cee_win32_wide(patterns.text);
            if (filter_text != NULL) {
                COMDLG_FILTERSPEC filter = {filter_text, filter_text};
                IFileDialog_SetFileTypes(dialog, 1, &filter);
                if (kind == CEE_DIALOG_SAVE) {
                    IFileDialog_SetDefaultExtension(dialog, filter_text + 2); /* the first, after its "*." */
                }
            }
        }
        ceesafe_platform_free(patterns.text);
    }
    HRESULT shown = IFileDialog_Show(dialog, n->hwnd);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        result = 0;
    } else if (FAILED(shown)) {
        result = -CEE_WINDOW_RESOURCES;
    } else if (kind == CEE_DIALOG_FILES) {
        IShellItemArray *items = NULL;
        DWORD count = 0;
        if (FAILED(IFileOpenDialog_GetResults((IFileOpenDialog *)dialog, &items)) || items == NULL ||
            FAILED(IShellItemArray_GetCount(items, &count))) {
            result = -CEE_WINDOW_RESOURCES;
        }
        for (DWORD k = 0; k < count && result == 0; k++) {
            IShellItem *item = NULL;
            if (FAILED(IShellItemArray_GetItemAt(items, k, &item)) || item == NULL) {
                result = -CEE_WINDOW_RESOURCES;
            } else {
                result = cee_win32_answer_item(w, item);
                IShellItem_Release(item);
            }
        }
        if (items != NULL) {
            IShellItemArray_Release(items);
        }
    } else {
        IShellItem *item = NULL;
        if (FAILED(IFileDialog_GetResult(dialog, &item)) || item == NULL) {
            result = -CEE_WINDOW_RESOURCES;
        } else {
            result = cee_win32_answer_item(w, item);
            IShellItem_Release(item);
        }
    }
    IFileDialog_Release(dialog);
    ceesafe_platform_free(filter_text);
    return result;
}

/* COM on the program's first thread, apartment-threaded as the shell's dialogs need, for the call only: a thread that
   already initialized COM keeps its own (S_FALSE, or RPC_E_CHANGED_MODE for a multithreaded apartment). */
static int32_t cee_native_dialog(struct cee_window *w, int kind, const char *title, const char *name,
                                 const char *extensions, size_t extensions_length) {
    if (!cee_ole_load()) {
        return -CEE_WINDOW_UNSUPPORTED;
    }
    HRESULT initialized = cee_ole.initialize(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int32_t result = cee_win32_dialog(w, kind, title, name, extensions, extensions_length);
    if (SUCCEEDED(initialized)) {
        cee_ole.uninitialize();
    }
    return result;
}

/* Drops (ADR 0150): an IDropTarget for each window, registered with OLE, whose methods OLE calls on the window's thread
   while it dispatches messages (in a pump, so they queue as the window procedure does). OLE is ole32.dll's, loaded
   with LoadLibraryW like the dialogs' COM, and initialized from each window's open to its close (OleInitialize counts);
   on a thread whose COM is multithreaded it cannot be, and that window takes no drops. The drag's files are CF_HDROP;
   a drag without them is refused (DROPEFFECT_NONE), so the system shows no drop. */
static const IID cee_iid_unknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID cee_iid_drop_target = {0x00000122, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

static struct {
    int state; /* 0 not tried, 1 loaded, -1 failed */
    HRESULT(WINAPI *initialize)(LPVOID);
    void(WINAPI *uninitialize)(void);
    HRESULT(WINAPI *register_target)(HWND, LPDROPTARGET);
    HRESULT(WINAPI *revoke_target)(HWND);
    void(WINAPI *release_medium)(LPSTGMEDIUM);
} cee_oledrop;

static int cee_oledrop_load(void) {
    if (cee_oledrop.state != 0) {
        return cee_oledrop.state == 1;
    }
    cee_oledrop.state = -1;
    HMODULE ole = LoadLibraryW(L"ole32.dll");
    if (ole == NULL) {
        return 0;
    }
    FARPROC p[5] = {GetProcAddress(ole, "OleInitialize"), GetProcAddress(ole, "OleUninitialize"),
                    GetProcAddress(ole, "RegisterDragDrop"), GetProcAddress(ole, "RevokeDragDrop"),
                    GetProcAddress(ole, "ReleaseStgMedium")};
    for (int k = 0; k < 5; k++) {
        if (p[k] == NULL) {
            return 0;
        }
    }
    memcpy(&cee_oledrop.initialize, &p[0], sizeof p[0]);
    memcpy(&cee_oledrop.uninitialize, &p[1], sizeof p[1]);
    memcpy(&cee_oledrop.register_target, &p[2], sizeof p[2]);
    memcpy(&cee_oledrop.revoke_target, &p[3], sizeof p[3]);
    memcpy(&cee_oledrop.release_medium, &p[4], sizeof p[4]);
    cee_oledrop.state = 1;
    return 1;
}

static void cee_win32_ole_done(void) { cee_oledrop.uninitialize(); }

/* DROPFILES (shlobj.h): where the paths start, the drop point, and whether they are UTF-16. */
struct cee_win32_dropfiles {
    DWORD files;
    POINT point;
    BOOL nonclient;
    BOOL wide;
};

/* UTF-16 `units` as UTF-8 into `p`; 0 when memory ran out. A path that is not valid UTF-16 is left out. */
static int cee_win32_drop_path(struct cee_window_paths *p, const wchar_t *path, size_t units) {
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, (int)units, NULL, 0, NULL, NULL);
    if (count <= 0) {
        return 1;
    }
    char *utf8 = (char *)ceesafe_platform_alloc((size_t)count);
    if (utf8 == NULL) {
        return 0;
    }
    int ok = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, (int)units, utf8, count, NULL, NULL) != count ||
             cee_paths_add(p, utf8, (size_t)count);
    ceesafe_platform_free(utf8);
    return ok;
}

/* The drag's files: how many CF_HDROP holds, and with `p`, their paths. */
static uint32_t cee_win32_drop_files(IDataObject *data, struct cee_window_paths *p) {
    FORMATETC format = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium;
    memset(&medium, 0, sizeof medium);
    if (data == NULL || FAILED(IDataObject_GetData(data, &format, &medium))) {
        return 0;
    }
    uint32_t count = 0;
    const unsigned char *block =
        medium.tymed == TYMED_HGLOBAL ? (const unsigned char *)GlobalLock(medium.hGlobal) : NULL;
    SIZE_T size = block != NULL ? GlobalSize(medium.hGlobal) : 0;
    struct cee_win32_dropfiles header;
    if (block != NULL && size >= sizeof header) {
        memcpy(&header, block, sizeof header);
        if (header.files >= sizeof header && header.files < size) {
            /* The list, NUL-separated and ending with an empty one, as UTF-16 (copied, so it is aligned). */
            size_t bytes = (size_t)(size - header.files), units = 0;
            wchar_t *list = NULL;
            if (header.wide) {
                units = bytes / sizeof(wchar_t);
                list = (wchar_t *)ceesafe_platform_alloc((units + 1u) * sizeof(wchar_t));
                if (list != NULL) {
                    memcpy(list, block + header.files, units * sizeof(wchar_t));
                }
            } else {
                int wide = MultiByteToWideChar(CP_ACP, 0, (const char *)block + header.files, (int)bytes, NULL, 0);
                units = wide > 0 ? (size_t)wide : 0;
                list = (wchar_t *)ceesafe_platform_alloc((units + 1u) * sizeof(wchar_t));
                if (list != NULL && units > 0) {
                    MultiByteToWideChar(CP_ACP, 0, (const char *)block + header.files, (int)bytes, list, wide);
                }
            }
            if (list != NULL) {
                list[units] = L'\0';
                size_t start = 0;
                int ok = 1;
                for (size_t k = 0; k <= units && list[start] != L'\0'; k++) {
                    if (list[k] == L'\0') {
                        count++;
                        if (p != NULL && ok) {
                            ok = cee_win32_drop_path(p, list + start, k - start);
                        }
                        start = k + 1;
                    }
                }
                ceesafe_platform_free(list);
            }
        }
        GlobalUnlock(medium.hGlobal);
    }
    cee_oledrop.release_medium(&medium);
    return count;
}

/* The drag's position (screen pixels) in points from the content's top left; whether it is in the content. OLE's
   target is the whole window, its frame included, which macOS's view and X11's window are not: over the title bar or
   the border the window takes no drop. */
static int cee_win32_drop_at(struct cee_window *w, POINTL at, float *x, float *y) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    POINT p = {at.x, at.y};
    RECT client;
    ScreenToClient(n->hwnd, &p);
    GetClientRect(n->hwnd, &client);
    float scale = w->scale > 0.0f ? w->scale : 1.0f;
    *x = (float)p.x / scale;
    *y = (float)p.y / scale;
    return p.x >= client.left && p.y >= client.top && p.x < client.right && p.y < client.bottom;
}

/* The window's answer: copy where the source allows it, else link (the program only reads the files); none without
   files, or for a source that allows only a move. */
static DWORD cee_win32_drop_effect(DWORD allowed, uint32_t files) {
    if (files == 0) {
        return DROPEFFECT_NONE;
    }
    return (allowed & DROPEFFECT_COPY)   ? DROPEFFECT_COPY
           : (allowed & DROPEFFECT_LINK) ? DROPEFFECT_LINK
                                         : DROPEFFECT_NONE;
}

static HRESULT STDMETHODCALLTYPE cee_drop_query(IDropTarget *self, REFIID iid, void **out) {
    if (out == NULL) {
        return E_POINTER;
    }
    if (memcmp(iid, &cee_iid_unknown, sizeof(IID)) == 0 || memcmp(iid, &cee_iid_drop_target, sizeof(IID)) == 0) {
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

/* The window owns its target, so the references are not counted. */
static ULONG STDMETHODCALLTYPE cee_drop_add_ref(IDropTarget *self) {
    (void)self;
    return 1;
}

static ULONG STDMETHODCALLTYPE cee_drop_release(IDropTarget *self) {
    (void)self;
    return 1;
}

static HRESULT STDMETHODCALLTYPE cee_drop_over(IDropTarget *self, DWORD keys, POINTL at, DWORD *effect) {
    (void)keys;
    struct cee_win32_drop_target *t = (struct cee_win32_drop_target *)self;
    float x = 0.0f, y = 0.0f;
    int inside = cee_win32_drop_at(t->window, at, &x, &y);
    DWORD answer = inside ? cee_win32_drop_effect(*effect, t->files) : DROPEFFECT_NONE;
    cee_window_drag(t->window, answer != DROPEFFECT_NONE ? t->files : 0u, x, y);
    *effect = answer;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cee_drop_enter(IDropTarget *self, IDataObject *data, DWORD keys, POINTL at,
                                                DWORD *effect) {
    struct cee_win32_drop_target *t = (struct cee_win32_drop_target *)self;
    t->files = cee_win32_drop_files(data, NULL);
    return cee_drop_over(self, keys, at, effect);
}

static HRESULT STDMETHODCALLTYPE cee_drop_leave(IDropTarget *self) {
    struct cee_win32_drop_target *t = (struct cee_win32_drop_target *)self;
    t->files = 0;
    cee_window_drag(t->window, 0, 0.0f, 0.0f);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cee_drop_drop(IDropTarget *self, IDataObject *data, DWORD keys, POINTL at,
                                               DWORD *effect) {
    (void)keys;
    struct cee_win32_drop_target *t = (struct cee_win32_drop_target *)self;
    struct cee_window_paths p = {NULL, 0, 0};
    uint32_t files = cee_win32_drop_files(data, &p);
    float x = 0.0f, y = 0.0f;
    int inside = cee_win32_drop_at(t->window, at, &x, &y);
    *effect = inside && p.length > 0 ? cee_win32_drop_effect(*effect, files) : DROPEFFECT_NONE;
    if (*effect == DROPEFFECT_NONE) {
        ceesafe_platform_free(p.text);
        p.text = NULL;
        p.length = 0;
    }
    cee_window_dropped(t->window, x, y, &p);
    t->files = 0;
    return S_OK;
}

static IDropTargetVtbl cee_drop_vtable = {cee_drop_query, cee_drop_add_ref, cee_drop_release, cee_drop_enter,
                                          cee_drop_over,  cee_drop_leave,   cee_drop_drop};

/* At the window's open: OLE, then the window's drop target. */
static void cee_win32_drop_open(struct cee_window *w) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    if (!cee_oledrop_load() || FAILED(cee_oledrop.initialize(NULL))) {
        return; /* no drops for this window */
    }
    n->ole = 1;
    n->drop.target.lpVtbl = &cee_drop_vtable;
    n->drop.window = w;
    n->drop_registered = SUCCEEDED(cee_oledrop.register_target(n->hwnd, &n->drop.target));
}

/* Before the window is destroyed: OLE lets go of its drop target. */
static void cee_win32_drop_close(struct cee_window *w) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    if (n->drop_registered) {
        cee_oledrop.revoke_target(n->hwnd);
        n->drop_registered = 0;
    }
}

#elif defined(__ANDROID__)
/* Android (ADR 0154 §2.1, §4): the program is a NativeActivity's library. The runtime's own glue starts main on a
   thread of its own, with a looper; the activity's callbacks, on the activity's thread, record what happened (the
   window, the input queue, focus, the activity's end) under a lock and wake that looper, and the program's pump takes
   it. One window: the activity's. Frames are copied into the ANativeWindow's buffer (RGBX_8888, the bytes in the
   frames' order). The first touch is the primary pointer; the activity's end (Back, or the system's) is Closed.
   Standard output and error go to logcat. */
#include <android/configuration.h>
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <jni.h>
#include <pthread.h>
#include <unistd.h>

#define CEE_HANDLE_ANDROID 5
#define CEE_ANDROID_INPUT 1 /* the looper's identifier for the input queue */

static struct {
    pthread_mutex_t lock;
    ANativeActivity *activity;
    ANativeWindow *window;      /* the activity's window, while it has one */
    AInputQueue *queue;         /* the queue the activity gave, to attach */
    AInputQueue *attached;      /* the queue attached to the program's looper; used only under the lock */
    int focused;
    int destroyed;              /* the activity ended: Closed */
    ALooper *looper;            /* the program thread's */
    float scale;                /* pixels per point, from the density */
    int32_t safe[4];            /* the window's insets, left, top, right and bottom, in pixels (ADR 0156) */
} cee_android = {PTHREAD_MUTEX_INITIALIZER, NULL, NULL, NULL, NULL, 1, 0, NULL, 1.0f, {0, 0, 0, 0}};

/* The one window: the activity's state is cee_android's, and the window's touches are the shared layer's (ADR 0155);
   this marks the window as the system's. */
struct cee_droid {
    int32_t opened;
};
static struct cee_window *cee_android_shown;

static void cee_android_wake_program(void) {
    pthread_mutex_lock(&cee_android.lock);
    if (cee_android.looper != NULL) {
        ALooper_wake(cee_android.looper);
    }
    pthread_mutex_unlock(&cee_android.lock);
}

/* ---- The activity's callbacks, on the activity's thread ---- */

/* The safe area (ADR 0156) is the window's insets, which only the Java side knows: the activity's
   getWindow().getDecorView().getRootWindowInsets(). From Android 11 (API 30) they are
   getInsets(systemBars() | displayCutout()), an android.graphics.Insets; before, the system window insets and the
   display cutout's safe insets, the larger of the two on each side. Called on the activity's thread, whose JNIEnv
   the activity holds; every failure (a view not attached yet, a method an old system lacks) keeps the insets as they
   were, and no Java exception is left pending. */
static jobject cee_android_object(JNIEnv *env, jobject target, const char *name, const char *signature) {
    if (target == NULL) {
        return NULL;
    }
    jclass type = (*env)->GetObjectClass(env, target);
    jmethodID method = type != NULL ? (*env)->GetMethodID(env, type, name, signature) : NULL;
    jobject result = method != NULL ? (*env)->CallObjectMethod(env, target, method) : NULL;
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return NULL;
    }
    return result;
}

static int cee_android_int(JNIEnv *env, jobject target, const char *name, int32_t *out) {
    jclass type = (*env)->GetObjectClass(env, target);
    jmethodID method = type != NULL ? (*env)->GetMethodID(env, type, name, "()I") : NULL;
    jint value = method != NULL ? (*env)->CallIntMethod(env, target, method) : 0;
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return 0;
    }
    *out = (int32_t)value;
    return method != NULL;
}

static int cee_android_field(JNIEnv *env, jobject target, const char *name, int32_t *out) {
    jclass type = (*env)->GetObjectClass(env, target);
    jfieldID field = type != NULL ? (*env)->GetFieldID(env, type, name, "I") : NULL;
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return 0;
    }
    if (field == NULL) {
        return 0;
    }
    *out = (int32_t)(*env)->GetIntField(env, target, field);
    return 1;
}

static void cee_android_read_insets(ANativeActivity *activity) {
    JNIEnv *env = activity != NULL ? activity->env : NULL;
    if (env == NULL || activity->clazz == NULL) {
        return;
    }
    if ((*env)->PushLocalFrame(env, 24) != 0) {
        (*env)->ExceptionClear(env);
        return;
    }
    int32_t safe[4] = {0, 0, 0, 0};
    int found = 0;
    jobject window = cee_android_object(env, activity->clazz, "getWindow", "()Landroid/view/Window;");
    jobject decor = cee_android_object(env, window, "getDecorView", "()Landroid/view/View;");
    jobject insets = cee_android_object(env, decor, "getRootWindowInsets", "()Landroid/view/WindowInsets;");
    if (insets != NULL) {
        jclass type = (*env)->GetObjectClass(env, insets);
        jmethodID get = type != NULL ? (*env)->GetMethodID(env, type, "getInsets", "(I)Landroid/graphics/Insets;") : NULL;
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env); /* before Android 11 */
            get = NULL;
        }
        if (get != NULL) {
            /* WindowInsets.Type: statusBars 1, navigationBars 2, captionBar 4 (systemBars), displayCutout 128. */
            jobject both = (*env)->CallObjectMethod(env, insets, get, (jint)(1 | 2 | 4 | 128));
            if ((*env)->ExceptionCheck(env)) {
                (*env)->ExceptionClear(env);
                both = NULL;
            }
            found = both != NULL && cee_android_field(env, both, "left", &safe[0]) &&
                    cee_android_field(env, both, "top", &safe[1]) && cee_android_field(env, both, "right", &safe[2]) &&
                    cee_android_field(env, both, "bottom", &safe[3]);
        } else {
            found = cee_android_int(env, insets, "getSystemWindowInsetLeft", &safe[0]) &&
                    cee_android_int(env, insets, "getSystemWindowInsetTop", &safe[1]) &&
                    cee_android_int(env, insets, "getSystemWindowInsetRight", &safe[2]) &&
                    cee_android_int(env, insets, "getSystemWindowInsetBottom", &safe[3]);
            jobject cutout = cee_android_object(env, insets, "getDisplayCutout", "()Landroid/view/DisplayCutout;");
            static const char *const sides[4] = {"getSafeInsetLeft", "getSafeInsetTop", "getSafeInsetRight",
                                                 "getSafeInsetBottom"};
            for (int k = 0; found && cutout != NULL && k < 4; k++) {
                int32_t cut = 0;
                if (cee_android_int(env, cutout, sides[k], &cut) && cut > safe[k]) {
                    safe[k] = cut;
                }
            }
        }
    }
    (void)(*env)->PopLocalFrame(env, NULL);
    if (found) {
        pthread_mutex_lock(&cee_android.lock);
        for (int k = 0; k < 4; k++) {
            cee_android.safe[k] = safe[k] > 0 ? safe[k] : 0;
        }
        pthread_mutex_unlock(&cee_android.lock);
    }
}

/* The density's scale, pixels per point, from the activity's configuration. */
static void cee_android_read_scale(ANativeActivity *activity) {
    AConfiguration *configuration = AConfiguration_new();
    if (configuration != NULL) {
        AConfiguration_fromAssetManager(configuration, activity->assetManager);
        int32_t density = AConfiguration_getDensity(configuration);
        if (density > 0 && density != ACONFIGURATION_DENSITY_ANY && density != ACONFIGURATION_DENSITY_NONE) {
            pthread_mutex_lock(&cee_android.lock);
            cee_android.scale = (float)density / 160.0f;
            pthread_mutex_unlock(&cee_android.lock);
        }
        AConfiguration_delete(configuration);
    }
}

/* The window's geometry may have changed (its size after a rotation, its insets, the display's density): the insets
   and the scale are read again, and the program's pump, woken, takes them with the window's size (ADR 0156). */
static void cee_android_window_resized(ANativeActivity *activity, ANativeWindow *window) {
    (void)window;
    cee_android_read_insets(activity);
    cee_android_wake_program();
}

static void cee_android_content_rect(ANativeActivity *activity, const ARect *rect) {
    (void)rect;
    cee_android_read_insets(activity);
    cee_android_wake_program();
}

static void cee_android_configuration(ANativeActivity *activity) {
    cee_android_read_scale(activity);
    cee_android_read_insets(activity);
    cee_android_wake_program();
}

static void cee_android_window_created(ANativeActivity *activity, ANativeWindow *window) {
    cee_android_read_insets(activity);
    pthread_mutex_lock(&cee_android.lock);
    cee_android.window = window;
    pthread_mutex_unlock(&cee_android.lock);
    cee_android_wake_program();
}

/* The window goes when this returns: it is taken under the lock, which a present holds while it draws. */
static void cee_android_window_destroyed(ANativeActivity *activity, ANativeWindow *window) {
    (void)activity;
    (void)window;
    pthread_mutex_lock(&cee_android.lock);
    cee_android.window = NULL;
    pthread_mutex_unlock(&cee_android.lock);
    cee_android_wake_program();
}

static void cee_android_queue_created(ANativeActivity *activity, AInputQueue *queue) {
    (void)activity;
    pthread_mutex_lock(&cee_android.lock);
    cee_android.queue = queue;
    pthread_mutex_unlock(&cee_android.lock);
    cee_android_wake_program();
}

/* The queue goes when this returns: it is detached here, under the lock, which the program's pump holds while it
   reads the queue (detaching from another thread is safe: it removes the queue's descriptor from the looper). */
static void cee_android_drop_queue(AInputQueue *queue) {
    if (cee_android.attached == queue && queue != NULL) {
        AInputQueue_detachLooper(queue);
        cee_android.attached = NULL;
    }
    if (cee_android.queue == queue) {
        cee_android.queue = NULL;
    }
}

static void cee_android_queue_destroyed(ANativeActivity *activity, AInputQueue *queue) {
    (void)activity;
    pthread_mutex_lock(&cee_android.lock);
    cee_android_drop_queue(queue);
    pthread_mutex_unlock(&cee_android.lock);
}

static void cee_android_focus_changed(ANativeActivity *activity, int focused) {
    cee_android_read_insets(activity); /* the window's views are attached by now, when they were not at its creation */
    pthread_mutex_lock(&cee_android.lock);
    cee_android.focused = focused != 0;
    pthread_mutex_unlock(&cee_android.lock);
    cee_android_wake_program();
}

/* The activity ends (Back, or the system): the program gets Closed, and nothing touches the activity again. */
static void cee_android_destroy(ANativeActivity *activity) {
    pthread_mutex_lock(&cee_android.lock);
    cee_android.destroyed = 1;
    if (cee_android.activity == activity) {
        cee_android.activity = NULL;
    }
    cee_android_drop_queue(cee_android.attached); /* the activity may dispose of its queue without a callback */
    cee_android_drop_queue(cee_android.queue);
    pthread_mutex_unlock(&cee_android.lock);
    cee_android_wake_program();
}

/* Standard output and error, which Android sends nowhere, read from a pipe into logcat ("ceesafe"), line by line. */
static int cee_android_output[2] = {-1, -1};
static pthread_t cee_android_logger_thread;
static int cee_android_logging;

static void *cee_android_logger(void *argument) {
    (void)argument;
    char line[1024];
    size_t used = 0;
    for (;;) {
        ssize_t n = read(cee_android_output[0], line + used, sizeof line - 1u - used);
        if (n <= 0) {
            break;
        }
        used += (size_t)n;
        size_t start = 0;
        for (size_t k = 0; k < used; k++) {
            if (line[k] == '\n') {
                line[k] = '\0';
                __android_log_write(ANDROID_LOG_INFO, "ceesafe", line + start);
                start = k + 1;
            }
        }
        memmove(line, line + start, used - start);
        used -= start;
        if (used == sizeof line - 1u) {
            line[used] = '\0';
            __android_log_write(ANDROID_LOG_INFO, "ceesafe", line);
            used = 0;
        }
    }
    return NULL;
}

int main(int argc, char **argv);

/* The program's thread: its looper, then main; when main returns, the activity is finished and the process exits. */
static void *cee_android_program(void *argument) {
    (void)argument;
    cee_android_program_tid = (pid_t)syscall(SYS_gettid);
    cee_android.looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    char name[] = "ceesafe";
    char *argv[] = {name, NULL};
    int code = main(1, argv);
    fflush(stdout);
    fflush(stderr);
    pthread_mutex_lock(&cee_android.lock);
    cee_android_drop_queue(cee_android.attached); /* detached while the looper still exists */
    cee_android.looper = NULL; /* the thread's looper ends with it: the activity's callbacks must not wake it */
    pthread_mutex_unlock(&cee_android.lock);
    __android_log_print(ANDROID_LOG_INFO, "ceesafe", "main returned %d", code);
    pthread_mutex_lock(&cee_android.lock);
    if (cee_android.activity != NULL) {
        ANativeActivity_finish(cee_android.activity); /* not after onDestroy, which cleared it */
    }
    pthread_mutex_unlock(&cee_android.lock);
    /* As on every other system, the process ends with main. Not by the thread's own exit, which would run its
       thread-local destructors after ceesafe_shutdown has ended the runtime, nor by exit(), whose static destructors
       Android's own threads (hwui) still use: both abort in bionic (a destroyed mutex). The output pipe is closed and
       the logger drained first, so the program's last lines reach logcat. */
    if (cee_android_logging) {
        close(1);
        close(2);
        close(cee_android_output[1]);
        pthread_join(cee_android_logger_thread, NULL);
    }
    _exit(code);
}

/* The library's entry, which a NativeActivity calls on its thread (ADR 0154 §2.1). */
__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *activity, void *saved,
                                                                     size_t saved_size) {
    (void)saved;
    (void)saved_size;
    cee_android.activity = activity;
    activity->callbacks->onNativeWindowCreated = cee_android_window_created;
    activity->callbacks->onNativeWindowDestroyed = cee_android_window_destroyed;
    activity->callbacks->onInputQueueCreated = cee_android_queue_created;
    activity->callbacks->onInputQueueDestroyed = cee_android_queue_destroyed;
    activity->callbacks->onWindowFocusChanged = cee_android_focus_changed;
    activity->callbacks->onDestroy = cee_android_destroy;
    activity->callbacks->onNativeWindowResized = cee_android_window_resized;
    activity->callbacks->onNativeWindowRedrawNeeded = cee_android_window_resized;
    activity->callbacks->onContentRectChanged = cee_android_content_rect;
    activity->callbacks->onConfigurationChanged = cee_android_configuration;
    cee_android_read_scale(activity);
    static int started;
    if (started) {
        return; /* the activity again, in the same process: the program is still running */
    }
    started = 1;
    if (pipe(cee_android_output) == 0) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        dup2(cee_android_output[1], 1);
        dup2(cee_android_output[1], 2);
        cee_android_logging = pthread_create(&cee_android_logger_thread, NULL, cee_android_logger, NULL) == 0;
    }
    pthread_t program;
    if (pthread_create(&program, NULL, cee_android_program, NULL) == 0) {
        pthread_detach(program);
    }
}

/* ---- The window, on the program's thread ---- */

static int cee_native_open(struct cee_window *w, const char *title) {
    (void)title;
    pthread_mutex_lock(&cee_android.lock);
    int started = cee_android.activity != NULL && cee_android.looper != NULL;
    pthread_mutex_unlock(&cee_android.lock);
    if (!started) {
        return CEE_WINDOW_NO_DISPLAY; /* not started by an activity, or it has ended */
    }
    if (cee_android_shown != NULL) {
        return CEE_WINDOW_RESOURCES; /* one window, the activity's */
    }
    /* The activity's window comes after the activity starts: wait for it, ten seconds at most. */
    for (int k = 0; k < 100; k++) {
        pthread_mutex_lock(&cee_android.lock);
        ANativeWindow *window = cee_android.window;
        pthread_mutex_unlock(&cee_android.lock);
        if (window != NULL) {
            break;
        }
        (void)ALooper_pollOnce(100, NULL, NULL, NULL);
    }
    pthread_mutex_lock(&cee_android.lock);
    ANativeWindow *window = cee_android.window;
    int32_t width = window != NULL ? ANativeWindow_getWidth(window) : 0;
    int32_t height = window != NULL ? ANativeWindow_getHeight(window) : 0;
    float density = cee_android.scale;
    int32_t safe[4];
    memcpy(safe, cee_android.safe, sizeof safe);
    pthread_mutex_unlock(&cee_android.lock);
    if (window == NULL || width <= 0 || height <= 0) {
        return CEE_WINDOW_NO_DISPLAY;
    }
    struct cee_droid *m = (struct cee_droid *)ceesafe_platform_alloc_zeroed(sizeof *m);
    if (m == NULL) {
        return CEE_WINDOW_RESOURCES;
    }
    m->opened = 1;
    w->native = m;
    w->scale = density > 0.0f ? density : 1.0f;
    w->width = (uint32_t)((float)width / w->scale + 0.5f);
    w->height = (uint32_t)((float)height / w->scale + 0.5f);
    for (int k = 0; k < 4; k++) {
        w->safe[k] = (float)safe[k] / w->scale;
    }
    cee_android_shown = w;
    return 0;
}

static void cee_native_close(struct cee_window *w) {
    if (cee_android_shown == w) {
        cee_android_shown = NULL;
    }
    ceesafe_platform_free(w->native);
    w->native = NULL;
}

/* One input event: a motion's pointers, each a finger under its pointer id + 1, in points, at the event's time (the
   monotonic clock's nanoseconds) (ADR 0155): a batched move's historical samples first, so a fling's velocity is not
   undersampled. A mouse or a trackpad is the pointer whether touches are on or not. Whether it was taken. */
static int cee_android_event(struct cee_window *w, AInputEvent *event) {
    float scale = w->scale > 0.0f ? w->scale : 1.0f;
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
        int touchscreen = (AInputEvent_getSource(event) & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN;
        int on = w->touches_on;
        if (!touchscreen) {
            w->touches_on = 0; /* the pointer, for this event */
        }
        int32_t action = AMotionEvent_getAction(event);
        int32_t kind = action & AMOTION_EVENT_ACTION_MASK;
        size_t index = (size_t)((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                                AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        size_t count = AMotionEvent_getPointerCount(event);
        uint64_t time = (uint64_t)AMotionEvent_getEventTime(event);
        if (kind == AMOTION_EVENT_ACTION_DOWN || kind == AMOTION_EVENT_ACTION_POINTER_DOWN) {
            size_t at = kind == AMOTION_EVENT_ACTION_DOWN ? 0 : index;
            if (at < count) {
                cee_touch_began(w, (uintptr_t)AMotionEvent_getPointerId(event, at) + 1u,
                                AMotionEvent_getX(event, at) / scale, AMotionEvent_getY(event, at) / scale, time);
            }
        } else if (kind == AMOTION_EVENT_ACTION_MOVE) {
            size_t history = AMotionEvent_getHistorySize(event);
            for (size_t h = 0; h <= history; h++) {
                for (size_t k = 0; k < count; k++) {
                    uintptr_t key = (uintptr_t)AMotionEvent_getPointerId(event, k) + 1u;
                    if (h < history) {
                        cee_touch_moved(w, key, AMotionEvent_getHistoricalX(event, k, h) / scale,
                                        AMotionEvent_getHistoricalY(event, k, h) / scale,
                                        (uint64_t)AMotionEvent_getHistoricalEventTime(event, h));
                    } else {
                        cee_touch_moved(w, key, AMotionEvent_getX(event, k) / scale,
                                        AMotionEvent_getY(event, k) / scale, time);
                    }
                }
            }
        } else if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_POINTER_UP) {
            size_t at = kind == AMOTION_EVENT_ACTION_UP ? 0 : index;
            if (at < count) {
                cee_touch_ended(w, (uintptr_t)AMotionEvent_getPointerId(event, at) + 1u,
                                AMotionEvent_getX(event, at) / scale, AMotionEvent_getY(event, at) / scale, time, 0);
            }
        } else if (kind == AMOTION_EVENT_ACTION_CANCEL) {
            for (size_t k = 0; k < count; k++) {
                cee_touch_ended(w, (uintptr_t)AMotionEvent_getPointerId(event, k) + 1u,
                                AMotionEvent_getX(event, k) / scale, AMotionEvent_getY(event, k) / scale, time, 1);
            }
        }
        w->touches_on = on;
        return 1;
    }
    return 0; /* keys, Back among them, are the system's: Back ends the activity, which gives Closed (onDestroy) */
}

/* Takes what the activity recorded (the queue to attach or detach, focus, the end), then the input events, then the
   window's scale, size and safe area. */
static void cee_native_pump(struct cee_window *w) {
    (void)ALooper_pollOnce(0, NULL, NULL, NULL); /* the looper's own wake, taken without waiting */
    pthread_mutex_lock(&cee_android.lock);
    if (cee_android.queue != NULL && cee_android.attached != cee_android.queue && cee_android.looper != NULL) {
        if (cee_android.attached != NULL) {
            AInputQueue_detachLooper(cee_android.attached);
        }
        AInputQueue_attachLooper(cee_android.queue, cee_android.looper, CEE_ANDROID_INPUT, NULL, NULL);
        cee_android.attached = cee_android.queue;
    }
    int focused = cee_android.focused, destroyed = cee_android.destroyed;
    ANativeWindow *window = cee_android.window;
    int32_t width = window != NULL ? ANativeWindow_getWidth(window) : 0;
    int32_t height = window != NULL ? ANativeWindow_getHeight(window) : 0;
    float density = cee_android.scale;
    int32_t safe[4];
    memcpy(safe, cee_android.safe, sizeof safe);
    cee_android.destroyed = 0;
    /* The queue is read under the lock: the activity's thread detaches and disposes of it under the same lock. */
    AInputQueue *queue = cee_android.attached;
    if (queue != NULL && w != NULL && w->native != NULL) {
        AInputEvent *event = NULL;
        for (int k = 0; k < 256 && AInputQueue_hasEvents(queue) > 0 && AInputQueue_getEvent(queue, &event) >= 0; k++) {
            if (AInputQueue_preDispatchEvent(queue, event)) {
                continue;
            }
            AInputQueue_finishEvent(queue, event, cee_android_event(w, event));
        }
    }
    pthread_mutex_unlock(&cee_android.lock);
    if (w == NULL || w->native == NULL) {
        return;
    }
    if (focused != w->focused) {
        cee_window_focus(w, focused);
    }
    cee_window_rescaled(w, density); /* the display's density changed (ADR 0156) */
    float scale = w->scale > 0.0f ? w->scale : 1.0f;
    if (width > 0 && height > 0) {
        cee_window_resized(w, (uint32_t)((float)width / scale + 0.5f), (uint32_t)((float)height / scale + 0.5f));
    }
    cee_window_safe_area(w, (float)safe[0] / scale, (float)safe[1] / scale, (float)safe[2] / scale,
                         (float)safe[3] / scale);
    if (destroyed) {
        cee_window_push(w, CEE_EVENT_CLOSED, 0, 0, 0, 0, 0.0f, 0.0f);
    }
}

static int32_t cee_native_handles(struct cee_window *w, uint64_t *first, uint64_t *second) {
    (void)w;
    pthread_mutex_lock(&cee_android.lock);
    *first = (uint64_t)(uintptr_t)cee_android.window;
    *second = (uint64_t)(uintptr_t)cee_android.activity;
    pthread_mutex_unlock(&cee_android.lock);
    return CEE_HANDLE_ANDROID;
}

/* The frame into the window's buffer, scaled by the system to the window when its size differs (the buffer's
   geometry is the frame's). Under the lock, so the activity's thread cannot take the window away meanwhile. */
static int32_t cee_native_present(struct cee_window *w, const uint32_t *pixels, uint32_t width, uint32_t height) {
    (void)w;
    pthread_mutex_lock(&cee_android.lock);
    ANativeWindow *window = cee_android.window;
    if (window == NULL) {
        pthread_mutex_unlock(&cee_android.lock);
        return 0; /* no window while the activity is in the background: nothing to show */
    }
    int32_t result = 0;
    ANativeWindow_Buffer buffer;
    if (ANativeWindow_setBuffersGeometry(window, (int32_t)width, (int32_t)height, WINDOW_FORMAT_RGBX_8888) != 0 ||
        ANativeWindow_lock(window, &buffer, NULL) != 0) {
        result = -CEE_WINDOW_RESOURCES;
    } else {
        uint32_t rows = (uint32_t)buffer.height < height ? (uint32_t)buffer.height : height;
        uint32_t columns = (uint32_t)buffer.width < width ? (uint32_t)buffer.width : width;
        for (uint32_t y = 0; y < rows; y++) {
            memcpy((uint32_t *)buffer.bits + (size_t)y * (size_t)buffer.stride, pixels + (size_t)y * width,
                   (size_t)columns * 4u);
        }
        ANativeWindow_unlockAndPost(window);
    }
    pthread_mutex_unlock(&cee_android.lock);
    return result;
}

static int32_t cee_native_present_boxes(struct cee_window *w, const uint32_t *pixels, uint32_t width, uint32_t height,
                                        const struct cee_window_box *boxes, size_t count) {
    (void)boxes;
    (void)count;
    return cee_native_present(w, pixels, width, height);
}

static void cee_native_cursor(struct cee_window *w) { (void)w; }
static void cee_native_title(struct cee_window *w, const char *title) { (void)w; (void)title; }
static void cee_native_text_input(struct cee_window *w, int on) { (void)w; (void)on; } /* P4 step 5 */
static void cee_native_text_area(struct cee_window *w) { (void)w; }

/* Android's clipboard is a Java service (ClipboardManager): not covered yet (ADR 0154 §4). */
static int32_t cee_native_clipboard_set(struct cee_window *w, const char *text, size_t length) {
    (void)w;
    (void)text;
    (void)length;
    return -CEE_WINDOW_UNSUPPORTED;
}

static int cee_native_clipboard_get(struct cee_window *w) {
    (void)w;
    return 0;
}

static void cee_native_wake(void) { cee_android_wake_program(); }

/* Until the looper is woken (an activity callback, a wake, input) or `seconds`. */
static void cee_native_wait(struct cee_window *w, double seconds) {
    (void)w;
    double ms = seconds * 1000.0 + 0.999;
    (void)ALooper_pollOnce(ms <= 0.0 ? 0 : ms >= 2147483647.0 ? 2147483647 : (int)ms, NULL, NULL, NULL);
}

static void cee_native_wait_any(double seconds) { cee_native_wait(NULL, seconds); }

static int32_t cee_native_menu(struct cee_window *w) {
    (void)w;
    return -CEE_WINDOW_UNSUPPORTED;
}

static int32_t cee_native_dialog(struct cee_window *w, int kind, const char *title, const char *name,
                                 const char *extensions, size_t extensions_length) {
    (void)w;
    (void)kind;
    (void)title;
    (void)name;
    (void)extensions;
    (void)extensions_length;
    return -CEE_WINDOW_UNSUPPORTED;
}

#elif defined(__linux__) || defined(__FreeBSD__)
/* X11 through libX11.so.6, loaded with dlopen (Linux, and FreeBSD with Xorg: ADR 0149): the declarations below are
   Xlib's (X11/Xlib.h, LP64), so no headers are needed to build. A frame is converted to the visual's pixel layout,
   scaled to the window's size, and drawn with XPutImage. */
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
    int (*translate_coordinates)(cee_x_display *, cee_xid, cee_xid, int, int, int *, int *, cee_xid *); /* ADR 0150 */
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
    /* Drops (ADR 0150): XDND's atoms, and the drag in progress: its source window and protocol version, whether it
       offers text/uri-list, the conversion asked for (0 none, 1 to count the files while it hovers, 2 the drop's),
       whether the count is known and what it is, and where the pointer is in the content. */
    unsigned long xdnd_aware, xdnd_enter, xdnd_position, xdnd_status, xdnd_leave, xdnd_drop, xdnd_finished;
    unsigned long xdnd_selection, xdnd_copy, xdnd_type_list, uri_list_atom, xdnd_property;
    cee_xid xdnd_source;
    int xdnd_version, xdnd_offered, xdnd_converting, xdnd_known;
    uint32_t xdnd_files;
    float xdnd_x, xdnd_y;
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
    CEE_X(translate_coordinates, int (*)(cee_x_display *, cee_xid, cee_xid, int, int, int *, int *, cee_xid *),
          "XTranslateCoordinates")
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
    /* A drop target, XDND version 5 (ADR 0150). */
    n->xdnd_aware = cee_x.intern_atom(display, "XdndAware", 0);
    n->xdnd_enter = cee_x.intern_atom(display, "XdndEnter", 0);
    n->xdnd_position = cee_x.intern_atom(display, "XdndPosition", 0);
    n->xdnd_status = cee_x.intern_atom(display, "XdndStatus", 0);
    n->xdnd_leave = cee_x.intern_atom(display, "XdndLeave", 0);
    n->xdnd_drop = cee_x.intern_atom(display, "XdndDrop", 0);
    n->xdnd_finished = cee_x.intern_atom(display, "XdndFinished", 0);
    n->xdnd_selection = cee_x.intern_atom(display, "XdndSelection", 0);
    n->xdnd_copy = cee_x.intern_atom(display, "XdndActionCopy", 0);
    n->xdnd_type_list = cee_x.intern_atom(display, "XdndTypeList", 0);
    n->uri_list_atom = cee_x.intern_atom(display, "text/uri-list", 0);
    n->xdnd_property = cee_x.intern_atom(display, "CEESAFE_XDND", 0);
    unsigned long version = 5; /* format 32 is longs */
    cee_x.change_property(display, n->window, n->xdnd_aware, 4 /* XA_ATOM */, 32, 0 /* PropModeReplace */,
                          (const unsigned char *)&version, 1);
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

/* Drops (ADR 0150): the XDND protocol, version 5, as a target. The window's XdndAware says so. A drag's source sends
   XdndEnter (its types), XdndPosition (root coordinates; answered with XdndStatus), XdndLeave or XdndDrop; the target
   converts XdndSelection to text/uri-list and answers a drop with XdndFinished. The files are counted while the drag
   hovers, by a conversion at its first position, so a drag shows (dragOver) only once its count is known; the drop
   converts again, at the drop's time. A drag without text/uri-list, or whose list holds no local file, is refused
   (XdndStatus without accept), so the system shows no drop. There are no INCR transfers: a list longer than one
   property is no files. */

/* Sends an XDND client message to the drag's source, from this window. */
static void cee_x_xdnd_send(struct cee_x11 *n, unsigned long type, long a, long b, long c, long d) {
    cee_x_event e;
    memset(&e, 0, sizeof e);
    e.client.type = 33; /* ClientMessage */
    e.client.display = n->display;
    e.client.window = n->xdnd_source;
    e.client.message_type = type;
    e.client.format = 32;
    e.client.data[0] = (long)n->window;
    e.client.data[1] = a;
    e.client.data[2] = b;
    e.client.data[3] = c;
    e.client.data[4] = d;
    cee_x.send_event(n->display, n->xdnd_source, 0, 0 /* NoEventMask */, &e);
    cee_x.flush(n->display);
}

/* No drag: the protocol's state is forgotten and the window's drag ends. */
static void cee_x_xdnd_reset(struct cee_window *w) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    n->xdnd_source = 0;
    n->xdnd_version = 0;
    n->xdnd_offered = 0;
    n->xdnd_converting = 0;
    n->xdnd_known = 0;
    n->xdnd_files = 0;
    cee_window_drag(w, 0, 0.0f, 0.0f);
}

static int cee_x_hex(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

/* Whether a URI of the list is a local file (`file:///path`, `file://localhost/path`, `file://HOST/path` with this
   host's name, or `file:/path`); with `p`, its path, percent-decoded, is added (0 in *ok when memory ran out). */
static int cee_x_uri_file(const char *uri, size_t length, struct cee_window_paths *p, int *ok) {
    static const char scheme[] = "file:";
    if (length < 6) {
        return 0;
    }
    for (size_t k = 0; k < 5; k++) {
        char c = uri[k] >= 'A' && uri[k] <= 'Z' ? (char)(uri[k] - 'A' + 'a') : uri[k];
        if (c != scheme[k]) {
            return 0;
        }
    }
    const char *path = uri + 5;
    size_t rest = length - 5;
    if (rest >= 2 && path[0] == '/' && path[1] == '/') {
        size_t slash = 2;
        while (slash < rest && path[slash] != '/') {
            slash++;
        }
        size_t host = slash - 2;
        char name[256];
        if (slash == rest) {
            return 0;
        }
        if (host != 0 && !(host == 9 && memcmp(path + 2, "localhost", 9) == 0)) {
            if (gethostname(name, sizeof name) != 0) {
                return 0;
            }
            name[sizeof name - 1] = '\0';
            if (strlen(name) != host || memcmp(name, path + 2, host) != 0) {
                return 0; /* another machine's file */
            }
        }
        path += slash;
        rest -= slash;
    } else if (rest == 0 || path[0] != '/') {
        return 0;
    }
    size_t end = 0;
    while (end < rest && path[end] != '?' && path[end] != '#') {
        if (path[end] == '%' && (end + 2 >= rest || cee_x_hex(path[end + 1]) < 0 || cee_x_hex(path[end + 2]) < 0)) {
            return 0; /* a malformed escape */
        }
        end += path[end] == '%' ? 3u : 1u;
    }
    if (p == NULL || !*ok) {
        return 1;
    }
    char *decoded = (char *)ceesafe_platform_alloc(end + 1u);
    if (decoded == NULL) {
        *ok = 0;
        return 1;
    }
    size_t n = 0;
    for (size_t k = 0; k < end; k++) {
        if (path[k] == '%') {
            decoded[n++] = (char)(cee_x_hex(path[k + 1]) * 16 + cee_x_hex(path[k + 2]));
            k += 2;
        } else {
            decoded[n++] = path[k];
        }
    }
    *ok = cee_paths_add(p, decoded, n); /* one with a zero byte (%00) is left out */
    ceesafe_platform_free(decoded);
    return 1;
}

/* The local files of a text/uri-list (CRLF-separated, `#` comments): how many, and with `p`, their paths. */
static uint32_t cee_x_uri_list(const char *text, size_t length, struct cee_window_paths *p) {
    uint32_t count = 0;
    size_t start = 0;
    int ok = 1;
    for (size_t k = 0; k <= length; k++) {
        if (k == length || text[k] == '\n') {
            size_t end = k;
            if (end > start && text[end - 1] == '\r') {
                end--;
            }
            if (end > start && text[start] != '#' && count < 0xffffffffu) {
                count += (uint32_t)cee_x_uri_file(text + start, end - start, p, &ok);
            }
            start = k + 1;
        }
    }
    return count;
}

/* The SelectionNotify of an XDND conversion: the hover's count, or the drop, which is then finished. */
static void cee_x_xdnd_data(struct cee_window *w, const cee_x_selection *s) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    int stage = n->xdnd_converting;
    n->xdnd_converting = 0;
    struct cee_window_paths p = {NULL, 0, 0};
    uint32_t files = 0;
    if (s->property != 0) {
        unsigned long type = 0, items = 0, after = 0;
        int format = 0;
        unsigned char *data = NULL;
        if (cee_x.get_window_property(n->display, n->window, n->xdnd_property, 0, 0x1fffffffL, 1 /* delete */,
                                      0 /* AnyPropertyType */, &type, &format, &items, &after, &data) == 0) {
            if (stage != 0 && type != n->incr_atom && format == 8 && data != NULL) {
                files = cee_x_uri_list((const char *)data, (size_t)items, stage == 2 ? &p : NULL);
            }
            if (data != NULL) {
                cee_x.release(data);
            }
        }
    }
    if (stage == 0 || n->xdnd_source == 0) {
        ceesafe_platform_free(p.text);
        return; /* a conversion no drag waits for */
    }
    if (stage == 1) {
        n->xdnd_known = 1;
        n->xdnd_files = files;
        cee_window_drag(w, files, n->xdnd_x, n->xdnd_y);
        return;
    }
    int taken = p.length > 0;
    cee_window_dropped(w, n->xdnd_x, n->xdnd_y, &p);
    if (n->xdnd_version >= 2) {
        cee_x_xdnd_send(n, n->xdnd_finished, taken, taken ? (long)n->xdnd_copy : 0, 0, 0);
    }
    cee_x_xdnd_reset(w);
}

/* An XDND client message from a drag's source. */
static void cee_x_xdnd_message(struct cee_window *w, const cee_x_client *c) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    cee_xid source = (cee_xid)c->data[0];
    if (c->message_type == n->xdnd_enter) {
        cee_x_xdnd_reset(w);
        int version = (int)(((unsigned long)c->data[1] >> 24) & 0xffu);
        if (version > 5 || source == 0) {
            return; /* a version newer than the window's XdndAware: the source should not have sent it */
        }
        n->xdnd_source = source;
        n->xdnd_version = version;
        if ((unsigned long)c->data[1] & 1ul) { /* more than three types: XdndTypeList on the source */
            unsigned long type = 0, items = 0, after = 0;
            int format = 0;
            unsigned char *data = NULL;
            if (cee_x.get_window_property(n->display, source, n->xdnd_type_list, 0, 4096, 0, 4 /* XA_ATOM */, &type,
                                          &format, &items, &after, &data) == 0 &&
                data != NULL) {
                for (unsigned long k = 0; format == 32 && k < items; k++) {
                    unsigned long atom;
                    memcpy(&atom, data + k * sizeof atom, sizeof atom); /* format 32 is longs */
                    n->xdnd_offered |= atom == n->uri_list_atom;
                }
            }
            if (data != NULL) {
                cee_x.release(data);
            }
        } else {
            for (int k = 2; k < 5; k++) {
                n->xdnd_offered |= (unsigned long)c->data[k] == n->uri_list_atom;
            }
        }
        return;
    }
    if (source == 0 || source != n->xdnd_source) {
        return; /* another drag's, or one that ended */
    }
    if (c->message_type == n->xdnd_position) {
        int rx = (int)(((unsigned long)c->data[2] >> 16) & 0xffffu), ry = (int)((unsigned long)c->data[2] & 0xffffu);
        int x = 0, y = 0;
        cee_xid child = 0;
        cee_x.translate_coordinates(n->display, cee_x.root_window(n->display, n->screen), n->window, rx, ry, &x, &y,
                                    &child);
        n->xdnd_x = (float)x;
        n->xdnd_y = (float)y;
        if (n->xdnd_offered && !n->xdnd_known && n->xdnd_converting == 0) {
            cee_x.convert_selection(n->display, n->xdnd_selection, n->uri_list_atom, n->xdnd_property, n->window,
                                    n->xdnd_version >= 1 ? (unsigned long)c->data[3] : 0ul);
            n->xdnd_converting = 1;
        }
        if (n->xdnd_known) {
            cee_window_drag(w, n->xdnd_files, n->xdnd_x, n->xdnd_y);
        }
        int accept = n->xdnd_offered && (!n->xdnd_known || n->xdnd_files > 0);
        cee_x_xdnd_send(n, n->xdnd_status, accept | 2, 0, 0, accept ? (long)n->xdnd_copy : 0);
    } else if (c->message_type == n->xdnd_leave) {
        cee_x_xdnd_reset(w);
    } else if (c->message_type == n->xdnd_drop) {
        cee_window_drag(w, 0, 0.0f, 0.0f);
        if (n->xdnd_offered && (!n->xdnd_known || n->xdnd_files > 0)) {
            cee_x.convert_selection(n->display, n->xdnd_selection, n->uri_list_atom, n->xdnd_property, n->window,
                                    n->xdnd_version >= 1 ? (unsigned long)c->data[2] : 0ul);
            cee_x.flush(n->display);
            n->xdnd_converting = 2;
        } else {
            if (n->xdnd_version >= 2) {
                cee_x_xdnd_send(n, n->xdnd_finished, 0, 0, 0, 0);
            }
            cee_x_xdnd_reset(w);
        }
    }
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
            } else {
                cee_x_xdnd_message(w, &e.client); /* ADR 0150 */
            }
            break;
        case 31: /* SelectionNotify: an XDND conversion's (the clipboard's is waited for where it is asked) */
            if (e.selection.selection == n->xdnd_selection) {
                cee_x_xdnd_data(w, &e.selection);
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
            if (e.selection.selection == n->xdnd_selection) {
                cee_x_xdnd_data(w, &e.selection); /* a drop's (ADR 0150), not the clipboard's */
                continue;
            }
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

/* waitAny's wait (ADR 0145): every window has its own connection, so `poll` takes each one's descriptor, after Xlib's
   queues, with the process's pipe. Past 16 windows the descriptors are allocated; if that fails, the first 15 are
   polled for at most 10 ms at a time, so the loop around this still sees the others. */
static void cee_native_wait_any(double seconds) {
    struct pollfd few[16];
    struct pollfd *p = few;
    size_t count = 1;
    for (struct cee_window *w = cee_wake_windows; w != NULL; w = w->wake_link) {
        if (!w->scripted) {
            if (cee_x.pending(((struct cee_x11 *)w->native)->display) > 0) {
                return;
            }
            count++;
        }
    }
    if (count > 16) {
        p = (struct pollfd *)ceesafe_platform_alloc(count * sizeof *p);
        if (p == NULL) {
            p = few;
            count = 16;
            seconds = seconds < 0.01 ? seconds : 0.01;
        }
    }
    size_t n = 0;
    p[n].fd = cee_wake_any_read;
    p[n].events = POLLIN;
    p[n++].revents = 0;
    for (struct cee_window *w = cee_wake_windows; w != NULL && n < count; w = w->wake_link) {
        if (!w->scripted) {
            p[n].fd = cee_x.connection_number(((struct cee_x11 *)w->native)->display);
            p[n].events = POLLIN;
            p[n++].revents = 0;
        }
    }
    double ms = seconds * 1000.0 + 0.999;
    poll(p, (nfds_t)n, ms <= 0.0 ? 0 : ms >= 2147483647.0 ? 2147483647 : (int)ms);
    if (p != few) {
        ceesafe_platform_free(p);
    }
}

/* Menus (ADR 0146): X11 has no menu bar of its own, so a program draws its menus. */
static int32_t cee_native_menu(struct cee_window *w) {
    (void)w;
    return -CEE_WINDOW_UNSUPPORTED;
}

/* File dialogs (ADR 0146): another program, zenity or kdialog (kdialog first on KDE), found on PATH. Its standard
   output is the answer, a path a line; exit status 1 is a cancel. While it runs, every window is pumped, so each
   repaints its last frame and answers the clipboard; their input is queued as usual, since the dialog is another
   program's window and not modal to them. */
#include <spawn.h>
#include <sys/wait.h>
#if defined(__FreeBSD__)
extern char **environ; /* POSIX's, which FreeBSD's unistd.h does not declare (glibc does, under _GNU_SOURCE) */
#endif

/* `a` then `b`, allocated, or NULL. */
static char *cee_x_join(const char *a, const char *b) {
    size_t n = strlen(a), m = strlen(b);
    char *s = (char *)ceesafe_platform_alloc(n + m + 1u);
    if (s != NULL) {
        memcpy(s, a, n);
        memcpy(s + n, b, m + 1u);
    }
    return s;
}

/* The filter's patterns being built: "*.png *.jpg". */
struct cee_x_patterns {
    char *text;
    size_t length;
};

static void cee_x_pattern(const char *extension, size_t length, void *context) {
    struct cee_x_patterns *p = (struct cee_x_patterns *)context;
    if (p->length > 0) {
        p->text[p->length++] = ' ';
    }
    p->text[p->length++] = '*';
    p->text[p->length++] = '.';
    memcpy(p->text + p->length, extension, length);
    p->length += length;
    p->text[p->length] = '\0';
}

/* Runs argv with its output on a pipe, its input and errors on /dev/null, and waits, pumping every window: 0 with the
   answer, 1 if no such program is on PATH, or minus an error. */
static int32_t cee_x_run_dialog(struct cee_window *w, char *const *argv, int first) {
    int ends[2];
    if (pipe(ends) != 0) {
        return -CEE_WINDOW_RESOURCES;
    }
    (void)fcntl(ends[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    pid_t pid = -1;
    int started = posix_spawn_file_actions_init(&actions);
    if (started == 0) {
        started = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        if (started == 0) {
            started = posix_spawn_file_actions_adddup2(&actions, ends[1], 1);
        }
        if (started == 0) {
            started = posix_spawn_file_actions_addclose(&actions, ends[1]);
        }
        if (started == 0) {
            started = posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
        }
        if (started == 0) {
            started = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
        }
        (void)posix_spawn_file_actions_destroy(&actions);
    }
    close(ends[1]);
    if (started != 0) {
        close(ends[0]);
        return started == ENOENT ? 1 : -CEE_WINDOW_RESOURCES;
    }
    char *out = NULL;
    size_t used = 0, capacity = 0;
    int failed = 0;
    for (;;) {
        struct pollfd p = {ends[0], POLLIN, 0};
        int ready = poll(&p, 1, 50);
        if (ready > 0) {
            if (!cee_window_reserve(&out, &capacity, used + 4096u)) {
                failed = 1;
                break; /* the dialog's output then fails to write, and it ends */
            }
            ssize_t got = read(ends[0], out + used, 4096u);
            if (got > 0) {
                used += (size_t)got;
            } else if (got == 0 || errno != EINTR) {
                break;
            }
        } else if (ready < 0 && errno != EINTR) {
            failed = 1;
            break;
        }
        for (struct cee_window *other = cee_wake_windows; other != NULL; other = other->wake_link) {
            if (!other->scripted) {
                cee_native_pump(other);
            }
        }
    }
    close(ends[0]);
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    int32_t result = 0;
    if (failed || waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) > 1) {
        result = -CEE_WINDOW_RESOURCES;
    } else if (WEXITSTATUS(status) == 0 && !cee_window_answer_split(w, out, used, '\n', first)) {
        result = -CEE_WINDOW_RESOURCES;
    }
    ceesafe_platform_free(out);
    return result;
}

static int32_t cee_native_dialog(struct cee_window *w, int kind, const char *title, const char *name,
                                 const char *extensions, size_t extensions_length) {
    struct cee_x_patterns patterns = {(char *)ceesafe_platform_alloc(extensions_length * 3u + 4u), 0};
    if (patterns.text == NULL) {
        return -CEE_WINDOW_RESOURCES;
    }
    patterns.text[0] = '\0';
    if (kind != CEE_DIALOG_FOLDER) {
        cee_dialog_extensions(extensions, extensions_length, cee_x_pattern, &patterns);
    }
    char xid[24];
    snprintf(xid, sizeof xid, "%lu", (unsigned long)((struct cee_x11 *)w->native)->window);
    const char *desktop = getenv("XDG_CURRENT_DESKTOP");
    int kde = desktop != NULL && strstr(desktop, "KDE") != NULL;
    int32_t result = 1;
    for (int attempt = 0; attempt < 2 && result == 1; attempt++) {
        int kdialog = (attempt == 0) == kde;
        char *owned[3] = {NULL, NULL, NULL};
        const char *argv[16];
        int n = 0, ok = 1;
        if (kdialog) {
            argv[n++] = "kdialog";
            argv[n++] = "--attach";
            argv[n++] = xid;
            if (title[0] != '\0') {
                argv[n++] = "--title";
                argv[n++] = title;
            }
            argv[n++] = kind == CEE_DIALOG_SAVE     ? "--getsavefilename"
                        : kind == CEE_DIALOG_FOLDER ? "--getexistingdirectory"
                                                    : "--getopenfilename";
            argv[n++] = kind == CEE_DIALOG_SAVE && name[0] != '\0' ? name : ".";
            if (patterns.length > 0) {
                argv[n++] = patterns.text;
            }
            if (kind == CEE_DIALOG_FILES) {
                argv[n++] = "--multiple";
                argv[n++] = "--separate-output";
            }
        } else {
            argv[n++] = "zenity";
            argv[n++] = "--file-selection";
            if (title[0] != '\0') {
                ok = (owned[0] = cee_x_join("--title=", title)) != NULL;
                argv[n++] = owned[0];
            }
            if (patterns.length > 0) {
                ok = ok && (owned[1] = cee_x_join("--file-filter=", patterns.text)) != NULL;
                argv[n++] = owned[1];
            }
            if (kind == CEE_DIALOG_FILES) {
                argv[n++] = "--multiple";
                argv[n++] = "--separator=\n";
            } else if (kind == CEE_DIALOG_SAVE) {
                argv[n++] = "--save";
                argv[n++] = "--confirm-overwrite";
                if (name[0] != '\0') {
                    ok = ok && (owned[2] = cee_x_join("--filename=", name)) != NULL;
                    argv[n++] = owned[2];
                }
            } else if (kind == CEE_DIALOG_FOLDER) {
                argv[n++] = "--directory";
            }
        }
        argv[n] = NULL;
        /* posix_spawnp takes `char *const *`: through uintptr_t, which casts no `const` away (-Wcast-qual). */
        result = ok ? cee_x_run_dialog(w, (char *const *)(uintptr_t)argv, kind != CEE_DIALOG_FILES)
                    : -CEE_WINDOW_RESOURCES;
        for (int k = 0; k < 3; k++) {
            ceesafe_platform_free(owned[k]);
        }
    }
    ceesafe_platform_free(patterns.text);
    return result == 1 ? -CEE_WINDOW_UNSUPPORTED : result;
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
    ceesafe_platform_free(w->menu);
    ceesafe_platform_free(w->menu_ids);
    ceesafe_platform_free(w->answer);
    for (unsigned k = 0; k < w->drop_count; k++) {
        ceesafe_platform_free(w->drops[(w->drop_head + k) % CEE_DROP_QUEUE].paths);
    }
    ceesafe_platform_free(w->dropped);
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

/* The safe area's four insets, left, top, right and bottom, in points (ADR 0156). */
void ceesafe_window_safe_area(void *window, float *insets) {
    memcpy(insets, ((struct cee_window *)window)->safe, 4u * sizeof(float));
}

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
    if (w->scripted || now >= target || w->count > 0 || w->choice_count > 0 || w->drop_count > 0 || w->drag_changed ||
        w->touch_count > 0 || cee_wake_pending(w)) {
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

/* Whether nextEvent would return an event (one is queued that was not taken back), nextMenuChoice an id (ADR 0146:
   a scripted window's choice only of an enabled item of its menu), or nextDrop a drop, or whether the drag of files
   over the window changed (ADR 0150), or nextTouch a touch (ADR 0155). */
static int cee_window_has_event(struct cee_window *w) {
    if (w->drop_count > 0 || w->drag_changed || w->touch_count > 0) {
        return 1;
    }
    for (unsigned k = 0; k < w->count; k++) {
        if (w->queue[(w->head + k) % CEE_WINDOW_QUEUE].kind != CEE_EVENT_DROPPED) {
            return 1;
        }
    }
    for (unsigned k = 0; k < w->choice_count; k++) {
        if (!w->scripted || cee_menu_enabled(w, w->choices[(w->choice_head + k) % CEE_MENU_CHOICES])) {
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

/* Whether any open window has an event queued, or, with `scripts`, is scripted and its script still holds events. */
static int cee_window_any_event(int scripts) {
    for (struct cee_window *w = cee_wake_windows; w != NULL; w = w->wake_link) {
        if (cee_window_has_event(w) || (scripts && w->scripted && w->script_next < w->script_count)) {
            return 1;
        }
    }
    return 0;
}

/* Sleeps until any open window has an event queued or was woken, or `timeout_ms` pass, pumping every window meanwhile
   (ADR 0145), then takes every window's wake. Only the main thread opens and closes windows, so it walks their list
   without the lock; the flags are read under it. On another thread, which has no window, and with no window open, it
   is a sleep. */
void ceesafe_window_wait_any(uint32_t timeout_ms) {
    double deadline = cee_window_now() + (double)timeout_ms / 1000.0;
    if (!cee_window_on_main_thread() || cee_wake_windows == NULL) {
        cee_window_sleep_until(deadline);
        return;
    }
    for (;;) {
        if (cee_window_any_event(0) || cee_wake_any_pending()) {
            break;
        }
        int native = 0;
        for (struct cee_window *w = cee_wake_windows; w != NULL; w = w->wake_link) {
            if (w->scripted) {
                cee_window_script_pump(w);
            }
#if !defined(CEE_WINDOW_NO_NATIVE)
            else {
                cee_native_pump(w);
                native = 1;
            }
#endif
        }
        if (cee_window_any_event(1)) {
            break;
        }
        double left = deadline - cee_window_now();
        if (left <= 0.0 || !cee_wake_any_arm()) {
            break;
        }
#if !defined(CEE_WINDOW_NO_NATIVE)
        if (native) {
            cee_native_wait_any(left);
            continue;
        }
#endif
        cee_wake_any_wait(left);
        (void)native;
    }
    cee_wake_take_all();
}

/* The id ceesafe_window_wake knows the window by. */
uint64_t ceesafe_window_waker(void *window) { return ((struct cee_window *)window)->wake_id; }

/* From any thread: sets the woken flag and the signal of the open window `id` names, once until a wait takes it, and
   the process's signal (ADR 0145); a window that was closed is not in the list, and nothing happens. The system's
   wake follows for a scripted window too: in a program with both kinds, it ends waitAny's wait on the system. */
void ceesafe_window_wake(uint64_t id) {
    int woke = 0;
    cee_wake_acquire();
    for (struct cee_window *w = cee_wake_windows; w != NULL; w = w->wake_link) {
        if (w->wake_id == id) {
            if (!w->woken) {
                w->woken = 1;
#if defined(_WIN32)
                SetEvent((HANDLE)w->wake_event);
                SetEvent(cee_wake_any_event);
#else
                ssize_t written = write(w->wake_write, "", 1); /* the pipe is empty: there is room for the byte */
                (void)written;
                written = write(cee_wake_any_write, "", 1); /* if the pipe is full, it is readable already */
                (void)written;
#endif
                woke = 1;
            }
            break;
        }
    }
    cee_wake_release();
#if !defined(CEE_WINDOW_NO_NATIVE)
    if (woke) {
        cee_native_wake();
    }
#else
    (void)woke;
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

/* ---- Menus and file dialogs (ADR 0146) --------------------------------------------------------------------------- */

/* Shows lib/std's encoding of a menu as the window's menus: 0, or minus a WindowError's number. The window keeps a
   copy, and its items' ids in order; on a failure it keeps the menu it had. */
int32_t ceesafe_window_set_menu(void *window, const uint8_t *entries, uint64_t length) {
    struct cee_window *w = (struct cee_window *)window;
    size_t items = 0;
    if (length > 0x7fffffffu || (length > 0 && entries == NULL)) {
        return -CEE_WINDOW_UNSUPPORTED;
    }
    int bad = cee_menu_check(entries, (size_t)length, &items);
    if (bad != 0) {
        return -bad;
    }
    uint8_t *copy = (uint8_t *)ceesafe_platform_alloc((size_t)length + 1u);
    uint32_t *ids = (uint32_t *)ceesafe_platform_alloc((items + 1u) * sizeof(uint32_t));
    if (copy == NULL || ids == NULL) {
        ceesafe_platform_free(copy);
        ceesafe_platform_free(ids);
        return -CEE_WINDOW_RESOURCES;
    }
    if (length > 0) {
        memcpy(copy, entries, (size_t)length);
    }
    size_t at = 0, k = 0;
    struct cee_menu_entry e;
    while (cee_menu_next(copy, (size_t)length, &at, &e)) {
        if (e.kind == CEE_MENU_ITEM) {
            ids[k++] = e.id;
        }
    }
    uint8_t *old_menu = w->menu;
    uint32_t *old_ids = w->menu_ids;
    size_t old_length = w->menu_length, old_items = w->menu_items;
    w->menu = copy;
    w->menu_length = (size_t)length;
    w->menu_ids = ids;
    w->menu_items = items;
    int32_t result = 0;
#if !defined(CEE_WINDOW_NO_NATIVE)
    if (!w->scripted) {
        result = cee_native_menu(w);
    }
#endif
    if (result != 0) {
        w->menu = old_menu;
        w->menu_length = old_length;
        w->menu_ids = old_ids;
        w->menu_items = old_items;
        old_menu = copy;
        old_ids = ids;
    }
    ceesafe_platform_free(old_menu);
    ceesafe_platform_free(old_ids);
    return result;
}

/* The next chosen item's id, or -1. With none queued, the window takes the system's (or its script's) events first,
   as ceesafe_window_poll does. A scripted choice of an item that the window's menu does not have enabled is dropped:
   the system never delivers one. */
int64_t ceesafe_window_menu_choice(void *window) {
    struct cee_window *w = (struct cee_window *)window;
    if (w->choice_count == 0) {
        if (w->scripted) {
            cee_window_script_pump(w);
        }
#if !defined(CEE_WINDOW_NO_NATIVE)
        else {
            cee_native_pump(w);
        }
#endif
    }
    while (w->choice_count > 0) {
        uint32_t id = w->choices[w->choice_head];
        w->choice_head = (w->choice_head + 1u) % CEE_MENU_CHOICES;
        w->choice_count--;
        if (!w->scripted || cee_menu_enabled(w, id)) {
            return (int64_t)id;
        }
    }
    return -1;
}

/* Runs a file dialog of `kind` (CEE_DIALOG_*) for the window: the answer's length (its paths, each followed by a zero
   byte, at *answer until the window's next dialog), 0 for a cancel, or minus a WindowError's number. A scripted window
   answers with its script's first due `dialog` item, and cancels without one. */
int64_t ceesafe_window_dialog(void *window, int32_t kind, const uint8_t *title, uint64_t title_length,
                              const uint8_t *name, uint64_t name_length, const uint8_t *extensions,
                              uint64_t extensions_length, const uint8_t **answer) {
    struct cee_window *w = (struct cee_window *)window;
    *answer = NULL;
    w->answer_length = 0;
    if (kind < CEE_DIALOG_FILE || kind > CEE_DIALOG_FOLDER || title_length > 0x7fffffffu ||
        name_length > 0x7fffffffu || extensions_length > 0x7fffffffu) {
        return -CEE_WINDOW_UNSUPPORTED;
    }
    if (!cee_window_on_main_thread()) {
        return -CEE_WINDOW_NOT_MAIN_THREAD; /* not reached: a Window is never on another thread */
    }
    int32_t result = 0;
    if (w->scripted) {
        cee_window_script_pump(w);
        if (w->answer_count > 0) {
            struct cee_window_answer a = w->answers[w->answer_head];
            w->answer_head = (w->answer_head + 1u) % CEE_DIALOG_ANSWERS;
            w->answer_count--;
            if (!a.cancel && !cee_window_answer_split(w, w->script_text + a.offset, (size_t)a.length, '|',
                                                       kind != CEE_DIALOG_FILES)) {
                result = -CEE_WINDOW_RESOURCES;
            }
        }
    } else {
#if !defined(CEE_WINDOW_NO_NATIVE)
        char *t = cee_window_cstring(title, title_length);
        char *n = cee_window_cstring(name, name_length);
        char *x = cee_window_cstring(extensions, extensions_length);
        result = t == NULL || n == NULL || x == NULL
                     ? -CEE_WINDOW_RESOURCES
                     : cee_native_dialog(w, (int)kind, t, n, x, (size_t)extensions_length);
        ceesafe_platform_free(t);
        ceesafe_platform_free(n);
        ceesafe_platform_free(x);
#else
        (void)title;
        (void)name;
        (void)extensions;
        result = -CEE_WINDOW_UNSUPPORTED; /* not reached: without a backend, only scripted windows open */
#endif
    }
    if (result < 0) {
        w->answer_length = 0;
        return result;
    }
    *answer = (const uint8_t *)w->answer;
    return (int64_t)w->answer_length;
}

/* ---- Drops (ADR 0150) -------------------------------------------------------------------------------------------- */

/* The next drop: the length of its paths (each followed by a zero byte, at *paths until the next call) with its
   position, or -1. With none queued, the window takes the system's (or its script's) events first, as
   ceesafe_window_poll does. */
int64_t ceesafe_window_drop(void *window, float *position, const uint8_t **paths) {
    struct cee_window *w = (struct cee_window *)window;
    *paths = NULL;
    position[0] = 0.0f;
    position[1] = 0.0f;
    ceesafe_platform_free(w->dropped); /* the copy lib/std took at the last call */
    w->dropped = NULL;
    if (w->drop_count == 0) {
        if (w->scripted) {
            cee_window_script_pump(w);
        }
#if !defined(CEE_WINDOW_NO_NATIVE)
        else {
            cee_native_pump(w);
        }
#endif
    }
    if (w->drop_count == 0) {
        return -1;
    }
    struct cee_window_drop d = w->drops[w->drop_head];
    w->drop_head = (w->drop_head + 1u) % CEE_DROP_QUEUE;
    w->drop_count--;
    w->dropped = d.paths;
    position[0] = d.x;
    position[1] = d.y;
    *paths = (const uint8_t *)d.paths;
    return (int64_t)d.length;
}

/* The drag of files over the window: how many files (0 for none), with its position. */
uint32_t ceesafe_window_drag_over(void *window, float *position) {
    struct cee_window *w = (struct cee_window *)window;
    position[0] = w->drag_x;
    position[1] = w->drag_y;
    return w->drag_count;
}

/* ---- Touches (ADR 0155) ------------------------------------------------------------------------------------------ */

/* Turns touches on or off; whether the system gives this window touches (a scripted window, iOS and Android). On, the
   finger that is the pointer lifts it; off, every finger down is Cancelled, and the next finger down is the pointer. */
int32_t ceesafe_window_set_touches(void *window, int32_t on) {
    struct cee_window *w = (struct cee_window *)window;
#if defined(CEE_WINDOW_UIKIT) || defined(__ANDROID__)
    int capable = 1;
#else
    int capable = w->scripted;
#endif
    if (!capable) {
        return 0;
    }
    if (on && !w->touches_on) {
        if (w->pointer_finger) {
            w->pointer_finger = 0;
            cee_window_push(w, CEE_EVENT_BUTTON, 0, 0, 0, 0, w->pointer_x, w->pointer_y);
        }
        w->touches_on = 1;
    } else if (!on && w->touches_on) {
        cee_touch_cancel_all(w);
        w->touches_on = 0;
    }
    return 1;
}

/* The next touch: 1 with its fields, or 0. With none queued, the window takes the system's (or its script's) events
   first, as ceesafe_window_poll does. */
int32_t ceesafe_window_touch(void *window, uint32_t *id, int32_t *phase, float *position, uint64_t *time) {
    struct cee_window *w = (struct cee_window *)window;
    if (w->touch_count == 0) {
        if (w->scripted) {
            cee_window_script_pump(w);
        }
#if !defined(CEE_WINDOW_NO_NATIVE)
        else {
            cee_native_pump(w);
        }
#endif
    }
    if (w->touch_count == 0) {
        return 0;
    }
    struct cee_window_touch t = w->touches[w->touch_head];
    w->touch_head = (w->touch_head + 1u) % CEE_TOUCH_QUEUE;
    w->touch_count--;
    *id = t.id;
    *phase = t.phase;
    position[0] = t.x;
    position[1] = t.y;
    *time = t.time;
    return 1;
}
