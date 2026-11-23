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
/* A scripted soft keyboard (`keyboard:H`, ADR 0157): never queued, it updates the keyboard's inset and queues
   RESIZED. */
#define CEE_SCRIPT_KEYBOARD 108
/* A scripted app event (`app:background`, `app:foreground`, `app:memory`, ADR 0159): never queued as a window event,
   it is queued for nextAppEvent. */
#define CEE_SCRIPT_APP 109
/* A scripted request of a screen reader (`access:ID,ACTION`, ADR 0164): never queued as an event, it is queued for
   takeAccessAction if the window's list has the element. */
#define CEE_SCRIPT_ACCESS 110

/* App events (ADR 0159): WindowAppEvent's numbers, and how many a window keeps that the program has not taken. */
#define CEE_APP_BACKGROUND 0
#define CEE_APP_FOREGROUND 1
#define CEE_APP_MEMORY 2
#define CEE_APP_QUEUE 16u

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
/* ADR 0158: a file of the program's given to the user to save. `name` is the suggested name and `extensions` holds
   the file's path. On iOS and Android the system saves it, and the answer is one empty path; elsewhere the answer is
   the save dialog's path, which lib/std copies the file to. */
#define CEE_DIALOG_EXPORT 4
/* ADR 0178: the PDF at `extensions` given to the system's print dialog, `title` the job's name. The answer is one
   empty path when it was printed, none when the user cancelled. A backend that prints defines CEE_HAS_NATIVE_PRINT
   and cee_native_print (1 printed, 0 cancelled, or minus an error); elsewhere it is Unsupported. */
#define CEE_DIALOG_PRINT 5
#define CEE_DIALOG_ANSWERS 64u

/* WindowSurface's numbers (ADR 0137): who draws the content. */
#define CEE_SURFACE_FRAMES 0
#define CEE_SURFACE_EXTERNAL 1

/* WindowHandle's kinds (ceesafe_window_native, ADR 0137). */
#define CEE_HANDLE_SCRIPTED 0
#define CEE_HANDLE_APPKIT 1
#define CEE_HANDLE_WIN32 2
#define CEE_HANDLE_XLIB 3
#define CEE_HANDLE_WAYLAND 6 /* ADR 0205 */

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

/* An element of a window for a screen reader (ADR 0164), from the ten words lib/std gives for it. */
struct cee_access_node {
    uint64_t id;
    double min, max, now;
    float x, y, width, height;
    uint32_t role, flags;
    uint32_t label_start, label_length, value_start, value_length;
    uint32_t caret, anchor; /* a text field's, as byte offsets into its value */
    uint32_t layer;         /* 0 the page; higher is drawn over it (ADR 0164 §10) */
    /* Containers (ADR 0207): the container's id (0 the window) and its index in the list (CEE_ACCESS_TOP for the
       window); the element's place in it, from 1, and a container's counts, numbered by the runtime where the
       program gave 0. */
    uint64_t parent;
    uint32_t up;
    uint32_t row, column, rows, columns;
};
#define CEE_ACCESS_TOP 0xFFFFFFFFu

/* The element at point (x, y) of the content, in points, among the first `count` of `nodes`: of those that hold it,
   the highest layer's, and of one layer the latest in the list (drawn later, so on top); -1 for none. The list is in
   reading order, so a popup read before the page it covers is above it by its layer, not by its place. AppKit's, UI
   Automation's and AT-SPI's hit tests ask; UIKit and Android choose for themselves. */
#if defined(_WIN32) || (defined(__APPLE__) && !defined(CEE_WINDOW_UIKIT)) ||                                          \
    ((defined(__linux__) || defined(__FreeBSD__)) && !defined(__ANDROID__))
static inline int64_t cee_access_at(const struct cee_access_node *nodes, uint32_t count, double x, double y) {
    int64_t found = -1;
    for (uint32_t k = 0; k < count; k++) {
        const struct cee_access_node *n = &nodes[k];
        if (x >= (double)n->x && y >= (double)n->y && x < (double)n->x + (double)n->width &&
            y < (double)n->y + (double)n->height && (found < 0 || n->layer >= nodes[found].layer)) {
            found = (int64_t)k;
        }
    }
    return found;
}
#endif
#define CEE_ACCESS_CHECKED 1u
#define CEE_ACCESS_SELECTED 2u
#define CEE_ACCESS_FOCUSED 4u
#define CEE_ACCESS_DISABLED 8u
#define CEE_ACCESS_EXPANDED 16u
#define CEE_ACCESS_SECURE 32u
#define CEE_ACCESS_ROLES 20u
/* ADR 0207's roles: Group 11, List 12, Menu 13, MenuBar 14, TabList 15, Table 16, Row 17, Cell 18, ColumnHeader 19.
   A container holds others: all but ColumnHeader. */
#define CEE_ACCESS_GROUP 11u
#define CEE_ACCESS_LIST 12u
#define CEE_ACCESS_MENU 13u
#define CEE_ACCESS_MENU_BAR 14u
#define CEE_ACCESS_TAB_LIST 15u
#define CEE_ACCESS_TABLE 16u
#define CEE_ACCESS_ROW 17u
#define CEE_ACCESS_CELL 18u
#define CEE_ACCESS_COLUMN_HEADER 19u
#define CEE_ACCESS_CONTAINER(role) ((role) >= CEE_ACCESS_GROUP && (role) <= CEE_ACCESS_CELL)

/* iOS and Android have no rows (ADR 0207 §10, §11). Whether row k of `nodes` is read whole, as one element as wide
   as its table: it has a name, which says what it holds (its cells too, which are then left out with their
   contents, as a list's row holds its text), or it holds nothing. A row with no name that holds cells is only
   their container. */
#if defined(CEE_WINDOW_UIKIT) || defined(__ANDROID__)
static int cee_access_whole_row(const struct cee_access_node *nodes, uint32_t count, uint32_t k) {
    if (nodes[k].role != CEE_ACCESS_ROW) {
        return 0;
    }
    if (nodes[k].label_length > 0) {
        return 1;
    }
    for (uint32_t j = k + 1u; j < count; j++) {
        if (nodes[j].up == k) {
            return 0;
        }
    }
    return 1;
}
#endif
#define CEE_ACCESS_PRESS 0
#define CEE_ACCESS_FOCUS 1
#define CEE_ACCESS_INCREMENT 2
#define CEE_ACCESS_DECREMENT 3
#define CEE_ACCESS_QUEUE 16u

struct cee_access_request {
    uint64_t id;
    int32_t action;
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
    /* The soft keyboard (ADR 0157): how much of the content's bottom it covers, in points, 0 without one; and the
       kind of keyboard the program asked for (WindowTextKind's number). */
    float keyboard;
    int32_t text_kind;
    /* ADR 0206: the program asked to draw the title bar, and the system lets it (`program_bar`); the bar's layout in
       points (its height, then the minimize, maximize, close and menu boxes, x, y, width, height each); whether the
       window is maximized, or tiled or fullscreen (no edge resizes then). */
    int wants_bar, program_bar, maximized, tiled;
    /* The area the system's own buttons take in the program's bar, in points from the content's top left (macOS's
       traffic lights, with their margins); 0 by 0 where the program draws them. */
    float sys_buttons[4];
    float bar[17];
    int32_t return_key; /* WindowReturnKey's number, 0 to 5 (ADR 0161) */
    /* Accessibility (ADR 0164): the list last given and the text its ranges point into (the runtime's copies), the
       requests not yet taken (a ring), and whether anything has asked the system for the list. */
    struct cee_access_node *access;
    uint32_t access_count;
    char *access_text;
    uint32_t access_text_length;
    struct cee_access_request access_requests[CEE_ACCESS_QUEUE];
    unsigned access_head, access_requests_count;
    int access_active;
    /* The app's life (ADR 0159): whether the app is in the background, as the last app event queued says; the app
       events not yet taken (a ring); and whether the program took a BACKGROUND and has not waited or polled since,
       which is the time it is given to save. */
    int background;
    uint8_t app_events[CEE_APP_QUEUE];
    unsigned app_head, app_count;
    int saving;
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
void ceesafe_window_keyboard_inset(void *window, float *inset);
void ceesafe_window_set_text_kind(void *window, int32_t kind);
int32_t ceesafe_window_program_title_bar(void *window);
void ceesafe_window_system_buttons(void *window, float *area);
void ceesafe_window_set_title_bar(void *window, const float *bar);
void ceesafe_window_minimize(void *window);
void ceesafe_window_toggle_maximized(void *window);
int32_t ceesafe_window_maximized(void *window);
void ceesafe_window_set_return_key(void *window, int32_t key);
int32_t ceesafe_window_app_event(void *window);
int32_t ceesafe_window_set_access(void *window, const void *nodes, uint32_t count, const uint8_t *text, uint32_t length);
int32_t ceesafe_window_access_request(void *window, uint64_t *id, int32_t *action);
int32_t ceesafe_window_access_active(void *window);
void ceesafe_window_announce(void *window, const uint8_t *text, uint64_t length);

/* A screen reader asks `action` of element `id` (ADR 0164): queued if the window's list has the element and the
   ring has room. The caller ends the wait. */
static int cee_access_ask(struct cee_window *w, uint64_t id, int32_t action) {
    if (w->access_requests_count == CEE_ACCESS_QUEUE) {
        return 0;
    }
    for (uint32_t k = 0; k < w->access_count; k++) {
        if (w->access[k].id == id) {
            struct cee_access_request *r =
                &w->access_requests[(w->access_head + w->access_requests_count) % CEE_ACCESS_QUEUE];
            r->id = id;
            r->action = action;
            w->access_requests_count++;
            return 1;
        }
    }
    return 0;
}

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

/* The soft keyboard now covers `inset` points of the content's bottom (ADR 0157), never more than the content's
   height: RESIZED with the size in points, as for a safe area. */
static void cee_window_keyboard(struct cee_window *w, float inset) {
    if (!(inset > 0.0f)) {
        inset = 0.0f;
    }
    if (inset > (float)w->height) {
        inset = (float)w->height; /* a system's number while it turns the screen may be for the other orientation */
    }
    if (inset == w->keyboard) {
        return;
    }
    w->keyboard = inset;
    cee_window_push(w, CEE_EVENT_RESIZED, (int32_t)w->width, (int32_t)w->height, 0, 0, 0.0f, 0.0f);
}

/* An app event for nextAppEvent (ADR 0159). BACKGROUND and FOREGROUND alternate, a window being in the foreground
   when it opens: one that says what the last said is not queued. A full queue (a program that never asks) drops its
   oldest event. */
static void cee_window_app(struct cee_window *w, int kind) {
    if (kind == CEE_APP_BACKGROUND || kind == CEE_APP_FOREGROUND) {
        int background = kind == CEE_APP_BACKGROUND;
        if (background == w->background) {
            return;
        }
        w->background = background;
    }
    if (w->app_count == CEE_APP_QUEUE) {
        w->app_head = (w->app_head + 1u) % CEE_APP_QUEUE;
        w->app_count--;
    }
    w->app_events[(w->app_head + w->app_count) % CEE_APP_QUEUE] = (uint8_t)kind;
    w->app_count++;
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

#if defined(CEE_WINDOW_UIKIT) || defined(__ANDROID__)
static uint32_t cee_window_utf8(const unsigned char *s, size_t n, size_t *i);

/* Text the soft keyboard's input method committed (ADR 0157): TEXT for each scalar of its UTF-8, then no composition,
   as on the desktop (ADR 0141). */
static void cee_window_commit(struct cee_window *w, const char *text, size_t length) {
    size_t i = 0;
    while (i < length) {
        cee_window_text(w, cee_window_utf8((const unsigned char *)text, length, &i));
    }
    cee_window_compose(w, "", 0, 0);
}

/* A key of the soft keyboard that is no text (Backspace, Enter): down, then up. */
static void cee_window_key_tap(struct cee_window *w, int32_t key) {
    cee_window_push(w, CEE_EVENT_KEY, key, 1, 0, 0, 0.0f, 0.0f);
    cee_window_push(w, CEE_EVENT_KEY, key, 0, 0, 0, 0.0f, 0.0f);
}
#endif

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
#elif defined(__linux__) || defined(__FreeBSD__) || defined(_WIN32)
/* Linux, FreeBSD and Windows (ADR 0201): nothing in the system ties windows to the process's first thread, and a host
   may not own that thread (a JVM runs Java's main on one of its own). The window thread is the first that opens a
   window, recorded by that open with a compare-and-set, so of two first opens at once one wins; every later window
   call is on it. Before any open, no thread is it. */
#if defined(_WIN32)
static volatile LONG cee_window_thread_id;
static long cee_window_this_thread(void) { return (long)GetCurrentThreadId(); }
static long cee_window_recorded(void) { return (long)InterlockedCompareExchange(&cee_window_thread_id, 0, 0); }
static long cee_window_record(long me) {
    return (long)InterlockedCompareExchange(&cee_window_thread_id, (LONG)me, 0);
}
#else
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
static long cee_window_this_thread(void) { return (long)syscall(SYS_gettid); }
#else
#include <pthread_np.h>
static long cee_window_this_thread(void) { return (long)pthread_getthreadid_np(); }
#endif
static long cee_window_thread_id;
static long cee_window_recorded(void) { return __atomic_load_n(&cee_window_thread_id, __ATOMIC_ACQUIRE); }
static long cee_window_record(long me) {
    long expected = 0;
    __atomic_compare_exchange_n(&cee_window_thread_id, &expected, me, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return expected; /* 0 when this call recorded it, else the thread recorded before */
}
#endif
static int cee_window_on_main_thread(void) {
    long recorded = cee_window_recorded();
    return recorded != 0 && recorded == cee_window_this_thread();
}
/* An open's test: the window thread, recording this one when there is none yet. */
static int cee_window_claim_thread(void) {
    long me = cee_window_this_thread();
    long before = cee_window_record(me);
    return before == 0 || before == me;
}
#define CEE_WINDOW_CLAIMS_THREAD 1
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
    if (CEE_IS("keyboard")) { /* keyboard:H, the points of the bottom a soft keyboard covers (ADR 0157) */
        if (cee_window_numbers(args, args_length, v, 3) != 1 || !(v[0] >= 0.0) || v[0] > CEE_WINDOW_MAX_SIDE) {
            return 0;
        }
        return cee_window_script_add(w, capacity, frame, CEE_SCRIPT_KEYBOARD, 0, 0, 0, 0, v[0], 0.0);
    }
    if (CEE_IS("app")) { /* app:background, app:foreground, app:memory (ADR 0159) */
        static const char *const kinds[3] = {"background", "foreground", "memory"};
        for (int32_t k = 0; k < 3; k++) {
            if (args_length == strlen(kinds[k]) && memcmp(args, kinds[k], args_length) == 0) {
                return cee_window_script_add(w, capacity, frame, CEE_SCRIPT_APP, k, 0, 0, 0, 0.0, 0.0);
            }
        }
        return 0;
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
    if (CEE_IS("access")) { /* access:ID,ACTION (ADR 0164) */
        static const char *const actions[4] = {"press", "focus", "increment", "decrement"};
        uint64_t id = 0;
        size_t at = 0;
        while (at < args_length && args[at] >= '0' && args[at] <= '9' && id <= 100000000u) {
            id = id * 10u + (uint64_t)(args[at++] - '0');
        }
        if (at == 0 || at == args_length || args[at] != ',' || id == 0) {
            return 0;
        }
        at++;
        for (int32_t k = 0; k < 4; k++) {
            if (args_length - at == strlen(actions[k]) && memcmp(args + at, actions[k], args_length - at) == 0) {
                return cee_window_script_add(w, capacity, frame, CEE_SCRIPT_ACCESS, (int32_t)id, k, 0, 0, 0.0, 0.0);
            }
        }
        return 0;
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
        } else if (e.kind == CEE_SCRIPT_APP) {
            cee_window_app(w, e.fields[0]);
        } else if (e.kind == CEE_SCRIPT_KEYBOARD) {
            cee_window_keyboard(w, e.x);
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
        } else if (e.kind == CEE_SCRIPT_ACCESS) {
            cee_access_ask(w, (uint64_t)(uint32_t)e.fields[0], e.fields[1]);
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
    cee_fn send_super;
    cee_id field_class;       /* CeeSafeTextField (ADR 0157) */
    cee_id *keyboard_changed; /* UIKeyboardWillChangeFrameNotification */
    cee_id *keyboard_end;     /* UIKeyboardFrameEndUserInfoKey */
    /* Accessibility (ADR 0164): CeeSafeAccessElement, a UIAccessibilityElement that takes a reader's actions;
       UIAccessibilityPostNotification with its layout-changed and announcement numbers; whether VoiceOver runs. */
    cee_id access_class;
    /* Containers (ADR 0207): the element's subclasses that answer the data table's questions, a table's and a
       cell's, and UIAccessibilityTraitTabBar. */
    cee_id access_table_class;
    cee_id access_cell_class;
    uint64_t access_tab_bar;
    void (*access_post)(uint32_t, cee_id);
    const uint32_t *access_layout_changed;
    const uint32_t *access_announcement;
    signed char (*access_voiceover)(void);
} cee_uikit;

/* The one window, its view, and the hidden text field that is its editor while text input is on (ADR 0157), with
   how many UTF-16 units of the field's text were already given as TEXT. */
struct cee_ios {
    cee_id window;
    cee_id view;
    cee_id field;
    unsigned long sent;
    /* Accessibility (ADR 0164): the element of each node of the window's list, in its order, each retained. */
    cee_id *access_elements;
    uint32_t access_count;
};
/* How much of the screen's bottom the soft keyboard covers, in points, as its last notification said. */
static float cee_ios_keyboard;
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

/* ---- Text input (ADR 0157): a hidden UITextField, CeeSafeTextField, is the editor the keyboard's input method
   types into. It holds only what is being composed: text without a marked range is committed (TEXT) and taken out of
   the field at once, the marked text is the composition, Backspace in an empty field and Return are keys. The input
   method therefore sees no text around the caret, so automatic capitals, correction and spell checking are off. ---- */

typedef struct {
    unsigned long location, length;
} cee_ios_range;

static void cee_ios_utf8_range(struct cee_window *w, cee_id text, unsigned long from, unsigned long to, int commit) {
    if (to <= from) {
        if (!commit) {
            cee_window_compose(w, "", 0, 0);
        }
        return;
    }
    cee_ios_range range = {from, to - from};
    cee_id part = ((cee_id(*)(cee_id, cee_sel, cee_ios_range))cee_uikit.send)(text, cee_ios_sel("substringWithRange:"),
                                                                             range);
    const char *utf8 = part != NULL ? cee_ios_send_cstr(part, "UTF8String") : NULL;
    if (utf8 == NULL) {
        return;
    }
    if (commit) {
        cee_window_commit(w, utf8, strlen(utf8));
    } else {
        cee_window_compose(w, utf8, strlen(utf8), strlen(utf8));
    }
}

/* The field's text changed (UIControlEventEditingChanged). */
static void cee_ios_field_changed(cee_id self, cee_sel cmd, cee_id sender) {
    (void)cmd;
    (void)sender;
    struct cee_window *w = cee_ios_shown;
    struct cee_ios *m = w != NULL ? (struct cee_ios *)w->native : NULL;
    if (m == NULL || m->field != self) {
        return;
    }
    cee_id text = cee_ios_send(self, "text");
    unsigned long length = text != NULL ? cee_ios_send_ulong(text, "length") : 0ul;
    cee_id marked = cee_ios_send(self, "markedTextRange");
    if (marked != NULL) {
        /* Text before the marked range was committed with it; the marked text is the composition. */
        long start = ((long (*)(cee_id, cee_sel, cee_id, cee_id))cee_uikit.send)(
            self, cee_ios_sel("offsetFromPosition:toPosition:"), cee_ios_send(self, "beginningOfDocument"),
            cee_ios_send(marked, "start"));
        long end = ((long (*)(cee_id, cee_sel, cee_id, cee_id))cee_uikit.send)(
            self, cee_ios_sel("offsetFromPosition:toPosition:"), cee_ios_send(self, "beginningOfDocument"),
            cee_ios_send(marked, "end"));
        unsigned long from = start > 0 ? (unsigned long)start : 0ul;
        unsigned long to = end > 0 ? (unsigned long)end : 0ul;
        if (from > length) {
            from = length;
        }
        if (to > length) {
            to = length;
        }
        if (from > m->sent) {
            cee_ios_utf8_range(w, text, m->sent, from, 1);
            m->sent = from;
        }
        cee_ios_utf8_range(w, text, from, to, 0);
        return;
    }
    if (length > m->sent) {
        cee_ios_utf8_range(w, text, m->sent, length, 1);
    } else {
        cee_window_compose(w, "", 0, 0); /* the composition was deleted away */
    }
    m->sent = 0;
    if (length > 0) {
        cee_ios_send_v_id(self, "setText:", cee_ios_string(""));
    }
}

/* Backspace: in an empty field it is the program's key; otherwise it edits the composition, which the change tells. */
static void cee_ios_field_delete(cee_id self, cee_sel cmd) {
    struct cee_window *w = cee_ios_shown;
    cee_id text = cee_ios_send(self, "text");
    if (w != NULL && (text == NULL || cee_ios_send_ulong(text, "length") == 0ul)) {
        cee_window_key_tap(w, CEE_KEY_BACKSPACE);
    }
    struct {
        cee_id receiver;
        cee_id super_class;
    } super = {self, cee_ios_cls("UITextField")};
    ((void (*)(void *, cee_sel))cee_uikit.send_super)(&super, cmd);
}

/* Return: the program's Enter key; the field keeps its focus. */
static signed char cee_ios_field_return(cee_id self, cee_sel cmd, cee_id field) {
    (void)self;
    (void)cmd;
    (void)field;
    if (cee_ios_shown != NULL) {
        cee_window_key_tap(cee_ios_shown, CEE_KEY_ENTER);
    }
    return 0;
}

/* The keyboard's frame will change (shown, hidden, another keyboard): its end frame's top against the screen's
   bottom is how much it covers. The pump gives it to the window. */
static void cee_ios_field_keyboard(cee_id self, cee_sel cmd, cee_id notification) {
    (void)self;
    (void)cmd;
    cee_id info = cee_ios_send(notification, "userInfo");
    cee_id value = info != NULL && cee_uikit.keyboard_end != NULL
                       ? ((cee_id(*)(cee_id, cee_sel, cee_id))cee_uikit.send)(info, cee_ios_sel("objectForKey:"),
                                                                              *cee_uikit.keyboard_end)
                       : NULL;
    if (value == NULL || cee_ios_scene == NULL) {
        return;
    }
    cee_rect frame = cee_ios_send_rect(value, "CGRectValue");
    cee_rect screen = cee_ios_send_rect(cee_ios_send(cee_ios_scene, "screen"), "bounds");
    double covered = screen.size.height - frame.origin.y;
    cee_ios_keyboard = covered > 0.0 && frame.size.height > 0.0 ? (float)covered : 0.0f;
}

/* The traits of the window's kind of text (WindowTextKind's numbers): UIKeyboardType's default 0, number pad 4,
   decimal pad 8, email address 7, URL 3; a password is secure text. */
static void cee_ios_field_traits(struct cee_window *w, cee_id field) {
    static const long types[6] = {0, 4, 8, 7, 3, 0};
    int kind = w->text_kind >= 0 && w->text_kind <= 5 ? w->text_kind : 0;
    ((void (*)(cee_id, cee_sel, long))cee_uikit.send)(field, cee_ios_sel("setKeyboardType:"), types[kind]);
    ((void (*)(cee_id, cee_sel, signed char))cee_uikit.send)(field, cee_ios_sel("setSecureTextEntry:"), kind == 5);
    /* The Return key's face (WindowReturnKey's numbers, ADR 0161): UIReturnKeyType's default 0, done 9, go 1, next 4,
       search 6, send 7. */
    static const long returns[6] = {0, 9, 1, 4, 6, 7};
    int key = w->return_key >= 0 && w->return_key <= 5 ? w->return_key : 0;
    ((void (*)(cee_id, cee_sel, long))cee_uikit.send)(field, cee_ios_sel("setReturnKeyType:"), returns[key]);
    ((void (*)(cee_id, cee_sel, long))cee_uikit.send)(field, cee_ios_sel("setAutocorrectionType:"), 1);     /* no */
    ((void (*)(cee_id, cee_sel, long))cee_uikit.send)(field, cee_ios_sel("setAutocapitalizationType:"), 0); /* none */
    ((void (*)(cee_id, cee_sel, long))cee_uikit.send)(field, cee_ios_sel("setSpellCheckingType:"), 1);      /* no */
}

/* The window's field, made at the first use: hidden in the view, its own target, delegate and observer. */
static cee_id cee_ios_field(struct cee_window *w) {
    struct cee_ios *m = (struct cee_ios *)w->native;
    if (m->field != NULL || cee_uikit.field_class == NULL) {
        return m->field;
    }
    cee_id field = cee_ios_send(cee_ios_send(cee_uikit.field_class, "alloc"), "init");
    if (field == NULL) {
        return NULL;
    }
    ((void (*)(cee_id, cee_sel, signed char))cee_uikit.send)(field, cee_ios_sel("setHidden:"), 1);
    cee_ios_send_v_id(field, "setDelegate:", field);
    ((void (*)(cee_id, cee_sel, cee_id, cee_sel, unsigned long))cee_uikit.send)(
        field, cee_ios_sel("addTarget:action:forControlEvents:"), field, cee_ios_sel("ceesafeChanged:"),
        1ul << 17); /* UIControlEventEditingChanged */
    if (cee_uikit.keyboard_changed != NULL) {
        ((void (*)(cee_id, cee_sel, cee_id, cee_sel, cee_id, cee_id))cee_uikit.send)(
            cee_ios_send(cee_ios_cls("NSNotificationCenter"), "defaultCenter"),
            cee_ios_sel("addObserver:selector:name:object:"), field, cee_ios_sel("ceesafeKeyboard:"),
            *cee_uikit.keyboard_changed, NULL);
    }
    cee_ios_send_v_id(m->view, "addSubview:", field);
    cee_ios_send_v(field, "release"); /* the view keeps it */
    m->field = field;
    return field;
}

/* The wake source's callback: nothing to do; that a source was handled ends the run loop's wait (ADR 0142). */
static void cee_ios_wake_perform(void *info) { (void)info; }

/* ---- Accessibility on iOS (ADR 0164) -----------------------------------------------------------------------------
   Each node of the window's list is a UIAccessibilityElement in the view, which is their container: the view's
   accessibilityElements are the elements in the list's order, the order VoiceOver moves in. An element is kept
   from one list to the next while its id is in both. iOS has traits and no roles: a button, a tab, a radio button,
   a list's row and a menu's item are buttons (selected when they are), a checkbox a toggle button whose value is
   1 or 0 (said "on" and "off"; a plain button with that value before iOS 17), a slider adjustable with its value as
   text. A text field is an element with a name and a value: its keyboard comes when it is activated, as a tap
   does it.

   Containers (ADR 0207): a container that holds something is an element VoiceOver does not stop on
   (isAccessibilityElement NO) whose accessibilityElements are its children, and their accessibilityContainer; the
   view's are the top level's. Its accessibilityContainerType says what it is: a group a semantic group (its name
   is said on the way in), a list, a menu, a menu bar and a tab list a list, a table a data table. A tab list has
   the tab bar trait, so its tabs are said "tab, 2 of 4". A table answers UIAccessibilityContainerDataTable (its
   counts, the element at a row and column, a column's headers) and its cells and headers
   UIAccessibilityContainerDataTableCell (their row and column from 0); iOS has no rows, so a row that holds
   cells is a plain container, and one that holds none (nothing at all, or, having a name, only what says it
   again: its texts, which are then left out, as where there are no containers) is a cell as wide as the table,
   a button as a list's row is. What a cell holds answers the cell's place. A container that holds nothing is
   not there, but a row or a cell, which is then what is read. */
#define CEE_WINDOW_ACCESS 1

/* How many elements each node of `nodes` holds, into `held` (count entries). */
static void cee_ios_access_held(const struct cee_access_node *nodes, uint32_t count, uint32_t *held) {
    for (uint32_t k = 0; k < count; k++) {
        held[k] = 0;
    }
    for (uint32_t k = 0; k < count; k++) {
        if (nodes[k].up != CEE_ACCESS_TOP && nodes[k].up < count) {
            held[nodes[k].up]++;
        }
    }
}

/* The class of node k's element: 2 a table's, 1 a cell's (a cell, a column header, a row read whole, and what a
   cell holds), 0 the plain one. */
static int cee_ios_access_kind(const struct cee_access_node *nodes, uint32_t count, uint32_t k) {
    const struct cee_access_node *n = &nodes[k];
    if (n->role == CEE_ACCESS_TABLE) {
        return 2;
    }
    if (n->role == CEE_ACCESS_CELL || n->role == CEE_ACCESS_COLUMN_HEADER || cee_access_whole_row(nodes, count, k)) {
        return 1;
    }
    return n->up != CEE_ACCESS_TOP && nodes[n->up].role == CEE_ACCESS_CELL ? 1 : 0;
}

static cee_id cee_ios_string_of(const char *text, size_t length) {
    cee_id string = cee_ios_send(cee_ios_cls("NSString"), "alloc");
    string = ((cee_id(*)(cee_id, cee_sel, const void *, unsigned long, unsigned long))cee_uikit.send)(
        string, cee_ios_sel("initWithBytes:length:encoding:"), length > 0 ? (const void *)text : (const void *)"",
        (unsigned long)length, 4ul /* NSUTF8StringEncoding */);
    return string != NULL ? cee_ios_send(string, "autorelease") : cee_ios_string("");
}

/* The index of `element` in the shown window's list, or -1. */
static long cee_ios_access_index(cee_id element) {
    struct cee_window *w = cee_ios_shown;
    if (w == NULL || w->native == NULL) {
        return -1;
    }
    struct cee_ios *m = (struct cee_ios *)w->native;
    for (uint32_t k = 0; k < m->access_count && k < w->access_count; k++) {
        if (m->access_elements[k] == element) {
            return (long)k;
        }
    }
    return -1;
}

/* VoiceOver asks `action` of the element: queued, and the run loop woken so a wait ends. */
static signed char cee_ios_access_asked(cee_id self, int32_t action) {
    long index = cee_ios_access_index(self);
    struct cee_window *w = cee_ios_shown;
    if (index < 0 || (w->access[index].flags & CEE_ACCESS_DISABLED) != 0) {
        return 0;
    }
    if (cee_access_ask(w, w->access[index].id, action) && cee_uikit.wake_source != NULL) {
        cee_uikit.source_signal(cee_uikit.wake_source);
        cee_uikit.loop_wake_up(cee_uikit.main_loop);
    }
    return 1;
}

static signed char cee_ios_access_activate(cee_id self, cee_sel cmd) {
    (void)cmd;
    return cee_ios_access_asked(self, CEE_ACCESS_PRESS);
}

static void cee_ios_access_increment(cee_id self, cee_sel cmd) {
    (void)cmd;
    cee_ios_access_asked(self, CEE_ACCESS_INCREMENT);
}

static void cee_ios_access_decrement(cee_id self, cee_sel cmd) {
    (void)cmd;
    cee_ios_access_asked(self, CEE_ACCESS_DECREMENT);
}

/* Where node k's container is in the view, into x and y: an element's frame is given in its container's space,
   which for a container's child is the container's frame (ADR 0207), not the view. */
static void cee_ios_access_origin(const struct cee_access_node *nodes, uint32_t k, float *x, float *y) {
    uint32_t up = nodes[k].up;
    *x = up != CEE_ACCESS_TOP && up < k ? nodes[up].x : 0.0f;
    *y = up != CEE_ACCESS_TOP && up < k ? nodes[up].y : 0.0f;
}

/* Gives `element` what node `n` says: its name, value, traits and place in its container, which is at (ox, oy) of
   the view. */
static void cee_ios_access_apply(cee_id element, const struct cee_access_node *n, const char *text, float ox, float oy) {
    /* UIAccessibilityTraits, as UIKit's headers have had them since iOS 3 (toggle button since iOS 17). */
    const uint64_t button = 1u, image = 4u, selected = 8u, static_text = 0x40u, not_enabled = 0x100u,
                   adjustable = 0x1000u, toggle = (uint64_t)1 << 53;
    uint64_t traits = 0;
    cee_id label = cee_ios_string_of(text + n->label_start, n->label_length);
    cee_id value = NULL;
    char number[32];
    switch (n->role) {
    case 0:
        traits = static_text;
        break;
    case 2: /* a checkbox is a toggle button: its value 1 or 0 is said "on" or "off" */
        traits = button | toggle;
        value = cee_ios_string((n->flags & CEE_ACCESS_CHECKED) != 0 ? "1" : "0");
        break;
    case 3: /* a radio button is a button, selected when it is the chosen one */
        traits = button | ((n->flags & CEE_ACCESS_CHECKED) != 0 ? selected : 0u);
        break;
    case 4:
        if ((n->flags & CEE_ACCESS_SECURE) == 0 && n->value_length > 0) {
            value = cee_ios_string_of(text + n->value_start, n->value_length);
        }
        break;
    case 5: /* a slider says the program's text for its value, or its share of the range */
        traits = adjustable;
        if (n->value_length > 0) {
            value = cee_ios_string_of(text + n->value_start, n->value_length);
        } else {
            double share = n->max > n->min ? (n->now - n->min) / (n->max - n->min) : 0.0;
            share = share < 0.0 ? 0.0 : share > 1.0 ? 1.0 : share;
            snprintf(number, sizeof number, "%d%%", (int)(share * 100.0 + 0.5));
            value = cee_ios_string(number);
        }
        break;
    case 9:
        traits = button;
        value = cee_ios_string_of(text + n->value_start, n->value_length);
        break;
    case 10:
        traits = image;
        break;
    case CEE_ACCESS_GROUP:
    case CEE_ACCESS_LIST:
    case CEE_ACCESS_MENU:
    case CEE_ACCESS_MENU_BAR:
    case CEE_ACCESS_TABLE:
        break;
    case CEE_ACCESS_TAB_LIST: /* its tabs are said "tab, 2 of 4" */
        traits = cee_uikit.access_tab_bar;
        break;
    case CEE_ACCESS_CELL: /* one that holds nothing is read as its text */
        traits = static_text;
        break;
    default: /* a button, a tab, a list's row, a menu's item, a table's row, a column header */
        traits = button;
        break;
    }
    /* UIAccessibilityContainerType: 1 a data table, 2 a list, 4 a semantic group (iOS 13). */
    long container = n->role == CEE_ACCESS_TABLE   ? 1
                     : n->role == CEE_ACCESS_GROUP ? 4
                     : n->role == CEE_ACCESS_LIST || n->role == CEE_ACCESS_MENU || n->role == CEE_ACCESS_MENU_BAR ||
                               n->role == CEE_ACCESS_TAB_LIST
                           ? 2
                           : 0;
    ((void (*)(cee_id, cee_sel, long))cee_uikit.send)(element, cee_ios_sel("setAccessibilityContainerType:"), container);
    if ((n->flags & CEE_ACCESS_SELECTED) != 0) {
        traits |= selected;
    }
    if ((n->flags & CEE_ACCESS_DISABLED) != 0) {
        traits |= not_enabled;
    }
    cee_ios_send_v_id(element, "setAccessibilityLabel:", label);
    cee_ios_send_v_id(element, "setAccessibilityValue:", value);
    ((void (*)(cee_id, cee_sel, uint64_t))cee_uikit.send)(element, cee_ios_sel("setAccessibilityTraits:"), traits);
    cee_rect frame = {{(double)(n->x - ox), (double)(n->y - oy)}, {(double)n->width, (double)n->height}};
    ((void (*)(cee_id, cee_sel, cee_rect))cee_uikit.send)(element, cee_ios_sel("setAccessibilityFrameInContainerSpace:"),
                                                         frame);
}

static int cee_ios_access_same_text(const char *a, uint32_t a_start, uint32_t a_length, const char *b, uint32_t b_start,
                                    uint32_t b_length) {
    return a_length == b_length && (a_length == 0 || memcmp(a + a_start, b + b_start, a_length) == 0);
}

/* The data table's questions (ADR 0207). UIKit counts rows and columns from 0; cee_ios_range is NSRange. */
#define CEE_IOS_NOT_FOUND (~0ul >> 1) /* NSNotFound: NSIntegerMax */

/* The index of the table node `index` is in (itself, if it is one), or CEE_ACCESS_TOP. */
static uint32_t cee_ios_access_table_of(const struct cee_window *w, uint32_t index) {
    while (index != CEE_ACCESS_TOP && index < w->access_count && w->access[index].role != CEE_ACCESS_TABLE) {
        index = w->access[index].up;
    }
    return index < w->access_count ? index : CEE_ACCESS_TOP;
}

/* The node whose place a cell's element answers: itself, or the cell it is in. */
static const struct cee_access_node *cee_ios_access_cell_node(cee_id self) {
    long index = cee_ios_access_index(self);
    if (index < 0) {
        return NULL;
    }
    const struct cee_window *w = cee_ios_shown;
    const struct cee_access_node *n = &w->access[index];
    if (n->role != CEE_ACCESS_CELL && n->role != CEE_ACCESS_COLUMN_HEADER && n->role != CEE_ACCESS_ROW &&
        n->up != CEE_ACCESS_TOP && w->access[n->up].role == CEE_ACCESS_CELL) {
        n = &w->access[n->up];
    }
    return n;
}

static cee_ios_range cee_ios_access_row_range(cee_id self, cee_sel cmd) {
    (void)cmd;
    const struct cee_access_node *n = cee_ios_access_cell_node(self);
    cee_ios_range none = {CEE_IOS_NOT_FOUND, 0};
    cee_ios_range range = {n != NULL ? (unsigned long)n->row - 1ul : 0ul, 1};
    return n != NULL && n->row > 0 ? range : none;
}

static cee_ios_range cee_ios_access_column_range(cee_id self, cee_sel cmd) {
    (void)cmd;
    const struct cee_access_node *n = cee_ios_access_cell_node(self);
    cee_ios_range none = {CEE_IOS_NOT_FOUND, 0};
    if (n == NULL) {
        return none;
    }
    if (n->role == CEE_ACCESS_ROW) { /* a row read whole is as wide as its table */
        const struct cee_window *w = cee_ios_shown;
        uint32_t t = cee_ios_access_table_of(w, n->up);
        cee_ios_range all = {0, t != CEE_ACCESS_TOP && w->access[t].columns > 0 ? w->access[t].columns : 1u};
        return all;
    }
    cee_ios_range range = {(unsigned long)n->column - 1ul, 1};
    return n->column > 0 ? range : none;
}

static unsigned long cee_ios_access_row_count(cee_id self, cee_sel cmd) {
    (void)cmd;
    long index = cee_ios_access_index(self);
    return index >= 0 ? cee_ios_shown->access[index].rows : 0ul;
}

static unsigned long cee_ios_access_column_count(cee_id self, cee_sel cmd) {
    (void)cmd;
    long index = cee_ios_access_index(self);
    return index >= 0 ? cee_ios_shown->access[index].columns : 0ul;
}

/* The element at `row` and `column` of the table, from 0: its cell or column header there, or its row when the
   row is read whole. */
static cee_id cee_ios_access_cell_at(cee_id self, cee_sel cmd, unsigned long row, unsigned long column) {
    (void)cmd;
    long index = cee_ios_access_index(self);
    if (index < 0) {
        return NULL;
    }
    const struct cee_window *w = cee_ios_shown;
    const struct cee_ios *m = (const struct cee_ios *)w->native;
    cee_id whole = NULL;
    for (uint32_t k = (uint32_t)index + 1u; k < w->access_count && k < m->access_count; k++) {
        const struct cee_access_node *n = &w->access[k];
        if ((unsigned long)n->row != row + 1ul || cee_ios_access_table_of(w, n->up) != (uint32_t)index) {
            continue;
        }
        if ((n->role == CEE_ACCESS_CELL || n->role == CEE_ACCESS_COLUMN_HEADER) && (unsigned long)n->column == column + 1ul &&
            !cee_access_whole_row(w->access, w->access_count, n->up)) { /* a row read whole has left its cells out */
            return m->access_elements[k];
        }
        if (whole == NULL && cee_access_whole_row(w->access, w->access_count, k)) {
            whole = m->access_elements[k];
        }
    }
    return whole;
}

/* The column headers of `column`, from 0, of the table. */
static cee_id cee_ios_access_column_headers(cee_id self, cee_sel cmd, unsigned long column) {
    (void)cmd;
    long index = cee_ios_access_index(self);
    if (index < 0) {
        return NULL;
    }
    const struct cee_window *w = cee_ios_shown;
    const struct cee_ios *m = (const struct cee_ios *)w->native;
    cee_id headers = cee_ios_send(cee_ios_cls("NSMutableArray"), "array");
    for (uint32_t k = (uint32_t)index + 1u; k < w->access_count && k < m->access_count; k++) {
        const struct cee_access_node *n = &w->access[k];
        if (n->role == CEE_ACCESS_COLUMN_HEADER && (unsigned long)n->column == column + 1ul &&
            m->access_elements[k] != NULL && cee_ios_access_table_of(w, n->up) == (uint32_t)index &&
            !cee_access_whole_row(w->access, w->access_count, n->up)) {
            cee_ios_send_v_id(headers, "addObject:", m->access_elements[k]);
        }
    }
    return cee_ios_send_long(headers, "count") > 0 ? headers : NULL;
}

/* The window's list changed (w->access is the new one; `old` the last, with its text): the elements follow it. */
static void cee_native_access(struct cee_window *w, const struct cee_access_node *old, uint32_t old_count,
                              const char *old_text) {
    struct cee_ios *m = (struct cee_ios *)w->native;
    if (m == NULL || cee_uikit.access_class == NULL) {
        return;
    }
    void *pool = cee_uikit.pool_push();
    uint32_t count = w->access_count;
    if (old_count > m->access_count) {
        old_count = m->access_count;
    }
    cee_id *elements = count > 0 ? (cee_id *)ceesafe_platform_alloc_zeroed((size_t)count * sizeof *elements) : NULL;
    /* Containers (ADR 0207): how many each node holds; later, by container (the window last), where its children
       are in `kids`. */
    uint32_t *held = count > 0 ? (uint32_t *)ceesafe_platform_alloc_zeroed(((size_t)count + 1u) * sizeof *held) : NULL;
    uint32_t *old_held =
        old_count > 0 ? (uint32_t *)ceesafe_platform_alloc_zeroed((size_t)old_count * sizeof *old_held) : NULL;
    cee_id *kids = count > 0 ? (cee_id *)ceesafe_platform_alloc_zeroed((size_t)count * sizeof *kids) : NULL;
    if (count > 0 && (elements == NULL || held == NULL || kids == NULL || (old_count > 0 && old_held == NULL))) {
        ceesafe_platform_free(elements);
        elements = NULL;
        count = 0; /* out of memory: the window has no elements until the next list */
    }
    if (count > 0) {
        cee_ios_access_held(w->access, count, held);
        if (old_count > 0) {
            cee_ios_access_held(old, old_count, old_held);
        }
    }
    const cee_id classes[3] = {cee_uikit.access_class,
                               cee_uikit.access_cell_class != NULL ? cee_uikit.access_cell_class : cee_uikit.access_class,
                               cee_uikit.access_table_class != NULL ? cee_uikit.access_table_class
                                                                    : cee_uikit.access_class};
    int layout = count != old_count;
    for (uint32_t k = 0; k < count; k++) {
        const struct cee_access_node *n = &w->access[k];
        int kind = cee_ios_access_kind(w->access, count, k);
        float ox, oy;
        cee_ios_access_origin(w->access, k, &ox, &oy);
        uint32_t j = old_count;
        if (k < old_count && old[k].id == n->id) {
            j = k;
        } else {
            for (uint32_t i = 0; i < old_count; i++) {
                if (old[i].id == n->id) {
                    j = i;
                    break;
                }
            }
            layout = 1;
        }
        /* An element is kept while its class is the same: a row that came to hold cells, say, is a new one. */
        if (j < old_count && m->access_elements[j] != NULL && cee_ios_access_kind(old, old_count, j) == kind) {
            const struct cee_access_node *o = &old[j];
            elements[k] = m->access_elements[j];
            m->access_elements[j] = NULL;
            if (n->parent != o->parent || (held[k] > 0) != (old_held[j] > 0) ||
                (n->role == CEE_ACCESS_ROW && (n->label_length > 0) != (o->label_length > 0))) {
                layout = 1;
            }
            float old_ox, old_oy;
            cee_ios_access_origin(old, j, &old_ox, &old_oy);
            if (n->role != o->role || n->flags != o->flags || n->now != o->now || n->min != o->min || n->max != o->max ||
                n->x - ox != o->x - old_ox || n->y - oy != o->y - old_oy || n->width != o->width ||
                n->height != o->height ||
                !cee_ios_access_same_text(w->access_text, n->label_start, n->label_length, old_text, o->label_start,
                                          o->label_length) ||
                !cee_ios_access_same_text(w->access_text, n->value_start, n->value_length, old_text, o->value_start,
                                          o->value_length)) {
                cee_ios_access_apply(elements[k], n, w->access_text, ox, oy);
            }
        } else {
            cee_id element = ((cee_id(*)(cee_id, cee_sel, cee_id))cee_uikit.send)(
                cee_ios_send(classes[kind], "alloc"), cee_ios_sel("initWithAccessibilityContainer:"), m->view);
            elements[k] = element;
            if (element != NULL) {
                cee_ios_access_apply(element, n, w->access_text, ox, oy);
            }
            layout = 1;
        }
    }
    for (uint32_t j = 0; j < m->access_count; j++) { /* the elements the new list does not have */
        if (m->access_elements[j] != NULL) {
            cee_ios_send_v(m->access_elements[j], "release");
            layout = 1;
        }
    }
    ceesafe_platform_free(m->access_elements);
    m->access_elements = elements;
    m->access_count = count;
    if (layout && count == 0) {
        cee_ios_send_v_id(m->view, "setAccessibilityElements:", cee_ios_send(cee_ios_cls("NSArray"), "array"));
        if (cee_uikit.access_post != NULL && cee_uikit.access_layout_changed != NULL) {
            cee_uikit.access_post(*cee_uikit.access_layout_changed, NULL);
        }
    } else if (layout) {
        /* The tree (ADR 0207): each container's children in the list's order, the view's the top level's. `held`
           becomes where each container's children start in `kids` (the window's at index count), and, once they
           are in, where they end, which is where the next one's start. An element that could not be made is left
           out. */
        uint32_t next = 0;
        for (uint32_t k = 0; k <= count; k++) {
            uint32_t own = k < count ? held[k] : 0;
            if (k == count) {
                for (uint32_t i = 0; i < count; i++) {
                    own += w->access[i].up == CEE_ACCESS_TOP ? 1u : 0u;
                }
            }
            held[k] = next;
            next += own;
        }
        for (uint32_t k = 0; k < count; k++) {
            uint32_t up = w->access[k].up != CEE_ACCESS_TOP && w->access[k].up < k ? w->access[k].up : count;
            kids[held[up]++] = elements[k]; /* held[up] is now the end of up's children */
        }
        cee_sel array_sel = cee_ios_sel("arrayWithObjects:count:");
        cee_id array_class = cee_ios_cls("NSArray");
        for (uint32_t k = 0; k <= count; k++) {
            uint32_t end = held[k], start = k == 0 ? 0u : held[k - 1u];
            uint32_t made = 0;
            for (uint32_t i = start; i < end; i++) {
                if (kids[i] != NULL) {
                    kids[start + made++] = kids[i];
                }
            }
            cee_id target = k < count ? elements[k] : m->view;
            if (target == NULL) {
                continue;
            }
            int whole = k < count && cee_access_whole_row(w->access, count, k);
            if (whole) {
                made = 0; /* what it holds says it again: left out */
            }
            cee_id array = made > 0 || k == count
                               ? ((cee_id(*)(cee_id, cee_sel, const cee_id *, unsigned long))cee_uikit.send)(
                                     array_class, array_sel, kids + start, (unsigned long)made)
                               : NULL;
            cee_ios_send_v_id(target, "setAccessibilityElements:", array);
            if (k < count) {
                uint32_t up = w->access[k].up;
                cee_id holder = up != CEE_ACCESS_TOP && up < k && elements[up] != NULL ? elements[up] : m->view;
                cee_ios_send_v_id(target, "setAccessibilityContainer:", holder);
                /* VoiceOver stops on what holds nothing, but a container that is neither a row nor a cell, and
                   on a row read whole. */
                uint32_t role = w->access[k].role;
                signed char stop = whole || (made == 0 && (!CEE_ACCESS_CONTAINER(role) || role == CEE_ACCESS_ROW ||
                                                           role == CEE_ACCESS_CELL));
                ((void (*)(cee_id, cee_sel, signed char))cee_uikit.send)(target, cee_ios_sel("setIsAccessibilityElement:"),
                                                                         stop);
            }
        }
        if (cee_uikit.access_post != NULL && cee_uikit.access_layout_changed != NULL) {
            cee_uikit.access_post(*cee_uikit.access_layout_changed, NULL);
        }
    }
    ceesafe_platform_free(held);
    ceesafe_platform_free(old_held);
    ceesafe_platform_free(kids);
    cee_uikit.pool_pop(pool);
}

/* Says `text` through VoiceOver. */
static void cee_native_announce(struct cee_window *w, const char *text, size_t length) {
    (void)w;
    if (cee_uikit.access_post == NULL || cee_uikit.access_announcement == NULL) {
        return;
    }
    void *pool = cee_uikit.pool_push();
    cee_uikit.access_post(*cee_uikit.access_announcement, cee_ios_string_of(text, length));
    cee_uikit.pool_pop(pool);
}

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
    /* Accessibility (ADR 0164). */
    cee_uikit.access_post = (void (*)(uint32_t, cee_id))cee_ios_symbol(uikit, "UIAccessibilityPostNotification");
    cee_uikit.access_layout_changed = (const uint32_t *)dlsym(uikit, "UIAccessibilityLayoutChangedNotification");
    cee_uikit.access_announcement = (const uint32_t *)dlsym(uikit, "UIAccessibilityAnnouncementNotification");
    cee_uikit.access_voiceover = (signed char (*)(void))cee_ios_symbol(uikit, "UIAccessibilityIsVoiceOverRunning");
    cee_id access = cee_uikit.allocate_class(cee_ios_cls("UIAccessibilityElement"), "CeeSafeAccessElement", 0);
    if (access == NULL) {
        access = cee_ios_cls("CeeSafeAccessElement");
    } else {
        cee_uikit.add_method(access, cee_ios_sel("accessibilityActivate"), (cee_fn)cee_ios_access_activate, "c@:");
        cee_uikit.add_method(access, cee_ios_sel("accessibilityIncrement"), (cee_fn)cee_ios_access_increment, "v@:");
        cee_uikit.add_method(access, cee_ios_sel("accessibilityDecrement"), (cee_fn)cee_ios_access_decrement, "v@:");
        cee_uikit.register_class(access);
    }
    cee_uikit.access_class = access;
    /* Containers (ADR 0207): a table's element and a cell's answer the data table's questions. */
    const uint64_t *tab_bar = (const uint64_t *)dlsym(uikit, "UIAccessibilityTraitTabBar");
    cee_uikit.access_tab_bar = tab_bar != NULL ? *tab_bar : 0u;
    cee_id table = access != NULL ? cee_uikit.allocate_class(access, "CeeSafeAccessTable", 0) : NULL;
    if (table == NULL) {
        table = cee_ios_cls("CeeSafeAccessTable");
    } else {
        cee_uikit.add_method(table, cee_ios_sel("accessibilityRowCount"), (cee_fn)cee_ios_access_row_count, "Q@:");
        cee_uikit.add_method(table, cee_ios_sel("accessibilityColumnCount"), (cee_fn)cee_ios_access_column_count, "Q@:");
        cee_uikit.add_method(table, cee_ios_sel("accessibilityDataTableCellElementForRow:column:"),
                             (cee_fn)cee_ios_access_cell_at, "@@:QQ");
        cee_uikit.add_method(table, cee_ios_sel("accessibilityHeaderElementsForColumn:"),
                             (cee_fn)cee_ios_access_column_headers, "@@:Q");
        void *protocol =
            cee_uikit.get_protocol != NULL ? cee_uikit.get_protocol("UIAccessibilityContainerDataTable") : NULL;
        if (protocol != NULL && cee_uikit.add_protocol != NULL) {
            cee_uikit.add_protocol(table, protocol);
        }
        cee_uikit.register_class(table);
    }
    cee_uikit.access_table_class = table;
    cee_id cell = access != NULL ? cee_uikit.allocate_class(access, "CeeSafeAccessCell", 0) : NULL;
    if (cell == NULL) {
        cell = cee_ios_cls("CeeSafeAccessCell");
    } else {
        cee_uikit.add_method(cell, cee_ios_sel("accessibilityRowRange"), (cee_fn)cee_ios_access_row_range,
                             "{_NSRange=QQ}@:");
        cee_uikit.add_method(cell, cee_ios_sel("accessibilityColumnRange"), (cee_fn)cee_ios_access_column_range,
                             "{_NSRange=QQ}@:");
        void *protocol =
            cee_uikit.get_protocol != NULL ? cee_uikit.get_protocol("UIAccessibilityContainerDataTableCell") : NULL;
        if (protocol != NULL && cee_uikit.add_protocol != NULL) {
            cee_uikit.add_protocol(cell, protocol);
        }
        cee_uikit.register_class(cell);
    }
    cee_uikit.access_cell_class = cell;
    cee_uikit.send_super = cee_ios_symbol(objc, "objc_msgSendSuper");
    cee_uikit.keyboard_changed = (cee_id *)dlsym(uikit, "UIKeyboardWillChangeFrameNotification");
    cee_uikit.keyboard_end = (cee_id *)dlsym(uikit, "UIKeyboardFrameEndUserInfoKey");
    cee_id field = cee_uikit.send_super != NULL
                       ? cee_uikit.allocate_class(cee_ios_cls("UITextField"), "CeeSafeTextField", 0)
                       : NULL;
    if (field != NULL) {
        cee_uikit.add_method(field, cee_ios_sel("ceesafeChanged:"), (cee_fn)cee_ios_field_changed, "v@:@");
        cee_uikit.add_method(field, cee_ios_sel("ceesafeKeyboard:"), (cee_fn)cee_ios_field_keyboard, "v@:@");
        cee_uikit.add_method(field, cee_ios_sel("deleteBackward"), (cee_fn)cee_ios_field_delete, "v@:");
        cee_uikit.add_method(field, cee_ios_sel("textFieldShouldReturn:"), (cee_fn)cee_ios_field_return, "c@:@");
        cee_uikit.register_class(field);
    } else if (cee_uikit.send_super != NULL) {
        field = cee_ios_cls("CeeSafeTextField"); /* a second runtime in the process registered it */
    }
    cee_uikit.field_class = field; /* NULL: no text input, the window still works */
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

/* The app's life (ADR 0159). The scene's delegate and the application's are told it on the program's own thread,
   inside its poll or wait, so each word goes straight into the window's queue, in order (a background and a
   foreground that come together are both given), and ends the wait. `cee_ios_task` is the background task that keeps
   the app running while the program saves: 0 for none (UIBackgroundTaskInvalid). */
static unsigned long cee_ios_task;
static void cee_native_wake(void);

static void cee_ios_task_end(void) {
    if (cee_ios_task != 0ul) {
        unsigned long task = cee_ios_task;
        cee_ios_task = 0ul;
        ((void (*)(cee_id, cee_sel, unsigned long))cee_uikit.send)(
            cee_ios_send(cee_ios_cls("UIApplication"), "sharedApplication"), cee_ios_sel("endBackgroundTask:"), task);
    }
}

/* The background task's expiration handler, a block: the system's patience is over, and the task must end now or
   the app is ended. A block without captures is a constant: its class (_NSConcreteGlobalBlock), the flag that says
   so (1 << 28), and the function, which gets the block itself. */
struct cee_ios_block_descriptor {
    unsigned long reserved, size;
};
struct cee_ios_block {
    void *isa;
    int flags, reserved;
    void (*invoke)(void *);
    const struct cee_ios_block_descriptor *descriptor;
};
static void cee_ios_task_expired(void *block) {
    (void)block;
    cee_ios_task_end();
}
static const struct cee_ios_block_descriptor cee_ios_expired_descriptor = {0ul, sizeof(struct cee_ios_block)};
static struct cee_ios_block cee_ios_expired = {NULL, 1 << 28, 0, cee_ios_task_expired, &cee_ios_expired_descriptor};

static void cee_ios_app_event(int kind) {
    if (cee_ios_shown != NULL) {
        cee_window_app(cee_ios_shown, kind);
        cee_native_wake(); /* the wait this came in ends, as for an event */
    }
}

/* Off the screen: the system is asked for time, so that the program can take BACKGROUND and save before the app is
   suspended. The task ends when the program waits again after taking it (cee_native_saved), when the app comes back,
   or when the system says its time is up. */
static void cee_ios_scene_background(cee_id self, cee_sel cmd, cee_id scene) {
    (void)self;
    (void)cmd;
    (void)scene;
    if (cee_ios_shown != NULL && cee_ios_task == 0ul) {
        if (cee_ios_expired.isa == NULL) {
            cee_ios_expired.isa = dlsym(RTLD_DEFAULT, "_NSConcreteGlobalBlock");
        }
        if (cee_ios_expired.isa != NULL) {
            cee_ios_task = ((unsigned long (*)(cee_id, cee_sel, void *))cee_uikit.send)(
                cee_ios_send(cee_ios_cls("UIApplication"), "sharedApplication"),
                cee_ios_sel("beginBackgroundTaskWithExpirationHandler:"), &cee_ios_expired);
        }
    }
    cee_ios_app_event(CEE_APP_BACKGROUND);
}

static void cee_ios_scene_foreground(cee_id self, cee_sel cmd, cee_id scene) {
    (void)self;
    (void)cmd;
    (void)scene;
    cee_ios_task_end();
    cee_ios_app_event(CEE_APP_FOREGROUND);
}

static void cee_ios_memory_warning(cee_id self, cee_sel cmd, cee_id application) {
    (void)self;
    (void)cmd;
    (void)application;
    cee_ios_app_event(CEE_APP_MEMORY);
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
        cee_uikit.add_method(delegate, cee_ios_sel("applicationDidReceiveMemoryWarning:"),
                             (cee_fn)cee_ios_memory_warning, "v@:@");
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
        cee_uikit.add_method(scene_delegate, cee_ios_sel("sceneDidEnterBackground:"),
                             (cee_fn)cee_ios_scene_background, "v@:@");
        cee_uikit.add_method(scene_delegate, cee_ios_sel("sceneWillEnterForeground:"),
                             (cee_fn)cee_ios_scene_foreground, "v@:@");
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
    if (m->field != NULL) {
        ((void (*)(cee_id, cee_sel, cee_id))cee_uikit.send)(
            cee_ios_send(cee_ios_cls("NSNotificationCenter"), "defaultCenter"), cee_ios_sel("removeObserver:"),
            m->field);
        cee_ios_send_v_id(m->field, "setDelegate:", NULL);
        cee_ios_send_v(m->field, "removeFromSuperview");
        m->field = NULL;
        cee_ios_keyboard = 0.0f;
    }
    for (uint32_t k = 0; k < m->access_count; k++) { /* the window's elements (ADR 0164) */
        if (m->access_elements[k] != NULL) {
            cee_ios_send_v(m->access_elements[k], "release");
        }
    }
    ceesafe_platform_free(m->access_elements);
    m->access_elements = NULL;
    m->access_count = 0;
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
    if (!w->access_active && cee_uikit.access_voiceover != NULL && cee_uikit.access_voiceover()) {
        w->access_active = 1; /* VoiceOver runs: something asks for the window's elements (ADR 0164) */
    }
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
        cee_window_keyboard(w, cee_ios_keyboard); /* the soft keyboard's inset (ADR 0157) */
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
/* Text input on makes the field the first responder, which shows the soft keyboard (none with a hardware keyboard);
   off resigns it, which hides it (ADR 0157). */
static void cee_native_text_input(struct cee_window *w, int on) {
    struct cee_ios *m = (struct cee_ios *)w->native;
    if (m == NULL) {
        return;
    }
    void *pool = cee_uikit.pool_push();
    cee_id field = on ? cee_ios_field(w) : m->field;
    if (field != NULL) {
        if (on) {
            cee_ios_field_traits(w, field);
            cee_ios_send_v_id(field, "setText:", cee_ios_string(""));
            m->sent = 0;
            (void)((signed char (*)(cee_id, cee_sel))cee_uikit.send)(field, cee_ios_sel("becomeFirstResponder"));
        } else {
            (void)((signed char (*)(cee_id, cee_sel))cee_uikit.send)(field, cee_ios_sel("resignFirstResponder"));
            cee_ios_send_v_id(field, "setText:", cee_ios_string(""));
            m->sent = 0;
        }
    }
    cee_uikit.pool_pop(pool);
}

static void cee_native_text_area(struct cee_window *w) { (void)w; } /* the field is hidden: nothing follows the caret */

/* The program has saved (it waits or polls again after BACKGROUND): the background time is given back (ADR 0159). */
static void cee_native_saved(struct cee_window *w) {
    (void)w;
    cee_ios_task_end();
}

/* A new kind of text: the field's traits, and the keyboard shown again for them. */
static void cee_native_text_kind(struct cee_window *w) {
    struct cee_ios *m = (struct cee_ios *)w->native;
    if (m == NULL || m->field == NULL) {
        return; /* the traits are set when text input is turned on */
    }
    void *pool = cee_uikit.pool_push();
    cee_ios_field_traits(w, m->field);
    cee_ios_send_v(m->field, "reloadInputViews");
    cee_uikit.pool_pop(pool);
}

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

/* ---- File dialogs (ADR 0158): the system's document picker, presented by the window's view controller. The call is
   modal: the run loop runs inside it until the picker's delegate, a CeeSafePickerDelegate, has the answer. ---- */

static int cee_ios_picker_state; /* 0 waiting, 1 picked, 2 cancelled */
/* The picker whose answer is waited for. A picker may tell its delegate more than once, and late (a cancel after
   its pick, while it leaves the screen): only this one's first word counts, so an earlier dialog's cannot answer
   the next. */
static cee_id cee_ios_picker_current;
static cee_id cee_ios_picker_urls; /* the picked NSURLs, retained until they are read */

static void cee_ios_picker_picked(cee_id self, cee_sel cmd, cee_id picker, cee_id urls) {
    (void)self;
    (void)cmd;
    if (cee_ios_picker_state == 0 && picker == cee_ios_picker_current) {
        cee_ios_picker_urls = urls != NULL ? cee_ios_send(urls, "retain") : NULL;
        cee_ios_picker_state = 1;
    }
}

static void cee_ios_picker_cancelled(cee_id self, cee_sel cmd, cee_id picker) {
    (void)self;
    (void)cmd;
    if (cee_ios_picker_state == 0 && picker == cee_ios_picker_current) {
        cee_ios_picker_state = 2;
    }
}

/* The delegate's class, made at the first dialog. */
static cee_id cee_ios_picker_delegate(void) {
    cee_id made = cee_uikit.allocate_class(cee_ios_cls("NSObject"), "CeeSafePickerDelegate", 0);
    if (made != NULL) {
        cee_uikit.add_method(made, cee_ios_sel("documentPicker:didPickDocumentsAtURLs:"),
                             (cee_fn)cee_ios_picker_picked, "v@:@@");
        cee_uikit.add_method(made, cee_ios_sel("documentPickerWasCancelled:"), (cee_fn)cee_ios_picker_cancelled,
                             "v@:@");
        cee_uikit.register_class(made);
    } else {
        made = cee_ios_cls("CeeSafePickerDelegate");
    }
    return made != NULL ? cee_ios_send(cee_ios_send(made, "alloc"), "init") : NULL;
}

/* Presents `picker` and waits for its answer: 1 picked (the URLs in cee_ios_picker_urls), 0 cancelled. */
static int32_t cee_ios_picker_run(struct cee_window *w, cee_id picker) {
    struct cee_ios *m = (struct cee_ios *)w->native;
    cee_id delegate = cee_ios_picker_delegate();
    cee_id controller = cee_ios_send(m->window, "rootViewController");
    if (picker == NULL || delegate == NULL || controller == NULL) {
        if (delegate != NULL) {
            cee_ios_send_v(delegate, "release");
        }
        return -CEE_WINDOW_RESOURCES;
    }
    cee_ios_picker_state = 0;
    cee_ios_picker_urls = NULL;
    cee_ios_picker_current = picker;
    cee_ios_send_v_id(picker, "setDelegate:", delegate);
    ((void (*)(cee_id, cee_sel, cee_id, signed char, cee_id))cee_uikit.send)(
        controller, cee_ios_sel("presentViewController:animated:completion:"), picker, 1, NULL);
    while (cee_ios_picker_state == 0) {
        void *pool = cee_uikit.pool_push();
        (void)cee_uikit.run_in_mode(*cee_uikit.default_mode, 0.1, 1);
        cee_uikit.pool_pop(pool);
    }
    cee_ios_picker_current = NULL;
    /* The picker leaves the screen after its answer (it is dismissed here if it has not begun to): the call returns
       once it has, five seconds at most, so that a dialog the program opens next is not presented over one still
       going away (UIKit would not show it, and its answer would never come). */
    if (cee_ios_send(controller, "presentedViewController") == picker &&
        !((signed char (*)(cee_id, cee_sel))cee_uikit.send)(picker, cee_ios_sel("isBeingDismissed"))) {
        ((void (*)(cee_id, cee_sel, signed char, cee_id))cee_uikit.send)(
            controller, cee_ios_sel("dismissViewControllerAnimated:completion:"), 1, NULL);
    }
    for (int k = 0; k < 50 && cee_ios_send(controller, "presentedViewController") != NULL; k++) {
        void *pool = cee_uikit.pool_push();
        (void)cee_uikit.run_in_mode(*cee_uikit.default_mode, 0.1, 1);
        cee_uikit.pool_pop(pool);
    }
    cee_ios_send_v_id(picker, "setDelegate:", NULL);
    cee_ios_send_v(delegate, "release");
    return cee_ios_picker_state == 1 ? 1 : 0;
}

/* The uniform types of a list of file name extensions ("png, .jpg"), for the picker: every item when the list is
   empty or names an extension the system does not know. */
static cee_id cee_ios_types(const char *extensions, size_t length) {
    cee_id type_class = cee_ios_cls("UTType");
    cee_id types = cee_ios_send(cee_ios_cls("NSMutableArray"), "array");
    if (type_class == NULL || types == NULL) {
        return NULL;
    }
    int every = 0;
    size_t k = 0;
    size_t found = 0;
    while (k < length && !every) {
        while (k < length && (extensions[k] == ',' || extensions[k] == ' ' || extensions[k] == '.')) {
            k++;
        }
        char one[64];
        size_t n = 0;
        while (k < length && extensions[k] != ',' && extensions[k] != ' ') {
            if (n + 1 < sizeof one) {
                one[n++] = extensions[k];
            }
            k++;
        }
        one[n] = '\0';
        if (n == 0) {
            continue;
        }
        cee_id type = ((cee_id(*)(cee_id, cee_sel, cee_id))cee_uikit.send)(
            type_class, cee_ios_sel("typeWithFilenameExtension:"), cee_ios_string(one));
        if (type == NULL) {
            every = 1;
        } else {
            cee_ios_send_v_id(types, "addObject:", type);
            found++;
        }
    }
    if (every || found == 0) {
        cee_id item = ((cee_id(*)(cee_id, cee_sel, cee_id))cee_uikit.send)(
            type_class, cee_ios_sel("typeWithIdentifier:"), cee_ios_string("public.item"));
        if (item == NULL) {
            return NULL;
        }
        cee_ios_send_v(types, "removeAllObjects");
        cee_ios_send_v_id(types, "addObject:", item);
    }
    return types;
}

static void cee_ios_picker_load(void) {
    static int loaded;
    if (!loaded) {
        loaded = 1;
        (void)dlopen("/System/Library/Frameworks/UniformTypeIdentifiers.framework/UniformTypeIdentifiers",
                     RTLD_NOW | RTLD_GLOBAL);
    }
}

/* Open: the picker asked for copies, so each answer is a file in the app's own temporary folder (ADR 0158 §2.1). */
static int32_t cee_native_dialog(struct cee_window *w, int kind, const char *title, const char *name,
                                 const char *extensions, size_t extensions_length) {
    (void)title; /* the picker has no title */
    (void)name;
    if (kind != CEE_DIALOG_FILE && kind != CEE_DIALOG_FILES) {
        return -CEE_WINDOW_UNSUPPORTED; /* a save's path and a folder: not a phone's to give (ADR 0158 §2.2) */
    }
    cee_ios_picker_load();
    void *pool = cee_uikit.pool_push();
    cee_id types = cee_ios_types(extensions, extensions_length);
    cee_id picker_class = cee_ios_cls("UIDocumentPickerViewController");
    cee_id picker = types != NULL && picker_class != NULL
                        ? ((cee_id(*)(cee_id, cee_sel, cee_id, signed char))cee_uikit.send)(
                              cee_ios_send(picker_class, "alloc"), cee_ios_sel("initForOpeningContentTypes:asCopy:"),
                              types, 1)
                        : NULL;
    if (picker == NULL) {
        cee_uikit.pool_pop(pool);
        return -CEE_WINDOW_UNSUPPORTED;
    }
    ((void (*)(cee_id, cee_sel, signed char))cee_uikit.send)(picker, cee_ios_sel("setAllowsMultipleSelection:"),
                                                             kind == CEE_DIALOG_FILES);
    int32_t result = cee_ios_picker_run(w, picker);
    cee_ios_send_v(picker, "release");
    if (result == 1 && cee_ios_picker_urls != NULL) {
        unsigned long count = cee_ios_send_ulong(cee_ios_picker_urls, "count");
        if (kind == CEE_DIALOG_FILE && count > 1) {
            count = 1;
        }
        result = 0;
        for (unsigned long k = 0; k < count && result == 0; k++) {
            cee_id url = ((cee_id(*)(cee_id, cee_sel, unsigned long))cee_uikit.send)(
                cee_ios_picker_urls, cee_ios_sel("objectAtIndex:"), k);
            cee_id path = cee_ios_send(url, "path");
            const char *utf8 = path != NULL ? cee_ios_send_cstr(path, "UTF8String") : NULL;
            if (utf8 != NULL && !cee_window_answer_add(w, utf8, strlen(utf8))) {
                result = -CEE_WINDOW_RESOURCES;
            }
        }
    }
    if (cee_ios_picker_urls != NULL) {
        cee_ios_send_v(cee_ios_picker_urls, "release");
        cee_ios_picker_urls = NULL;
    }
    cee_uikit.pool_pop(pool);
    return result < 0 ? result : 0;
}

/* Export (ADR 0158 §2.2): the file is copied to the temporary folder under the suggested name, and the picker for
   exporting saves that copy where the user chooses. 1 saved, 0 cancelled. */
static int32_t cee_native_export(struct cee_window *w, const char *title, const char *name, const char *file) {
    (void)title;
    void *pool = cee_uikit.pool_push();
    cee_id manager = cee_ios_send(cee_ios_cls("NSFileManager"), "defaultManager");
    cee_id source = cee_ios_string(file);
    cee_id folder = ((cee_id(*)(cee_id, cee_sel, cee_id))cee_uikit.send)(
        cee_ios_send(manager, "temporaryDirectory"), cee_ios_sel("URLByAppendingPathComponent:"),
        cee_ios_string("ceesafe-export"));
    cee_id last = source != NULL ? cee_ios_send(source, "lastPathComponent") : NULL;
    cee_id wanted = name[0] != '\0' ? cee_ios_send(cee_ios_string(name), "lastPathComponent") : last;
    cee_id staged = folder != NULL && wanted != NULL
                        ? ((cee_id(*)(cee_id, cee_sel, cee_id))cee_uikit.send)(
                              folder, cee_ios_sel("URLByAppendingPathComponent:"), wanted)
                        : NULL;
    cee_id from = source != NULL ? ((cee_id(*)(cee_id, cee_sel, cee_id))cee_uikit.send)(
                                       cee_ios_cls("NSURL"), cee_ios_sel("fileURLWithPath:"), source)
                                 : NULL;
    int32_t result = -CEE_WINDOW_RESOURCES;
    if (staged != NULL && from != NULL) {
        (void)((signed char (*)(cee_id, cee_sel, cee_id, signed char, cee_id, cee_id *))cee_uikit.send)(
            manager, cee_ios_sel("createDirectoryAtURL:withIntermediateDirectories:attributes:error:"), folder, 1, NULL,
            NULL);
        (void)((signed char (*)(cee_id, cee_sel, cee_id, cee_id *))cee_uikit.send)(
            manager, cee_ios_sel("removeItemAtURL:error:"), staged, NULL);
        signed char copied = ((signed char (*)(cee_id, cee_sel, cee_id, cee_id, cee_id *))cee_uikit.send)(
            manager, cee_ios_sel("copyItemAtURL:toURL:error:"), from, staged, NULL);
        cee_id picker_class = cee_ios_cls("UIDocumentPickerViewController");
        if (copied && picker_class != NULL) {
            cee_id urls = ((cee_id(*)(cee_id, cee_sel, cee_id))cee_uikit.send)(
                cee_ios_cls("NSArray"), cee_ios_sel("arrayWithObject:"), staged);
            cee_id picker = ((cee_id(*)(cee_id, cee_sel, cee_id, signed char))cee_uikit.send)(
                cee_ios_send(picker_class, "alloc"), cee_ios_sel("initForExportingURLs:asCopy:"), urls, 1);
            result = cee_ios_picker_run(w, picker);
            if (picker != NULL) {
                cee_ios_send_v(picker, "release");
            }
            if (cee_ios_picker_urls != NULL) {
                cee_ios_send_v(cee_ios_picker_urls, "release");
                cee_ios_picker_urls = NULL;
            }
            (void)((signed char (*)(cee_id, cee_sel, cee_id, cee_id *))cee_uikit.send)(
                manager, cee_ios_sel("removeItemAtURL:error:"), staged, NULL);
        }
    }
    cee_uikit.pool_pop(pool);
    return result;
}

/* Printing a PDF (ADR 0178): AirPrint's UIPrintInteractionController, given the file's URL, presented as a sheet;
   the call is modal as the pickers are, the run loop running until the sheet's completion handler (a block without
   captures, as the background task's) says printed, cancelled or failed. */
static int cee_ios_print_state; /* 0 waiting, 1 printed, 2 cancelled, 3 failed */
struct cee_ios_print_block {
    void *isa;
    int flags, reserved;
    void (*invoke)(void *, cee_id, signed char, cee_id);
    const struct cee_ios_block_descriptor *descriptor;
};
static void cee_ios_print_done(void *block, cee_id controller, signed char completed, cee_id error) {
    (void)block;
    (void)controller;
    if (cee_ios_print_state == 0) {
        cee_ios_print_state = error != NULL ? 3 : completed ? 1 : 2;
    }
}
static const struct cee_ios_block_descriptor cee_ios_print_descriptor = {0ul, sizeof(struct cee_ios_print_block)};
static struct cee_ios_print_block cee_ios_print_handler = {NULL, 1 << 28, 0, cee_ios_print_done,
                                                           &cee_ios_print_descriptor};

#define CEE_HAS_NATIVE_PRINT 1
static int32_t cee_native_print(struct cee_window *w, const char *title, const char *file) {
    (void)w; /* the sheet is presented over the app's key window */
    cee_id controller_class = cee_ios_cls("UIPrintInteractionController");
    if (cee_ios_print_handler.isa == NULL) {
        cee_ios_print_handler.isa = dlsym(RTLD_DEFAULT, "_NSConcreteGlobalBlock");
    }
    if (controller_class == NULL || cee_ios_print_handler.isa == NULL ||
        !((signed char (*)(cee_id, cee_sel))cee_uikit.send)(controller_class, cee_ios_sel("isPrintingAvailable"))) {
        return -CEE_WINDOW_UNSUPPORTED;
    }
    void *pool = cee_uikit.pool_push();
    cee_id path = cee_ios_string(file);
    cee_id url = path != NULL ? ((cee_id(*)(cee_id, cee_sel, cee_id))cee_uikit.send)(
                                    cee_ios_cls("NSURL"), cee_ios_sel("fileURLWithPath:"), path)
                              : NULL;
    if (url == NULL ||
        !((signed char (*)(cee_id, cee_sel, cee_id))cee_uikit.send)(controller_class, cee_ios_sel("canPrintURL:"), url)) {
        cee_uikit.pool_pop(pool);
        return -CEE_WINDOW_RESOURCES; /* not a PDF the system reads */
    }
    cee_id controller = cee_ios_send(controller_class, "sharedPrintController");
    cee_id info = cee_ios_send(cee_ios_cls("UIPrintInfo"), "printInfo");
    if (controller == NULL || info == NULL) {
        cee_uikit.pool_pop(pool);
        return -CEE_WINDOW_RESOURCES;
    }
    ((void (*)(cee_id, cee_sel, long))cee_uikit.send)(info, cee_ios_sel("setOutputType:"), 0l); /* General */
    cee_ios_send_v_id(info, "setJobName:",
                      title[0] != '\0' ? cee_ios_string(title) : cee_ios_send(path, "lastPathComponent"));
    cee_ios_send_v_id(controller, "setPrintInfo:", info);
    cee_ios_send_v_id(controller, "setPrintingItem:", url);
    cee_ios_print_state = 0;
    signed char shown = ((signed char (*)(cee_id, cee_sel, signed char, void *))cee_uikit.send)(
        controller, cee_ios_sel("presentAnimated:completionHandler:"), 1, &cee_ios_print_handler);
    if (!shown) {
        cee_ios_print_state = 3;
    }
    while (cee_ios_print_state == 0) {
        void *inner = cee_uikit.pool_push();
        (void)cee_uikit.run_in_mode(*cee_uikit.default_mode, 0.1, 1);
        cee_uikit.pool_pop(inner);
    }
    cee_ios_send_v_id(controller, "setPrintingItem:", NULL);
    cee_uikit.pool_pop(pool);
    return cee_ios_print_state == 1 ? 1 : cee_ios_print_state == 2 ? 0 : -CEE_WINDOW_RESOURCES;
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
    /* Accessibility (ADR 0164): CeeSafeAccessElement, an NSAccessibilityElement that answers from the window's list;
       NSAccessibilityPostNotification and …WithUserInfo. NULL without them: the window then has no elements. */
    cee_id access_class;
    cee_id access_text_class; /* CeeSafeAccessText: a text field's element, which also answers for its text */
    void (*access_post)(cee_id, cee_id);
    void (*access_post_info)(cee_id, cee_id, cee_id);
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
    /* Accessibility (ADR 0164): the element of each node of the window's list, in its order, each retained. */
    cee_id *access_elements;
    uint32_t access_count;
    /* The view's own accessibility children, the top level of the tree (ADR 0207), retained. */
    cee_id access_top;
    /* A title bar the program draws (ADR 0206): the traffic lights' row, in points from the content's top left, and
       the bar's height they were centered in. */
    float lights[4];
    float lights_bar;
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

/* A title bar the program draws (ADR 0206 step 3): the window's content fills it, under a transparent title bar with
   no title, and the traffic lights stay the system's. They are centered in the program's bar (its height from
   setTitleBar, AppKit's title bar's until then), as far in from the left as from the top, and the area they take,
   with that margin on each side, is `systemButtons()`. AppKit lays them out again when the window changes, so they
   are placed again then. In full screen AppKit hides them: the area is none. A change of it is a Resized. */
static void cee_mac_place_buttons(struct cee_window *w) {
    struct cee_mac *m = (struct cee_mac *)w->native;
    if (!w->program_bar || m == NULL) {
        return;
    }
    float area[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    cee_id close = cee_send_id_ulong(m->window, "standardWindowButton:", 0ul);
    cee_id mini = cee_send_id_ulong(m->window, "standardWindowButton:", 1ul);
    cee_id zoom = cee_send_id_ulong(m->window, "standardWindowButton:", 2ul);
    int full = (cee_send_ulong(m->window, "styleMask") & (1ul << 14)) != 0; /* NSWindowStyleMaskFullScreen */
    if (close != NULL && mini != NULL && zoom != NULL && !full) {
        cee_id titlebar = cee_send(close, "superview");
        double room = cee_send_rect(titlebar, "bounds").size.height;
        cee_rect c = cee_send_rect(close, "frame");
        double size = c.size.height, step = cee_send_rect(mini, "frame").origin.x - c.origin.x;
        double bar = w->bar[0] > 0.0f ? (double)w->bar[0] : room;
        double top = (bar - size) / 2.0;
        if (top + size > room) {
            top = room - size; /* a bar taller than AppKit's title bar: the lights stay inside it */
        }
        if (top < 0.0) {
            top = 0.0;
        }
        double left = (bar - size) / 2.0 < 7.0 ? 7.0 : (bar - size) / 2.0;
        int flipped = cee_send_bool(titlebar, "isFlipped") != 0;
        cee_id buttons[3] = {close, mini, zoom};
        for (int k = 0; k < 3; k++) {
            cee_point at = {left + step * k, flipped ? top : room - top - size};
            ((void (*)(cee_id, cee_sel, cee_point))cee_objc.send)(buttons[k], cee_mac_sel("setFrameOrigin:"), at);
        }
        m->lights[0] = (float)left;
        m->lights[1] = (float)top;
        m->lights[2] = (float)(step * 2.0 + size);
        m->lights[3] = (float)size;
        m->lights_bar = (float)bar;
        area[2] = (float)(left * 2.0 + step * 2.0 + size);
        area[3] = (float)bar;
    } else {
        m->lights[2] = 0.0f;
    }
    if (memcmp(area, w->sys_buttons, sizeof area) != 0) {
        memcpy(w->sys_buttons, area, sizeof area);
        cee_window_push(w, CEE_EVENT_RESIZED, (int32_t)w->width, (int32_t)w->height, 0, 0, 0.0f, 0.0f);
    }
}

/* Whether (x, y) is in box `k` of the program's title bar (0 minimize, 1 maximize, 2 close, 3 menu). */
static int cee_mac_in_box(const struct cee_window *w, int k, float x, float y) {
    const float *b = &w->bar[1 + 4 * k];
    return b[2] > 0.0f && b[3] > 0.0f && x >= b[0] && y >= b[1] && x < b[0] + b[2] && y < b[1] + b[3];
}

/* A primary press the runtime takes for a window with the program's title bar (ADR 0206): in the bar but in none of
   the program's boxes and not on a traffic light, it moves the window (performWindowDragWithEvent:), and a double
   press does what the user chose in System Settings (AppleActionOnDoubleClick: zoom by default, minimize, or
   nothing). 1 when taken: the program sees no press then, and AppKit's title bar no event. -1 for a press in a
   program's box: the program's, and not AppKit's title bar's, which would move the window by it. -2 for one on a
   traffic light or in the top 4 points, the frame's edge, which resizes there: AppKit's only. 0 otherwise. */
static int cee_mac_bar_press(struct cee_window *w, cee_id event, float x, float y) {
    struct cee_mac *m = (struct cee_mac *)w->native;
    if (!w->program_bar || y >= w->bar[0]) {
        return 0;
    }
    const float *l = m->lights;
    if (y < 4.0f || (l[2] > 0.0f && x >= l[0] && x < l[0] + l[2] && y >= l[1] && y < l[1] + l[3])) {
        return -2;
    }
    if (cee_mac_in_box(w, 0, x, y) || cee_mac_in_box(w, 1, x, y) || cee_mac_in_box(w, 2, x, y) ||
        cee_mac_in_box(w, 3, x, y)) {
        return -1;
    }
    if (cee_send_long(event, "clickCount") >= 2) {
        cee_id choice = cee_send_id_id(cee_send(cee_mac_cls("NSUserDefaults"), "standardUserDefaults"),
                                       "stringForKey:", cee_nsstring("AppleActionOnDoubleClick"));
        const char *action = choice != NULL ? (const char *)cee_send(choice, "UTF8String") : NULL;
        if (action != NULL && strcmp(action, "Minimize") == 0) {
            cee_send_v_id(m->window, "miniaturize:", NULL);
        } else if (action == NULL || strcmp(action, "None") != 0) {
            cee_send_v_id(m->window, "zoom:", NULL);
        }
        return 1;
    }
    cee_send_v_id(m->window, "performWindowDragWithEvent:", event);
    return 1;
}

/* Whether the window is zoomed (ADR 0206's maximized), kept for every window; a change is a Resized. */
static void cee_mac_update_zoomed(struct cee_window *w) {
    struct cee_mac *m = (struct cee_mac *)w->native;
    int zoomed = cee_send_bool(m->window, "isZoomed") != 0;
    if (zoomed != w->maximized) {
        w->maximized = zoomed;
        cee_window_push(w, CEE_EVENT_RESIZED, (int32_t)w->width, (int32_t)w->height, 0, 0, 0.0f, 0.0f);
    }
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

static void cee_mac_wake_perform(void *info);

/* The delegate's notifications come inside AppKit's run loop, often with no event of their own behind them: a resize
   by another program (an accessibility client's, a window manager's, a script's) or the window's being brought to
   the front. Each posts the wake event (ADR 0142) when it queued one of the program's, so that a wait it came in
   returns and the program draws for it; without that, the window kept the old frame stretched to the new size until
   the next input. */
static void cee_mac_did_resize(cee_id self, cee_sel cmd, cee_id notification) {
    (void)self;
    (void)cmd;
    struct cee_window *w = cee_mac_owner(cee_send(notification, "object"));
    if (w != NULL) {
        unsigned queued = w->count;
        cee_mac_update_size(w);
        cee_mac_update_zoomed(w);
        cee_mac_place_buttons(w);
        if (w->count != queued) {
            cee_mac_wake_perform(NULL);
        }
    }
}

static void cee_mac_did_focus(cee_id self, cee_sel cmd, cee_id notification) {
    (void)self;
    (void)cmd;
    struct cee_window *w = cee_mac_owner(cee_send(notification, "object"));
    if (w != NULL) {
        unsigned queued = w->count;
        cee_window_focus(w, 1);
        if (w->count != queued) {
            cee_mac_wake_perform(NULL);
        }
    }
}

static void cee_mac_did_unfocus(cee_id self, cee_sel cmd, cee_id notification) {
    (void)self;
    (void)cmd;
    struct cee_window *w = cee_mac_owner(cee_send(notification, "object"));
    if (w != NULL) {
        unsigned queued = w->count;
        cee_window_focus(w, 0);
        if (w->count != queued) {
            cee_mac_wake_perform(NULL);
        }
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

/* ---- Accessibility on macOS (ADR 0164) --------------------------------------------------------------------------
   Each node of the window's list is an NSAccessibilityElement whose parent is the content view. An element is kept
   from one list to the next while its id is in both, so a reader's cursor stays on it. The elements answer their
   frame from the list as it is now, in screen coordinates, so a moved window needs no telling. Containers (ADR
   0207) are elements too: a container's element is its children's parent, the view only the top level's. */
#define CEE_WINDOW_ACCESS 1

static cee_id cee_mac_string(const char *text, size_t length);

/* The window and the index of `element` in its list, or NULL. */
static struct cee_window *cee_mac_access_owner(cee_id element, uint32_t *index) {
    for (int k = 0; k < CEE_MAC_WINDOWS; k++) {
        struct cee_window *w = cee_mac_windows[k].window;
        if (w == NULL || w->native == NULL) {
            continue;
        }
        struct cee_mac *m = (struct cee_mac *)w->native;
        for (uint32_t i = 0; i < m->access_count && i < w->access_count; i++) {
            if (m->access_elements[i] == element) {
                *index = i;
                return w;
            }
        }
    }
    return NULL;
}

static cee_rect cee_mac_access_frame(cee_id self, cee_sel cmd) {
    (void)cmd;
    cee_rect r = {{0.0, 0.0}, {0.0, 0.0}};
    uint32_t index = 0;
    struct cee_window *w = cee_mac_access_owner(self, &index);
    if (w == NULL) {
        return r;
    }
    const struct cee_access_node *n = &w->access[index];
    /* The content view's coordinates are the window's, origin bottom left (as cee_mac_first_rect has it). */
    r.origin.x = (double)n->x;
    r.origin.y = (double)w->height - (double)n->y - (double)n->height;
    r.size.width = (double)n->width;
    r.size.height = (double)n->height;
    return cee_send_rect_rect(((struct cee_mac *)w->native)->window, "convertRectToScreen:", r);
}

/* The reader asks `action` of the element: queued, and the wait ended. */
static signed char cee_mac_access_asked(cee_id self, int32_t action) {
    uint32_t index = 0;
    struct cee_window *w = cee_mac_access_owner(self, &index);
    if (w == NULL || (w->access[index].flags & CEE_ACCESS_DISABLED) != 0) {
        return 0;
    }
    if (cee_access_ask(w, w->access[index].id, action)) {
        cee_mac_wake_perform(NULL);
    }
    return 1;
}

static signed char cee_mac_access_press(cee_id self, cee_sel cmd) {
    (void)cmd;
    return cee_mac_access_asked(self, CEE_ACCESS_PRESS);
}

static signed char cee_mac_access_increment(cee_id self, cee_sel cmd) {
    (void)cmd;
    return cee_mac_access_asked(self, CEE_ACCESS_INCREMENT);
}

static signed char cee_mac_access_decrement(cee_id self, cee_sel cmd) {
    (void)cmd;
    return cee_mac_access_asked(self, CEE_ACCESS_DECREMENT);
}

static void cee_mac_access_set_focused(cee_id self, cee_sel cmd, signed char on) {
    (void)cmd;
    if (on) {
        cee_mac_access_asked(self, CEE_ACCESS_FOCUS);
    }
}

static signed char cee_mac_access_is_focused(cee_id self, cee_sel cmd) {
    (void)cmd;
    uint32_t index = 0;
    struct cee_window *w = cee_mac_access_owner(self, &index);
    return w != NULL && (w->access[index].flags & CEE_ACCESS_FOCUSED) != 0;
}

/* A text field's element (CeeSafeAccessText) answers what a reader asks of text: without these VoiceOver says the
   field's name twice and never its contents. The text is the element's value, one line, the caret at its end:
   reading by character and selection are not here (ADR 0164 §2.3). */
static cee_id cee_mac_access_text_of(cee_id self) {
    cee_id value = cee_send(self, "accessibilityValue");
    return value != NULL ? value : cee_nsstring("");
}

static long cee_mac_access_text_count(cee_id self, cee_sel cmd) {
    (void)cmd;
    return (long)cee_send_ulong(cee_mac_access_text_of(self), "length");
}

static cee_range cee_mac_access_text_all(cee_id self, cee_sel cmd) {
    (void)cmd;
    cee_range r = {0, cee_send_ulong(cee_mac_access_text_of(self), "length")};
    return r;
}

/* The UTF-16 units of the first `bytes` bytes of UTF-8 `text`: NSString's offsets are in them. */
static unsigned long cee_utf16_units(const char *text, uint32_t bytes) {
    unsigned long units = 0;
    for (uint32_t k = 0; k < bytes; k++) {
        unsigned char c = (unsigned char)text[k];
        if ((c & 0xc0u) != 0x80u) {
            units += c >= 0xf0u ? 2u : 1u; /* a lead byte: one unit, or two for a scalar past the BMP */
        }
    }
    return units;
}

/* The field's selection, from its node's caret and anchor: the caret alone when they are equal. */
static cee_range cee_mac_access_text_selected(cee_id self, cee_sel cmd) {
    (void)cmd;
    cee_range r = {0, 0};
    uint32_t index = 0;
    struct cee_window *w = cee_mac_access_owner(self, &index);
    if (w == NULL || (w->access[index].flags & CEE_ACCESS_SECURE) != 0) {
        return r;
    }
    const struct cee_access_node *n = &w->access[index];
    uint32_t from = n->caret < n->anchor ? n->caret : n->anchor, to = n->caret < n->anchor ? n->anchor : n->caret;
    r.location = cee_utf16_units(w->access_text + n->value_start, from);
    r.length = cee_utf16_units(w->access_text + n->value_start, to) - r.location;
    return r;
}

static cee_id cee_mac_access_text_string(cee_id self, cee_sel cmd, cee_range range) {
    (void)cmd;
    cee_id text = cee_mac_access_text_of(self);
    unsigned long length = cee_send_ulong(text, "length");
    if (range.location > length) {
        range.location = length;
    }
    if (range.length > length - range.location) {
        range.length = length - range.location;
    }
    return ((cee_id(*)(cee_id, cee_sel, cee_range))cee_objc.send)(text, cee_mac_sel("substringWithRange:"), range);
}

/* VoiceOver reads a field through this one: without it, it reads as many characters of the field's name. */
static cee_id cee_mac_access_text_attributed(cee_id self, cee_sel cmd, cee_range range) {
    cee_id string = cee_mac_access_text_string(self, cmd, range);
    cee_id attributed = cee_send_id_id(cee_send(cee_mac_cls("NSAttributedString"), "alloc"), "initWithString:", string);
    return attributed != NULL ? cee_send(attributed, "autorelease") : NULL;
}

/* A field's value and selection can be set, which is what makes it "edit text" to VoiceOver, with the characters
   the caret passes said; what sets them is the keyboard, through the program. */
static signed char cee_mac_access_text_settable(cee_id self, cee_sel cmd, cee_id attribute) {
    (void)self;
    (void)cmd;
    return cee_send_bool_id(attribute, "isEqualToString:", cee_nsstring("AXValue")) ||
           cee_send_bool_id(attribute, "isEqualToString:", cee_nsstring("AXSelectedTextRange")) ||
           cee_send_bool_id(attribute, "isEqualToString:", cee_nsstring("AXFocused"));
}

static cee_id cee_mac_access_text_selection(cee_id self, cee_sel cmd) {
    return cee_mac_access_text_string(self, cmd, cee_mac_access_text_selected(self, cmd));
}

static long cee_mac_access_text_line(cee_id self, cee_sel cmd) {
    (void)self;
    (void)cmd;
    return 0;
}

static long cee_mac_access_text_line_for(cee_id self, cee_sel cmd, long index) {
    (void)self;
    (void)cmd;
    (void)index;
    return 0;
}

static cee_range cee_mac_access_text_range_for_line(cee_id self, cee_sel cmd, long line) {
    (void)line;
    return cee_mac_access_text_all(self, cmd);
}

static cee_rect cee_mac_access_text_frame_for(cee_id self, cee_sel cmd, cee_range range) {
    (void)range;
    return cee_mac_access_frame(self, cmd);
}

/* The content view's part: its children are the elements, and it is a group of them. */
static cee_id cee_mac_access_children(cee_id self, cee_sel cmd) {
    (void)cmd;
    struct cee_window *w = cee_mac_view_owner(self);
    if (w == NULL) {
        return cee_send(cee_mac_cls("NSArray"), "array");
    }
    w->access_active = 1;
    struct cee_mac *m = (struct cee_mac *)w->native;
    return m->access_top != NULL ? m->access_top : cee_send(cee_mac_cls("NSArray"), "array");
}

static signed char cee_mac_access_view_is_element(cee_id self, cee_sel cmd) {
    (void)self;
    (void)cmd;
    return 1;
}

static cee_id cee_mac_access_view_role(cee_id self, cee_sel cmd) {
    (void)self;
    (void)cmd;
    return cee_nsstring("AXGroup");
}

/* The element at a point of the screen: the last in the list that holds it (drawn later, so on top), or the view. */
static cee_id cee_mac_access_hit(cee_id self, cee_sel cmd, cee_point point) {
    (void)cmd;
    struct cee_window *w = cee_mac_view_owner(self);
    if (w == NULL) {
        return self;
    }
    w->access_active = 1;
    struct cee_mac *m = (struct cee_mac *)w->native;
    cee_point p = ((cee_point(*)(cee_id, cee_sel, cee_point))cee_objc.send)(m->window,
                                                                         cee_mac_sel("convertPointFromScreen:"), point);
    double x = p.x, y = (double)w->height - p.y;
    int64_t k = cee_access_at(w->access, m->access_count < w->access_count ? m->access_count : w->access_count, x, y);
    return k >= 0 ? m->access_elements[k] : self;
}

static cee_id cee_mac_access_focused_element(cee_id self, cee_sel cmd) {
    (void)cmd;
    struct cee_window *w = cee_mac_view_owner(self);
    if (w != NULL) {
        struct cee_mac *m = (struct cee_mac *)w->native;
        for (uint32_t k = 0; k < m->access_count && k < w->access_count; k++) {
            if ((w->access[k].flags & CEE_ACCESS_FOCUSED) != 0) {
                return m->access_elements[k];
            }
        }
    }
    return self;
}

static cee_id cee_mac_number(double value) {
    return ((cee_id(*)(cee_id, cee_sel, double))cee_objc.send)(cee_mac_cls("NSNumber"), cee_mac_sel("numberWithDouble:"),
                                                              value);
}

/* Gives `element` what node `n` says: its role, name, value and state. Its frame it answers itself. */
static void cee_mac_access_apply(cee_id element, const struct cee_access_node *n, const char *text) {
    static const char *const roles[CEE_ACCESS_ROLES] = {
        "AXStaticText", "AXButton", "AXCheckBox", "AXRadioButton", "AXTextField", "AXSlider", "AXRadioButton",
        "AXButton", "AXMenuItem", "AXPopUpButton", "AXImage",
        /* ADR 0207's containers; a column header is a cell of the table's first row, named its column's header */
        "AXGroup", "AXList", "AXMenu", "AXMenuBar", "AXTabGroup", "AXTable", "AXRow", "AXCell", "AXCell"};
    cee_id label = cee_mac_string(text + n->label_start, n->label_length);
    cee_id value = NULL;
    const char *subrole = NULL;
    switch (n->role) {
    case 0: /* text: what is read is its value */
        value = label;
        label = NULL;
        break;
    case 2: case 3:
        value = cee_mac_number((n->flags & CEE_ACCESS_CHECKED) != 0 ? 1.0 : 0.0);
        break;
    case 6: /* a tab is a radio button of the tab kind */
        subrole = "AXTabButton";
        value = cee_mac_number((n->flags & CEE_ACCESS_SELECTED) != 0 ? 1.0 : 0.0);
        break;
    case 5:
        value = cee_mac_number(n->now);
        cee_send_v_id(element, "setAccessibilityMinValue:", cee_mac_number(n->min));
        cee_send_v_id(element, "setAccessibilityMaxValue:", cee_mac_number(n->max));
        /* The value as the program writes it, when it gives one: a reader says a slider's value as a share of its
           range otherwise, which is wrong for a count ("3%" for 3 of 99). */
        cee_send_v_id(element, "setAccessibilityValueDescription:",
                      n->value_length > 0 ? cee_mac_string(text + n->value_start, n->value_length) : NULL);
        break;
    case 4:
        if ((n->flags & CEE_ACCESS_SECURE) != 0) {
            subrole = "AXSecureTextField"; /* its text is not given */
            value = cee_nsstring("");
            break;
        }
        /* An empty field has no value: given an empty string, VoiceOver says the field's name in its place. */
        value = n->value_length > 0 ? cee_mac_string(text + n->value_start, n->value_length) : NULL;
        break;
    case 9:
        value = cee_mac_string(text + n->value_start, n->value_length);
        break;
    default:
        break;
    }
    cee_send_v_id(element, "setAccessibilityRole:", cee_nsstring(roles[n->role]));
    cee_send_v_id(element, "setAccessibilitySubrole:", subrole != NULL ? cee_nsstring(subrole) : NULL);
    cee_send_v_id(element, "setAccessibilityLabel:", label);
    cee_send_v_id(element, "setAccessibilityValue:", value);
    cee_send_v_bool(element, "setAccessibilityEnabled:", (n->flags & CEE_ACCESS_DISABLED) == 0);
    cee_send_v_bool(element, "setAccessibilitySelected:", (n->flags & CEE_ACCESS_SELECTED) != 0);
    if (n->role == 8 || n->role == 9) {
        cee_send_v_bool(element, "setAccessibilityExpanded:", (n->flags & CEE_ACCESS_EXPANDED) != 0);
    }
    /* Places and counts (ADR 0207): a reader says "3 of 40", "row 2, column 3". */
    if (n->role == CEE_ACCESS_TABLE || n->role == CEE_ACCESS_LIST) {
        cee_send_v_long(element, "setAccessibilityRowCount:", (long)n->rows);
    }
    if (n->role == CEE_ACCESS_TABLE) {
        cee_send_v_long(element, "setAccessibilityColumnCount:", (long)n->columns);
    }
    if (n->role == CEE_ACCESS_CELL || n->role == CEE_ACCESS_COLUMN_HEADER) {
        cee_range rows = {n->row > 0 ? n->row - 1u : 0u, 1u}, columns = {n->column > 0 ? n->column - 1u : 0u, 1u};
        ((void (*)(cee_id, cee_sel, cee_range))cee_objc.send)(element, cee_mac_sel("setAccessibilityRowIndexRange:"),
                                                              rows);
        ((void (*)(cee_id, cee_sel, cee_range))cee_objc.send)(
            element, cee_mac_sel("setAccessibilityColumnIndexRange:"), columns);
    } else if (n->up != CEE_ACCESS_TOP && n->row > 0 && n->column == 0) {
        /* an item's place in its list, menu or table; what is in a table's row has a column, not an index */
        cee_send_v_long(element, "setAccessibilityIndex:", (long)n->row - 1);
    }
}

static cee_id cee_mac_array(const cee_id *items, unsigned long count) {
    return ((cee_id(*)(cee_id, cee_sel, const cee_id *, unsigned long))cee_objc.send)(
        cee_mac_cls("NSArray"), cee_mac_sel("arrayWithObjects:count:"), items, count);
}

/* The tree (ADR 0207): each element's parent (its container's element, or the view), each container's children, a
   table's rows and column headers, a tab list's tabs; and the view's own children, the top level's, kept in
   `m->access_top`. Called when the list's shape may have changed. */
static void cee_mac_access_tree(struct cee_window *w, cee_id *elements, uint32_t count) {
    struct cee_mac *m = (struct cee_mac *)w->native;
    cee_id *items = count > 0 ? (cee_id *)ceesafe_platform_alloc((size_t)count * sizeof *items) : NULL;
    cee_id *headers = count > 0 ? (cee_id *)ceesafe_platform_alloc((size_t)count * sizeof *headers) : NULL;
    if (count > 0 && (items == NULL || headers == NULL)) {
        ceesafe_platform_free(items);
        ceesafe_platform_free(headers);
        return;
    }
    unsigned long top = 0;
    for (uint32_t k = 0; k < count; k++) {
        const struct cee_access_node *n = &w->access[k];
        if (elements[k] == NULL) {
            continue;
        }
        cee_send_v_id(elements[k], "setAccessibilityParent:", n->up == CEE_ACCESS_TOP ? m->view : elements[n->up]);
        if (n->up == CEE_ACCESS_TOP) {
            items[top++] = elements[k];
        }
    }
    cee_id view_children = cee_mac_array(items, top);
    if (m->access_top != NULL) {
        cee_send_v(m->access_top, "release");
    }
    m->access_top = cee_send(view_children, "retain");
    for (uint32_t c = 0; c < count; c++) { /* each container: its children, in list order */
        const struct cee_access_node *p = &w->access[c];
        if (!CEE_ACCESS_CONTAINER(p->role) || elements[c] == NULL) {
            continue;
        }
        unsigned long held = 0, heads = 0;
        for (uint32_t k = c + 1; k < count; k++) {
            if (w->access[k].up == c && elements[k] != NULL) {
                items[held++] = elements[k];
            }
            if (p->role == CEE_ACCESS_TABLE && w->access[k].role == CEE_ACCESS_COLUMN_HEADER &&
                w->access[k].up != CEE_ACCESS_TOP && w->access[w->access[k].up].up == c && elements[k] != NULL) {
                headers[heads++] = elements[k];
            }
        }
        cee_id children = cee_mac_array(items, held);
        cee_send_v_id(elements[c], "setAccessibilityChildren:", children);
        if (p->role == CEE_ACCESS_TABLE) {
            cee_send_v_id(elements[c], "setAccessibilityRows:", children);
            cee_send_v_id(elements[c], "setAccessibilityVisibleRows:", children);
            cee_send_v_id(elements[c], "setAccessibilityColumnHeaderUIElements:", cee_mac_array(headers, heads));
        } else if (p->role == CEE_ACCESS_TAB_LIST) {
            cee_send_v_id(elements[c], "setAccessibilityTabs:", children);
        }
    }
    ceesafe_platform_free(items);
    ceesafe_platform_free(headers);
}

static int cee_access_same_text(const char *a, uint32_t a_start, uint32_t a_length, const char *b, uint32_t b_start,
                                uint32_t b_length) {
    return a_length == b_length && (a_length == 0 || memcmp(a + a_start, b + b_start, a_length) == 0);
}

/* The window's list changed (w->access is the new one; `old` the last, with its text): the elements follow it, and
   the system is told what a reader needs to hear. */
static void cee_native_access(struct cee_window *w, const struct cee_access_node *old, uint32_t old_count,
                              const char *old_text) {
    struct cee_mac *m = (struct cee_mac *)w->native;
    if (m == NULL || cee_objc.access_class == NULL) {
        return;
    }
    void *pool = cee_objc.pool_push();
    uint32_t count = w->access_count;
    if (old_count > m->access_count) {
        old_count = m->access_count; /* a list whose elements could not be made */
    }
    cee_id *elements = count > 0 ? (cee_id *)ceesafe_platform_alloc_zeroed((size_t)count * sizeof *elements) : NULL;
    if (count > 0 && elements == NULL) {
        count = 0; /* out of memory: the window has no elements until the next list */
    }
    int layout = count != old_count;
    for (uint32_t k = 0; k < count; k++) {
        const struct cee_access_node *n = &w->access[k];
        uint32_t j = old_count;
        if (k < old_count && old[k].id == n->id) {
            j = k;
        } else {
            for (uint32_t i = 0; i < old_count; i++) {
                if (old[i].id == n->id) {
                    j = i;
                    break;
                }
            }
            layout = 1;
        }
        /* A text field's element is of its own class: a node that became one, or stopped being one, gets a new one. */
        if (j < old_count && m->access_elements[j] != NULL && (old[j].role == 4) == (n->role == 4)) {
            const struct cee_access_node *o = &old[j];
            cee_id element = m->access_elements[j];
            m->access_elements[j] = NULL;
            elements[k] = element;
            int named = !cee_access_same_text(w->access_text, n->label_start, n->label_length, old_text, o->label_start,
                                              o->label_length);
            if (n->parent != o->parent || n->row != o->row || n->column != o->column) {
                layout = 1; /* the tree is laid out again below */
            }
            int valued = n->now != o->now || n->min != o->min || n->max != o->max ||
                         ((n->flags ^ o->flags) & (CEE_ACCESS_CHECKED | CEE_ACCESS_SELECTED | CEE_ACCESS_EXPANDED)) != 0 ||
                         !cee_access_same_text(w->access_text, n->value_start, n->value_length, old_text, o->value_start,
                                               o->value_length);
            if (named || valued || n->role != o->role || n->flags != o->flags || n->row != o->row ||
                n->column != o->column || n->rows != o->rows || n->columns != o->columns || n->up != o->up) {
                cee_mac_access_apply(element, n, w->access_text);
            }
            if (w->access_active && cee_objc.access_post != NULL) {
                if (named) {
                    cee_objc.access_post(element, cee_nsstring(n->role == 0 ? "AXValueChanged" : "AXTitleChanged"));
                }
                if (valued) {
                    cee_objc.access_post(element, cee_nsstring("AXValueChanged"));
                }
                if (n->role == 4 && (valued || n->caret != o->caret || n->anchor != o->anchor)) {
                    cee_objc.access_post(element, cee_nsstring("AXSelectedTextChanged")); /* the caret moved */
                }
                if ((n->flags & CEE_ACCESS_FOCUSED) != 0 && (o->flags & CEE_ACCESS_FOCUSED) == 0) {
                    cee_objc.access_post(element, cee_nsstring("AXFocusedUIElementChanged"));
                }
            }
        } else {
            cee_id element = cee_send(
                cee_send(n->role == 4 && cee_objc.access_text_class != NULL ? cee_objc.access_text_class
                                                                              : cee_objc.access_class,
                         "alloc"),
                "init");
            elements[k] = element;
            if (element != NULL) {
                cee_send_v_id(element, "setAccessibilityParent:", m->view);
                cee_mac_access_apply(element, n, w->access_text);
            }
            layout = 1;
        }
    }
    for (uint32_t j = 0; j < m->access_count; j++) { /* the elements the new list does not have */
        if (m->access_elements[j] != NULL) {
            cee_send_v_id(m->access_elements[j], "setAccessibilityParent:", NULL);
            cee_send_v(m->access_elements[j], "release");
            layout = 1;
        }
    }
    ceesafe_platform_free(m->access_elements);
    m->access_elements = elements;
    m->access_count = count;
    if (layout || m->access_top == NULL) {
        cee_mac_access_tree(w, elements, count);
    }
    if (layout && w->access_active && cee_objc.access_post != NULL) {
        cee_objc.access_post(m->view, cee_nsstring("AXLayoutChanged"));
    }
    cee_objc.pool_pop(pool);
}

/* Says `text` through the reader: an announcement of the application's, at high priority. */
static void cee_native_announce(struct cee_window *w, const char *text, size_t length) {
    struct cee_mac *m = (struct cee_mac *)w->native;
    if (m == NULL || cee_objc.access_post_info == NULL) {
        return;
    }
    void *pool = cee_objc.pool_push();
    cee_id keys[2] = {cee_nsstring("AXAnnouncementKey"), cee_nsstring("AXPriorityKey")};
    cee_id values[2] = {cee_mac_string(text, length), cee_mac_number(90.0)}; /* NSAccessibilityPriorityHigh */
    cee_id info = ((cee_id(*)(cee_id, cee_sel, const cee_id *, const cee_id *, unsigned long))cee_objc.send)(
        cee_mac_cls("NSDictionary"), cee_mac_sel("dictionaryWithObjects:forKeys:count:"), values, keys, 2ul);
    cee_objc.access_post_info(m->window, cee_nsstring("AXAnnouncementRequested"), info);
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
        cee_objc.add_method(view, cee_mac_sel("accessibilityChildren"), (cee_fn)cee_mac_access_children, "@@:");
        /* The list's order is the reading order: without this VoiceOver sorts the elements by where they are, and
           reads a sidebar's rows between a page's. */
        cee_objc.add_method(view, cee_mac_sel("accessibilityChildrenInNavigationOrder"),
                            (cee_fn)cee_mac_access_children, "@@:");
        cee_objc.add_method(view, cee_mac_sel("isAccessibilityElement"), (cee_fn)cee_mac_access_view_is_element, "c@:");
        cee_objc.add_method(view, cee_mac_sel("accessibilityRole"), (cee_fn)cee_mac_access_view_role, "@@:");
        cee_objc.add_method(view, cee_mac_sel("accessibilityHitTest:"), (cee_fn)cee_mac_access_hit, "@@:{CGPoint=dd}");
        cee_objc.add_method(view, cee_mac_sel("accessibilityFocusedUIElement"), (cee_fn)cee_mac_access_focused_element,
                            "@@:");
        cee_objc.register_class(view);
    }
    cee_objc.view_class = view;
    /* Accessibility (ADR 0164): the view's children are the window's elements; an element answers from its node. */
    cee_objc.access_post = (void (*)(cee_id, cee_id))cee_mac_symbol(appkit, "NSAccessibilityPostNotification");
    cee_objc.access_post_info =
        (void (*)(cee_id, cee_id, cee_id))cee_mac_symbol(appkit, "NSAccessibilityPostNotificationWithUserInfo");
    cee_id access = cee_objc.allocate_class(cee_mac_cls("NSAccessibilityElement"), "CeeSafeAccessElement", 0);
    if (access == NULL) {
        access = cee_mac_cls("CeeSafeAccessElement");
    } else {
        cee_objc.add_method(access, cee_mac_sel("accessibilityFrame"), (cee_fn)cee_mac_access_frame,
                            "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
        cee_objc.add_method(access, cee_mac_sel("accessibilityPerformPress"), (cee_fn)cee_mac_access_press, "c@:");
        cee_objc.add_method(access, cee_mac_sel("accessibilityPerformIncrement"), (cee_fn)cee_mac_access_increment, "c@:");
        cee_objc.add_method(access, cee_mac_sel("accessibilityPerformDecrement"), (cee_fn)cee_mac_access_decrement, "c@:");
        cee_objc.add_method(access, cee_mac_sel("setAccessibilityFocused:"), (cee_fn)cee_mac_access_set_focused, "v@:c");
        cee_objc.add_method(access, cee_mac_sel("isAccessibilityFocused"), (cee_fn)cee_mac_access_is_focused, "c@:");
        cee_objc.register_class(access);
    }
    cee_objc.access_class = access;
    cee_id access_text = access != NULL ? cee_objc.allocate_class(access, "CeeSafeAccessText", 0) : NULL;
    if (access_text == NULL) {
        access_text = cee_mac_cls("CeeSafeAccessText");
    } else {
        cee_objc.add_method(access_text, cee_mac_sel("accessibilityNumberOfCharacters"),
                            (cee_fn)cee_mac_access_text_count, "q@:");
        cee_objc.add_method(access_text, cee_mac_sel("accessibilityVisibleCharacterRange"),
                            (cee_fn)cee_mac_access_text_all, "{_NSRange=QQ}@:");
        cee_objc.add_method(access_text, cee_mac_sel("accessibilitySelectedTextRange"),
                            (cee_fn)cee_mac_access_text_selected, "{_NSRange=QQ}@:");
        cee_objc.add_method(access_text, cee_mac_sel("accessibilitySelectedText"), (cee_fn)cee_mac_access_text_selection,
                            "@@:");
        cee_objc.add_method(access_text, cee_mac_sel("accessibilityStringForRange:"),
                            (cee_fn)cee_mac_access_text_string, "@@:{_NSRange=QQ}");
        cee_objc.add_method(access_text, cee_mac_sel("accessibilityAttributedStringForRange:"),
                            (cee_fn)cee_mac_access_text_attributed, "@@:{_NSRange=QQ}");
        cee_objc.add_method(access_text, cee_mac_sel("accessibilityIsAttributeSettable:"),
                            (cee_fn)cee_mac_access_text_settable, "c@:@");
        cee_objc.add_method(access_text, cee_mac_sel("accessibilityInsertionPointLineNumber"),
                            (cee_fn)cee_mac_access_text_line, "q@:");
        cee_objc.add_method(access_text, cee_mac_sel("accessibilityLineForIndex:"),
                            (cee_fn)cee_mac_access_text_line_for, "q@:q");
        cee_objc.add_method(access_text, cee_mac_sel("accessibilityRangeForLine:"),
                            (cee_fn)cee_mac_access_text_range_for_line, "{_NSRange=QQ}@:q");
        cee_objc.add_method(access_text, cee_mac_sel("accessibilityFrameForRange:"),
                            (cee_fn)cee_mac_access_text_frame_for, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{_NSRange=QQ}");
        cee_objc.register_class(access_text);
    }
    cee_objc.access_text_class = access_text;
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
    /* Titled, closable, miniaturizable, resizable; for the program's title bar (ADR 0206) also a full-size content
       view; buffered backing. */
    w->program_bar = w->wants_bar;
    unsigned long style = 15ul | (w->program_bar ? 1ul << 15 /* NSWindowStyleMaskFullSizeContentView */ : 0ul);
    window = ((cee_id(*)(cee_id, cee_sel, cee_rect, unsigned long, unsigned long, signed char))cee_objc.send)(
        window, cee_mac_sel("initWithContentRect:styleMask:backing:defer:"), frame, style, 2ul, 0);
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
    if (w->program_bar) { /* the title stays the window's (the Window menu, Mission Control), not drawn */
        cee_send_v_bool(window, "setTitlebarAppearsTransparent:", 1);
        cee_send_v_long(window, "setTitleVisibility:", 1 /* NSWindowTitleHidden */);
    }
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
    cee_mac_place_buttons(w);
    cee_objc.pool_pop(pool);
    return 0;
}

static void cee_native_close(struct cee_window *w) {
    struct cee_mac *m = (struct cee_mac *)w->native;
    if (m == NULL) {
        return;
    }
    void *pool = cee_objc.pool_push();
    for (uint32_t k = 0; k < m->access_count; k++) { /* the window's elements (ADR 0164) */
        if (m->access_elements[k] != NULL) {
            cee_send_v_id(m->access_elements[k], "setAccessibilityParent:", NULL);
            cee_send_v(m->access_elements[k], "release");
        }
    }
    ceesafe_platform_free(m->access_elements);
    m->access_elements = NULL;
    m->access_count = 0;
    if (m->access_top != NULL) {
        cee_send_v(m->access_top, "release");
        m->access_top = NULL;
    }
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
            int bar = type == 1 && inside ? cee_mac_bar_press(w, event, x, y) : 0;
            if (bar == 1) {
                return 0; /* the runtime's (ADR 0206) */
            }
            if (bar == -2) {
                return 1; /* the system's */
            }
            if (!down || inside) {
                cee_window_push(w, CEE_EVENT_BUTTON, button, down, 0, 0, x, y);
            }
            if (bar == -1) {
                return 0;
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

/* Printing a PDF (ADR 0178): PDFKit's PDFDocument and its print operation (the pages scaled to the paper, turned to
   fit), whose print panel runs app-modally, as the file dialogs do. PDFKit is loaded with dlopen, as AppKit is. */
#define CEE_HAS_NATIVE_PRINT 1
static int32_t cee_native_print(struct cee_window *w, const char *title, const char *file) {
    struct cee_mac *m = (struct cee_mac *)w->native;
    static int pdfkit; /* 0 not tried, 1 loaded, -1 not there */
    if (pdfkit == 0) {
        void *kit = dlopen("/System/Library/Frameworks/PDFKit.framework/PDFKit", RTLD_NOW | RTLD_GLOBAL);
        pdfkit = kit != NULL && cee_mac_cls("PDFDocument") != NULL ? 1 : -1;
    }
    if (pdfkit != 1) {
        return -CEE_WINDOW_UNSUPPORTED;
    }
    void *pool = cee_objc.pool_push();
    cee_id url = cee_send_id_id(cee_mac_cls("NSURL"), "fileURLWithPath:", cee_nsstring(file));
    cee_id document = url != NULL ? cee_send_id_id(cee_send(cee_mac_cls("PDFDocument"), "alloc"), "initWithURL:", url)
                                  : NULL;
    if (document == NULL) {
        cee_objc.pool_pop(pool);
        return -CEE_WINDOW_RESOURCES; /* not a PDF this can read */
    }
    cee_id info = cee_send(cee_mac_cls("NSPrintInfo"), "sharedPrintInfo");
    /* kPDFPrintPageScaleToFit (1), and autoRotate. */
    cee_id operation = ((cee_id(*)(cee_id, cee_sel, cee_id, long, signed char))cee_objc.send)(
        document, cee_mac_sel("printOperationForPrintInfo:scalingMode:autoRotate:"), info, 1l, 1);
    int32_t result = -CEE_WINDOW_RESOURCES;
    if (operation != NULL) {
        if (title[0] != '\0') {
            cee_send_v_id(operation, "setJobTitle:", cee_nsstring(title));
        }
        cee_send_v_bool(operation, "setShowsPrintPanel:", 1);
        cee_send_v_bool(operation, "setShowsProgressPanel:", 1);
        cee_send_v_bool(cee_objc.app, "activateIgnoringOtherApps:", 1);
        signed char printed =
            ((signed char (*)(cee_id, cee_sel))cee_objc.send)(operation, cee_mac_sel("runOperation"));
        result = printed ? 1 : 0;
    }
    cee_send_v(document, "release");
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
#include <stddef.h> /* offsetof, for a COM object's interfaces (ADR 0164) */
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
    /* Accessibility (ADR 0164): the window's UI Automation provider and each node's, in the list's order, each
       holding a reference; and what a client asked, for the pump to queue (a provider may be called on a thread
       of UI Automation's). All under cee_uia_lock. */
    struct cee_uia_root *uia_root;
    struct cee_uia_element **uia_elements;
    uint32_t uia_count;
    struct cee_access_request uia_asked[CEE_ACCESS_QUEUE];
    unsigned uia_asked_count;
    int uia_active;
    int uia_focus_told; /* whether clients were told where the focus is since the provider was first given out */
    int nc_tracked; /* ADR 0206: the pointer is over the program's maximize box, a frame area: its leaving is tracked */
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

/* ---- Accessibility on Windows (ADR 0164): UI Automation providers ------------------------------------------------
   The window answers WM_GETOBJECT with a fragment root whose children are the list's elements, each a provider of
   its own that is kept from one list to the next while its id is in both. The interfaces are written here as their
   vtables, in the order UIAutomationCore.h declares them, and their functions come from uiautomationcore.dll and
   oleaut32.dll, loaded at first use, so no program needs a header or a link flag. NVDA and Narrator read through
   these. */
#define CEE_WINDOW_ACCESS 1

static const IID cee_iid_unknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID cee_iid_uia_simple = {0xd6dd68d1, 0x86fd, 0x4332, {0x86, 0x66, 0x9a, 0xbe, 0xde, 0xa2, 0xd2, 0x4c}};
static const IID cee_iid_uia_fragment = {0xf7063da8, 0x8359, 0x439c, {0x92, 0x97, 0xbb, 0xc5, 0x29, 0x9a, 0x7d, 0x87}};
static const IID cee_iid_uia_root = {0x620ce2a5, 0xab8f, 0x40a9, {0x86, 0xcb, 0xde, 0x3c, 0x75, 0x59, 0x9b, 0x58}};
static const IID cee_iid_uia_invoke = {0x54fcb24b, 0xe18e, 0x47a2, {0xb4, 0xd3, 0xec, 0xcb, 0xe7, 0x75, 0x99, 0xa2}};
static const IID cee_iid_uia_toggle = {0x56d00bd0, 0xc4f4, 0x433c, {0xa8, 0x36, 0x1a, 0x52, 0xa5, 0x7e, 0x08, 0x92}};
static const IID cee_iid_uia_range = {0x36dc7aef, 0x33e6, 0x4691, {0xaf, 0xe1, 0x2b, 0xe7, 0x27, 0x4b, 0x3d, 0x33}};
static const IID cee_iid_uia_value = {0xc7935180, 0x6fb3, 0x4201, {0xb1, 0x74, 0x7d, 0xf7, 0x3a, 0xdb, 0xf6, 0x4a}};
static const IID cee_iid_uia_selection = {0x2acad808, 0xb2d4, 0x452d, {0xa4, 0x07, 0x91, 0xff, 0x1a, 0xd1, 0x67, 0xb2}};
static const IID cee_iid_uia_expand = {0xd847d3a5, 0xcab0, 0x4a98, {0x8c, 0x32, 0xec, 0xb4, 0x5c, 0x59, 0xad, 0x24}};
/* ADR 0207: a table's (IGridProvider, ITableProvider) and its cells' (IGridItemProvider, ITableItemProvider). */
static const IID cee_iid_uia_grid = {0xb17d6187, 0x0907, 0x464b, {0xa1, 0x68, 0x0e, 0xf1, 0x7a, 0x15, 0x72, 0xb1}};
static const IID cee_iid_uia_grid_item = {0xd02541f1, 0xfb81, 0x4d64, {0xae, 0x32, 0xf5, 0x20, 0xf8, 0xa6, 0xdb, 0xd1}};
static const IID cee_iid_uia_table = {0x9c860395, 0x97b3, 0x490a, {0xb5, 0x2a, 0x85, 0x8c, 0xc2, 0x2a, 0xf1, 0x66}};
static const IID cee_iid_uia_table_item = {0xb9734fa6, 0x771f, 0x4d78, {0x9c, 0x90, 0x25, 0x17, 0x99, 0x93, 0x49, 0xcd}};

/* UI Automation's numbers (UIAutomationClient.h). */
#define CEE_UIA_ROOT_OBJECT (-25)
#define CEE_UIA_SERVER_SIDE 0x2 /* ProviderOptions_ServerSideProvider (0x1 is a client-side one, whose events go nowhere) */
#define CEE_UIA_E_NOT_ENABLED ((HRESULT)0x80040200L)
#define CEE_UIA_E_NOT_AVAILABLE ((HRESULT)0x80040201L)
#define CEE_UIA_PATTERN_INVOKE 10000
#define CEE_UIA_PATTERN_VALUE 10002
#define CEE_UIA_PATTERN_RANGE 10003
#define CEE_UIA_PATTERN_EXPAND 10005
#define CEE_UIA_PATTERN_SELECTION_ITEM 10010
#define CEE_UIA_PATTERN_TOGGLE 10015
#define CEE_UIA_PATTERN_GRID 10006
#define CEE_UIA_PATTERN_GRID_ITEM 10007
#define CEE_UIA_PATTERN_TABLE 10012
#define CEE_UIA_PATTERN_TABLE_ITEM 10013
#define CEE_UIA_PROPERTY_POSITION_IN_SET 30152 /* Windows 10 1703 on: "3 of 40" */
#define CEE_UIA_PROPERTY_SIZE_OF_SET 30153
#define CEE_UIA_PROPERTY_CONTROL_TYPE 30003
#define CEE_UIA_PROPERTY_NAME 30005
#define CEE_UIA_PROPERTY_HAS_FOCUS 30008
#define CEE_UIA_PROPERTY_FOCUSABLE 30009
#define CEE_UIA_PROPERTY_ENABLED 30010
#define CEE_UIA_PROPERTY_AUTOMATION_ID 30011
#define CEE_UIA_PROPERTY_CONTROL_ELEMENT 30016
#define CEE_UIA_PROPERTY_CONTENT_ELEMENT 30017
#define CEE_UIA_PROPERTY_PASSWORD 30019
#define CEE_UIA_PROPERTY_FRAMEWORK 30024
#define CEE_UIA_PROPERTY_VALUE 30045
#define CEE_UIA_PROPERTY_RANGE_VALUE 30047
#define CEE_UIA_PROPERTY_EXPAND_STATE 30070
#define CEE_UIA_PROPERTY_IS_SELECTED 30079
#define CEE_UIA_PROPERTY_TOGGLE_STATE 30086
#define CEE_UIA_EVENT_FOCUS 20005
#define CEE_UIA_CHILDREN_INVALIDATED 2

struct cee_uia_rect {
    double left, top, width, height;
};

/* The vtables. `self` is the address of the object's pointer to the vtable. */
struct cee_uia_simple_vtbl {
    HRESULT(STDMETHODCALLTYPE *query)(void *self, REFIID iid, void **out);
    ULONG(STDMETHODCALLTYPE *add_ref)(void *self);
    ULONG(STDMETHODCALLTYPE *release)(void *self);
    HRESULT(STDMETHODCALLTYPE *options)(void *self, int *out);
    HRESULT(STDMETHODCALLTYPE *pattern)(void *self, int pattern, void **out);
    HRESULT(STDMETHODCALLTYPE *property)(void *self, int property, VARIANT *out);
    HRESULT(STDMETHODCALLTYPE *host)(void *self, void **out);
};
struct cee_uia_fragment_vtbl {
    HRESULT(STDMETHODCALLTYPE *query)(void *self, REFIID iid, void **out);
    ULONG(STDMETHODCALLTYPE *add_ref)(void *self);
    ULONG(STDMETHODCALLTYPE *release)(void *self);
    HRESULT(STDMETHODCALLTYPE *navigate)(void *self, int direction, void **out);
    HRESULT(STDMETHODCALLTYPE *runtime_id)(void *self, SAFEARRAY **out);
    HRESULT(STDMETHODCALLTYPE *bounds)(void *self, struct cee_uia_rect *out);
    HRESULT(STDMETHODCALLTYPE *embedded)(void *self, SAFEARRAY **out);
    HRESULT(STDMETHODCALLTYPE *set_focus)(void *self);
    HRESULT(STDMETHODCALLTYPE *fragment_root)(void *self, void **out);
};
struct cee_uia_root_vtbl {
    HRESULT(STDMETHODCALLTYPE *query)(void *self, REFIID iid, void **out);
    ULONG(STDMETHODCALLTYPE *add_ref)(void *self);
    ULONG(STDMETHODCALLTYPE *release)(void *self);
    HRESULT(STDMETHODCALLTYPE *from_point)(void *self, double x, double y, void **out);
    HRESULT(STDMETHODCALLTYPE *focus)(void *self, void **out);
};
struct cee_uia_invoke_vtbl {
    HRESULT(STDMETHODCALLTYPE *query)(void *self, REFIID iid, void **out);
    ULONG(STDMETHODCALLTYPE *add_ref)(void *self);
    ULONG(STDMETHODCALLTYPE *release)(void *self);
    HRESULT(STDMETHODCALLTYPE *invoke)(void *self);
};
struct cee_uia_toggle_vtbl {
    HRESULT(STDMETHODCALLTYPE *query)(void *self, REFIID iid, void **out);
    ULONG(STDMETHODCALLTYPE *add_ref)(void *self);
    ULONG(STDMETHODCALLTYPE *release)(void *self);
    HRESULT(STDMETHODCALLTYPE *toggle)(void *self);
    HRESULT(STDMETHODCALLTYPE *state)(void *self, int *out);
};
struct cee_uia_range_vtbl {
    HRESULT(STDMETHODCALLTYPE *query)(void *self, REFIID iid, void **out);
    ULONG(STDMETHODCALLTYPE *add_ref)(void *self);
    ULONG(STDMETHODCALLTYPE *release)(void *self);
    HRESULT(STDMETHODCALLTYPE *set_value)(void *self, double value);
    HRESULT(STDMETHODCALLTYPE *value)(void *self, double *out);
    HRESULT(STDMETHODCALLTYPE *read_only)(void *self, BOOL *out);
    HRESULT(STDMETHODCALLTYPE *maximum)(void *self, double *out);
    HRESULT(STDMETHODCALLTYPE *minimum)(void *self, double *out);
    HRESULT(STDMETHODCALLTYPE *large_change)(void *self, double *out);
    HRESULT(STDMETHODCALLTYPE *small_change)(void *self, double *out);
};
struct cee_uia_value_vtbl {
    HRESULT(STDMETHODCALLTYPE *query)(void *self, REFIID iid, void **out);
    ULONG(STDMETHODCALLTYPE *add_ref)(void *self);
    ULONG(STDMETHODCALLTYPE *release)(void *self);
    HRESULT(STDMETHODCALLTYPE *set_value)(void *self, LPCWSTR value);
    HRESULT(STDMETHODCALLTYPE *value)(void *self, BSTR *out);
    HRESULT(STDMETHODCALLTYPE *read_only)(void *self, BOOL *out);
};
struct cee_uia_selection_vtbl {
    HRESULT(STDMETHODCALLTYPE *query)(void *self, REFIID iid, void **out);
    ULONG(STDMETHODCALLTYPE *add_ref)(void *self);
    ULONG(STDMETHODCALLTYPE *release)(void *self);
    HRESULT(STDMETHODCALLTYPE *select)(void *self);
    HRESULT(STDMETHODCALLTYPE *add)(void *self);
    HRESULT(STDMETHODCALLTYPE *remove)(void *self);
    HRESULT(STDMETHODCALLTYPE *is_selected)(void *self, BOOL *out);
    HRESULT(STDMETHODCALLTYPE *container)(void *self, void **out);
};
struct cee_uia_expand_vtbl {
    HRESULT(STDMETHODCALLTYPE *query)(void *self, REFIID iid, void **out);
    ULONG(STDMETHODCALLTYPE *add_ref)(void *self);
    ULONG(STDMETHODCALLTYPE *release)(void *self);
    HRESULT(STDMETHODCALLTYPE *expand)(void *self);
    HRESULT(STDMETHODCALLTYPE *collapse)(void *self);
    HRESULT(STDMETHODCALLTYPE *state)(void *self, int *out);
};

struct cee_uia_grid_vtbl {
    HRESULT(STDMETHODCALLTYPE *query)(void *self, REFIID iid, void **out);
    ULONG(STDMETHODCALLTYPE *add_ref)(void *self);
    ULONG(STDMETHODCALLTYPE *release)(void *self);
    HRESULT(STDMETHODCALLTYPE *item)(void *self, int row, int column, void **out);
    HRESULT(STDMETHODCALLTYPE *rows)(void *self, int *out);
    HRESULT(STDMETHODCALLTYPE *columns)(void *self, int *out);
};
struct cee_uia_grid_item_vtbl {
    HRESULT(STDMETHODCALLTYPE *query)(void *self, REFIID iid, void **out);
    ULONG(STDMETHODCALLTYPE *add_ref)(void *self);
    ULONG(STDMETHODCALLTYPE *release)(void *self);
    HRESULT(STDMETHODCALLTYPE *row)(void *self, int *out);
    HRESULT(STDMETHODCALLTYPE *column)(void *self, int *out);
    HRESULT(STDMETHODCALLTYPE *row_span)(void *self, int *out);
    HRESULT(STDMETHODCALLTYPE *column_span)(void *self, int *out);
    HRESULT(STDMETHODCALLTYPE *grid)(void *self, void **out);
};
struct cee_uia_table_vtbl {
    HRESULT(STDMETHODCALLTYPE *query)(void *self, REFIID iid, void **out);
    ULONG(STDMETHODCALLTYPE *add_ref)(void *self);
    ULONG(STDMETHODCALLTYPE *release)(void *self);
    HRESULT(STDMETHODCALLTYPE *row_headers)(void *self, SAFEARRAY **out);
    HRESULT(STDMETHODCALLTYPE *column_headers)(void *self, SAFEARRAY **out);
    HRESULT(STDMETHODCALLTYPE *major)(void *self, int *out);
};
struct cee_uia_table_item_vtbl {
    HRESULT(STDMETHODCALLTYPE *query)(void *self, REFIID iid, void **out);
    ULONG(STDMETHODCALLTYPE *add_ref)(void *self);
    ULONG(STDMETHODCALLTYPE *release)(void *self);
    HRESULT(STDMETHODCALLTYPE *row_headers)(void *self, SAFEARRAY **out);
    HRESULT(STDMETHODCALLTYPE *column_headers)(void *self, SAFEARRAY **out);
};

/* The window's provider: a fragment root. `w` is NULL once the window closed. */
struct cee_uia_root {
    const struct cee_uia_simple_vtbl *simple;
    const struct cee_uia_fragment_vtbl *fragment;
    const struct cee_uia_root_vtbl *root;
    LONG refs;
    struct cee_window *w;
};

/* An element's provider. `w` is NULL once the element left the list or the window closed; `hint` is where it was
   in the list when last looked for. */
struct cee_uia_element {
    const struct cee_uia_simple_vtbl *simple;
    const struct cee_uia_fragment_vtbl *fragment;
    const struct cee_uia_invoke_vtbl *invoke;
    const struct cee_uia_toggle_vtbl *toggle;
    const struct cee_uia_range_vtbl *range;
    const struct cee_uia_value_vtbl *value;
    const struct cee_uia_selection_vtbl *selection;
    const struct cee_uia_expand_vtbl *expand;
    const struct cee_uia_grid_vtbl *grid;
    const struct cee_uia_grid_item_vtbl *grid_item;
    const struct cee_uia_table_vtbl *table;
    const struct cee_uia_table_item_vtbl *table_item;
    LONG refs;
    struct cee_window *w;
    uint64_t id;
    uint32_t hint;
};

static struct {
    int state; /* 0 not tried, 1 loaded, -1 failed */
    CRITICAL_SECTION lock;
    LRESULT(WINAPI *return_provider)(HWND, WPARAM, LPARAM, void *);
    HRESULT(WINAPI *host_provider)(HWND, void **);
    HRESULT(WINAPI *raise_event)(void *, int);
    HRESULT(WINAPI *raise_property)(void *, int, VARIANT, VARIANT);
    HRESULT(WINAPI *raise_structure)(void *, int, int *, int);
    BOOL(WINAPI *listening)(void);
    HRESULT(WINAPI *raise_notification)(void *, int, int, BSTR, BSTR); /* Windows 10 1709 on; NULL before */
    BSTR(WINAPI *string)(const OLECHAR *, UINT);
    void(WINAPI *string_free)(BSTR);
    SAFEARRAY *(WINAPI *array)(VARTYPE, LONG, ULONG);
    HRESULT(WINAPI *array_put)(SAFEARRAY *, LONG *, void *);
} cee_uia;

static int cee_uia_load(void) {
    if (cee_uia.state != 0) {
        return cee_uia.state == 1;
    }
    cee_uia.state = -1;
    HMODULE core = LoadLibraryW(L"uiautomationcore.dll");
    HMODULE automation = LoadLibraryW(L"oleaut32.dll");
    if (core == NULL || automation == NULL) {
        return 0;
    }
    FARPROC p[10] = {GetProcAddress(core, "UiaReturnRawElementProvider"), GetProcAddress(core, "UiaHostProviderFromHwnd"),
                     GetProcAddress(core, "UiaRaiseAutomationEvent"),
                     GetProcAddress(core, "UiaRaiseAutomationPropertyChangedEvent"),
                     GetProcAddress(core, "UiaRaiseStructureChangedEvent"), GetProcAddress(core, "UiaClientsAreListening"),
                     GetProcAddress(automation, "SysAllocStringLen"), GetProcAddress(automation, "SysFreeString"),
                     GetProcAddress(automation, "SafeArrayCreateVector"), GetProcAddress(automation, "SafeArrayPutElement")};
    for (int k = 0; k < 10; k++) {
        if (p[k] == NULL) {
            return 0;
        }
    }
    FARPROC notification = GetProcAddress(core, "UiaRaiseNotificationEvent");
    memcpy(&cee_uia.return_provider, &p[0], sizeof p[0]);
    memcpy(&cee_uia.host_provider, &p[1], sizeof p[0]);
    memcpy(&cee_uia.raise_event, &p[2], sizeof p[0]);
    memcpy(&cee_uia.raise_property, &p[3], sizeof p[0]);
    memcpy(&cee_uia.raise_structure, &p[4], sizeof p[0]);
    memcpy(&cee_uia.listening, &p[5], sizeof p[0]);
    memcpy(&cee_uia.string, &p[6], sizeof p[0]);
    memcpy(&cee_uia.string_free, &p[7], sizeof p[0]);
    memcpy(&cee_uia.array, &p[8], sizeof p[0]);
    memcpy(&cee_uia.array_put, &p[9], sizeof p[0]);
    if (notification != NULL) {
        memcpy(&cee_uia.raise_notification, &notification, sizeof notification);
    }
    InitializeCriticalSection(&cee_uia.lock);
    cee_uia.state = 1;
    return 1;
}

/* `length` bytes of UTF-8 as a BSTR (an empty one for none or for text that is not UTF-8), or NULL without memory. */
static BSTR cee_uia_text(const char *text, uint32_t length) {
    int units = length > 0 ? MultiByteToWideChar(CP_UTF8, 0, text, (int)length, NULL, 0) : 0;
    BSTR out = cee_uia.string(NULL, (UINT)(units > 0 ? units : 0));
    if (out != NULL && units > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text, (int)length, out, units);
    }
    return out;
}

#define CEE_UIA_OBJECT(type, field, self) ((struct type *)(void *)((char *)(self) - offsetof(struct type, field)))

/* The element's node in its window's list as it is now, or NULL. Under the lock. */
static const struct cee_access_node *cee_uia_node(struct cee_uia_element *e) {
    struct cee_window *w = e->w;
    if (w == NULL) {
        return NULL;
    }
    if (e->hint < w->access_count && w->access[e->hint].id == e->id) {
        return &w->access[e->hint];
    }
    for (uint32_t k = 0; k < w->access_count; k++) {
        if (w->access[k].id == e->id) {
            e->hint = k;
            return &w->access[k];
        }
    }
    return NULL;
}

/* The tree (ADR 0207), in the list: from node `at` (CEE_ACCESS_TOP for the window), the next sibling (1), the one
   before (2), the first child (3) or the last (4); -1 for none. A node's children come after it, in list order. */
static int64_t cee_uia_relative(struct cee_window *w, uint32_t count, uint32_t at, int direction) {
    if (count > w->access_count) {
        count = w->access_count;
    }
    const struct cee_access_node *nodes = w->access;
    if (direction == 1 || direction == 2) {
        uint32_t up = nodes[at].up;
        if (direction == 1) {
            for (uint32_t k = at + 1u; k < count; k++) {
                if (nodes[k].up == up) {
                    return (int64_t)k;
                }
            }
        } else {
            for (uint32_t k = at; k-- > 0;) {
                if (nodes[k].up == up) {
                    return (int64_t)k;
                }
            }
        }
        return -1;
    }
    uint32_t from = at == CEE_ACCESS_TOP ? 0u : at + 1u;
    int64_t found = -1;
    for (uint32_t k = from; k < count; k++) {
        if (nodes[k].up == at) {
            found = (int64_t)k;
            if (direction == 3) {
                break;
            }
        }
    }
    return found;
}

/* The table node `t` is in, for a cell or a header (its row's container), or -1. */
static int64_t cee_uia_table_of(const struct cee_access_node *nodes, uint32_t count, uint32_t k) {
    uint32_t row = nodes[k].up;
    if (row == CEE_ACCESS_TOP || row >= count || nodes[row].role != CEE_ACCESS_ROW) {
        return -1;
    }
    uint32_t table = nodes[row].up;
    return table != CEE_ACCESS_TOP && table < count && nodes[table].role == CEE_ACCESS_TABLE ? (int64_t)table : -1;
}

/* A client asks `action` of the element: kept for the window's pump, which is woken. */
static HRESULT cee_uia_ask(struct cee_uia_element *e, int32_t action) {
    HRESULT result = CEE_UIA_E_NOT_AVAILABLE;
    EnterCriticalSection(&cee_uia.lock);
    const struct cee_access_node *n = cee_uia_node(e);
    if (n != NULL && (n->flags & CEE_ACCESS_DISABLED) != 0) {
        result = CEE_UIA_E_NOT_ENABLED;
    } else if (n != NULL) {
        struct cee_win32 *m = (struct cee_win32 *)e->w->native;
        if (m != NULL && m->uia_asked_count < CEE_ACCESS_QUEUE) {
            m->uia_asked[m->uia_asked_count].id = e->id;
            m->uia_asked[m->uia_asked_count].action = action;
            m->uia_asked_count++;
        }
        if (m != NULL) {
            PostMessageW(m->hwnd, WM_NULL, 0, 0); /* a message ends the window's wait */
        }
        result = S_OK;
    }
    LeaveCriticalSection(&cee_uia.lock);
    return result;
}

/* ---- An element ---- */

static HRESULT cee_uia_element_query(struct cee_uia_element *e, REFIID iid, void **out) {
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL;
    if (IsEqualIID(iid, &cee_iid_unknown) || IsEqualIID(iid, &cee_iid_uia_simple)) {
        *out = &e->simple;
    } else if (IsEqualIID(iid, &cee_iid_uia_fragment)) {
        *out = &e->fragment;
    } else if (IsEqualIID(iid, &cee_iid_uia_invoke)) {
        *out = &e->invoke;
    } else if (IsEqualIID(iid, &cee_iid_uia_toggle)) {
        *out = &e->toggle;
    } else if (IsEqualIID(iid, &cee_iid_uia_range)) {
        *out = &e->range;
    } else if (IsEqualIID(iid, &cee_iid_uia_value)) {
        *out = &e->value;
    } else if (IsEqualIID(iid, &cee_iid_uia_selection)) {
        *out = &e->selection;
    } else if (IsEqualIID(iid, &cee_iid_uia_expand)) {
        *out = &e->expand;
    } else if (IsEqualIID(iid, &cee_iid_uia_grid)) {
        *out = &e->grid;
    } else if (IsEqualIID(iid, &cee_iid_uia_grid_item)) {
        *out = &e->grid_item;
    } else if (IsEqualIID(iid, &cee_iid_uia_table)) {
        *out = &e->table;
    } else if (IsEqualIID(iid, &cee_iid_uia_table_item)) {
        *out = &e->table_item;
    } else {
        return E_NOINTERFACE;
    }
    InterlockedIncrement(&e->refs);
    return S_OK;
}

static ULONG cee_uia_element_release(struct cee_uia_element *e) {
    LONG left = InterlockedDecrement(&e->refs);
    if (left == 0) {
        ceesafe_platform_free(e);
    }
    return (ULONG)left;
}

/* Each interface's IUnknown, which finds the object from the interface's place in it. */
#define CEE_UIA_ELEMENT_UNKNOWN(name, field)                                                                           \
    static HRESULT STDMETHODCALLTYPE cee_uia_##name##_query(void *self, REFIID iid, void **out) {                      \
        return cee_uia_element_query(CEE_UIA_OBJECT(cee_uia_element, field, self), iid, out);                          \
    }                                                                                                                  \
    static ULONG STDMETHODCALLTYPE cee_uia_##name##_add_ref(void *self) {                                              \
        return (ULONG)InterlockedIncrement(&CEE_UIA_OBJECT(cee_uia_element, field, self)->refs);                       \
    }                                                                                                                  \
    static ULONG STDMETHODCALLTYPE cee_uia_##name##_release(void *self) {                                              \
        return cee_uia_element_release(CEE_UIA_OBJECT(cee_uia_element, field, self));                                  \
    }
CEE_UIA_ELEMENT_UNKNOWN(esimple, simple)
CEE_UIA_ELEMENT_UNKNOWN(efragment, fragment)
CEE_UIA_ELEMENT_UNKNOWN(einvoke, invoke)
CEE_UIA_ELEMENT_UNKNOWN(etoggle, toggle)
CEE_UIA_ELEMENT_UNKNOWN(erange, range)
CEE_UIA_ELEMENT_UNKNOWN(evalue, value)
CEE_UIA_ELEMENT_UNKNOWN(eselection, selection)
CEE_UIA_ELEMENT_UNKNOWN(eexpand, expand)
CEE_UIA_ELEMENT_UNKNOWN(egrid, grid)
CEE_UIA_ELEMENT_UNKNOWN(egriditem, grid_item)
CEE_UIA_ELEMENT_UNKNOWN(etable, table)
CEE_UIA_ELEMENT_UNKNOWN(etableitem, table_item)

static HRESULT STDMETHODCALLTYPE cee_uia_options(void *self, int *out) {
    (void)self;
    if (out == NULL) {
        return E_POINTER;
    }
    *out = CEE_UIA_SERVER_SIDE; /* called on any thread: the lock is what keeps the list whole */
    return S_OK;
}

/* The pattern a role has: a button is invoked, a checkbox toggled, a radio button, a tab and a list's row selected,
   a slider has a range, a field and a dropdown a value, a dropdown and a menu's item are also expanded. ADR 0207: a
   table's row is invoked and selected as a list's row is, a column header invoked (it sorts); a table is a grid and
   a table, its cells and headers grid items and table items. */
static HRESULT STDMETHODCALLTYPE cee_uia_element_pattern(void *self, int pattern, void **out) {
    struct cee_uia_element *e = CEE_UIA_OBJECT(cee_uia_element, simple, self);
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL;
    EnterCriticalSection(&cee_uia.lock);
    const struct cee_access_node *n = cee_uia_node(e);
    uint32_t role = n != NULL ? n->role : 0;
    LeaveCriticalSection(&cee_uia.lock);
    if (n == NULL) {
        return CEE_UIA_E_NOT_AVAILABLE;
    }
    int cell = role == CEE_ACCESS_CELL || role == CEE_ACCESS_COLUMN_HEADER;
    if (pattern == CEE_UIA_PATTERN_INVOKE &&
        (role == 1 || role == 7 || role == 8 || role == CEE_ACCESS_ROW || role == CEE_ACCESS_COLUMN_HEADER)) {
        *out = &e->invoke;
    } else if (pattern == CEE_UIA_PATTERN_TOGGLE && role == 2) {
        *out = &e->toggle;
    } else if (pattern == CEE_UIA_PATTERN_SELECTION_ITEM &&
               (role == 3 || role == 6 || role == 7 || role == CEE_ACCESS_ROW)) {
        *out = &e->selection;
    } else if (pattern == CEE_UIA_PATTERN_GRID && role == CEE_ACCESS_TABLE) {
        *out = &e->grid;
    } else if (pattern == CEE_UIA_PATTERN_TABLE && role == CEE_ACCESS_TABLE) {
        *out = &e->table;
    } else if (pattern == CEE_UIA_PATTERN_GRID_ITEM && cell) {
        *out = &e->grid_item;
    } else if (pattern == CEE_UIA_PATTERN_TABLE_ITEM && cell) {
        *out = &e->table_item;
    } else if (pattern == CEE_UIA_PATTERN_RANGE && role == 5) {
        *out = &e->range;
    } else if (pattern == CEE_UIA_PATTERN_VALUE && (role == 4 || role == 9)) {
        *out = &e->value;
    } else if (pattern == CEE_UIA_PATTERN_EXPAND && role == 9) {
        *out = &e->expand;
    }
    if (*out != NULL) {
        InterlockedIncrement(&e->refs);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cee_uia_element_property(void *self, int property, VARIANT *out) {
    /* Text, Button, CheckBox, RadioButton, Edit, Slider, TabItem, ListItem, MenuItem, ComboBox, Image; ADR 0207's
       Group, List, Menu, MenuBar, Tab (a tab list), DataGrid, DataItem (a row), DataItem (a cell), HeaderItem. */
    static const LONG types[CEE_ACCESS_ROLES] = {50020, 50000, 50002, 50013, 50004, 50015, 50019, 50007, 50011, 50003,
                                                 50006, 50026, 50008, 50009, 50010, 50018, 50028, 50029, 50029, 50035};
    struct cee_uia_element *e = CEE_UIA_OBJECT(cee_uia_element, simple, self);
    if (out == NULL) {
        return E_POINTER;
    }
    V_VT(out) = VT_EMPTY;
    EnterCriticalSection(&cee_uia.lock);
    const struct cee_access_node *n = cee_uia_node(e);
    if (n == NULL) {
        LeaveCriticalSection(&cee_uia.lock);
        return CEE_UIA_E_NOT_AVAILABLE;
    }
    const char *text = e->w->access_text;
    switch (property) {
    case CEE_UIA_PROPERTY_CONTROL_TYPE:
        V_VT(out) = VT_I4;
        V_I4(out) = types[n->role];
        break;
    case CEE_UIA_PROPERTY_NAME:
        V_BSTR(out) = cee_uia_text(text + n->label_start, n->label_length);
        V_VT(out) = V_BSTR(out) != NULL ? VT_BSTR : VT_EMPTY;
        break;
    case CEE_UIA_PROPERTY_AUTOMATION_ID: {
        char number[24];
        int length = snprintf(number, sizeof number, "%llu", (unsigned long long)n->id);
        V_BSTR(out) = cee_uia_text(number, (uint32_t)length);
        V_VT(out) = V_BSTR(out) != NULL ? VT_BSTR : VT_EMPTY;
        break;
    }
    case CEE_UIA_PROPERTY_FRAMEWORK:
        V_BSTR(out) = cee_uia_text("CeeSafe", 7u);
        V_VT(out) = V_BSTR(out) != NULL ? VT_BSTR : VT_EMPTY;
        break;
    case CEE_UIA_PROPERTY_ENABLED:
        V_VT(out) = VT_BOOL;
        V_BOOL(out) = (n->flags & CEE_ACCESS_DISABLED) == 0 ? VARIANT_TRUE : VARIANT_FALSE;
        break;
    case CEE_UIA_PROPERTY_HAS_FOCUS:
        V_VT(out) = VT_BOOL;
        V_BOOL(out) = (n->flags & CEE_ACCESS_FOCUSED) != 0 ? VARIANT_TRUE : VARIANT_FALSE;
        break;
    case CEE_UIA_PROPERTY_FOCUSABLE:
        V_VT(out) = VT_BOOL;
        V_BOOL(out) = n->role != 0 && n->role != 10 && (n->flags & CEE_ACCESS_DISABLED) == 0 ? VARIANT_TRUE : VARIANT_FALSE;
        break;
    case CEE_UIA_PROPERTY_CONTROL_ELEMENT:
    case CEE_UIA_PROPERTY_CONTENT_ELEMENT:
        V_VT(out) = VT_BOOL;
        V_BOOL(out) = VARIANT_TRUE;
        break;
    case CEE_UIA_PROPERTY_PASSWORD:
        V_VT(out) = VT_BOOL;
        V_BOOL(out) = (n->flags & CEE_ACCESS_SECURE) != 0 ? VARIANT_TRUE : VARIANT_FALSE;
        break;
    case CEE_UIA_PROPERTY_POSITION_IN_SET: /* an item's place in its container, "3 of 40" (ADR 0207) */
    case CEE_UIA_PROPERTY_SIZE_OF_SET:
        if (n->up != CEE_ACCESS_TOP && n->row > 0 && n->column == 0) {
            V_VT(out) = VT_I4;
            V_I4(out) = property == CEE_UIA_PROPERTY_POSITION_IN_SET ? (LONG)n->row : (LONG)e->w->access[n->up].rows;
        }
        break;
    default:
        break;
    }
    LeaveCriticalSection(&cee_uia.lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cee_uia_element_host(void *self, void **out) {
    (void)self;
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL; /* only the root has a window of its own */
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cee_uia_element_navigate(void *self, int direction, void **out) {
    struct cee_uia_element *e = CEE_UIA_OBJECT(cee_uia_element, fragment, self);
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL;
    HRESULT result = S_OK;
    EnterCriticalSection(&cee_uia.lock);
    const struct cee_access_node *n = cee_uia_node(e);
    struct cee_win32 *m = n != NULL ? (struct cee_win32 *)e->w->native : NULL;
    if (m == NULL) {
        result = CEE_UIA_E_NOT_AVAILABLE;
    } else if (direction == 0 && (n->up == CEE_ACCESS_TOP || n->up >= m->uia_count)) { /* the parent: the window */
        if (m->uia_root != NULL) {
            InterlockedIncrement(&m->uia_root->refs);
            *out = &m->uia_root->fragment;
        }
    } else {
        /* The tree (ADR 0207): its container, the siblings in it, its own children. */
        uint32_t at = (uint32_t)(n - e->w->access);
        int64_t k = direction == 0 ? (int64_t)n->up : cee_uia_relative(e->w, m->uia_count, at, direction);
        if (k >= 0 && m->uia_elements[k] != NULL) {
            InterlockedIncrement(&m->uia_elements[k]->refs);
            *out = &m->uia_elements[k]->fragment;
        }
    }
    LeaveCriticalSection(&cee_uia.lock);
    return result;
}

/* An element's runtime id: UiaAppendRuntimeId, then the two halves of the program's id. */
static HRESULT STDMETHODCALLTYPE cee_uia_element_runtime_id(void *self, SAFEARRAY **out) {
    struct cee_uia_element *e = CEE_UIA_OBJECT(cee_uia_element, fragment, self);
    if (out == NULL) {
        return E_POINTER;
    }
    *out = cee_uia.array(VT_I4, 0, 3);
    if (*out == NULL) {
        return E_OUTOFMEMORY;
    }
    int parts[3] = {3, (int)(uint32_t)e->id, (int)(uint32_t)(e->id >> 32)};
    for (LONG k = 0; k < 3; k++) {
        cee_uia.array_put(*out, &k, &parts[k]);
    }
    return S_OK;
}

/* Where the element is, on the screen in pixels. */
static HRESULT STDMETHODCALLTYPE cee_uia_element_bounds(void *self, struct cee_uia_rect *out) {
    struct cee_uia_element *e = CEE_UIA_OBJECT(cee_uia_element, fragment, self);
    if (out == NULL) {
        return E_POINTER;
    }
    memset(out, 0, sizeof *out);
    HRESULT result = CEE_UIA_E_NOT_AVAILABLE;
    EnterCriticalSection(&cee_uia.lock);
    const struct cee_access_node *n = cee_uia_node(e);
    struct cee_win32 *m = n != NULL ? (struct cee_win32 *)e->w->native : NULL;
    if (m != NULL) {
        float scale = e->w->scale > 0.0f ? e->w->scale : 1.0f;
        POINT origin = {0, 0};
        ClientToScreen(m->hwnd, &origin);
        out->left = (double)origin.x + (double)(n->x * scale);
        out->top = (double)origin.y + (double)(n->y * scale);
        out->width = (double)(n->width * scale);
        out->height = (double)(n->height * scale);
        result = S_OK;
    }
    LeaveCriticalSection(&cee_uia.lock);
    return result;
}

static HRESULT STDMETHODCALLTYPE cee_uia_embedded(void *self, SAFEARRAY **out) {
    (void)self;
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cee_uia_element_set_focus(void *self) {
    return cee_uia_ask(CEE_UIA_OBJECT(cee_uia_element, fragment, self), CEE_ACCESS_FOCUS);
}

static HRESULT STDMETHODCALLTYPE cee_uia_element_fragment_root(void *self, void **out) {
    struct cee_uia_element *e = CEE_UIA_OBJECT(cee_uia_element, fragment, self);
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL;
    EnterCriticalSection(&cee_uia.lock);
    struct cee_win32 *m = e->w != NULL ? (struct cee_win32 *)e->w->native : NULL;
    if (m != NULL && m->uia_root != NULL) {
        InterlockedIncrement(&m->uia_root->refs);
        *out = &m->uia_root->root;
    }
    LeaveCriticalSection(&cee_uia.lock);
    return *out != NULL ? S_OK : CEE_UIA_E_NOT_AVAILABLE;
}

static HRESULT STDMETHODCALLTYPE cee_uia_invoke(void *self) {
    return cee_uia_ask(CEE_UIA_OBJECT(cee_uia_element, invoke, self), CEE_ACCESS_PRESS);
}

static HRESULT STDMETHODCALLTYPE cee_uia_toggle(void *self) {
    return cee_uia_ask(CEE_UIA_OBJECT(cee_uia_element, toggle, self), CEE_ACCESS_PRESS);
}

/* A flag of the element's node as 1 or 0, or -1 when the element is gone. */
static int cee_uia_flag(struct cee_uia_element *e, uint32_t flag) {
    EnterCriticalSection(&cee_uia.lock);
    const struct cee_access_node *n = cee_uia_node(e);
    int on = n != NULL ? (n->flags & flag) != 0 : -1;
    LeaveCriticalSection(&cee_uia.lock);
    return on;
}

static HRESULT STDMETHODCALLTYPE cee_uia_toggle_state(void *self, int *out) {
    int on = cee_uia_flag(CEE_UIA_OBJECT(cee_uia_element, toggle, self), CEE_ACCESS_CHECKED);
    if (out == NULL) {
        return E_POINTER;
    }
    *out = on > 0; /* ToggleState_On is 1, Off 0 */
    return on < 0 ? CEE_UIA_E_NOT_AVAILABLE : S_OK;
}

/* A slider's numbers as clients are given them. One whose program writes its value (a count: "3") has its own
   range. Any other is given as a percentage of its range, 0 to 100: readers say a slider's value as a whole
   number (NVDA says "0" for 0.5 of 0 to 1). Which: 0 its value, 1 its minimum, 2 its maximum, 3 a step. */
static double cee_uia_range_of(const struct cee_access_node *n, int which) {
    if (n->value_length > 0) {
        return which == 0 ? n->now : which == 1 ? n->min : which == 2 ? n->max : (n->max - n->min) / 10.0;
    }
    if (which == 0) {
        double share = n->max > n->min ? (n->now - n->min) / (n->max - n->min) : 0.0;
        share = share < 0.0 ? 0.0 : share > 1.0 ? 1.0 : share;
        return (double)(long)(share * 100.0 + 0.5);
    }
    return which == 1 ? 0.0 : which == 2 ? 100.0 : 10.0;
}

/* A slider is set by steps: a value above its own asks one step up, a value below one step down. */
static HRESULT STDMETHODCALLTYPE cee_uia_range_set(void *self, double value) {
    struct cee_uia_element *e = CEE_UIA_OBJECT(cee_uia_element, range, self);
    EnterCriticalSection(&cee_uia.lock);
    const struct cee_access_node *n = cee_uia_node(e);
    double now = n != NULL ? cee_uia_range_of(n, 0) : 0.0;
    LeaveCriticalSection(&cee_uia.lock);
    if (n == NULL) {
        return CEE_UIA_E_NOT_AVAILABLE;
    }
    if (value == now) {
        return S_OK;
    }
    return cee_uia_ask(e, value > now ? CEE_ACCESS_INCREMENT : CEE_ACCESS_DECREMENT);
}

/* One of a slider's numbers (cee_uia_range_of). */
static HRESULT cee_uia_range_number(struct cee_uia_element *e, int which, double *out) {
    if (out == NULL) {
        return E_POINTER;
    }
    *out = 0.0;
    EnterCriticalSection(&cee_uia.lock);
    const struct cee_access_node *n = cee_uia_node(e);
    if (n != NULL) {
        *out = cee_uia_range_of(n, which);
    }
    LeaveCriticalSection(&cee_uia.lock);
    return n != NULL ? S_OK : CEE_UIA_E_NOT_AVAILABLE;
}

static HRESULT STDMETHODCALLTYPE cee_uia_range_value(void *self, double *out) {
    return cee_uia_range_number(CEE_UIA_OBJECT(cee_uia_element, range, self), 0, out);
}
static HRESULT STDMETHODCALLTYPE cee_uia_range_minimum(void *self, double *out) {
    return cee_uia_range_number(CEE_UIA_OBJECT(cee_uia_element, range, self), 1, out);
}
static HRESULT STDMETHODCALLTYPE cee_uia_range_maximum(void *self, double *out) {
    return cee_uia_range_number(CEE_UIA_OBJECT(cee_uia_element, range, self), 2, out);
}
static HRESULT STDMETHODCALLTYPE cee_uia_range_change(void *self, double *out) {
    return cee_uia_range_number(CEE_UIA_OBJECT(cee_uia_element, range, self), 3, out);
}

static HRESULT STDMETHODCALLTYPE cee_uia_range_read_only(void *self, BOOL *out) {
    int off = cee_uia_flag(CEE_UIA_OBJECT(cee_uia_element, range, self), CEE_ACCESS_DISABLED);
    if (out == NULL) {
        return E_POINTER;
    }
    *out = off != 0;
    return off < 0 ? CEE_UIA_E_NOT_AVAILABLE : S_OK;
}

/* A field's text is the keyboard's to change, through the program: a client cannot set it. */
static HRESULT STDMETHODCALLTYPE cee_uia_value_set(void *self, LPCWSTR value) {
    (void)self;
    (void)value;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cee_uia_value_get(void *self, BSTR *out) {
    struct cee_uia_element *e = CEE_UIA_OBJECT(cee_uia_element, value, self);
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL;
    EnterCriticalSection(&cee_uia.lock);
    const struct cee_access_node *n = cee_uia_node(e);
    if (n != NULL) {
        *out = (n->flags & CEE_ACCESS_SECURE) != 0 ? cee_uia_text("", 0u)
                                                    : cee_uia_text(e->w->access_text + n->value_start, n->value_length);
    }
    LeaveCriticalSection(&cee_uia.lock);
    return n == NULL ? CEE_UIA_E_NOT_AVAILABLE : *out != NULL ? S_OK : E_OUTOFMEMORY;
}

/* A field is not read-only to a reader ("edit"), though a client cannot set it; a dropdown's value is. */
static HRESULT STDMETHODCALLTYPE cee_uia_value_read_only(void *self, BOOL *out) {
    struct cee_uia_element *e = CEE_UIA_OBJECT(cee_uia_element, value, self);
    if (out == NULL) {
        return E_POINTER;
    }
    EnterCriticalSection(&cee_uia.lock);
    const struct cee_access_node *n = cee_uia_node(e);
    *out = n == NULL || n->role != 4 || (n->flags & CEE_ACCESS_DISABLED) != 0;
    LeaveCriticalSection(&cee_uia.lock);
    return n != NULL ? S_OK : CEE_UIA_E_NOT_AVAILABLE;
}

static HRESULT STDMETHODCALLTYPE cee_uia_select(void *self) {
    return cee_uia_ask(CEE_UIA_OBJECT(cee_uia_element, selection, self), CEE_ACCESS_PRESS);
}

static HRESULT STDMETHODCALLTYPE cee_uia_selection_other(void *self) {
    (void)self;
    return E_NOTIMPL; /* one of a set is chosen: nothing is added to it or taken from it */
}

/* A radio button is selected when checked; a tab and a list's row when selected. */
static HRESULT STDMETHODCALLTYPE cee_uia_is_selected(void *self, BOOL *out) {
    int on = cee_uia_flag(CEE_UIA_OBJECT(cee_uia_element, selection, self), CEE_ACCESS_CHECKED | CEE_ACCESS_SELECTED);
    if (out == NULL) {
        return E_POINTER;
    }
    *out = on > 0;
    return on < 0 ? CEE_UIA_E_NOT_AVAILABLE : S_OK;
}

/* The container a selected item is chosen in: its list, tab list or table (ADR 0207); none at the top level. */
static HRESULT STDMETHODCALLTYPE cee_uia_selection_container(void *self, void **out) {
    struct cee_uia_element *e = CEE_UIA_OBJECT(cee_uia_element, selection, self);
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL;
    EnterCriticalSection(&cee_uia.lock);
    const struct cee_access_node *n = cee_uia_node(e);
    struct cee_win32 *m = n != NULL ? (struct cee_win32 *)e->w->native : NULL;
    if (m != NULL && n->up != CEE_ACCESS_TOP && n->up < m->uia_count && m->uia_elements[n->up] != NULL) {
        InterlockedIncrement(&m->uia_elements[n->up]->refs);
        *out = &m->uia_elements[n->up]->simple;
    }
    LeaveCriticalSection(&cee_uia.lock);
    return S_OK;
}

/* ---- Tables (ADR 0207): the Grid and Table patterns of a table, the GridItem and TableItem patterns of its cells
   and column headers. A cell's place is its node's row and column, from 1 there, from 0 here. */

/* The provider of node `k`, with a reference for the caller, or NULL. The lock is held. */
static void *cee_uia_provider_at(struct cee_win32 *m, int64_t k) {
    if (k < 0 || (uint32_t)k >= m->uia_count || m->uia_elements[k] == NULL) {
        return NULL;
    }
    InterlockedIncrement(&m->uia_elements[k]->refs);
    return &m->uia_elements[k]->simple;
}

static HRESULT STDMETHODCALLTYPE cee_uia_grid_item(void *self, int row, int column, void **out) {
    struct cee_uia_element *e = CEE_UIA_OBJECT(cee_uia_element, grid, self);
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL;
    HRESULT result = E_INVALIDARG;
    EnterCriticalSection(&cee_uia.lock);
    const struct cee_access_node *n = cee_uia_node(e);
    struct cee_win32 *m = n != NULL ? (struct cee_win32 *)e->w->native : NULL;
    if (m != NULL && row >= 0 && column >= 0) {
        uint32_t table = (uint32_t)(n - e->w->access);
        for (uint32_t k = table + 1u; k < m->uia_count && k < e->w->access_count; k++) {
            const struct cee_access_node *c = &e->w->access[k];
            if ((c->role == CEE_ACCESS_CELL || c->role == CEE_ACCESS_COLUMN_HEADER) && c->row == (uint32_t)row + 1u &&
                c->column == (uint32_t)column + 1u && cee_uia_table_of(e->w->access, m->uia_count, k) == table) {
                *out = cee_uia_provider_at(m, k);
                result = *out != NULL ? S_OK : E_INVALIDARG;
                break;
            }
        }
    }
    LeaveCriticalSection(&cee_uia.lock);
    return result;
}

static HRESULT cee_uia_number_of(struct cee_uia_element *e, int which, int *out) {
    if (out == NULL) {
        return E_POINTER;
    }
    *out = 0;
    EnterCriticalSection(&cee_uia.lock);
    const struct cee_access_node *n = cee_uia_node(e);
    if (n != NULL) {
        switch (which) {
        case 0: *out = (int)n->rows; break;
        case 1: *out = (int)n->columns; break;
        case 2: *out = n->row > 0 ? (int)n->row - 1 : 0; break;
        case 3: *out = n->column > 0 ? (int)n->column - 1 : 0; break;
        default: *out = 1; break; /* a span */
        }
    }
    LeaveCriticalSection(&cee_uia.lock);
    return n != NULL ? S_OK : CEE_UIA_E_NOT_AVAILABLE;
}
static HRESULT STDMETHODCALLTYPE cee_uia_grid_rows(void *self, int *out) {
    return cee_uia_number_of(CEE_UIA_OBJECT(cee_uia_element, grid, self), 0, out);
}
static HRESULT STDMETHODCALLTYPE cee_uia_grid_columns(void *self, int *out) {
    return cee_uia_number_of(CEE_UIA_OBJECT(cee_uia_element, grid, self), 1, out);
}
static HRESULT STDMETHODCALLTYPE cee_uia_cell_row(void *self, int *out) {
    return cee_uia_number_of(CEE_UIA_OBJECT(cee_uia_element, grid_item, self), 2, out);
}
static HRESULT STDMETHODCALLTYPE cee_uia_cell_column(void *self, int *out) {
    return cee_uia_number_of(CEE_UIA_OBJECT(cee_uia_element, grid_item, self), 3, out);
}
static HRESULT STDMETHODCALLTYPE cee_uia_cell_span(void *self, int *out) {
    return cee_uia_number_of(CEE_UIA_OBJECT(cee_uia_element, grid_item, self), 4, out);
}
static HRESULT STDMETHODCALLTYPE cee_uia_cell_grid(void *self, void **out) {
    struct cee_uia_element *e = CEE_UIA_OBJECT(cee_uia_element, grid_item, self);
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL;
    EnterCriticalSection(&cee_uia.lock);
    const struct cee_access_node *n = cee_uia_node(e);
    struct cee_win32 *m = n != NULL ? (struct cee_win32 *)e->w->native : NULL;
    if (m != NULL) {
        *out = cee_uia_provider_at(m, cee_uia_table_of(e->w->access, m->uia_count, (uint32_t)(n - e->w->access)));
    }
    LeaveCriticalSection(&cee_uia.lock);
    return S_OK;
}

/* The column headers of table `table` (all of them, or those of `column`, from 1, when it is not 0), as an array of
   providers. The lock is held. */
static SAFEARRAY *cee_uia_headers(struct cee_window *w, struct cee_win32 *m, uint32_t table, uint32_t column) {
    LONG found = 0;
    for (uint32_t k = table + 1u; k < m->uia_count && k < w->access_count; k++) {
        found += w->access[k].role == CEE_ACCESS_COLUMN_HEADER && (column == 0 || w->access[k].column == column) &&
                 cee_uia_table_of(w->access, m->uia_count, k) == (int64_t)table && m->uia_elements[k] != NULL;
    }
    SAFEARRAY *array = cee_uia.array(VT_UNKNOWN, 0, (ULONG)found);
    LONG at = 0;
    for (uint32_t k = table + 1u; array != NULL && k < m->uia_count && k < w->access_count && at < found; k++) {
        if (w->access[k].role == CEE_ACCESS_COLUMN_HEADER && (column == 0 || w->access[k].column == column) &&
            cee_uia_table_of(w->access, m->uia_count, k) == (int64_t)table && m->uia_elements[k] != NULL) {
            cee_uia.array_put(array, &at, &m->uia_elements[k]->simple); /* it takes its own reference */
            at++;
        }
    }
    return array;
}

static HRESULT STDMETHODCALLTYPE cee_uia_no_headers(void *self, SAFEARRAY **out) {
    (void)self;
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL; /* no row headers */
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE cee_uia_table_columns(void *self, SAFEARRAY **out) {
    struct cee_uia_element *e = CEE_UIA_OBJECT(cee_uia_element, table, self);
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL;
    EnterCriticalSection(&cee_uia.lock);
    const struct cee_access_node *n = cee_uia_node(e);
    struct cee_win32 *m = n != NULL ? (struct cee_win32 *)e->w->native : NULL;
    if (m != NULL) {
        *out = cee_uia_headers(e->w, m, (uint32_t)(n - e->w->access), 0);
    }
    LeaveCriticalSection(&cee_uia.lock);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE cee_uia_table_major(void *self, int *out) {
    (void)self;
    if (out == NULL) {
        return E_POINTER;
    }
    *out = 0; /* RowOrColumnMajor_RowMajor */
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE cee_uia_cell_columns(void *self, SAFEARRAY **out) {
    struct cee_uia_element *e = CEE_UIA_OBJECT(cee_uia_element, table_item, self);
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL;
    EnterCriticalSection(&cee_uia.lock);
    const struct cee_access_node *n = cee_uia_node(e);
    struct cee_win32 *m = n != NULL ? (struct cee_win32 *)e->w->native : NULL;
    if (m != NULL) {
        int64_t table = cee_uia_table_of(e->w->access, m->uia_count, (uint32_t)(n - e->w->access));
        if (table >= 0 && n->role != CEE_ACCESS_COLUMN_HEADER) {
            *out = cee_uia_headers(e->w, m, (uint32_t)table, n->column);
        }
    }
    LeaveCriticalSection(&cee_uia.lock);
    return S_OK;
}

/* A dropdown is opened and closed by a press: only when it is not already as asked. */
static HRESULT cee_uia_expand_to(struct cee_uia_element *e, int open) {
    int is = cee_uia_flag(e, CEE_ACCESS_EXPANDED);
    if (is < 0) {
        return CEE_UIA_E_NOT_AVAILABLE;
    }
    return is == open ? S_OK : cee_uia_ask(e, CEE_ACCESS_PRESS);
}

static HRESULT STDMETHODCALLTYPE cee_uia_expand(void *self) {
    return cee_uia_expand_to(CEE_UIA_OBJECT(cee_uia_element, expand, self), 1);
}

static HRESULT STDMETHODCALLTYPE cee_uia_collapse(void *self) {
    return cee_uia_expand_to(CEE_UIA_OBJECT(cee_uia_element, expand, self), 0);
}

static HRESULT STDMETHODCALLTYPE cee_uia_expand_state(void *self, int *out) {
    int is = cee_uia_flag(CEE_UIA_OBJECT(cee_uia_element, expand, self), CEE_ACCESS_EXPANDED);
    if (out == NULL) {
        return E_POINTER;
    }
    *out = is > 0; /* ExpandCollapseState_Expanded is 1, Collapsed 0 */
    return is < 0 ? CEE_UIA_E_NOT_AVAILABLE : S_OK;
}

static const struct cee_uia_simple_vtbl cee_uia_element_simple = {
    cee_uia_esimple_query, cee_uia_esimple_add_ref, cee_uia_esimple_release, cee_uia_options,
    cee_uia_element_pattern, cee_uia_element_property, cee_uia_element_host};
static const struct cee_uia_fragment_vtbl cee_uia_element_fragment = {
    cee_uia_efragment_query, cee_uia_efragment_add_ref, cee_uia_efragment_release, cee_uia_element_navigate,
    cee_uia_element_runtime_id, cee_uia_element_bounds, cee_uia_embedded, cee_uia_element_set_focus,
    cee_uia_element_fragment_root};
static const struct cee_uia_invoke_vtbl cee_uia_element_invoke = {cee_uia_einvoke_query, cee_uia_einvoke_add_ref,
                                                                 cee_uia_einvoke_release, cee_uia_invoke};
static const struct cee_uia_toggle_vtbl cee_uia_element_toggle = {
    cee_uia_etoggle_query, cee_uia_etoggle_add_ref, cee_uia_etoggle_release, cee_uia_toggle, cee_uia_toggle_state};
static const struct cee_uia_range_vtbl cee_uia_element_range = {
    cee_uia_erange_query, cee_uia_erange_add_ref, cee_uia_erange_release, cee_uia_range_set, cee_uia_range_value,
    cee_uia_range_read_only, cee_uia_range_maximum, cee_uia_range_minimum, cee_uia_range_change, cee_uia_range_change};
static const struct cee_uia_value_vtbl cee_uia_element_value = {
    cee_uia_evalue_query, cee_uia_evalue_add_ref, cee_uia_evalue_release, cee_uia_value_set, cee_uia_value_get,
    cee_uia_value_read_only};
static const struct cee_uia_selection_vtbl cee_uia_element_selection = {
    cee_uia_eselection_query, cee_uia_eselection_add_ref, cee_uia_eselection_release, cee_uia_select,
    cee_uia_selection_other, cee_uia_selection_other, cee_uia_is_selected, cee_uia_selection_container};
static const struct cee_uia_expand_vtbl cee_uia_element_expand = {
    cee_uia_eexpand_query, cee_uia_eexpand_add_ref, cee_uia_eexpand_release, cee_uia_expand, cee_uia_collapse,
    cee_uia_expand_state};
static const struct cee_uia_grid_vtbl cee_uia_element_grid = {cee_uia_egrid_query, cee_uia_egrid_add_ref,
                                                             cee_uia_egrid_release, cee_uia_grid_item,
                                                             cee_uia_grid_rows, cee_uia_grid_columns};
static const struct cee_uia_grid_item_vtbl cee_uia_element_grid_item = {
    cee_uia_egriditem_query, cee_uia_egriditem_add_ref, cee_uia_egriditem_release, cee_uia_cell_row,
    cee_uia_cell_column, cee_uia_cell_span, cee_uia_cell_span, cee_uia_cell_grid};
static const struct cee_uia_table_vtbl cee_uia_element_table = {cee_uia_etable_query, cee_uia_etable_add_ref,
                                                               cee_uia_etable_release, cee_uia_no_headers,
                                                               cee_uia_table_columns, cee_uia_table_major};
static const struct cee_uia_table_item_vtbl cee_uia_element_table_item = {
    cee_uia_etableitem_query, cee_uia_etableitem_add_ref, cee_uia_etableitem_release, cee_uia_no_headers,
    cee_uia_cell_columns};

static struct cee_uia_element *cee_uia_element_new(struct cee_window *w, uint64_t id, uint32_t at) {
    struct cee_uia_element *e = (struct cee_uia_element *)ceesafe_platform_alloc_zeroed(sizeof *e);
    if (e != NULL) {
        e->simple = &cee_uia_element_simple;
        e->fragment = &cee_uia_element_fragment;
        e->invoke = &cee_uia_element_invoke;
        e->toggle = &cee_uia_element_toggle;
        e->range = &cee_uia_element_range;
        e->value = &cee_uia_element_value;
        e->selection = &cee_uia_element_selection;
        e->expand = &cee_uia_element_expand;
        e->grid = &cee_uia_element_grid;
        e->grid_item = &cee_uia_element_grid_item;
        e->table = &cee_uia_element_table;
        e->table_item = &cee_uia_element_table_item;
        e->refs = 1;
        e->w = w;
        e->id = id;
        e->hint = at;
    }
    return e;
}

/* ---- The window's root ---- */

static HRESULT cee_uia_root_query(struct cee_uia_root *r, REFIID iid, void **out) {
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL;
    if (IsEqualIID(iid, &cee_iid_unknown) || IsEqualIID(iid, &cee_iid_uia_simple)) {
        *out = &r->simple;
    } else if (IsEqualIID(iid, &cee_iid_uia_fragment)) {
        *out = &r->fragment;
    } else if (IsEqualIID(iid, &cee_iid_uia_root)) {
        *out = &r->root;
    } else {
        return E_NOINTERFACE;
    }
    InterlockedIncrement(&r->refs);
    return S_OK;
}

static ULONG cee_uia_root_release(struct cee_uia_root *r) {
    LONG left = InterlockedDecrement(&r->refs);
    if (left == 0) {
        ceesafe_platform_free(r);
    }
    return (ULONG)left;
}

#define CEE_UIA_ROOT_UNKNOWN(name, field)                                                                              \
    static HRESULT STDMETHODCALLTYPE cee_uia_##name##_query(void *self, REFIID iid, void **out) {                      \
        return cee_uia_root_query(CEE_UIA_OBJECT(cee_uia_root, field, self), iid, out);                                \
    }                                                                                                                  \
    static ULONG STDMETHODCALLTYPE cee_uia_##name##_add_ref(void *self) {                                              \
        return (ULONG)InterlockedIncrement(&CEE_UIA_OBJECT(cee_uia_root, field, self)->refs);                          \
    }                                                                                                                  \
    static ULONG STDMETHODCALLTYPE cee_uia_##name##_release(void *self) {                                              \
        return cee_uia_root_release(CEE_UIA_OBJECT(cee_uia_root, field, self));                                        \
    }
CEE_UIA_ROOT_UNKNOWN(rsimple, simple)
CEE_UIA_ROOT_UNKNOWN(rfragment, fragment)
CEE_UIA_ROOT_UNKNOWN(rroot, root)

static HRESULT STDMETHODCALLTYPE cee_uia_root_pattern(void *self, int pattern, void **out) {
    (void)self;
    (void)pattern;
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL;
    return S_OK;
}

/* The window's own properties are its host provider's (the title, the frame): this adds nothing to them. */
static HRESULT STDMETHODCALLTYPE cee_uia_root_property(void *self, int property, VARIANT *out) {
    (void)self;
    (void)property;
    if (out == NULL) {
        return E_POINTER;
    }
    V_VT(out) = VT_EMPTY;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cee_uia_root_host(void *self, void **out) {
    struct cee_uia_root *r = CEE_UIA_OBJECT(cee_uia_root, simple, self);
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL;
    EnterCriticalSection(&cee_uia.lock);
    struct cee_win32 *m = r->w != NULL ? (struct cee_win32 *)r->w->native : NULL;
    HWND hwnd = m != NULL ? m->hwnd : NULL;
    LeaveCriticalSection(&cee_uia.lock);
    return hwnd != NULL ? cee_uia.host_provider(hwnd, out) : CEE_UIA_E_NOT_AVAILABLE;
}

static HRESULT STDMETHODCALLTYPE cee_uia_root_navigate(void *self, int direction, void **out) {
    struct cee_uia_root *r = CEE_UIA_OBJECT(cee_uia_root, fragment, self);
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL;
    EnterCriticalSection(&cee_uia.lock);
    struct cee_win32 *m = r->w != NULL ? (struct cee_win32 *)r->w->native : NULL;
    if (m != NULL && (direction == 3 || direction == 4)) { /* the first child, the last: of the top level */
        int64_t k = cee_uia_relative(r->w, m->uia_count, CEE_ACCESS_TOP, direction);
        if (k >= 0 && m->uia_elements[k] != NULL) {
            InterlockedIncrement(&m->uia_elements[k]->refs);
            *out = &m->uia_elements[k]->fragment;
        }
    }
    LeaveCriticalSection(&cee_uia.lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cee_uia_root_runtime_id(void *self, SAFEARRAY **out) {
    (void)self;
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL; /* the window's own, from its host provider */
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cee_uia_root_bounds(void *self, struct cee_uia_rect *out) {
    (void)self;
    if (out == NULL) {
        return E_POINTER;
    }
    memset(out, 0, sizeof *out); /* the window's own, from its host provider */
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cee_uia_root_set_focus(void *self) {
    (void)self;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cee_uia_root_fragment_root(void *self, void **out) {
    struct cee_uia_root *r = CEE_UIA_OBJECT(cee_uia_root, fragment, self);
    if (out == NULL) {
        return E_POINTER;
    }
    InterlockedIncrement(&r->refs);
    *out = &r->root;
    return S_OK;
}

/* The element at a point of the screen: cee_access_at's (the highest layer's, then the latest), or the root. */
static HRESULT STDMETHODCALLTYPE cee_uia_root_from_point(void *self, double x, double y, void **out) {
    struct cee_uia_root *r = CEE_UIA_OBJECT(cee_uia_root, root, self);
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL;
    EnterCriticalSection(&cee_uia.lock);
    struct cee_win32 *m = r->w != NULL ? (struct cee_win32 *)r->w->native : NULL;
    if (m != NULL) {
        float scale = r->w->scale > 0.0f ? r->w->scale : 1.0f;
        POINT origin = {0, 0};
        ClientToScreen(m->hwnd, &origin);
        double px = (x - (double)origin.x) / (double)scale, py = (y - (double)origin.y) / (double)scale;
        int64_t k = cee_access_at(r->w->access, m->uia_count < r->w->access_count ? m->uia_count : r->w->access_count,
                                  px, py);
        if (k >= 0 && m->uia_elements[k] != NULL) {
            InterlockedIncrement(&m->uia_elements[k]->refs);
            *out = &m->uia_elements[k]->fragment;
        }
    }
    if (*out == NULL) {
        InterlockedIncrement(&r->refs);
        *out = &r->fragment;
    }
    LeaveCriticalSection(&cee_uia.lock);
    return S_OK;
}

/* The element that has the keyboard focus, if one says so. */
static HRESULT STDMETHODCALLTYPE cee_uia_root_focus(void *self, void **out) {
    struct cee_uia_root *r = CEE_UIA_OBJECT(cee_uia_root, root, self);
    if (out == NULL) {
        return E_POINTER;
    }
    *out = NULL;
    EnterCriticalSection(&cee_uia.lock);
    struct cee_win32 *m = r->w != NULL ? (struct cee_win32 *)r->w->native : NULL;
    for (uint32_t k = 0; m != NULL && k < m->uia_count && k < r->w->access_count; k++) {
        if ((r->w->access[k].flags & CEE_ACCESS_FOCUSED) != 0 && m->uia_elements[k] != NULL) {
            InterlockedIncrement(&m->uia_elements[k]->refs);
            *out = &m->uia_elements[k]->fragment;
            break;
        }
    }
    LeaveCriticalSection(&cee_uia.lock);
    return S_OK;
}

static const struct cee_uia_simple_vtbl cee_uia_root_simple = {
    cee_uia_rsimple_query, cee_uia_rsimple_add_ref, cee_uia_rsimple_release, cee_uia_options,
    cee_uia_root_pattern, cee_uia_root_property, cee_uia_root_host};
static const struct cee_uia_fragment_vtbl cee_uia_root_fragment = {
    cee_uia_rfragment_query, cee_uia_rfragment_add_ref, cee_uia_rfragment_release, cee_uia_root_navigate,
    cee_uia_root_runtime_id, cee_uia_root_bounds, cee_uia_embedded, cee_uia_root_set_focus,
    cee_uia_root_fragment_root};
static const struct cee_uia_root_vtbl cee_uia_root_root = {cee_uia_rroot_query, cee_uia_rroot_add_ref,
                                                          cee_uia_rroot_release, cee_uia_root_from_point,
                                                          cee_uia_root_focus};

/* WM_GETOBJECT for UI Automation's root: the window's provider, made at the first question. */
static int cee_uia_get_object(struct cee_window *w, HWND hwnd, WPARAM wparam, LPARAM lparam, LRESULT *result) {
    struct cee_win32 *m = (struct cee_win32 *)w->native;
    if ((LONG)lparam != CEE_UIA_ROOT_OBJECT || m == NULL || !cee_uia_load()) {
        return 0;
    }
    EnterCriticalSection(&cee_uia.lock);
    if (m->uia_root == NULL) {
        struct cee_uia_root *r = (struct cee_uia_root *)ceesafe_platform_alloc_zeroed(sizeof *r);
        if (r != NULL) {
            r->simple = &cee_uia_root_simple;
            r->fragment = &cee_uia_root_fragment;
            r->root = &cee_uia_root_root;
            r->refs = 1;
            r->w = w;
            m->uia_root = r;
        }
    }
    struct cee_uia_root *root = m->uia_root;
    m->uia_active = 1;
    LeaveCriticalSection(&cee_uia.lock);
    if (root == NULL) {
        return 0;
    }
    *result = cee_uia.return_provider(hwnd, wparam, lparam, &root->simple);
    return 1;
}

static int cee_uia_same_text(const char *a, uint32_t a_start, uint32_t a_length, const char *b, uint32_t b_start,
                             uint32_t b_length) {
    return a_length == b_length && (a_length == 0 || memcmp(a + a_start, b + b_start, a_length) == 0);
}

static void cee_uia_focused(struct cee_window *w);

/* A property of an element that changed, to tell listening clients once the lock is let go: an event raised
   under it would wait for a client's question that waits for the lock. The element is held meanwhile. */
struct cee_uia_change {
    struct cee_uia_element *element;
    int property;
    VARIANT value;
};
#define CEE_UIA_CHANGES 64 /* a list's changes told at once; more are not told, and a reader asks when it comes by */

static void cee_uia_changed(struct cee_uia_change *changes, unsigned *count, struct cee_uia_element *e, int property,
                            VARIANT value) {
    if (*count == CEE_UIA_CHANGES) {
        if (V_VT(&value) == VT_BSTR) {
            cee_uia.string_free(V_BSTR(&value));
        }
        return;
    }
    InterlockedIncrement(&e->refs);
    changes[*count].element = e;
    changes[*count].property = property;
    changes[*count].value = value;
    (*count)++;
}

/* The window's list changed (w->access is the new one; `old` the last, with its text): the providers follow it,
   and listening clients are told what changed. */
static void cee_native_access(struct cee_window *w, const struct cee_access_node *old, uint32_t old_count,
                              const char *old_text) {
    struct cee_win32 *m = (struct cee_win32 *)w->native;
    if (m == NULL || !cee_uia_load()) {
        return;
    }
    struct cee_uia_change changes[CEE_UIA_CHANGES];
    unsigned change_count = 0;
    EnterCriticalSection(&cee_uia.lock);
    uint32_t count = w->access_count;
    if (old_count > m->uia_count) {
        old_count = m->uia_count;
    }
    struct cee_uia_element **elements =
        count > 0 ? (struct cee_uia_element **)ceesafe_platform_alloc_zeroed((size_t)count * sizeof *elements) : NULL;
    if (count > 0 && elements == NULL) {
        count = 0; /* out of memory: the window has no elements until the next list */
    }
    int listening = m->uia_root != NULL && cee_uia.listening();
    int layout = count != old_count;
    int refocus = 0; /* an element took the focus or gave it up */
    for (uint32_t k = 0; k < count; k++) {
        const struct cee_access_node *n = &w->access[k];
        uint32_t j = old_count;
        if (k < old_count && old[k].id == n->id) {
            j = k;
        } else {
            for (uint32_t i = 0; i < old_count; i++) {
                if (old[i].id == n->id) {
                    j = i;
                    break;
                }
            }
            layout = 1;
        }
        if (j < old_count && (old[j].parent != n->parent || old[j].row != n->row || old[j].column != n->column)) {
            layout = 1; /* the tree changed: its children are asked again (ADR 0207) */
        }
        if (j < old_count && m->uia_elements[j] != NULL) {
            const struct cee_access_node *o = &old[j];
            struct cee_uia_element *e = m->uia_elements[j];
            m->uia_elements[j] = NULL;
            elements[k] = e;
            e->hint = k;
            if (!listening) {
                continue;
            }
            VARIANT value;
            /* A control's new name is told; text that changes (a clock, a counter) is read when a reader comes
               by, and not told at each change. */
            if (n->role != 0 && !cee_uia_same_text(w->access_text, n->label_start, n->label_length, old_text,
                                                   o->label_start, o->label_length)) {
                V_BSTR(&value) = cee_uia_text(w->access_text + n->label_start, n->label_length);
                if (V_BSTR(&value) != NULL) {
                    V_VT(&value) = VT_BSTR;
                    cee_uia_changed(changes, &change_count, e, CEE_UIA_PROPERTY_NAME, value);
                }
            }
            if ((n->role == 4 || n->role == 9) &&
                !cee_uia_same_text(w->access_text, n->value_start, n->value_length, old_text, o->value_start,
                                   o->value_length)) {
                V_BSTR(&value) = cee_uia_text(w->access_text + n->value_start, n->value_length);
                if (V_BSTR(&value) != NULL) {
                    V_VT(&value) = VT_BSTR;
                    cee_uia_changed(changes, &change_count, e, CEE_UIA_PROPERTY_VALUE, value);
                }
            }
            if (n->role == 5 && n->now != o->now) {
                V_VT(&value) = VT_R8;
                V_R8(&value) = cee_uia_range_of(n, 0);
                cee_uia_changed(changes, &change_count, e, CEE_UIA_PROPERTY_RANGE_VALUE, value);
            }
            uint32_t changed = n->flags ^ o->flags;
            if (n->role == 2 && (changed & CEE_ACCESS_CHECKED) != 0) {
                V_VT(&value) = VT_I4;
                V_I4(&value) = (n->flags & CEE_ACCESS_CHECKED) != 0;
                cee_uia_changed(changes, &change_count, e, CEE_UIA_PROPERTY_TOGGLE_STATE, value);
            }
            if ((n->role == 3 || n->role == 6 || n->role == 7) &&
                (changed & (CEE_ACCESS_CHECKED | CEE_ACCESS_SELECTED)) != 0) {
                V_VT(&value) = VT_BOOL;
                V_BOOL(&value) = (n->flags & (CEE_ACCESS_CHECKED | CEE_ACCESS_SELECTED)) != 0 ? VARIANT_TRUE : VARIANT_FALSE;
                cee_uia_changed(changes, &change_count, e, CEE_UIA_PROPERTY_IS_SELECTED, value);
            }
            if (n->role == 9 && (changed & CEE_ACCESS_EXPANDED) != 0) {
                V_VT(&value) = VT_I4;
                V_I4(&value) = (n->flags & CEE_ACCESS_EXPANDED) != 0;
                cee_uia_changed(changes, &change_count, e, CEE_UIA_PROPERTY_EXPAND_STATE, value);
            }
            if ((changed & CEE_ACCESS_FOCUSED) != 0) {
                refocus = 1;
            }
        } else {
            elements[k] = cee_uia_element_new(w, n->id, k);
            layout = 1;
            if ((n->flags & CEE_ACCESS_FOCUSED) != 0) {
                refocus = 1;
            }
        }
    }
    for (uint32_t j = 0; j < m->uia_count; j++) { /* the elements the new list does not have */
        if (m->uia_elements[j] != NULL) {
            m->uia_elements[j]->w = NULL;
            cee_uia_element_release(m->uia_elements[j]);
            layout = 1;
        }
    }
    ceesafe_platform_free(m->uia_elements);
    m->uia_elements = elements;
    m->uia_count = count;
    struct cee_uia_root *root = m->uia_root;
    if (root != NULL) {
        InterlockedIncrement(&root->refs);
    }
    LeaveCriticalSection(&cee_uia.lock);
    for (unsigned k = 0; k < change_count; k++) { /* outside the lock */
        VARIANT none;
        V_VT(&none) = VT_EMPTY;
        cee_uia.raise_property(&changes[k].element->simple, changes[k].property, none, changes[k].value);
        if (V_VT(&changes[k].value) == VT_BSTR) {
            cee_uia.string_free(V_BSTR(&changes[k].value));
        }
        cee_uia_element_release(changes[k].element);
    }
    if (layout && listening && root != NULL) {
        cee_uia.raise_structure(&root->simple, CEE_UIA_CHILDREN_INVALIDATED, NULL, 0);
    }
    if (root != NULL) {
        cee_uia_root_release(root);
    }
    if (refocus && listening) {
        cee_uia_focused(w);
    }
}

/* Says `text` through a screen reader: a notification of the window's (Windows 10 1709 on; nothing before). */
static void cee_native_announce(struct cee_window *w, const char *text, size_t length) {
    struct cee_win32 *m = (struct cee_win32 *)w->native;
    if (m == NULL || m->uia_root == NULL || !cee_uia_load() || cee_uia.raise_notification == NULL) {
        return;
    }
    BSTR said = cee_uia_text(text, (uint32_t)length);
    BSTR activity = cee_uia_text("ceesafe", 7u);
    if (said != NULL && activity != NULL) {
        /* NotificationKind_Other, NotificationProcessing_All */
        cee_uia.raise_notification(&m->uia_root->simple, 4, 2, said, activity);
    }
    if (said != NULL) {
        cee_uia.string_free(said);
    }
    if (activity != NULL) {
        cee_uia.string_free(activity);
    }
}

/* The window closes: its providers stay with whoever holds them, answering that their element is gone. */
static void cee_uia_close(struct cee_window *w) {
    struct cee_win32 *m = (struct cee_win32 *)w->native;
    if (m == NULL || cee_uia.state != 1) {
        return;
    }
    EnterCriticalSection(&cee_uia.lock);
    for (uint32_t k = 0; k < m->uia_count; k++) {
        if (m->uia_elements[k] != NULL) {
            m->uia_elements[k]->w = NULL;
            cee_uia_element_release(m->uia_elements[k]);
        }
    }
    ceesafe_platform_free(m->uia_elements);
    m->uia_elements = NULL;
    m->uia_count = 0;
    struct cee_uia_root *root = m->uia_root;
    m->uia_root = NULL;
    if (root != NULL) {
        root->w = NULL;
    }
    LeaveCriticalSection(&cee_uia.lock);
    if (root != NULL) {
        cee_uia.return_provider(m->hwnd, 0, 0, NULL); /* the window's providers are no longer given out */
        cee_uia_root_release(root);
    }
}

/* Tells listening clients where the keyboard focus is: the element that says it has it, or else the window. A
   window with a provider of its own gets no focus event from UI Automation's own watching of windows, and a
   reader that hears none goes on believing the last window is in front (NVDA: "Foreground took too long to
   change"). Called when the window gets the focus, when an element takes or gives up the focus, and once after
   the provider is first given out to a window that has the focus already. */
static void cee_uia_focused(struct cee_window *w) {
    struct cee_win32 *m = (struct cee_win32 *)w->native;
    if (m == NULL || cee_uia.state != 1) {
        return;
    }
    void *target = NULL;
    struct cee_uia_element *element = NULL;
    struct cee_uia_root *root = NULL;
    EnterCriticalSection(&cee_uia.lock);
    if (m->uia_root != NULL && GetFocus() == m->hwnd) {
        for (uint32_t k = 0; k < m->uia_count && k < w->access_count; k++) {
            if ((w->access[k].flags & CEE_ACCESS_FOCUSED) != 0 && m->uia_elements[k] != NULL) {
                element = m->uia_elements[k];
                InterlockedIncrement(&element->refs);
                target = &element->simple;
                break;
            }
        }
        if (target == NULL) {
            root = m->uia_root;
            InterlockedIncrement(&root->refs);
            target = &root->simple;
        }
        m->uia_focus_told = 1;
    }
    LeaveCriticalSection(&cee_uia.lock);
    if (target != NULL) {
        cee_uia.raise_event(target, CEE_UIA_EVENT_FOCUS); /* outside the lock: a client may call back at once */
    }
    if (element != NULL) {
        cee_uia_element_release(element);
    }
    if (root != NULL) {
        cee_uia_root_release(root);
    }
}

/* What clients asked since the last pump, into the window's requests; and whether anything asked at all. */
static void cee_uia_pump(struct cee_window *w) {
    struct cee_win32 *m = (struct cee_win32 *)w->native;
    if (m == NULL || cee_uia.state != 1) {
        return;
    }
    EnterCriticalSection(&cee_uia.lock);
    for (unsigned k = 0; k < m->uia_asked_count; k++) {
        cee_access_ask(w, m->uia_asked[k].id, m->uia_asked[k].action);
    }
    m->uia_asked_count = 0;
    if (m->uia_active) {
        w->access_active = 1;
    }
    int tell = m->uia_active && !m->uia_focus_told;
    LeaveCriticalSection(&cee_uia.lock);
    if (tell) {
        cee_uia_focused(w); /* a reader that came after the window had the focus */
    }
}

/* ---- A title bar the program draws (ADR 0206 step 2) -----------------------------------------------------------
   The window keeps WS_OVERLAPPEDWINDOW, so Windows still frames it (its borders, its shadow, Windows 11's corners)
   and moves, snaps and resizes it; WM_NCCALCSIZE takes the caption away, the client area reaching the window's top,
   and WM_NCHITTEST answers from the program's layout (Window.setTitleBar): the bar outside the buttons is the
   caption (a drag moves the window, a double click maximizes it, a secondary press shows the window menu, Aero Snap),
   the maximize box Windows' maximize button (which opens Windows 11's snap layouts), the other boxes the client's.
   The pointer over the maximize box is then the frame's: its moves and presses are given to the program as the
   client's are, so the program's button lights and works as the others do. */

/* The frame's thickness across and down, in pixels at the window's DPI (the resize border and its padding). */
static int cee_win32_frame(HWND hwnd, int across) {
    typedef int(WINAPI * metrics_for_dpi)(int, UINT);
    static metrics_for_dpi f;
    static int tried;
    if (!tried) {
        tried = 1;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        FARPROC p = user32 != NULL ? GetProcAddress(user32, "GetSystemMetricsForDpi") : NULL;
        if (p != NULL) {
            memcpy(&f, &p, sizeof f);
        }
    }
    UINT dpi = (UINT)(cee_win32_scale(hwnd) * 96.0f + 0.5f);
    int frame = across ? 32 /* SM_CXFRAME */ : 33 /* SM_CYFRAME */;
    if (f != NULL) {
        return f(frame, dpi) + f(92 /* SM_CXPADDEDBORDER */, dpi);
    }
    return GetSystemMetrics(frame) + GetSystemMetrics(92);
}

/* Whether (x, y), in points from the content's top left, is in box `k` of the program's layout (0 minimize, 1
   maximize, 2 close, 3 menu). */
static int cee_win32_in_box(const struct cee_window *w, int k, float x, float y) {
    const float *b = &w->bar[1 + 4 * k];
    return b[2] > 0.0f && b[3] > 0.0f && x >= b[0] && y >= b[1] && x < b[0] + b[2] && y < b[1] + b[3];
}

/* A frame message's screen point, in points from the content's top left. */
static void cee_win32_client_point(HWND hwnd, LPARAM lparam, float scale, float *x, float *y) {
    POINT pt = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
    ScreenToClient(hwnd, &pt);
    *x = (float)pt.x / scale;
    *y = (float)pt.y / scale;
}

/* WM_NCHITTEST for a window with the program's title bar. */
static LRESULT cee_win32_hit(struct cee_window *w, HWND hwnd, WPARAM wparam, LPARAM lparam, float scale) {
    LRESULT hit = DefWindowProcW(hwnd, WM_NCHITTEST, wparam, lparam); /* the left, right and bottom borders */
    if (hit != HTCLIENT) {
        return hit;
    }
    POINT pt = {(short)LOWORD(lparam), (short)HIWORD(lparam)};
    ScreenToClient(hwnd, &pt);
    RECT client;
    GetClientRect(hwnd, &client);
    int fy = cee_win32_frame(hwnd, 0), fx = cee_win32_frame(hwnd, 1);
    if (!IsZoomed(hwnd) && pt.y < fy) { /* the top border, which the caption's removal took into the client */
        return pt.x < fx ? HTTOPLEFT : pt.x >= client.right - fx ? HTTOPRIGHT : HTTOP;
    }
    float x = (float)pt.x / scale, y = (float)pt.y / scale;
    if (y >= w->bar[0]) {
        return HTCLIENT;
    }
    if (cee_win32_in_box(w, 1, x, y)) {
        return HTMAXBUTTON;
    }
    if (cee_win32_in_box(w, 0, x, y) || cee_win32_in_box(w, 2, x, y) || cee_win32_in_box(w, 3, x, y)) {
        return HTCLIENT;
    }
    return HTCAPTION;
}

static void cee_win32_minimize(struct cee_window *w) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    if (n != NULL) {
        ShowWindow(n->hwnd, SW_MINIMIZE);
    }
}

static void cee_win32_toggle_maximized(struct cee_window *w) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    if (n != NULL) {
        ShowWindow(n->hwnd, IsZoomed(n->hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    }
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
    case WM_GETOBJECT: { /* UI Automation asks for the window's provider (ADR 0164) */
        LRESULT provider = 0;
        if (n != NULL && cee_uia_get_object(w, hwnd, wparam, lparam, &provider)) {
            return provider;
        }
        break;
    }
    case WM_CLOSE:
        cee_window_push(w, CEE_EVENT_CLOSED, 0, 0, 0, 0, 0.0f, 0.0f);
        return 0;
    case WM_NCCALCSIZE:
        if (w->program_bar && wparam && !IsIconic(hwnd)) { /* minimized, the frame is Windows' own */
            /* No caption: the client area keeps the frame's left, right and bottom borders and reaches the top. A
               maximized window overhangs the screen by the frame, so its top is kept off too. */
            NCCALCSIZE_PARAMS *params = (NCCALCSIZE_PARAMS *)lparam;
            int fx = cee_win32_frame(hwnd, 1), fy = cee_win32_frame(hwnd, 0);
            params->rgrc[0].left += fx;
            params->rgrc[0].right -= fx;
            params->rgrc[0].bottom -= fy;
            if (IsZoomed(hwnd)) {
                params->rgrc[0].top += fy;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST:
        if (w->program_bar) {
            return cee_win32_hit(w, hwnd, wparam, lparam, scale);
        }
        break;
    case WM_NCMOUSEMOVE:
        if (w->program_bar && wparam == HTMAXBUTTON) {
            float px = 0.0f, py = 0.0f;
            cee_win32_client_point(hwnd, lparam, scale, &px, &py);
            cee_window_push(w, CEE_EVENT_MOVED, 0, 0, 0, 0, px, py);
            if (!n->nc_tracked) {
                TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE | TME_NONCLIENT, hwnd, 0};
                n->nc_tracked = TrackMouseEvent(&t) != 0;
            }
            return 0;
        }
        break;
    case WM_NCMOUSELEAVE:
        if (n->nc_tracked) {
            n->nc_tracked = 0;
            cee_window_push(w, CEE_EVENT_MOVED, 0, 0, 0, 0, -1.0f, -1.0f); /* off the maximize box: no hover */
        }
        break;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONUP:
        if (w->program_bar && wparam == HTMAXBUTTON) {
            /* The program's button, not Windows' (which would draw a classic one): the press is the program's. */
            float px = 0.0f, py = 0.0f;
            cee_win32_client_point(hwnd, lparam, scale, &px, &py);
            cee_window_push(w, CEE_EVENT_BUTTON, 0, message == WM_NCLBUTTONDOWN, 0, 0, px, py);
            return 0;
        }
        break;
    case WM_SIZE: {
        int maximized = wparam == SIZE_MAXIMIZED;
        if (wparam == SIZE_MINIMIZED) {
            return 0; /* the content keeps its size while the window is minimized */
        }
        if (maximized != w->maximized) {
            w->maximized = maximized; /* ADR 0206: a change is a Resized, even at the same size */
            cee_window_push(w, CEE_EVENT_RESIZED, (int32_t)w->width, (int32_t)w->height, 0, 0, 0.0f, 0.0f);
        }
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
    case WM_SETFOCUS:
        cee_window_focus(w, 1);
        cee_uia_focused(w); /* a reader is told (ADR 0164) */
        return 0;
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
    w->program_bar = w->wants_bar; /* ADR 0206: the caption goes at WM_NCCALCSIZE */
    RECT r = {0, 0, (LONG)((float)w->width * w->scale + 0.5f), (LONG)((float)w->height * w->scale + 0.5f)};
    AdjustWindowRectEx(&r, style, FALSE, 0);
    if (w->program_bar) {
        /* The content and the frame's left, right and bottom borders: no caption above it. */
        r.top = 0;
        r.bottom = (LONG)((float)w->height * w->scale + 0.5f) + cee_win32_frame(hwnd, 0);
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)w);
    SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED); /* the frame asked again: WM_NCCALCSIZE */
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
    cee_uia_close(w);
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
    cee_uia_pump(w); /* what a screen reader asked while the messages were dispatched (ADR 0164) */
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

/* ---- Printing a PDF (ADR 0178 §6) ---------------------------------------------------------------------------------
   Win32 cannot give a PDF to a print dialog: the document is loaded by Windows' own renderer, Windows.Data.Pdf (WinRT,
   reached through its COM interfaces), the common print dialog (PrintDlgExW) gives the printer and the pages, and each
   page is rendered to a bitmap at the printer's resolution (300 dots per inch at most) and drawn on the dialog's
   device context, scaled to fit the printable area, centred, and turned when page and paper differ in orientation.
   combase.dll, shcore.dll and comdlg32.dll are loaded at the first print, and the interfaces' ids and slots are
   written here (from the SDK's windows.data.pdf.h and windows.storage.streams.h), so no program needs a link flag or
   the WinRT headers. For checking only (tools/print_check.py), CEESAFE_PRINT_PRINTER names a printer to print to
   without the dialog, and CEESAFE_PRINT_OUTPUT the file a printer that writes files (Microsoft Print to PDF) writes. */
#include <commdlg.h>

typedef void (*cee_rt_fn)(void);
struct cee_rt {
    const cee_rt_fn *vtbl;
};
/* The function in `slot` of a COM object's vtable, as `type`. IUnknown's are 0 to 2; IInspectable adds 3 to 5. */
#define CEE_RT(object, slot, type) ((type)((const struct cee_rt *)(object))->vtbl[slot])
typedef HRESULT(STDMETHODCALLTYPE *cee_rt_query)(void *, REFIID, void **);
typedef ULONG(STDMETHODCALLTYPE *cee_rt_release)(void *);
typedef HRESULT(STDMETHODCALLTYPE *cee_rt_get)(void *, void **);

static const IID cee_iid_pdf_statics = {0x433a0b5f, 0xc007, 0x4788, {0x90, 0xf2, 0x08, 0x14, 0x3d, 0x92, 0x25, 0x99}};
static const IID cee_iid_pdf_options = {0x3c98056f, 0xb7cf, 0x4c29, {0x9a, 0x04, 0x52, 0xd9, 0x02, 0x67, 0xf4, 0x25}};
static const IID cee_iid_random_stream = {0x905a0fe1, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
static const IID cee_iid_async_info = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID cee_iid_stream = {0x0000000c, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
/* BitmapEncoder.BmpEncoderId: the pages come as BMP, which is read here without a decoder. */
static const GUID cee_bmp_encoder = {0x69be8bb4, 0xd66d, 0x47c8, {0x86, 0x5a, 0xed, 0x15, 0x89, 0x43, 0x37, 0x82}};

static struct {
    int state; /* 0 not tried, 1 loaded, -1 failed */
    HRESULT(WINAPI *factory)(void *, REFIID, void **);              /* RoGetActivationFactory */
    HRESULT(WINAPI *activate)(void *, void **);                     /* RoActivateInstance */
    HRESULT(WINAPI *create_string)(const wchar_t *, UINT32, void **); /* WindowsCreateString */
    HRESULT(WINAPI *delete_string)(void *);                         /* WindowsDeleteString */
    HRESULT(WINAPI *file_stream)(const wchar_t *, DWORD, REFIID, void **); /* CreateRandomAccessStreamOnFile */
    HRESULT(WINAPI *over_stream)(void *, REFIID, void **);          /* CreateStreamOverRandomAccessStream */
    HRESULT(WINAPI *print_dialog)(LPPRINTDLGEXW);                   /* PrintDlgExW */
} cee_pdf;

static int cee_pdf_load(void) {
    if (cee_pdf.state != 0) {
        return cee_pdf.state == 1;
    }
    cee_pdf.state = -1;
    HMODULE combase = LoadLibraryW(L"combase.dll");
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    HMODULE comdlg = LoadLibraryW(L"comdlg32.dll");
    if (combase == NULL || shcore == NULL || comdlg == NULL) {
        return 0;
    }
    FARPROC p[7] = {GetProcAddress(combase, "RoGetActivationFactory"), GetProcAddress(combase, "RoActivateInstance"),
                    GetProcAddress(combase, "WindowsCreateString"),    GetProcAddress(combase, "WindowsDeleteString"),
                    GetProcAddress(shcore, "CreateRandomAccessStreamOnFile"),
                    GetProcAddress(shcore, "CreateStreamOverRandomAccessStream"),
                    GetProcAddress(comdlg, "PrintDlgExW")};
    for (int k = 0; k < 7; k++) {
        if (p[k] == NULL) {
            return 0;
        }
    }
    memcpy(&cee_pdf.factory, &p[0], sizeof p[0]);
    memcpy(&cee_pdf.activate, &p[1], sizeof p[1]);
    memcpy(&cee_pdf.create_string, &p[2], sizeof p[2]);
    memcpy(&cee_pdf.delete_string, &p[3], sizeof p[3]);
    memcpy(&cee_pdf.file_stream, &p[4], sizeof p[4]);
    memcpy(&cee_pdf.over_stream, &p[5], sizeof p[5]);
    memcpy(&cee_pdf.print_dialog, &p[6], sizeof p[6]);
    cee_pdf.state = 1;
    return 1;
}

static void cee_rt_free(void *object) {
    if (object != NULL) {
        (void)CEE_RT(object, 2, cee_rt_release)(object);
    }
}

/* A WinRT class's activation factory as `iid` (`instance` 0), or a new instance of it as `iid` (`instance` 1). */
static HRESULT cee_pdf_class(const wchar_t *name, int instance, REFIID iid, void **out) {
    void *string = NULL;
    *out = NULL;
    HRESULT h = cee_pdf.create_string(name, (UINT32)lstrlenW(name), &string);
    if (FAILED(h)) {
        return h;
    }
    if (instance) {
        void *object = NULL;
        h = cee_pdf.activate(string, &object);
        if (SUCCEEDED(h)) {
            h = CEE_RT(object, 0, cee_rt_query)(object, iid, out);
            cee_rt_free(object);
        }
    } else {
        h = cee_pdf.factory(string, iid, out);
    }
    (void)cee_pdf.delete_string(string);
    return h;
}

/* Waits for a WinRT operation or action (IAsyncInfo's status, slot 7: 0 started, 1 completed) for a minute at most:
   whether it completed. One that is still going is cancelled (slot 9). */
static int cee_pdf_wait(void *operation) {
    void *info = NULL;
    if (operation == NULL || FAILED(CEE_RT(operation, 0, cee_rt_query)(operation, &cee_iid_async_info, &info))) {
        return 0;
    }
    int status = 0;
    for (int k = 0; k < 60000 && status == 0; k++) {
        if (FAILED(CEE_RT(info, 7, HRESULT(STDMETHODCALLTYPE *)(void *, int *))(info, &status))) {
            status = 3;
        } else if (status == 0) {
            Sleep(1);
        }
    }
    if (status == 0) {
        (void)CEE_RT(info, 9, HRESULT(STDMETHODCALLTYPE *)(void *))(info);
    }
    cee_rt_free(info);
    return status == 1;
}

/* The PDF at `file` as an IPdfDocument (PdfDocument.LoadFromStreamAsync, slot 8 of its statics), or NULL when Windows
   does not read it. */
static void *cee_pdf_open(const char *file) {
    wchar_t *path = cee_win32_wide(file);
    void *stream = NULL, *statics = NULL, *operation = NULL, *document = NULL;
    if (path != NULL && SUCCEEDED(cee_pdf.file_stream(path, 0 /* FileAccessMode.Read */, &cee_iid_random_stream,
                                                      &stream)) &&
        SUCCEEDED(cee_pdf_class(L"Windows.Data.Pdf.PdfDocument", 0, &cee_iid_pdf_statics, &statics)) &&
        SUCCEEDED(CEE_RT(statics, 8, HRESULT(STDMETHODCALLTYPE *)(void *, void *, void **))(statics, stream,
                                                                                            &operation)) &&
        cee_pdf_wait(operation)) {
        /* IAsyncOperation<PdfDocument>.GetResults, slot 8 */
        if (FAILED(CEE_RT(operation, 8, cee_rt_get)(operation, &document))) {
            document = NULL;
        }
    }
    cee_rt_free(operation);
    cee_rt_free(statics);
    cee_rt_free(stream);
    ceesafe_platform_free(path);
    return document;
}

/* A page's pixels, top row first, as BGRX. */
struct cee_pdf_bitmap {
    uint32_t *pixels;
    uint32_t width, height;
};

static uint32_t cee_pdf_u32(const unsigned char *b) {
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

/* A BMP of 24 or 32 bits a pixel (as Windows' encoder writes it) into `out`: whether it was one. */
static int cee_pdf_bmp(const unsigned char *b, size_t n, struct cee_pdf_bitmap *out) {
    if (n < 54 || b[0] != 'B' || b[1] != 'M') {
        return 0;
    }
    uint32_t offset = cee_pdf_u32(b + 10), compression = cee_pdf_u32(b + 30);
    int32_t width = (int32_t)cee_pdf_u32(b + 18), height = (int32_t)cee_pdf_u32(b + 22);
    unsigned bits = (unsigned)b[28] | (unsigned)b[29] << 8;
    if (width <= 0 || height == 0 || height == INT32_MIN || (bits != 24 && bits != 32) ||
        (compression != 0 && compression != 3)) {
        return 0;
    }
    uint32_t w = (uint32_t)width, h = (uint32_t)(height < 0 ? -height : height);
    size_t stride = ((size_t)w * bits / 8u + 3u) & ~(size_t)3u;
    if (offset > n || (n - offset) / stride < h || (size_t)w * h > SIZE_MAX / 4u) {
        return 0;
    }
    uint32_t *pixels = (uint32_t *)ceesafe_platform_alloc((size_t)w * h * 4u);
    if (pixels == NULL) {
        return 0;
    }
    for (uint32_t y = 0; y < h; y++) {
        const unsigned char *row = b + offset + stride * (height < 0 ? y : h - 1u - y);
        uint32_t *to = pixels + (size_t)y * w;
        for (uint32_t x = 0; x < w; x++) {
            const unsigned char *p = row + (size_t)x * (bits / 8u);
            to[x] = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
        }
    }
    out->pixels = pixels;
    out->width = w;
    out->height = h;
    return 1;
}

/* `page` (an IPdfPage) rendered at `width` × `height` pixels into `out`: PdfPage.RenderWithOptionsToStreamAsync
   (slot 7) to a stream in memory, as BMP, read back through an IStream over it. Whether it was. */
static int cee_pdf_render(void *page, uint32_t width, uint32_t height, struct cee_pdf_bitmap *out) {
    void *options = NULL, *memory = NULL, *action = NULL;
    IStream *stream = NULL;
    unsigned char *bytes = NULL;
    int done = 0;
    if (SUCCEEDED(cee_pdf_class(L"Windows.Data.Pdf.PdfPageRenderOptions", 1, &cee_iid_pdf_options, &options)) &&
        SUCCEEDED(cee_pdf_class(L"Windows.Storage.Streams.InMemoryRandomAccessStream", 1, &cee_iid_random_stream,
                                &memory)) &&
        /* DestinationWidth (9), DestinationHeight (11), BitmapEncoderId (17) */
        SUCCEEDED(CEE_RT(options, 9, HRESULT(STDMETHODCALLTYPE *)(void *, UINT32))(options, width)) &&
        SUCCEEDED(CEE_RT(options, 11, HRESULT(STDMETHODCALLTYPE *)(void *, UINT32))(options, height)) &&
        SUCCEEDED(CEE_RT(options, 17, HRESULT(STDMETHODCALLTYPE *)(void *, GUID))(options, cee_bmp_encoder)) &&
        SUCCEEDED(CEE_RT(page, 7, HRESULT(STDMETHODCALLTYPE *)(void *, void *, void *, void **))(page, memory, options,
                                                                                              &action)) &&
        cee_pdf_wait(action) && SUCCEEDED(CEE_RT(action, 8, HRESULT(STDMETHODCALLTYPE *)(void *))(action)) &&
        SUCCEEDED(cee_pdf.over_stream(memory, &cee_iid_stream, (void **)&stream))) {
        STATSTG stat;
        LARGE_INTEGER start;
        start.QuadPart = 0;
        if (SUCCEEDED(IStream_Stat(stream, &stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart > 0 &&
            stat.cbSize.QuadPart < 0x7fffffffu && SUCCEEDED(IStream_Seek(stream, start, STREAM_SEEK_SET, NULL))) {
            ULONG size = (ULONG)stat.cbSize.QuadPart, got = 0;
            bytes = (unsigned char *)ceesafe_platform_alloc(size);
            while (bytes != NULL && got < size) {
                ULONG read = 0;
                if (FAILED(IStream_Read(stream, bytes + got, size - got, &read)) || read == 0) {
                    break;
                }
                got += read;
            }
            done = bytes != NULL && got == size && cee_pdf_bmp(bytes, size, out);
        }
    }
    ceesafe_platform_free(bytes);
    if (stream != NULL) {
        IStream_Release(stream);
    }
    cee_rt_free(action);
    cee_rt_free(memory);
    cee_rt_free(options);
    return done;
}

/* The document's page `index` printed on its own sheet of `dc`: whether it was. */
static int cee_pdf_page(void *document, uint32_t index, HDC dc) {
    void *page = NULL;
    struct {
        float width, height; /* Windows.Foundation.Size, in device-independent pixels (1/96 inch) */
    } size = {0.0f, 0.0f};
    /* IPdfDocument.GetPage (6), IPdfPage.Size (10) */
    if (FAILED(CEE_RT(document, 6, HRESULT(STDMETHODCALLTYPE *)(void *, UINT32, void **))(document, index, &page)) ||
        FAILED(CEE_RT(page, 10, HRESULT(STDMETHODCALLTYPE *)(void *, void *))(page, &size)) || size.width <= 0.0f ||
        size.height <= 0.0f) {
        cee_rt_free(page);
        return 0;
    }
    int area_w = GetDeviceCaps(dc, HORZRES), area_h = GetDeviceCaps(dc, VERTRES);
    int dpi_x = GetDeviceCaps(dc, LOGPIXELSX), dpi_y = GetDeviceCaps(dc, LOGPIXELSY);
    if (area_w <= 0 || area_h <= 0 || dpi_x <= 0 || dpi_y <= 0) {
        cee_rt_free(page);
        return 0;
    }
    /* The page in inches, turned when it is landscape on portrait paper or the other way. */
    double page_w = (double)size.width / 96.0, page_h = (double)size.height / 96.0;
    int turn = (page_w > page_h && area_h > area_w) || (page_h > page_w && area_w > area_h);
    double sheet_w = turn ? page_h : page_w, sheet_h = turn ? page_w : page_h;
    double scale_x = (double)area_w / (sheet_w * dpi_x), scale_y = (double)area_h / (sheet_h * dpi_y);
    double scale = scale_x < scale_y ? scale_x : scale_y;
    int draw_w = (int)(sheet_w * dpi_x * scale), draw_h = (int)(sheet_h * dpi_y * scale);
    /* The bitmap: the drawn size at the printer's resolution, 300 dots per inch at most. */
    double keep_x = dpi_x > 300 ? 300.0 / dpi_x : 1.0, keep_y = dpi_y > 300 ? 300.0 / dpi_y : 1.0;
    uint32_t bitmap_w = (uint32_t)(draw_w * keep_x), bitmap_h = (uint32_t)(draw_h * keep_y);
    if (draw_w <= 0 || draw_h <= 0 || bitmap_w == 0 || bitmap_h == 0) {
        cee_rt_free(page);
        return 0;
    }
    struct cee_pdf_bitmap bitmap = {NULL, 0, 0};
    int done = cee_pdf_render(page, turn ? bitmap_h : bitmap_w, turn ? bitmap_w : bitmap_h, &bitmap);
    cee_rt_free(page);
    if (done && turn) {
        /* A quarter turn clockwise: the pixel at (x, y) goes to (height - 1 - y, x). */
        uint32_t *turned = (uint32_t *)ceesafe_platform_alloc((size_t)bitmap.width * bitmap.height * 4u);
        if (turned == NULL) {
            done = 0;
        } else {
            for (uint32_t y = 0; y < bitmap.height; y++) {
                for (uint32_t x = 0; x < bitmap.width; x++) {
                    turned[(size_t)x * bitmap.height + (bitmap.height - 1u - y)] =
                        bitmap.pixels[(size_t)y * bitmap.width + x];
                }
            }
            ceesafe_platform_free(bitmap.pixels);
            bitmap.pixels = turned;
            uint32_t width = bitmap.width;
            bitmap.width = bitmap.height;
            bitmap.height = width;
        }
    }
    if (done) {
        BITMAPINFO info;
        memset(&info, 0, sizeof info);
        info.bmiHeader.biSize = sizeof info.bmiHeader;
        info.bmiHeader.biWidth = (LONG)bitmap.width;
        info.bmiHeader.biHeight = -(LONG)bitmap.height; /* top row first */
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        done = StartPage(dc) > 0;
        if (done) {
            (void)SetStretchBltMode(dc, HALFTONE);
            (void)SetBrushOrgEx(dc, 0, 0, NULL);
            done = StretchDIBits(dc, (area_w - draw_w) / 2, (area_h - draw_h) / 2, draw_w, draw_h, 0, 0,
                                 (int)bitmap.width, (int)bitmap.height, bitmap.pixels, &info, DIB_RGB_COLORS,
                                 SRCCOPY) != 0 &&
                   EndPage(dc) > 0;
        }
    }
    ceesafe_platform_free(bitmap.pixels);
    return done;
}

/* The document's pages (`ranges`, or every one with none), `copies` times, as one job on `dc`: 1 given to the spooler,
   or minus a WindowError's number (the job abandoned). */
static int32_t cee_pdf_job(void *document, uint32_t pages, HDC dc, const wchar_t *title, const wchar_t *output,
                           const PRINTPAGERANGE *ranges, DWORD range_count, DWORD copies, int collate) {
    DOCINFOW job;
    memset(&job, 0, sizeof job);
    job.cbSize = (int)sizeof job;
    job.lpszDocName = title;
    job.lpszOutput = output;
    if (StartDocW(dc, &job) <= 0) {
        return -CEE_WINDOW_RESOURCES;
    }
    PRINTPAGERANGE every = {1, pages};
    if (range_count == 0) {
        ranges = &every;
        range_count = 1;
    }
    if (copies == 0) {
        copies = 1;
    }
    int done = 1;
    /* Collated: the document, then again; else each page its copies, then the next. */
    for (DWORD round = 0; done && round < (collate ? copies : 1u); round++) {
        for (DWORD r = 0; done && r < range_count; r++) {
            DWORD from = ranges[r].nFromPage < 1 ? 1 : ranges[r].nFromPage;
            DWORD to = ranges[r].nToPage > pages ? pages : ranges[r].nToPage;
            for (DWORD p = from; done && p <= to; p++) {
                for (DWORD c = 0; done && c < (collate ? 1u : copies); c++) {
                    done = cee_pdf_page(document, (uint32_t)(p - 1u), dc);
                }
            }
        }
    }
    if (!done) {
        (void)AbortDoc(dc);
        return -CEE_WINDOW_RESOURCES;
    }
    return EndDoc(dc) > 0 ? 1 : -CEE_WINDOW_RESOURCES;
}

/* An environment variable (for checking: CEESAFE_PRINT_PRINTER, CEESAFE_PRINT_OUTPUT), or NULL when it is not set. */
static wchar_t *cee_pdf_variable(const wchar_t *name) {
    DWORD units = GetEnvironmentVariableW(name, NULL, 0);
    wchar_t *value = units > 1 ? (wchar_t *)ceesafe_platform_alloc((size_t)units * sizeof(wchar_t)) : NULL;
    if (value != NULL && GetEnvironmentVariableW(name, value, units) != units - 1u) {
        ceesafe_platform_free(value);
        value = NULL;
    }
    return value;
}

#define CEE_HAS_NATIVE_PRINT 1
static int32_t cee_native_print(struct cee_window *w, const char *title, const char *file) {
    struct cee_win32 *n = (struct cee_win32 *)w->native;
    if (!cee_ole_load() || !cee_pdf_load()) {
        return -CEE_WINDOW_UNSUPPORTED;
    }
    HRESULT initialized = cee_ole.initialize(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    void *document = cee_pdf_open(file);
    UINT32 pages = 0;
    int32_t result = -CEE_WINDOW_RESOURCES;
    /* IPdfDocument.PageCount (7) */
    if (document != NULL &&
        SUCCEEDED(CEE_RT(document, 7, HRESULT(STDMETHODCALLTYPE *)(void *, UINT32 *))(document, &pages)) &&
        pages > 0) {
        wchar_t *name = cee_win32_wide(title[0] != '\0' ? title : file);
        wchar_t *printer = cee_pdf_variable(L"CEESAFE_PRINT_PRINTER");
        wchar_t *output = cee_pdf_variable(L"CEESAFE_PRINT_OUTPUT");
        if (name == NULL) {
            result = -CEE_WINDOW_RESOURCES;
        } else if (printer != NULL) {
            HDC dc = CreateDCW(L"WINSPOOL", printer, NULL, NULL);
            result = dc != NULL ? cee_pdf_job(document, pages, dc, name, output, NULL, 0, 1, 1) : -CEE_WINDOW_RESOURCES;
            if (dc != NULL) {
                DeleteDC(dc);
            }
        } else {
            PRINTPAGERANGE ranges[16];
            PRINTDLGEXW dialog;
            memset(&dialog, 0, sizeof dialog);
            dialog.lStructSize = sizeof dialog;
            dialog.hwndOwner = n->hwnd;
            dialog.Flags = PD_RETURNDC | PD_NOSELECTION | PD_NOCURRENTPAGE | PD_USEDEVMODECOPIESANDCOLLATE;
            dialog.nMaxPageRanges = 16;
            dialog.lpPageRanges = ranges;
            dialog.nMinPage = 1;
            dialog.nMaxPage = pages;
            dialog.nCopies = 1;
            dialog.nStartPage = START_PAGE_GENERAL;
            if (FAILED(cee_pdf.print_dialog(&dialog))) {
                result = -CEE_WINDOW_RESOURCES;
            } else if (dialog.dwResultAction != PD_RESULT_PRINT || dialog.hDC == NULL) {
                result = 0; /* cancelled, or only applied */
            } else {
                result = cee_pdf_job(document, pages, dialog.hDC, name, NULL, ranges,
                                     (dialog.Flags & PD_PAGENUMS) != 0 ? dialog.nPageRanges : 0, dialog.nCopies,
                                     (dialog.Flags & PD_COLLATE) != 0);
            }
            if (dialog.hDC != NULL) {
                DeleteDC(dialog.hDC);
            }
            if (dialog.hDevMode != NULL) {
                GlobalFree(dialog.hDevMode);
            }
            if (dialog.hDevNames != NULL) {
                GlobalFree(dialog.hDevNames);
            }
        }
        ceesafe_platform_free(name);
        ceesafe_platform_free(printer);
        ceesafe_platform_free(output);
    }
    cee_rt_free(document);
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
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>
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
    /* Text input (ADR 0157). What the program wants (text input on, the kind of text), which the activity's thread
       applies when a byte arrives on `ui_pipe` (a view may be used only there); the soft keyboard's inset in pixels;
       and what the input method typed, queued for the program as records: a type (0 committed text, 1 the
       composition, 2 a key), a length, the bytes. All under the lock. */
    int want_input, want_kind;
    int ui_pipe[2];
    int32_t keyboard;
    char *typed;
    size_t typed_length, typed_capacity;
    /* The input view, an org.ceesafe.window.InputView, with its class and its `apply`: the activity's thread's. */
    jobject input_view;
    jclass input_class;
    jmethodID input_apply;
    int input_failed;
    /* The runtime's Java classes (ADR 0157, ADR 0158), loaded once: 0 not tried, 1 loaded, -1 failed; and the
       picker's class, an org.ceesafe.window.Picker. The activity's thread's. */
    int classes;
    jclass picker_class;
    /* A file dialog (ADR 0158), under the lock: 0 none, 1 asked for by the program, 2 started on the activity's
       thread, 3 answered; what it asks (the dialog's kind and its three texts, the program's until it is answered);
       and the answer: 1 with the paths, each followed by a zero byte, 0 a cancel, less an error. */
    int pick_state;
    int pick_kind;
    const char *pick_extensions, *pick_name, *pick_file;
    int32_t pick_status;
    char *pick_paths;
    size_t pick_length;
    /* Accessibility (ADR 0164). Under the lock: the program's list packed for AccessView.update, to be given to
       the view when `access_dirty` (the activity's thread is asked through `ui_pipe`); a text to announce; what
       TalkBack asked, for the program to take; whether anything asked for the elements. The activity's thread's:
       the access view, an org.ceesafe.window.AccessView, with its class and methods. */
    char *access_packed;
    size_t access_length;
    int access_dirty;
    char *access_said;
    size_t access_said_length;
    struct cee_access_request access_asked[CEE_ACCESS_QUEUE];
    unsigned access_asked_count;
    int access_active;
    jclass access_class;
    jobject access_view;
    jmethodID access_update, access_announce;
    int access_failed;
} cee_android = {PTHREAD_MUTEX_INITIALIZER, NULL, NULL, NULL, NULL, 1, 0, NULL, 1.0f, {0, 0, 0, 0}, 0, 0, {-1, -1}, 0,
                 NULL, 0, 0, NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, NULL, 0,
                 /* accessibility (ADR 0164) */
                 NULL, 0, 0, NULL, 0, {{0, 0}}, 0u, 0, NULL, NULL, NULL, NULL, 0};

/* The app's life (ADR 0159), under cee_android's lock: whether the activity is stopped (off the screen), whether the
   program asks for app events at all (only then is onStop held for it), and whether it has saved since the last
   stop. */
static struct {
    int background;
    int listening;
    int saved;
    /* What the activity was told since the program last took it, in order (WindowAppEvent's numbers): a stop and a
       start that come between two of the program's polls are both given. */
    uint8_t told[16];
    unsigned told_count;
} cee_android_life;

/* Under the lock. A full list drops its oldest word. */
static void cee_android_told(int kind) {
    if (cee_android_life.told_count == sizeof cee_android_life.told) {
        memmove(cee_android_life.told, cee_android_life.told + 1, sizeof cee_android_life.told - 1u);
        cee_android_life.told_count--;
    }
    cee_android_life.told[cee_android_life.told_count++] = (uint8_t)kind;
}

/* The input view's class as a dex file (ADR 0157), loaded from memory at the first text input. */
/* BEGIN android dex: written by tools/gen_android_dex.py from runtime/android; do not edit */
/* the Java sources' SHA-256: 0b38408f631a9a057efee5a93bd502380eace1bead84a273201bec4985923c2b */
static uint8_t cee_android_dex[38120] = {
    100,101,120,10,48,51,55,0,129,199,181,143,28,83,54,220,6,133,1,145,73,131,136,231,
    108,229,140,25,206,242,180,166,232,148,0,0,112,0,0,0,120,86,52,18,0,0,0,0,
    0,0,0,0,24,148,0,0,172,2,0,0,112,0,0,0,146,0,0,0,32,11,0,0,
    218,0,0,0,104,13,0,0,120,0,0,0,160,23,0,0,157,1,0,0,96,27,0,0,
    13,0,0,0,72,40,0,0,0,107,0,0,232,41,0,0,222,96,0,0,224,96,0,0,
    228,96,0,0,233,96,0,0,238,96,0,0,242,96,0,0,245,96,0,0,8,97,0,0,
    36,97,0,0,59,97,0,0,82,97,0,0,102,97,0,0,122,97,0,0,141,97,0,0,
    162,97,0,0,180,97,0,0,198,97,0,0,215,97,0,0,236,97,0,0,0,98,0,0,
    20,98,0,0,40,98,0,0,59,98,0,0,78,98,0,0,98,98,0,0,118,98,0,0,
    138,98,0,0,155,98,0,0,172,98,0,0,192,98,0,0,213,98,0,0,239,98,0,0,
    2,99,0,0,30,99,0,0,53,99,0,0,73,99,0,0,94,99,0,0,112,99,0,0,
    132,99,0,0,152,99,0,0,169,99,0,0,185,99,0,0,205,99,0,0,222,99,0,0,
    238,99,0,0,252,99,0,0,12,100,0,0,30,100,0,0,46,100,0,0,62,100,0,0,
    78,100,0,0,102,100,0,0,132,100,0,0,153,100,0,0,177,100,0,0,199,100,0,0,
    216,100,0,0,219,100,0,0,223,100,0,0,229,100,0,0,11,101,0,0,49,101,0,0,
    87,101,0,0,97,101,0,0,105,101,0,0,109,101,0,0,118,101,0,0,146,101,0,0,
    180,101,0,0,194,101,0,0,218,101,0,0,241,101,0,0,252,101,0,0,7,102,0,0,
    15,102,0,0,18,102,0,0,29,102,0,0,35,102,0,0,45,102,0,0,54,102,0,0,
    63,102,0,0,78,102,0,0,86,102,0,0,95,102,0,0,107,102,0,0,110,102,0,0,
    130,102,0,0,141,102,0,0,149,102,0,0,159,102,0,0,169,102,0,0,207,102,0,0,
    214,102,0,0,222,102,0,0,225,102,0,0,233,102,0,0,239,102,0,0,246,102,0,0,
    253,102,0,0,0,103,0,0,4,103,0,0,8,103,0,0,13,103,0,0,17,103,0,0,
    23,103,0,0,28,103,0,0,35,103,0,0,46,103,0,0,49,103,0,0,52,103,0,0,
    56,103,0,0,63,103,0,0,68,103,0,0,77,103,0,0,83,103,0,0,89,103,0,0,
    100,103,0,0,115,103,0,0,119,103,0,0,123,103,0,0,128,103,0,0,134,103,0,0,
    139,103,0,0,145,103,0,0,153,103,0,0,158,103,0,0,165,103,0,0,171,103,0,0,
    175,103,0,0,199,103,0,0,223,103,0,0,254,103,0,0,33,104,0,0,71,104,0,0,
    107,104,0,0,144,104,0,0,179,104,0,0,222,104,0,0,15,105,0,0,57,105,0,0,
    95,105,0,0,132,105,0,0,175,105,0,0,212,105,0,0,255,105,0,0,34,106,0,0,
    69,106,0,0,104,106,0,0,149,106,0,0,186,106,0,0,219,106,0,0,247,106,0,0,
    26,107,0,0,53,107,0,0,79,107,0,0,106,107,0,0,131,107,0,0,150,107,0,0,
    178,107,0,0,199,107,0,0,232,107,0,0,254,107,0,0,20,108,0,0,41,108,0,0,
    76,108,0,0,101,108,0,0,127,108,0,0,154,108,0,0,187,108,0,0,246,108,0,0,
    48,109,0,0,86,109,0,0,129,109,0,0,164,109,0,0,190,109,0,0,220,109,0,0,
    250,109,0,0,19,110,0,0,47,110,0,0,68,110,0,0,95,110,0,0,118,110,0,0,
    147,110,0,0,196,110,0,0,247,110,0,0,63,111,0,0,130,111,0,0,201,111,0,0,
    7,112,0,0,59,112,0,0,115,112,0,0,163,112,0,0,202,112,0,0,246,112,0,0,
    37,113,0,0,67,113,0,0,103,113,0,0,140,113,0,0,172,113,0,0,207,113,0,0,
    245,113,0,0,20,114,0,0,48,114,0,0,54,114,0,0,87,114,0,0,103,114,0,0,
    129,114,0,0,156,114,0,0,184,114,0,0,207,114,0,0,230,114,0,0,254,114,0,0,
    24,115,0,0,47,115,0,0,70,115,0,0,91,115,0,0,125,115,0,0,143,115,0,0,
    161,115,0,0,181,115,0,0,203,115,0,0,233,115,0,0,8,116,0,0,28,116,0,0,
    55,116,0,0,75,116,0,0,95,116,0,0,118,116,0,0,141,116,0,0,163,116,0,0,
    191,116,0,0,228,116,0,0,251,116,0,0,15,117,0,0,36,117,0,0,57,117,0,0,
    79,117,0,0,106,117,0,0,124,117,0,0,147,117,0,0,164,117,0,0,181,117,0,0,
    199,117,0,0,233,117,0,0,10,118,0,0,52,118,0,0,88,118,0,0,121,118,0,0,
    155,118,0,0,189,118,0,0,226,118,0,0,2,119,0,0,45,119,0,0,77,119,0,0,
    108,119,0,0,137,119,0,0,170,119,0,0,201,119,0,0,207,119,0,0,217,119,0,0,
    228,119,0,0,242,119,0,0,253,119,0,0,2,120,0,0,9,120,0,0,16,120,0,0,
    25,120,0,0,35,120,0,0,42,120,0,0,51,120,0,0,56,120,0,0,63,120,0,0,
    72,120,0,0,80,120,0,0,90,120,0,0,100,120,0,0,111,120,0,0,119,120,0,0,
    124,120,0,0,129,120,0,0,136,120,0,0,146,120,0,0,151,120,0,0,157,120,0,0,
    169,120,0,0,176,120,0,0,189,120,0,0,196,120,0,0,199,120,0,0,203,120,0,0,
    208,120,0,0,215,120,0,0,221,120,0,0,226,120,0,0,230,120,0,0,235,120,0,0,
    239,120,0,0,246,120,0,0,251,120,0,0,1,121,0,0,9,121,0,0,17,121,0,0,
    23,121,0,0,28,121,0,0,34,121,0,0,43,121,0,0,49,121,0,0,56,121,0,0,
    64,121,0,0,69,121,0,0,73,121,0,0,78,121,0,0,81,121,0,0,85,121,0,0,
    90,121,0,0,96,121,0,0,101,121,0,0,105,121,0,0,110,121,0,0,114,121,0,0,
    119,121,0,0,124,121,0,0,129,121,0,0,136,121,0,0,140,121,0,0,144,121,0,0,
    148,121,0,0,152,121,0,0,156,121,0,0,160,121,0,0,186,121,0,0,214,121,0,0,
    235,121,0,0,0,122,0,0,19,122,0,0,24,122,0,0,39,122,0,0,52,122,0,0,
    67,122,0,0,81,122,0,0,89,122,0,0,98,122,0,0,103,122,0,0,114,122,0,0,
    127,122,0,0,137,122,0,0,152,122,0,0,191,122,0,0,228,122,0,0,6,123,0,0,
    43,123,0,0,76,123,0,0,104,123,0,0,145,123,0,0,183,123,0,0,218,123,0,0,
    242,123,0,0,9,124,0,0,34,124,0,0,59,124,0,0,84,124,0,0,110,124,0,0,
    135,124,0,0,163,124,0,0,187,124,0,0,211,124,0,0,236,124,0,0,246,124,0,0,
    16,125,0,0,24,125,0,0,34,125,0,0,42,125,0,0,68,125,0,0,75,125,0,0,
    86,125,0,0,93,125,0,0,100,125,0,0,118,125,0,0,129,125,0,0,136,125,0,0,
    153,125,0,0,169,125,0,0,185,125,0,0,201,125,0,0,224,125,0,0,245,125,0,0,
    0,126,0,0,7,126,0,0,14,126,0,0,39,126,0,0,51,126,0,0,62,126,0,0,
    71,126,0,0,84,126,0,0,95,126,0,0,108,126,0,0,114,126,0,0,122,126,0,0,
    130,126,0,0,159,126,0,0,202,126,0,0,237,126,0,0,252,126,0,0,15,127,0,0,
    38,127,0,0,50,127,0,0,68,127,0,0,88,127,0,0,98,127,0,0,112,127,0,0,
    121,127,0,0,131,127,0,0,141,127,0,0,149,127,0,0,156,127,0,0,184,127,0,0,
    195,127,0,0,200,127,0,0,205,127,0,0,213,127,0,0,222,127,0,0,228,127,0,0,
    247,127,0,0,255,127,0,0,20,128,0,0,27,128,0,0,34,128,0,0,43,128,0,0,
    55,128,0,0,61,128,0,0,66,128,0,0,83,128,0,0,113,128,0,0,124,128,0,0,
    137,128,0,0,149,128,0,0,161,128,0,0,180,128,0,0,203,128,0,0,217,128,0,0,
    235,128,0,0,245,128,0,0,2,129,0,0,22,129,0,0,35,129,0,0,55,129,0,0,
    67,129,0,0,76,129,0,0,87,129,0,0,102,129,0,0,113,129,0,0,132,129,0,0,
    142,129,0,0,162,129,0,0,171,129,0,0,179,129,0,0,190,129,0,0,204,129,0,0,
    212,129,0,0,224,129,0,0,244,129,0,0,9,130,0,0,18,130,0,0,33,130,0,0,
    54,130,0,0,66,130,0,0,92,130,0,0,101,130,0,0,118,130,0,0,134,130,0,0,
    145,130,0,0,160,130,0,0,177,130,0,0,192,130,0,0,205,130,0,0,219,130,0,0,
    229,130,0,0,240,130,0,0,2,131,0,0,11,131,0,0,27,131,0,0,35,131,0,0,
    44,131,0,0,54,131,0,0,64,131,0,0,75,131,0,0,91,131,0,0,97,131,0,0,
    103,131,0,0,112,131,0,0,121,131,0,0,137,131,0,0,143,131,0,0,168,131,0,0,
    175,131,0,0,184,131,0,0,189,131,0,0,201,131,0,0,210,131,0,0,225,131,0,0,
    242,131,0,0,253,131,0,0,11,132,0,0,21,132,0,0,32,132,0,0,44,132,0,0,
    56,132,0,0,65,132,0,0,76,132,0,0,83,132,0,0,93,132,0,0,98,132,0,0,
    103,132,0,0,111,132,0,0,128,132,0,0,141,132,0,0,149,132,0,0,157,132,0,0,
    162,132,0,0,168,132,0,0,175,132,0,0,180,132,0,0,188,132,0,0,201,132,0,0,
    211,132,0,0,217,132,0,0,224,132,0,0,234,132,0,0,248,132,0,0,12,133,0,0,
    26,133,0,0,37,133,0,0,51,133,0,0,63,133,0,0,71,133,0,0,77,133,0,0,
    85,133,0,0,93,133,0,0,113,133,0,0,131,133,0,0,152,133,0,0,175,133,0,0,
    196,133,0,0,221,133,0,0,246,133,0,0,0,134,0,0,32,134,0,0,67,134,0,0,
    78,134,0,0,88,134,0,0,107,134,0,0,125,134,0,0,139,134,0,0,153,134,0,0,
    175,134,0,0,184,134,0,0,202,134,0,0,217,134,0,0,234,134,0,0,240,134,0,0,
    253,134,0,0,7,135,0,0,24,135,0,0,42,135,0,0,49,135,0,0,76,135,0,0,
    84,135,0,0,91,135,0,0,119,135,0,0,134,135,0,0,155,135,0,0,163,135,0,0,
    170,135,0,0,178,135,0,0,185,135,0,0,191,135,0,0,204,135,0,0,211,135,0,0,
    220,135,0,0,236,135,0,0,246,135,0,0,251,135,0,0,3,136,0,0,13,136,0,0,
    20,136,0,0,28,136,0,0,34,136,0,0,42,136,0,0,49,136,0,0,58,136,0,0,
    67,136,0,0,81,136,0,0,93,136,0,0,113,136,0,0,144,136,0,0,158,136,0,0,
    165,136,0,0,172,136,0,0,177,136,0,0,192,136,0,0,198,136,0,0,204,136,0,0,
    213,136,0,0,227,136,0,0,252,136,0,0,15,137,0,0,34,137,0,0,48,137,0,0,
    60,137,0,0,74,137,0,0,88,137,0,0,107,137,0,0,130,137,0,0,148,137,0,0,
    171,137,0,0,194,137,0,0,210,137,0,0,223,137,0,0,235,137,0,0,249,137,0,0,
    18,138,0,0,48,138,0,0,64,138,0,0,78,138,0,0,89,138,0,0,102,138,0,0,
    116,138,0,0,129,138,0,0,142,138,0,0,158,138,0,0,176,138,0,0,187,138,0,0,
    196,138,0,0,205,138,0,0,215,138,0,0,233,138,0,0,249,138,0,0,7,139,0,0,
    22,139,0,0,29,139,0,0,37,139,0,0,43,139,0,0,50,139,0,0,58,139,0,0,
    65,139,0,0,72,139,0,0,96,139,0,0,107,139,0,0,119,139,0,0,126,139,0,0,
    136,139,0,0,144,139,0,0,155,139,0,0,161,139,0,0,192,139,0,0,200,139,0,0,
    209,139,0,0,222,139,0,0,231,139,0,0,244,139,0,0,254,139,0,0,3,140,0,0,
    15,140,0,0,23,140,0,0,28,140,0,0,42,140,0,0,53,140,0,0,64,140,0,0,
    74,140,0,0,87,140,0,0,97,140,0,0,111,140,0,0,122,140,0,0,132,140,0,0,
    139,140,0,0,148,140,0,0,156,140,0,0,162,140,0,0,171,140,0,0,177,140,0,0,
    184,140,0,0,205,140,0,0,216,140,0,0,220,140,0,0,74,0,0,0,84,0,0,0,
    93,0,0,0,98,0,0,0,107,0,0,0,128,0,0,0,129,0,0,0,130,0,0,0,
    131,0,0,0,132,0,0,0,133,0,0,0,134,0,0,0,135,0,0,0,136,0,0,0,
    137,0,0,0,138,0,0,0,139,0,0,0,140,0,0,0,141,0,0,0,142,0,0,0,
    143,0,0,0,144,0,0,0,145,0,0,0,146,0,0,0,147,0,0,0,148,0,0,0,
    149,0,0,0,150,0,0,0,151,0,0,0,152,0,0,0,153,0,0,0,154,0,0,0,
    155,0,0,0,156,0,0,0,157,0,0,0,158,0,0,0,159,0,0,0,160,0,0,0,
    161,0,0,0,162,0,0,0,163,0,0,0,164,0,0,0,165,0,0,0,166,0,0,0,
    167,0,0,0,168,0,0,0,169,0,0,0,170,0,0,0,171,0,0,0,172,0,0,0,
    173,0,0,0,174,0,0,0,175,0,0,0,176,0,0,0,177,0,0,0,178,0,0,0,
    179,0,0,0,180,0,0,0,181,0,0,0,182,0,0,0,183,0,0,0,184,0,0,0,
    185,0,0,0,186,0,0,0,187,0,0,0,188,0,0,0,189,0,0,0,190,0,0,0,
    191,0,0,0,192,0,0,0,193,0,0,0,194,0,0,0,195,0,0,0,196,0,0,0,
    197,0,0,0,198,0,0,0,199,0,0,0,200,0,0,0,201,0,0,0,203,0,0,0,
    204,0,0,0,205,0,0,0,206,0,0,0,207,0,0,0,208,0,0,0,209,0,0,0,
    210,0,0,0,211,0,0,0,212,0,0,0,213,0,0,0,214,0,0,0,215,0,0,0,
    216,0,0,0,217,0,0,0,218,0,0,0,219,0,0,0,220,0,0,0,221,0,0,0,
    222,0,0,0,223,0,0,0,224,0,0,0,225,0,0,0,226,0,0,0,227,0,0,0,
    228,0,0,0,229,0,0,0,230,0,0,0,231,0,0,0,232,0,0,0,233,0,0,0,
    235,0,0,0,236,0,0,0,237,0,0,0,238,0,0,0,239,0,0,0,240,0,0,0,
    241,0,0,0,242,0,0,0,243,0,0,0,244,0,0,0,245,0,0,0,246,0,0,0,
    247,0,0,0,248,0,0,0,249,0,0,0,250,0,0,0,251,0,0,0,252,0,0,0,
    253,0,0,0,254,0,0,0,255,0,0,0,0,1,0,0,31,1,0,0,55,1,0,0,
    67,1,0,0,68,1,0,0,69,1,0,0,70,1,0,0,71,1,0,0,72,1,0,0,
    73,1,0,0,74,1,0,0,75,1,0,0,76,1,0,0,77,1,0,0,78,1,0,0,
    84,0,0,0,1,0,0,0,0,0,0,0,93,0,0,0,2,0,0,0,0,0,0,0,
    98,0,0,0,3,0,0,0,0,0,0,0,99,0,0,0,3,0,0,0,196,92,0,0,
    100,0,0,0,3,0,0,0,204,92,0,0,101,0,0,0,3,0,0,0,212,92,0,0,
    104,0,0,0,3,0,0,0,220,92,0,0,102,0,0,0,3,0,0,0,228,92,0,0,
    102,0,0,0,3,0,0,0,236,92,0,0,102,0,0,0,3,0,0,0,244,92,0,0,
    102,0,0,0,3,0,0,0,252,92,0,0,103,0,0,0,3,0,0,0,4,93,0,0,
    107,0,0,0,4,0,0,0,0,0,0,0,108,0,0,0,5,0,0,0,0,0,0,0,
    118,0,0,0,6,0,0,0,228,92,0,0,108,0,0,0,7,0,0,0,0,0,0,0,
    108,0,0,0,8,0,0,0,0,0,0,0,121,0,0,0,8,0,0,0,16,93,0,0,
    108,0,0,0,9,0,0,0,0,0,0,0,121,0,0,0,9,0,0,0,24,93,0,0,
    108,0,0,0,10,0,0,0,0,0,0,0,108,0,0,0,11,0,0,0,0,0,0,0,
    118,0,0,0,11,0,0,0,228,92,0,0,125,0,0,0,12,0,0,0,32,93,0,0,
    118,0,0,0,12,0,0,0,44,93,0,0,118,0,0,0,14,0,0,0,44,93,0,0,
    118,0,0,0,17,0,0,0,52,93,0,0,118,0,0,0,17,0,0,0,44,93,0,0,
    108,0,0,0,18,0,0,0,0,0,0,0,118,0,0,0,20,0,0,0,60,93,0,0,
    108,0,0,0,21,0,0,0,0,0,0,0,108,0,0,0,22,0,0,0,0,0,0,0,
    109,0,0,0,24,0,0,0,204,92,0,0,108,0,0,0,25,0,0,0,0,0,0,0,
    109,0,0,0,26,0,0,0,204,92,0,0,108,0,0,0,27,0,0,0,0,0,0,0,
    108,0,0,0,28,0,0,0,0,0,0,0,108,0,0,0,29,0,0,0,0,0,0,0,
    118,0,0,0,30,0,0,0,228,92,0,0,121,0,0,0,30,0,0,0,68,93,0,0,
    124,0,0,0,30,0,0,0,76,93,0,0,121,0,0,0,30,0,0,0,84,93,0,0,
    123,0,0,0,31,0,0,0,92,93,0,0,108,0,0,0,33,0,0,0,0,0,0,0,
    108,0,0,0,38,0,0,0,0,0,0,0,108,0,0,0,39,0,0,0,0,0,0,0,
    109,0,0,0,48,0,0,0,204,92,0,0,108,0,0,0,49,0,0,0,0,0,0,0,
    122,0,0,0,50,0,0,0,108,93,0,0,108,0,0,0,51,0,0,0,0,0,0,0,
    108,0,0,0,56,0,0,0,0,0,0,0,108,0,0,0,57,0,0,0,0,0,0,0,
    118,0,0,0,58,0,0,0,120,93,0,0,109,0,0,0,59,0,0,0,204,92,0,0,
    113,0,0,0,62,0,0,0,128,93,0,0,112,0,0,0,63,0,0,0,140,93,0,0,
    110,0,0,0,64,0,0,0,156,93,0,0,109,0,0,0,65,0,0,0,204,92,0,0,
    118,0,0,0,65,0,0,0,168,93,0,0,119,0,0,0,65,0,0,0,176,93,0,0,
    108,0,0,0,66,0,0,0,0,0,0,0,118,0,0,0,69,0,0,0,184,93,0,0,
    108,0,0,0,71,0,0,0,0,0,0,0,108,0,0,0,80,0,0,0,0,0,0,0,
    108,0,0,0,81,0,0,0,0,0,0,0,118,0,0,0,85,0,0,0,192,93,0,0,
    108,0,0,0,86,0,0,0,0,0,0,0,121,0,0,0,86,0,0,0,200,93,0,0,
    118,0,0,0,86,0,0,0,44,93,0,0,109,0,0,0,90,0,0,0,204,92,0,0,
    108,0,0,0,94,0,0,0,0,0,0,0,109,0,0,0,94,0,0,0,204,92,0,0,
    118,0,0,0,94,0,0,0,208,93,0,0,121,0,0,0,94,0,0,0,216,93,0,0,
    118,0,0,0,94,0,0,0,228,92,0,0,108,0,0,0,98,0,0,0,0,0,0,0,
    109,0,0,0,98,0,0,0,204,92,0,0,111,0,0,0,98,0,0,0,212,92,0,0,
    117,0,0,0,98,0,0,0,224,93,0,0,121,0,0,0,98,0,0,0,232,93,0,0,
    121,0,0,0,98,0,0,0,240,93,0,0,118,0,0,0,98,0,0,0,228,92,0,0,
    121,0,0,0,98,0,0,0,68,93,0,0,118,0,0,0,98,0,0,0,248,93,0,0,
    119,0,0,0,98,0,0,0,0,94,0,0,118,0,0,0,98,0,0,0,8,94,0,0,
    109,0,0,0,99,0,0,0,204,92,0,0,118,0,0,0,99,0,0,0,228,92,0,0,
    108,0,0,0,101,0,0,0,0,0,0,0,118,0,0,0,103,0,0,0,16,94,0,0,
    118,0,0,0,103,0,0,0,252,92,0,0,118,0,0,0,109,0,0,0,236,92,0,0,
    108,0,0,0,110,0,0,0,0,0,0,0,108,0,0,0,112,0,0,0,0,0,0,0,
    108,0,0,0,115,0,0,0,0,0,0,0,108,0,0,0,116,0,0,0,0,0,0,0,
    118,0,0,0,116,0,0,0,228,92,0,0,118,0,0,0,117,0,0,0,44,93,0,0,
    120,0,0,0,124,0,0,0,24,94,0,0,31,1,0,0,132,0,0,0,0,0,0,0,
    32,1,0,0,132,0,0,0,204,92,0,0,33,1,0,0,132,0,0,0,212,92,0,0,
    34,1,0,0,132,0,0,0,36,94,0,0,35,1,0,0,132,0,0,0,48,94,0,0,
    36,1,0,0,132,0,0,0,60,94,0,0,36,1,0,0,132,0,0,0,68,94,0,0,
    37,1,0,0,132,0,0,0,224,93,0,0,38,1,0,0,132,0,0,0,76,94,0,0,
    44,1,0,0,132,0,0,0,84,94,0,0,49,1,0,0,132,0,0,0,100,94,0,0,
    49,1,0,0,132,0,0,0,112,94,0,0,46,1,0,0,132,0,0,0,124,94,0,0,
    49,1,0,0,132,0,0,0,132,94,0,0,41,1,0,0,132,0,0,0,144,94,0,0,
    42,1,0,0,132,0,0,0,152,94,0,0,47,1,0,0,132,0,0,0,164,94,0,0,
    39,1,0,0,132,0,0,0,176,94,0,0,39,1,0,0,132,0,0,0,184,94,0,0,
    41,1,0,0,132,0,0,0,192,94,0,0,39,1,0,0,132,0,0,0,200,94,0,0,
    39,1,0,0,132,0,0,0,208,94,0,0,51,1,0,0,132,0,0,0,216,94,0,0,
    52,1,0,0,132,0,0,0,232,94,0,0,39,1,0,0,132,0,0,0,168,93,0,0,
    41,1,0,0,132,0,0,0,176,93,0,0,52,1,0,0,132,0,0,0,240,94,0,0,
    39,1,0,0,132,0,0,0,248,94,0,0,39,1,0,0,132,0,0,0,0,95,0,0,
    39,1,0,0,132,0,0,0,8,95,0,0,39,1,0,0,132,0,0,0,16,95,0,0,
    39,1,0,0,132,0,0,0,24,95,0,0,39,1,0,0,132,0,0,0,32,95,0,0,
    40,1,0,0,132,0,0,0,40,95,0,0,39,1,0,0,132,0,0,0,52,95,0,0,
    46,1,0,0,132,0,0,0,60,95,0,0,39,1,0,0,132,0,0,0,68,95,0,0,
    46,1,0,0,132,0,0,0,76,95,0,0,39,1,0,0,132,0,0,0,84,95,0,0,
    43,1,0,0,132,0,0,0,92,95,0,0,39,1,0,0,132,0,0,0,188,92,0,0,
    46,1,0,0,132,0,0,0,108,95,0,0,39,1,0,0,132,0,0,0,228,92,0,0,
    46,1,0,0,132,0,0,0,68,93,0,0,39,1,0,0,132,0,0,0,116,95,0,0,
    49,1,0,0,132,0,0,0,124,95,0,0,39,1,0,0,132,0,0,0,136,95,0,0,
    39,1,0,0,132,0,0,0,52,93,0,0,39,1,0,0,132,0,0,0,236,92,0,0,
    41,1,0,0,132,0,0,0,0,94,0,0,42,1,0,0,132,0,0,0,144,95,0,0,
    46,1,0,0,132,0,0,0,156,95,0,0,52,1,0,0,132,0,0,0,164,95,0,0,
    46,1,0,0,132,0,0,0,172,95,0,0,46,1,0,0,132,0,0,0,180,95,0,0,
    46,1,0,0,132,0,0,0,188,95,0,0,46,1,0,0,132,0,0,0,196,95,0,0,
    52,1,0,0,132,0,0,0,204,95,0,0,39,1,0,0,132,0,0,0,8,94,0,0,
    46,1,0,0,132,0,0,0,212,95,0,0,48,1,0,0,132,0,0,0,220,95,0,0,
    39,1,0,0,132,0,0,0,244,92,0,0,45,1,0,0,132,0,0,0,236,95,0,0,
    53,1,0,0,132,0,0,0,248,95,0,0,54,1,0,0,132,0,0,0,0,96,0,0,
    39,1,0,0,132,0,0,0,252,92,0,0,42,1,0,0,132,0,0,0,4,93,0,0,
    46,1,0,0,132,0,0,0,8,96,0,0,39,1,0,0,132,0,0,0,16,96,0,0,
    39,1,0,0,132,0,0,0,24,96,0,0,39,1,0,0,132,0,0,0,32,96,0,0,
    50,1,0,0,132,0,0,0,40,96,0,0,41,1,0,0,132,0,0,0,52,96,0,0,
    55,1,0,0,133,0,0,0,0,0,0,0,56,1,0,0,133,0,0,0,204,92,0,0,
    57,1,0,0,133,0,0,0,212,92,0,0,58,1,0,0,133,0,0,0,60,96,0,0,
    59,1,0,0,133,0,0,0,72,96,0,0,59,1,0,0,133,0,0,0,80,96,0,0,
    60,1,0,0,133,0,0,0,224,93,0,0,61,1,0,0,133,0,0,0,88,96,0,0,
    65,1,0,0,133,0,0,0,124,94,0,0,62,1,0,0,133,0,0,0,96,96,0,0,
    62,1,0,0,133,0,0,0,104,96,0,0,63,1,0,0,133,0,0,0,112,96,0,0,
    62,1,0,0,133,0,0,0,120,96,0,0,62,1,0,0,133,0,0,0,128,96,0,0,
    63,1,0,0,133,0,0,0,176,93,0,0,65,1,0,0,133,0,0,0,136,96,0,0,
    63,1,0,0,133,0,0,0,144,96,0,0,62,1,0,0,133,0,0,0,208,93,0,0,
    62,1,0,0,133,0,0,0,188,92,0,0,64,1,0,0,133,0,0,0,152,96,0,0,
    62,1,0,0,133,0,0,0,228,92,0,0,62,1,0,0,133,0,0,0,52,93,0,0,
    62,1,0,0,133,0,0,0,236,92,0,0,63,1,0,0,133,0,0,0,0,94,0,0,
    62,1,0,0,133,0,0,0,44,93,0,0,64,1,0,0,133,0,0,0,160,96,0,0,
    65,1,0,0,133,0,0,0,168,96,0,0,62,1,0,0,133,0,0,0,252,92,0,0,
    108,0,0,0,134,0,0,0,0,0,0,0,109,0,0,0,134,0,0,0,204,92,0,0,
    118,0,0,0,134,0,0,0,176,96,0,0,119,0,0,0,134,0,0,0,184,96,0,0,
    118,0,0,0,134,0,0,0,192,96,0,0,109,0,0,0,135,0,0,0,204,92,0,0,
    118,0,0,0,136,0,0,0,236,92,0,0,118,0,0,0,137,0,0,0,236,92,0,0,
    118,0,0,0,138,0,0,0,236,92,0,0,119,0,0,0,138,0,0,0,200,96,0,0,
    118,0,0,0,139,0,0,0,236,92,0,0,119,0,0,0,139,0,0,0,208,96,0,0,
    108,0,0,0,140,0,0,0,0,0,0,0,118,0,0,0,142,0,0,0,216,96,0,0,
    118,0,0,0,143,0,0,0,228,92,0,0,118,0,0,0,143,0,0,0,236,92,0,0,
    127,0,0,0,143,0,0,0,248,95,0,0,118,0,0,0,145,0,0,0,236,92,0,0,
    34,0,3,0,15,1,0,0,43,0,43,0,71,0,0,0,61,0,61,0,66,0,0,0,
    61,0,61,0,67,0,0,0,61,0,61,0,68,0,0,0,61,0,61,0,69,0,0,0,
    61,0,61,0,70,0,0,0,68,0,3,0,240,1,0,0,68,0,3,0,242,1,0,0,
    68,0,3,0,243,1,0,0,68,0,3,0,244,1,0,0,104,0,104,0,116,0,0,0,
    106,0,105,0,30,1,0,0,118,0,118,0,4,1,0,0,119,0,121,0,142,2,0,0,
    121,0,3,0,73,0,0,0,121,0,3,0,76,0,0,0,121,0,3,0,77,0,0,0,
    121,0,3,0,78,0,0,0,121,0,143,0,79,0,0,0,121,0,3,0,80,0,0,0,
    121,0,3,0,86,0,0,0,121,0,3,0,88,0,0,0,121,0,3,0,89,0,0,0,
    121,0,3,0,97,0,0,0,121,0,3,0,105,0,0,0,121,0,3,0,106,0,0,0,
    121,0,3,0,114,0,0,0,121,0,3,0,115,0,0,0,121,0,3,0,1,1,0,0,
    121,0,3,0,2,1,0,0,121,0,3,0,3,1,0,0,121,0,3,0,7,1,0,0,
    121,0,3,0,11,1,0,0,121,0,3,0,13,1,0,0,121,0,3,0,16,1,0,0,
    121,0,3,0,17,1,0,0,121,0,3,0,20,1,0,0,121,0,3,0,22,1,0,0,
    121,0,3,0,23,1,0,0,121,0,3,0,24,1,0,0,121,0,3,0,26,1,0,0,
    121,0,3,0,27,1,0,0,121,0,133,0,118,1,0,0,121,0,138,0,141,1,0,0,
    121,0,138,0,168,1,0,0,121,0,3,0,170,1,0,0,121,0,145,0,235,1,0,0,
    121,0,3,0,238,1,0,0,121,0,139,0,239,1,0,0,121,0,109,0,241,1,0,0,
    121,0,143,0,0,2,0,0,121,0,138,0,3,2,0,0,121,0,138,0,60,2,0,0,
    121,0,119,0,67,2,0,0,121,0,136,0,72,2,0,0,121,0,137,0,75,2,0,0,
    121,0,138,0,83,2,0,0,121,0,138,0,148,2,0,0,121,0,138,0,151,2,0,0,
    121,0,143,0,163,2,0,0,121,0,138,0,165,2,0,0,122,0,5,0,160,1,0,0,
    122,0,143,0,161,1,0,0,123,0,114,0,154,2,0,0,124,0,133,0,82,1,0,0,
    124,0,14,0,126,1,0,0,124,0,133,0,137,1,0,0,124,0,3,0,157,1,0,0,
    124,0,133,0,162,1,0,0,124,0,12,0,172,1,0,0,124,0,3,0,59,2,0,0,
    124,0,117,0,127,2,0,0,124,0,17,0,130,2,0,0,124,0,86,0,138,2,0,0,
    125,0,3,0,87,0,0,0,125,0,3,0,126,0,0,0,125,0,3,0,5,1,0,0,
    125,0,3,0,6,1,0,0,125,0,3,0,12,1,0,0,125,0,144,0,18,1,0,0,
    125,0,116,0,21,1,0,0,125,0,3,0,29,1,0,0,126,0,127,0,164,2,0,0,
    127,0,138,0,65,0,0,0,127,0,3,0,72,0,0,0,127,0,3,0,81,0,0,0,
    127,0,3,0,82,0,0,0,127,0,3,0,91,0,0,0,127,0,138,0,28,1,0,0,
    127,0,3,0,83,1,0,0,127,0,98,0,134,1,0,0,127,0,3,0,244,1,0,0,
    127,0,133,0,126,2,0,0,128,0,129,0,142,2,0,0,128,0,80,0,153,2,0,0,
    128,0,98,0,155,2,0,0,128,0,3,0,157,2,0,0,128,0,28,0,158,2,0,0,
    128,0,107,0,160,2,0,0,129,0,3,0,75,0,0,0,129,0,3,0,92,0,0,0,
    129,0,3,0,94,0,0,0,129,0,3,0,95,0,0,0,129,0,3,0,96,0,0,0,
    129,0,3,0,8,1,0,0,129,0,3,0,14,1,0,0,129,0,98,0,25,1,0,0,
    129,0,98,0,159,1,0,0,130,0,131,0,142,2,0,0,130,0,37,0,156,2,0,0,
    130,0,4,0,159,2,0,0,131,0,3,0,75,0,0,0,131,0,3,0,94,0,0,0,
    131,0,3,0,9,1,0,0,131,0,4,0,19,1,0,0,131,0,133,0,113,1,0,0,
    131,0,98,0,164,1,0,0,131,0,50,0,254,1,0,0,131,0,98,0,12,2,0,0,
    5,0,7,0,128,1,0,0,5,0,63,0,185,1,0,0,5,0,36,0,188,1,0,0,
    5,0,15,0,196,1,0,0,5,0,74,0,221,1,0,0,5,0,51,0,228,1,0,0,
    5,0,172,0,234,1,0,0,5,0,171,0,80,2,0,0,5,0,139,0,86,2,0,0,
    6,0,99,0,63,0,0,0,7,0,16,0,120,1,0,0,7,0,172,0,158,1,0,0,
    7,0,14,0,165,1,0,0,8,0,17,0,85,1,0,0,8,0,2,0,132,1,0,0,
    9,0,172,0,123,1,0,0,9,0,192,0,127,1,0,0,9,0,28,0,181,1,0,0,
    9,0,94,0,183,1,0,0,9,0,22,0,215,1,0,0,9,0,172,0,251,1,0,0,
    10,0,2,0,207,1,0,0,11,0,23,0,136,1,0,0,11,0,26,0,143,1,0,0,
    11,0,26,0,144,1,0,0,11,0,75,0,179,1,0,0,11,0,20,0,180,1,0,0,
    11,0,2,0,182,1,0,0,11,0,75,0,210,1,0,0,11,0,2,0,222,1,0,0,
    11,0,212,0,226,1,0,0,12,0,99,0,131,1,0,0,12,0,99,0,148,1,0,0,
    12,0,172,0,149,1,0,0,12,0,93,0,217,1,0,0,12,0,173,0,79,2,0,0,
    12,0,182,0,168,2,0,0,13,0,99,0,63,0,0,0,14,0,2,0,214,1,0,0,
    14,0,199,0,121,2,0,0,14,0,100,0,124,2,0,0,15,0,93,0,186,1,0,0,
    15,0,95,0,225,1,0,0,16,0,18,0,178,1,0,0,17,0,99,0,131,1,0,0,
    17,0,99,0,135,1,0,0,17,0,66,0,211,1,0,0,18,0,144,0,134,2,0,0,
    18,0,116,0,137,2,0,0,19,0,99,0,63,0,0,0,20,0,99,0,63,0,0,0,
    20,0,30,0,122,1,0,0,20,0,29,0,116,2,0,0,22,0,75,0,192,1,0,0,
    23,0,21,0,191,1,0,0,23,0,31,0,216,1,0,0,24,0,99,0,63,0,0,0,
    24,0,33,0,122,1,0,0,24,0,32,0,114,2,0,0,26,0,43,0,224,1,0,0,
    27,0,34,0,199,1,0,0,27,0,2,0,200,1,0,0,28,0,65,0,49,2,0,0,
    28,0,67,0,50,2,0,0,28,0,42,0,71,2,0,0,29,0,75,0,212,1,0,0,
    29,0,74,0,221,1,0,0,30,0,141,0,63,0,0,0,30,0,38,0,87,1,0,0,
    30,0,35,0,187,1,0,0,30,0,43,0,190,1,0,0,30,0,39,0,70,2,0,0,
    30,0,40,0,70,2,0,0,30,0,41,0,70,2,0,0,30,0,38,0,120,2,0,0,
    31,0,99,0,131,1,0,0,31,0,76,0,220,1,0,0,31,0,172,0,10,2,0,0,
    32,0,102,0,63,0,0,0,32,0,119,0,63,0,0,0,32,0,101,0,24,2,0,0,
    33,0,75,0,203,1,0,0,36,0,172,0,249,1,0,0,37,0,120,0,63,0,0,0,
    37,0,190,0,62,2,0,0,37,0,191,0,63,2,0,0,39,0,45,0,206,1,0,0,
    39,0,45,0,11,2,0,0,40,0,64,0,194,1,0,0,41,0,146,0,63,0,0,0,
    41,0,95,0,225,1,0,0,42,0,12,0,152,2,0,0,44,0,189,0,156,1,0,0,
    45,0,99,0,37,2,0,0,45,0,122,0,38,2,0,0,46,0,99,0,43,2,0,0,
    46,0,137,0,44,2,0,0,46,0,169,0,45,2,0,0,47,0,99,0,63,0,0,0,
    48,0,141,0,63,0,0,0,48,0,47,0,122,1,0,0,48,0,46,0,103,2,0,0,
    48,0,46,0,110,2,0,0,50,0,49,0,197,1,0,0,51,0,2,0,219,1,0,0,
    52,0,48,0,64,2,0,0,53,0,2,0,176,1,0,0,53,0,2,0,202,1,0,0,
    53,0,2,0,223,1,0,0,54,0,2,0,176,1,0,0,54,0,1,0,230,1,0,0,
    54,0,1,0,231,1,0,0,55,0,117,0,63,0,0,0,55,0,185,0,150,1,0,0,
    55,0,52,0,27,2,0,0,55,0,177,0,35,2,0,0,56,0,187,0,81,2,0,0,
    57,0,100,0,117,2,0,0,59,0,53,0,23,2,0,0,59,0,137,0,96,2,0,0,
    59,0,100,0,101,2,0,0,59,0,137,0,109,2,0,0,59,0,124,0,118,2,0,0,
    60,0,172,0,251,1,0,0,62,0,54,0,23,2,0,0,63,0,55,0,23,2,0,0,
    64,0,56,0,23,2,0,0,65,0,127,0,86,1,0,0,65,0,124,0,88,1,0,0,
    65,0,58,0,23,2,0,0,65,0,59,0,23,2,0,0,65,0,162,0,91,2,0,0,
    65,0,119,0,92,2,0,0,65,0,119,0,93,2,0,0,65,0,162,0,94,2,0,0,
    65,0,162,0,95,2,0,0,65,0,137,0,96,2,0,0,65,0,162,0,97,2,0,0,
    65,0,128,0,98,2,0,0,65,0,129,0,99,2,0,0,65,0,137,0,102,2,0,0,
    65,0,162,0,104,2,0,0,65,0,162,0,105,2,0,0,65,0,162,0,106,2,0,0,
    65,0,137,0,109,2,0,0,65,0,123,0,111,2,0,0,65,0,124,0,111,2,0,0,
    65,0,162,0,112,2,0,0,65,0,130,0,113,2,0,0,65,0,162,0,115,2,0,0,
    65,0,137,0,119,2,0,0,65,0,162,0,122,2,0,0,66,0,99,0,63,0,0,0,
    67,0,125,0,63,0,0,0,70,0,183,0,236,1,0,0,70,0,123,0,82,2,0,0,
    70,0,186,0,125,2,0,0,71,0,81,0,209,1,0,0,71,0,62,0,218,1,0,0,
    79,0,99,0,63,0,0,0,79,0,200,0,144,2,0,0,79,0,100,0,167,2,0,0,
    79,0,164,0,167,2,0,0,79,0,165,0,167,2,0,0,80,0,134,0,63,0,0,0,
    80,0,141,0,63,0,0,0,80,0,75,0,174,1,0,0,80,0,75,0,210,1,0,0,
    80,0,172,0,9,2,0,0,82,0,141,0,63,0,0,0,83,0,133,0,63,0,0,0,
    83,0,135,0,63,0,0,0,84,0,141,0,63,0,0,0,85,0,99,0,131,1,0,0,
    85,0,10,0,73,2,0,0,85,0,11,0,73,2,0,0,86,0,99,0,131,1,0,0,
    86,0,99,0,169,1,0,0,86,0,164,0,167,2,0,0,86,0,165,0,167,2,0,0,
    87,0,75,0,147,2,0,0,88,0,205,0,145,2,0,0,89,0,75,0,208,1,0,0,
    90,0,2,0,246,1,0,0,90,0,69,0,162,2,0,0,92,0,78,0,147,2,0,0,
    93,0,5,0,5,2,0,0,93,0,5,0,8,2,0,0,93,0,3,0,84,2,0,0,
    94,0,99,0,63,0,0,0,98,0,166,0,63,0,0,0,98,0,167,0,63,0,0,0,
    98,0,192,0,154,1,0,0,98,0,189,0,156,1,0,0,98,0,204,0,184,1,0,0,
    98,0,172,0,250,1,0,0,98,0,4,0,2,2,0,0,98,0,2,0,4,2,0,0,
    98,0,5,0,25,2,0,0,98,0,80,0,77,2,0,0,98,0,214,0,131,2,0,0,
    98,0,192,0,135,2,0,0,98,0,76,0,139,2,0,0,98,0,77,0,139,2,0,0,
    98,0,75,0,146,2,0,0,99,0,99,0,63,0,0,0,99,0,86,0,114,1,0,0,
    99,0,87,0,114,1,0,0,99,0,75,0,147,2,0,0,100,0,138,0,117,1,0,0,
    100,0,12,0,146,1,0,0,100,0,12,0,14,2,0,0,101,0,140,0,63,0,0,0,
    101,0,88,0,145,1,0,0,101,0,99,0,247,1,0,0,101,0,106,0,129,2,0,0,
    101,0,99,0,132,2,0,0,102,0,143,0,89,1,0,0,103,0,90,0,173,1,0,0,
    103,0,0,0,193,1,0,0,103,0,1,0,195,1,0,0,103,0,2,0,198,1,0,0,
    103,0,12,0,205,1,0,0,103,0,89,0,51,2,0,0,103,0,90,0,166,2,0,0,
    107,0,99,0,63,0,0,0,107,0,189,0,85,1,0,0,107,0,71,0,173,1,0,0,
    107,0,172,0,250,1,0,0,107,0,2,0,128,2,0,0,107,0,213,0,143,2,0,0,
    108,0,209,0,140,1,0,0,108,0,211,0,140,1,0,0,109,0,99,0,63,0,0,0,
    109,0,99,0,130,1,0,0,109,0,189,0,138,1,0,0,109,0,72,0,173,1,0,0,
    109,0,73,0,68,2,0,0,109,0,145,0,69,2,0,0,110,0,172,0,233,1,0,0,
    110,0,70,0,22,2,0,0,111,0,99,0,63,0,0,0,112,0,189,0,85,1,0,0,
    112,0,92,0,253,1,0,0,113,0,70,0,201,1,0,0,113,0,70,0,227,1,0,0,
    114,0,189,0,138,1,0,0,114,0,94,0,155,1,0,0,114,0,72,0,173,1,0,0,
    114,0,73,0,68,2,0,0,115,0,92,0,253,1,0,0,116,0,189,0,156,1,0,0,
    116,0,96,0,171,1,0,0,117,0,100,0,63,0,0,0,117,0,2,0,152,1,0,0,
    117,0,99,0,76,2,0,0,117,0,179,0,149,2,0,0,119,0,147,0,63,0,0,0,
    119,0,150,0,63,0,0,0,119,0,57,0,142,1,0,0,119,0,175,0,56,2,0,0,
    121,0,194,0,6,0,0,0,121,0,208,0,10,0,0,0,121,0,208,0,12,0,0,0,
    121,0,8,0,13,0,0,0,121,0,217,0,15,0,0,0,121,0,210,0,16,0,0,0,
    121,0,91,0,17,0,0,0,121,0,215,0,18,0,0,0,121,0,208,0,19,0,0,0,
    121,0,206,0,20,0,0,0,121,0,207,0,21,0,0,0,121,0,208,0,22,0,0,0,
    121,0,208,0,26,0,0,0,121,0,208,0,27,0,0,0,121,0,215,0,28,0,0,0,
    121,0,208,0,29,0,0,0,121,0,151,0,31,0,0,0,121,0,148,0,35,0,0,0,
    121,0,84,0,41,0,0,0,121,0,195,0,43,0,0,0,121,0,195,0,45,0,0,0,
    121,0,149,0,46,0,0,0,121,0,107,0,50,0,0,0,121,0,99,0,51,0,0,0,
    121,0,99,0,62,0,0,0,121,0,117,0,63,0,0,0,121,0,164,0,110,1,0,0,
    121,0,137,0,111,1,0,0,121,0,76,0,129,1,0,0,121,0,185,0,150,1,0,0,
    121,0,172,0,153,1,0,0,121,0,60,0,175,1,0,0,121,0,37,0,189,1,0,0,
    121,0,168,0,204,1,0,0,121,0,50,0,213,1,0,0,121,0,173,0,237,1,0,0,
    121,0,99,0,248,1,0,0,121,0,107,0,15,2,0,0,121,0,99,0,16,2,0,0,
    121,0,126,0,33,2,0,0,121,0,131,0,34,2,0,0,121,0,176,0,55,2,0,0,
    121,0,173,0,61,2,0,0,121,0,126,0,88,2,0,0,121,0,101,0,89,2,0,0,
    121,0,100,0,108,2,0,0,121,0,162,0,123,2,0,0,121,0,83,0,140,2,0,0,
    121,0,164,0,150,2,0,0,122,0,111,0,63,0,0,0,122,0,99,0,85,2,0,0,
    123,0,145,0,63,0,0,0,123,0,104,0,40,2,0,0,124,0,25,0,7,0,0,0,
    124,0,196,0,9,0,0,0,124,0,196,0,11,0,0,0,124,0,24,0,14,0,0,0,
    124,0,97,0,23,0,0,0,124,0,27,0,24,0,0,0,124,0,68,0,25,0,0,0,
    124,0,156,0,30,0,0,0,124,0,153,0,32,0,0,0,124,0,156,0,34,0,0,0,
    124,0,152,0,36,0,0,0,124,0,154,0,37,0,0,0,124,0,155,0,38,0,0,0,
    124,0,197,0,40,0,0,0,124,0,99,0,63,0,0,0,124,0,178,0,119,1,0,0,
    124,0,99,0,131,1,0,0,124,0,115,0,28,2,0,0,124,0,114,0,30,2,0,0,
    124,0,114,0,39,2,0,0,124,0,113,0,41,2,0,0,124,0,10,0,167,2,0,0,
    124,0,10,0,169,2,0,0,125,0,106,0,55,0,0,0,125,0,99,0,62,0,0,0,
    125,0,99,0,63,0,0,0,125,0,19,0,84,1,0,0,125,0,201,0,163,1,0,0,
    125,0,180,0,232,1,0,0,125,0,111,0,1,2,0,0,125,0,216,0,21,2,0,0,
    125,0,98,0,46,2,0,0,125,0,112,0,47,2,0,0,125,0,109,0,48,2,0,0,
    125,0,202,0,53,2,0,0,125,0,106,0,54,2,0,0,125,0,6,0,58,2,0,0,
    125,0,181,0,65,2,0,0,125,0,193,0,66,2,0,0,125,0,132,0,74,2,0,0,
    125,0,203,0,87,2,0,0,126,0,157,0,63,0,0,0,126,0,188,0,133,1,0,0,
    126,0,174,0,147,1,0,0,126,0,172,0,167,1,0,0,126,0,173,0,57,2,0,0,
    126,0,184,0,90,2,0,0,126,0,188,0,100,2,0,0,127,0,85,0,8,0,0,0,
    127,0,158,0,33,0,0,0,127,0,157,0,42,0,0,0,127,0,198,0,44,0,0,0,
    127,0,100,0,52,0,0,0,127,0,105,0,54,0,0,0,127,0,99,0,62,0,0,0,
    127,0,117,0,63,0,0,0,127,0,163,0,116,1,0,0,127,0,99,0,166,1,0,0,
    127,0,37,0,189,1,0,0,127,0,44,0,229,1,0,0,127,0,184,0,255,1,0,0,
    127,0,99,0,17,2,0,0,127,0,100,0,18,2,0,0,127,0,105,0,20,2,0,0,
    127,0,52,0,27,2,0,0,127,0,172,0,29,2,0,0,127,0,61,0,31,2,0,0,
    127,0,177,0,35,2,0,0,127,0,172,0,78,2,0,0,127,0,162,0,106,2,0,0,
    127,0,162,0,107,2,0,0,128,0,159,0,63,0,0,0,128,0,99,0,85,2,0,0,
    129,0,136,0,48,0,0,0,129,0,79,0,49,0,0,0,129,0,105,0,53,0,0,0,
    129,0,99,0,63,0,0,0,129,0,105,0,112,1,0,0,129,0,136,0,139,1,0,0,
    129,0,13,0,177,1,0,0,129,0,82,0,6,2,0,0,129,0,214,0,7,2,0,0,
    129,0,79,0,12,2,0,0,129,0,105,0,19,2,0,0,129,0,103,0,26,2,0,0,
    129,0,108,0,132,2,0,0,129,0,118,0,133,2,0,0,130,0,161,0,63,0,0,0,
    130,0,99,0,85,2,0,0,131,0,160,0,39,0,0,0,131,0,9,0,47,0,0,0,
    131,0,142,0,63,0,0,0,131,0,99,0,112,1,0,0,131,0,192,0,252,1,0,0,
    131,0,99,0,32,2,0,0,131,0,121,0,36,2,0,0,131,0,170,0,42,2,0,0,
    131,0,110,0,132,2,0,0,131,0,2,0,136,2,0,0,119,0,0,0,16,0,0,0,
    66,0,0,0,0,0,0,0,255,255,255,255,80,147,0,0,93,142,0,0,0,0,0,0,
    120,0,0,0,17,16,0,0,94,0,0,0,0,0,0,0,85,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,121,0,0,0,17,0,0,0,55,0,0,0,0,0,0,0,
    255,255,255,255,104,147,0,0,122,142,0,0,80,146,0,0,122,0,0,0,17,16,0,0,
    94,0,0,0,188,92,0,0,85,0,0,0,0,0,0,0,150,143,0,0,0,0,0,0,
    123,0,0,0,0,0,0,0,19,0,0,0,0,0,0,0,255,255,255,255,128,147,0,0,
    172,143,0,0,0,0,0,0,124,0,0,0,17,0,0,0,13,0,0,0,0,0,0,0,
    255,255,255,255,144,147,0,0,191,143,0,0,0,0,0,0,125,0,0,0,17,0,0,0,
    94,0,0,0,0,0,0,0,255,255,255,255,160,147,0,0,80,144,0,0,136,146,0,0,
    126,0,0,0,16,0,0,0,67,0,0,0,0,0,0,0,255,255,255,255,176,147,0,0,
    197,144,0,0,0,0,0,0,127,0,0,0,17,0,0,0,55,0,0,0,0,0,0,0,
    255,255,255,255,192,147,0,0,242,144,0,0,151,146,0,0,128,0,0,0,0,0,0,0,
    94,0,0,0,188,92,0,0,255,255,255,255,208,147,0,0,109,145,0,0,0,0,0,0,
    129,0,0,0,17,0,0,0,6,0,0,0,0,0,0,0,255,255,255,255,232,147,0,0,
    145,145,0,0,161,146,0,0,130,0,0,0,0,0,0,0,94,0,0,0,188,92,0,0,
    255,255,255,255,0,148,0,0,233,145,0,0,0,0,0,0,131,0,0,0,16,0,0,0,
    47,0,0,0,0,0,0,0,255,255,255,255,0,0,0,0,4,146,0,0,179,146,0,0,
    31,0,2,0,6,0,0,0,0,0,0,0,46,3,0,0,8,0,29,0,2,1,30,0,
    84,2,14,0,113,16,5,1,2,0,10,2,18,19,57,2,10,0,84,2,14,0,113,32,
    21,1,50,0,113,0,28,1,0,0,84,2,14,0,18,245,51,81,44,0,113,16,129,0,
    2,0,12,1,84,2,14,0,110,32,45,1,18,0,18,4,84,2,14,0,113,16,17,1,
    2,0,12,2,33,34,53,36,24,0,84,2,14,0,113,16,20,1,2,0,12,3,84,5,
    14,0,113,16,17,1,5,0,12,5,68,5,5,4,68,3,3,5,110,48,128,0,33,3,
    216,4,4,1,40,226,17,1,113,16,11,1,2,0,12,2,113,16,184,0,1,0,12,5,
    110,32,236,0,82,0,12,2,31,2,90,0,57,2,4,0,18,0,17,0,110,16,183,0,
    2,0,10,2,84,5,14,0,113,16,16,1,5,0,12,5,68,5,5,2,84,6,14,0,
    113,16,7,1,6,0,12,6,68,6,6,2,84,7,14,0,113,16,12,1,7,0,12,7,
    70,7,7,2,221,8,6,32,56,8,5,0,26,9,0,0,40,9,84,9,14,0,113,16,
    19,1,9,0,12,9,70,9,9,2,84,10,14,0,113,32,130,0,26,0,12,10,84,11,
    14,0,110,16,37,1,11,0,12,11,110,16,65,0,11,0,12,11,110,32,144,0,186,0,
    84,11,14,0,113,32,23,1,43,0,12,11,110,32,136,0,186,0,84,11,14,0,113,16,
    18,1,11,0,12,11,68,11,11,2,84,12,14,0,59,11,6,0,110,32,145,0,202,0,
    40,10,113,16,20,1,12,0,12,13,68,13,13,11,110,48,146,0,202,13,18,12,84,13,
    14,0,113,16,9,1,13,0,12,13,70,13,13,2,33,221,84,14,14,0,53,220,24,0,
    113,16,20,1,14,0,12,13,84,15,14,0,113,16,9,1,15,0,12,15,70,15,15,2,
    68,15,15,12,68,13,13,15,110,48,128,0,234,13,216,12,12,1,40,222,18,44,35,205,
    138,0,110,32,38,1,222,0,34,14,32,0,84,15,14,0,113,16,15,1,15,0,12,15,
    218,16,2,4,68,15,15,16,113,16,188,0,15,0,10,15,2,17,12,0,84,12,14,0,
    113,16,15,1,12,0,12,12,216,18,16,1,68,12,12,18,113,16,188,0,12,0,10,12,
    19,19,0,0,84,4,14,0,113,16,15,1,4,0,12,4,68,4,4,16,2,20,3,0,
    84,3,14,0,113,16,15,1,3,0,12,3,216,21,16,2,68,3,3,21,198,52,113,16,
    188,0,4,0,10,3,84,4,14,0,113,16,15,1,4,0,12,4,68,4,4,18,2,18,
    4,0,84,4,14,0,113,16,15,1,4,0,12,4,8,21,4,0,18,52,216,16,16,3,
    68,16,21,16,166,16,18,16,119,1,188,0,16,0,10,4,112,84,78,0,254,60,34,3,
    32,0,112,32,79,0,227,0,58,11,35,0,84,4,14,0,113,16,15,1,4,0,12,4,
    218,12,11,4,68,4,4,12,113,16,188,0,4,0,10,4,123,68,84,15,14,0,113,16,
    15,1,15,0,12,15,216,12,12,1,68,12,15,12,113,16,188,0,12,0,10,12,123,204,
    110,48,80,0,67,12,110,32,132,0,58,0,68,3,13,19,68,4,13,20,110,48,80,0,
    62,4,110,32,133,0,234,0,2,3,20,0,110,32,151,0,58,0,221,4,6,8,57,4,
    4,0,1,60,40,3,2,12,19,0,110,32,142,0,202,0,84,12,14,0,113,32,24,1,
    44,0,10,12,183,60,110,32,143,0,202,0,221,3,6,2,56,3,4,0,18,28,40,3,
    2,12,19,0,110,32,149,0,202,0,84,12,14,0,113,16,6,1,12,0,12,12,218,13,
    2,2,68,12,12,13,2,14,19,0,113,32,186,0,236,0,10,12,84,15,14,0,113,16,
    6,1,15,0,12,15,216,16,13,1,68,15,15,16,113,32,186,0,239,0,10,15,19,14,
    13,0,2,22,3,0,19,3,12,0,50,53,37,0,51,229,3,0,40,33,19,14,14,0,
    50,229,23,0,19,14,15,0,51,229,3,0,40,17,19,14,16,0,51,229,11,0,18,14,
    113,48,124,0,252,14,12,12,110,32,138,0,202,0,40,2,18,14,18,31,40,17,18,14,
    18,31,113,48,124,0,207,14,12,12,40,7,18,14,18,31,113,48,124,0,252,14,12,12,
    110,32,138,0,202,0,84,12,14,0,113,16,13,1,12,0,12,12,68,12,12,13,177,252,
    113,32,186,0,236,0,10,23,84,12,14,0,113,16,13,1,12,0,12,12,68,12,12,16,
    177,252,113,32,186,0,236,0,10,25,56,22,5,0,19,28,1,0,40,3,2,28,14,0,
    59,11,4,0,1,236,40,9,84,12,14,0,113,16,16,1,12,0,12,12,68,12,12,11,
    19,13,19,0,19,15,18,0,50,245,73,0,51,213,3,0,40,69,19,13,17,0,51,213,
    31,0,84,13,14,0,113,32,24,1,45,0,10,13,57,13,23,0,19,13,16,0,51,220,
    19,0,84,3,14,0,113,16,6,1,3,0,12,3,218,11,11,2,18,28,176,203,68,3,
    3,11,113,32,186,0,195,0,10,26,40,29,50,60,26,0,19,3,13,0,51,60,3,0,
    40,20,19,3,14,0,50,60,6,0,19,3,15,0,51,60,40,0,19,26,1,0,19,27,
    0,0,2,25,23,0,19,23,0,0,19,24,1,0,40,21,19,26,1,0,19,27,0,0,
    19,24,1,0,19,25,0,0,40,12,51,213,5,0,19,27,1,0,40,3,2,27,14,0,
    19,24,1,0,19,26,1,0,119,6,125,0,23,0,12,3,110,32,139,0,58,0,56,5,
    47,0,51,245,11,0,84,3,14,0,113,32,24,1,35,0,10,3,57,3,3,0,40,35,
    110,16,195,0,9,0,10,3,56,3,6,0,110,32,140,0,122,0,40,28,34,3,99,0,
    112,16,205,0,3,0,110,32,207,0,115,0,26,7,4,0,110,32,207,0,115,0,110,32,
    207,0,147,0,110,16,208,0,3,0,12,3,110,32,140,0,58,0,40,4,110,32,150,0,
    122,0,18,67,51,53,17,0,110,32,150,0,154,0,18,31,110,32,141,0,250,0,56,8,
    4,0,18,19,40,2,1,227,110,32,147,0,58,0,2,3,17,0,50,53,5,0,18,51,
    51,53,14,0,18,31,110,32,134,0,250,0,221,3,6,1,56,3,3,0,18,30,110,32,
    135,0,234,0,84,3,14,0,18,86,51,101,58,0,113,16,14,1,3,0,12,3,19,18,
    3,0,218,2,2,3,69,5,3,2,140,83,84,5,14,0,113,16,14,1,5,0,12,5,
    216,6,2,1,69,6,5,6,140,101,84,6,14,0,113,16,14,1,6,0,12,6,19,17,
    2,0,216,2,2,2,69,7,6,2,140,114,18,31,113,64,126,0,63,37,12,2,110,32,
    148,0,42,0,57,4,31,0,98,2,6,0,110,32,127,0,42,0,98,2,5,0,110,32,
    127,0,42,0,40,19,113,32,25,1,35,0,10,2,57,2,14,0,57,4,12,0,18,31,
    110,32,137,0,250,0,98,2,4,0,110,32,127,0,42,0,40,2,18,31,84,0,14,0,
    113,16,8,1,0,0,10,0,51,16,11,0,110,32,131,0,250,0,98,0,3,0,110,32,
    127,0,10,0,17,10,98,0,2,0,40,250,9,0,4,0,3,0,0,0,0,0,0,0,
    212,0,0,0,84,80,14,0,18,241,51,22,7,0,110,48,46,1,112,8,10,5,15,5,
    113,16,11,1,0,0,12,8,113,16,184,0,6,0,12,0,110,32,236,0,8,0,12,8,
    31,8,90,0,18,0,57,8,3,0,15,0,110,16,183,0,8,0,10,8,84,82,14,0,
    113,16,7,1,2,0,12,2,68,2,2,8,221,2,2,8,18,19,57,2,4,0,1,50,
    40,2,1,2,19,4,64,0,51,71,30,0,84,87,14,0,113,16,8,1,7,0,10,7,
    51,103,3,0,15,0,84,87,14,0,113,32,22,1,103,0,84,87,14,0,110,16,41,1,
    7,0,84,85,14,0,20,7,0,128,0,0,113,48,26,1,101,7,15,3,19,4,128,0,
    51,71,29,0,84,87,14,0,113,16,8,1,7,0,10,7,50,103,3,0,15,0,84,87,
    14,0,113,32,22,1,23,0,84,87,14,0,110,16,41,1,7,0,84,85,14,0,21,7,
    1,0,113,48,26,1,101,7,15,3,19,1,16,0,18,84,51,23,39,0,56,2,37,0,
    84,81,14,0,113,32,25,1,129,0,10,1,57,1,29,0,84,81,14,0,113,16,16,1,
    1,0,12,1,68,1,1,8,50,65,19,0,84,87,14,0,113,16,10,1,7,0,12,7,
    69,1,7,8,113,48,27,1,33,0,84,85,14,0,113,48,26,1,101,3,15,3,84,86,
    14,0,113,16,16,1,6,0,12,6,68,6,6,8,51,70,21,0,56,2,19,0,19,6,
    0,16,51,103,15,0,84,85,14,0,113,16,10,1,5,0,12,5,69,6,5,8,18,37,
    113,48,27,1,118,5,15,3,84,86,14,0,113,16,16,1,6,0,12,6,68,6,6,8,
    51,70,21,0,56,2,19,0,19,6,0,32,51,103,15,0,84,85,14,0,113,16,10,1,
    5,0,12,5,69,6,5,8,18,53,113,48,27,1,118,5,15,3,15,0,2,0,2,0,
    1,0,0,0,0,0,0,0,6,0,0,0,91,1,14,0,112,16,152,0,0,0,14,0,
    3,0,3,0,2,0,0,0,0,0,0,0,4,0,0,0,112,32,1,1,16,0,14,0,
    1,0,1,0,0,0,0,0,0,0,0,0,3,0,0,0,84,0,54,0,17,0,0,0,
    1,0,1,0,0,0,0,0,0,0,0,0,3,0,0,0,85,0,43,0,15,0,0,0,
    2,0,2,0,2,0,0,0,0,0,0,0,5,0,0,0,112,32,40,1,16,0,10,0,
    15,0,0,0,2,0,2,0,2,0,0,0,0,0,0,0,5,0,0,0,112,32,47,1,
    16,0,10,0,15,0,0,0,11,0,2,0,3,0,0,0,0,0,0,0,119,0,0,0,
    112,16,35,1,9,0,10,0,57,0,7,0,111,32,113,0,169,0,10,9,15,9,110,16,
    109,0,10,0,10,0,19,1,10,0,18,242,50,16,78,0,110,16,110,0,10,0,10,0,
    110,16,111,0,10,0,10,10,18,1,1,35,84,148,61,0,33,69,53,81,58,0,112,32,
    40,1,25,0,10,4,57,4,49,0,84,148,56,0,218,5,1,4,68,6,4,5,45,7,
    0,6,58,7,39,0,216,7,5,1,68,7,4,7,45,8,10,7,58,8,31,0,216,8,
    5,2,68,8,4,8,198,134,46,6,0,6,59,6,22,0,216,5,5,3,68,4,4,5,
    198,71,46,4,10,7,59,4,13,0,58,3,10,0,84,148,52,0,68,5,4,1,68,4,
    4,3,52,69,3,0,1,19,216,1,1,1,40,196,58,3,5,0,68,10,4,3,40,2,
    1,42,82,144,48,0,50,10,18,0,89,154,48,0,50,32,7,0,19,1,0,1,112,48,
    49,1,9,1,50,42,7,0,19,0,128,0,112,48,49,1,169,0,18,25,15,9,0,0,
    2,0,1,0,2,0,0,0,0,0,0,0,24,0,0,0,110,16,37,1,1,0,12,1,
    26,0,81,1,110,32,66,0,1,0,12,1,31,1,60,0,56,1,10,0,110,16,123,0,
    1,0,10,1,56,1,4,0,18,17,15,1,18,1,15,1,4,0,2,0,0,0,0,0,
    0,0,0,0,23,0,0,0,84,32,57,0,68,0,0,3,19,1,11,0,52,16,15,0,
    19,1,18,0,54,16,11,0,84,34,47,0,70,2,2,3,33,34,61,2,4,0,18,18,
    15,2,18,2,15,2,0,0,4,0,2,0,2,0,0,0,0,0,0,0,33,0,0,0,
    84,32,57,0,68,0,0,3,56,0,27,0,19,1,10,0,50,16,23,0,19,1,11,0,
    52,16,10,0,19,1,18,0,54,16,6,0,19,1,17,0,51,16,11,0,112,32,40,1,
    50,0,10,2,56,2,3,0,40,3,18,2,15,2,18,18,15,2,0,0,1,0,1,0,
    0,0,0,0,0,0,0,0,3,0,0,0,84,0,55,0,17,0,0,0,1,0,1,0,
    0,0,0,0,0,0,0,0,3,0,0,0,84,0,56,0,17,0,0,0,1,0,1,0,
    0,0,0,0,0,0,0,0,3,0,0,0,82,0,46,0,15,0,0,0,1,0,1,0,
    0,0,0,0,0,0,0,0,3,0,0,0,84,0,44,0,17,0,0,0,1,0,1,0,
    0,0,0,0,0,0,0,0,3,0,0,0,84,0,45,0,17,0,0,0,1,0,1,0,
    0,0,0,0,0,0,0,0,3,0,0,0,84,0,53,0,17,0,0,0,1,0,1,0,
    0,0,0,0,0,0,0,0,3,0,0,0,84,0,57,0,17,0,0,0,1,0,1,0,
    0,0,0,0,0,0,0,0,3,0,0,0,84,0,58,0,17,0,0,0,1,0,1,0,
    0,0,0,0,0,0,0,0,3,0,0,0,84,0,59,0,17,0,0,0,1,0,1,0,
    0,0,0,0,0,0,0,0,3,0,0,0,84,0,61,0,17,0,0,0,1,0,1,0,
    0,0,0,0,0,0,0,0,3,0,0,0,84,0,47,0,17,0,0,0,2,0,2,0,
    2,0,0,0,0,0,0,0,5,0,0,0,112,32,33,1,16,0,12,0,17,0,0,0,
    4,0,2,0,2,0,0,0,0,0,0,0,34,0,0,0,84,32,57,0,68,0,0,3,
    19,1,17,0,50,16,6,0,19,1,18,0,51,16,15,0,112,32,40,1,50,0,10,0,
    56,0,9,0,98,2,19,0,19,3,11,0,70,2,2,3,17,2,98,0,19,0,84,34,
    57,0,68,2,2,3,70,2,0,2,17,2,3,0,1,0,3,0,0,0,0,0,0,0,
    17,0,0,0,110,16,221,0,2,0,10,0,35,0,134,0,110,32,218,0,2,0,34,2,
    98,0,98,1,12,0,112,48,190,0,2,1,17,2,0,0,1,0,1,0,0,0,0,0,
    0,0,0,0,3,0,0,0,84,0,51,0,17,0,0,0,1,0,1,0,0,0,0,0,
    0,0,0,0,3,0,0,0,84,0,60,0,17,0,0,0,1,0,1,0,0,0,0,0,
    0,0,0,0,3,0,0,0,84,0,50,0,17,0,0,0,1,0,1,0,0,0,0,0,
    0,0,0,0,3,0,0,0,84,0,49,0,17,0,0,0,2,0,2,0,0,0,0,0,
    0,0,0,0,3,0,0,0,92,1,43,0,14,0,0,0,2,0,2,0,0,0,0,0,
    0,0,0,0,3,0,0,0,89,1,46,0,14,0,0,0,3,0,3,0,3,0,0,0,
    0,0,0,0,4,0,0,0,112,48,49,1,16,2,14,0,3,0,3,0,3,0,0,0,
    0,0,0,0,4,0,0,0,113,48,42,1,16,2,14,0,0,0,0,0,0,0,0,0,
    0,0,0,0,4,0,0,0,113,0,43,1,0,0,14,0,21,0,0,0,20,0,0,0,
    0,0,0,0,47,0,0,0,26,19,109,1,26,20,100,1,26,1,109,1,26,2,100,1,
    26,3,101,1,26,4,106,1,26,5,102,1,26,6,107,1,26,7,100,1,26,8,100,1,
    26,9,100,1,26,10,108,1,26,11,104,1,26,12,99,1,26,13,105,1,26,14,105,1,
    26,15,105,1,26,16,105,1,26,17,103,1,26,18,100,1,37,20,143,0,1,0,12,0,
    105,0,19,0,14,0,0,0,3,0,2,0,3,0,0,0,0,0,0,0,88,0,0,0,
    112,32,112,0,33,0,18,2,35,32,139,0,91,16,49,0,35,32,138,0,91,16,61,0,
    35,32,138,0,91,16,57,0,35,32,138,0,91,16,45,0,35,32,138,0,91,16,52,0,
    35,32,137,0,91,16,56,0,35,32,136,0,91,16,55,0,35,32,143,0,91,16,51,0,
    35,32,143,0,91,16,60,0,35,32,138,0,91,16,59,0,35,32,138,0,91,16,53,0,
    35,32,138,0,91,16,44,0,35,32,145,0,91,16,47,0,35,34,138,0,91,18,58,0,
    34,2,109,0,112,16,233,0,2,0,91,18,50,0,18,242,89,18,46,0,89,18,48,0,
    34,2,119,0,18,0,112,48,2,1,18,0,91,18,54,0,18,18,110,32,50,1,33,0,
    110,32,51,1,33,0,14,0,4,0,2,0,3,0,0,0,0,0,0,0,11,0,0,0,
    34,0,98,0,98,1,12,0,112,48,190,0,48,1,110,32,32,1,2,0,14,0,0,0,
    3,0,2,0,3,0,0,0,0,0,0,0,10,0,0,0,110,16,39,1,1,0,12,0,
    56,0,5,0,114,48,116,0,16,2,14,0,5,0,3,0,3,0,0,0,0,0,0,0,
    64,0,0,0,112,16,35,1,2,0,10,0,56,0,59,0,84,32,50,0,113,16,184,0,
    3,0,12,1,110,32,235,0,16,0,10,0,57,0,3,0,40,45,113,16,118,0,4,0,
    12,4,110,16,37,1,2,0,12,0,110,16,65,0,0,0,12,0,110,32,121,0,4,0,
    84,32,50,0,113,16,184,0,3,0,12,1,110,32,236,0,16,0,12,0,31,0,90,0,
    110,16,183,0,0,0,10,0,112,32,33,1,2,0,12,0,110,32,119,0,4,0,110,48,
    122,0,36,3,112,32,48,1,66,0,14,0,48,0,2,0,3,0,0,0,0,0,0,0,
    205,1,0,0,8,0,46,0,119,1,224,0,47,0,12,1,98,2,11,0,110,32,223,0,
    33,0,12,1,110,16,221,0,1,0,10,2,35,35,139,0,35,36,138,0,35,37,138,0,
    35,38,138,0,35,39,138,0,218,8,2,4,35,136,137,0,218,9,2,3,35,153,136,0,
    35,42,143,0,35,43,143,0,35,44,138,0,218,13,2,2,35,222,138,0,35,221,138,0,
    35,47,138,0,8,47,1,0,34,1,109,0,112,16,233,0,1,0,19,16,0,0,8,18,
    13,0,8,19,14,0,8,17,15,0,2,13,16,0,1,223,53,47,208,0,116,1,222,0,
    47,0,11,20,19,22,255,255,116,1,221,0,47,0,10,14,116,1,221,0,47,0,10,23,
    116,1,221,0,47,0,10,24,2,25,2,0,116,1,221,0,47,0,10,2,116,1,221,0,
    47,0,10,26,116,1,221,0,47,0,10,27,116,1,221,0,47,0,10,28,116,1,221,0,
    47,0,10,29,116,1,220,0,47,0,10,30,116,1,220,0,47,0,10,31,116,1,220,0,
    47,0,10,32,116,1,220,0,47,0,10,33,116,1,219,0,47,0,11,34,116,1,219,0,
    47,0,11,36,116,1,219,0,47,0,11,38,119,1,52,1,47,0,12,40,119,1,52,1,
    47,0,12,41,19,42,32,0,165,42,20,42,8,44,11,0,8,45,12,0,162,11,20,42,
    132,187,20,12,255,255,255,127,181,203,75,22,17,15,113,16,184,0,11,0,12,12,110,32,
    235,0,193,0,10,12,56,12,3,0,40,93,75,13,17,15,113,16,184,0,11,0,12,12,
    2,42,11,0,113,16,184,0,13,0,12,11,110,48,237,0,193,11,58,2,7,0,53,242,
    5,0,68,2,17,2,40,3,2,2,22,0,75,2,45,13,218,2,13,2,75,26,19,2,
    216,11,2,1,75,27,19,11,75,28,18,2,75,29,18,11,76,20,3,13,75,42,4,13,
    58,14,8,0,98,2,19,0,33,34,53,46,3,0,40,3,2,14,16,0,75,14,5,13,
    75,23,6,13,75,24,7,13,218,2,13,4,75,30,8,2,216,11,2,1,75,31,8,11,
    216,11,2,2,75,32,8,11,216,2,2,3,75,33,8,2,218,2,13,3,76,34,9,2,
    216,11,2,1,76,36,9,11,216,2,2,2,76,38,9,2,77,40,10,13,77,41,44,13,
    216,13,13,1,216,15,15,1,2,2,25,0,8,11,44,0,8,12,45,0,41,0,50,255,
    8,44,11,0,8,45,12,0,19,22,255,255,113,32,232,0,211,0,12,2,91,2,49,0,
    113,32,231,0,212,0,12,2,91,2,61,0,91,5,57,0,91,6,45,0,91,7,52,0,
    91,8,56,0,91,9,55,0,91,10,51,0,8,2,44,0,91,2,60,0,8,2,45,0,
    91,2,59,0,8,2,19,0,91,2,53,0,8,2,18,0,91,2,44,0,216,2,13,1,
    35,35,138,0,2,4,16,0,18,21,53,212,17,0,84,6,59,0,68,6,6,4,59,6,
    3,0,1,214,68,7,3,6,176,87,75,7,3,6,216,4,4,1,40,239,35,34,145,0,
    2,4,16,0,54,212,13,0,68,6,3,4,35,102,138,0,77,6,2,4,75,16,3,4,
    216,4,4,1,40,244,2,4,16,0,53,212,22,0,84,6,59,0,68,6,6,4,59,6,
    3,0,1,214,70,7,2,6,68,8,3,6,216,9,8,1,75,9,3,6,75,4,7,8,
    216,4,4,1,40,235,91,2,47,0,70,2,2,13,91,2,58,0,84,2,50,0,110,16,
    234,0,2,0,84,2,50,0,110,32,238,0,18,0,82,1,46,0,2,2,22,0,50,33,
    16,0,84,3,50,0,113,16,184,0,1,0,12,1,110,32,235,0,19,0,10,1,57,1,
    4,0,89,2,46,0,82,1,48,0,50,33,16,0,84,3,50,0,113,16,184,0,1,0,
    12,1,110,32,235,0,19,0,10,1,57,1,4,0,89,2,48,0,112,16,35,1,0,0,
    10,1,56,1,17,0,19,1,0,8,113,16,118,0,1,0,12,1,110,32,120,0,81,0,
    110,32,44,1,16,0,112,32,48,1,16,0,14,0,0,0,3,0,3,0,1,0,0,0,
    176,92,0,0,8,0,0,0,112,16,189,0,0,0,91,1,62,0,91,2,63,0,14,0,
    2,0,1,0,2,0,0,0,182,92,0,0,8,0,0,0,84,16,62,0,84,17,63,0,
    113,32,87,1,16,0,14,0,2,0,2,0,1,0,0,0,0,0,0,0,6,0,0,0,
    91,1,64,0,112,16,49,0,0,0,14,0,5,0,3,0,3,0,2,0,0,0,0,0,
    63,0,0,0,110,16,55,0,4,0,12,3,18,0,56,3,7,0,110,16,53,0,3,0,
    12,3,40,2,7,3,57,3,10,0,110,16,54,0,4,0,12,3,110,16,28,0,3,0,
    12,3,7,48,84,35,64,0,29,3,110,16,54,0,4,0,12,4,110,16,25,0,4,0,
    12,4,84,33,64,0,114,32,246,0,65,0,10,1,56,1,12,0,56,0,15,0,84,33,
    64,0,114,32,248,0,65,0,12,1,57,1,7,0,84,34,64,0,114,48,249,0,66,0,
    30,3,14,0,13,2,30,3,39,2,0,0,15,0,0,0,8,0,1,0,27,0,0,0,
    35,0,4,0,2,1,97,24,0,60,0,0,1,0,1,0,0,0,0,0,0,0,0,0,
    3,0,0,0,84,0,70,0,17,0,0,0,1,0,1,0,0,0,0,0,0,0,0,0,
    3,0,0,0,84,0,66,0,17,0,0,0,1,0,1,0,0,0,0,0,0,0,0,0,
    3,0,0,0,84,0,73,0,17,0,0,0,1,0,1,0,0,0,0,0,0,0,0,0,
    3,0,0,0,85,0,67,0,15,0,0,0,1,0,1,0,0,0,0,0,0,0,0,0,
    3,0,0,0,85,0,69,0,15,0,0,0,3,0,3,0,3,0,0,0,0,0,0,0,
    5,0,0,0,112,48,73,1,16,2,10,0,15,0,0,0,4,0,3,0,4,0,1,0,
    0,0,0,0,18,0,0,0,84,17,72,0,98,0,13,0,110,64,0,1,33,3,10,1,
    15,1,113,0,213,0,0,0,12,1,110,16,214,0,1,0,18,1,15,1,0,0,0,0,
    8,0,1,0,1,1,91,9,4,0,2,0,2,0,1,0,0,0,0,0,26,0,0,0,
    84,32,74,0,18,97,56,0,13,0,110,32,178,0,48,0,84,34,74,0,110,16,177,0,
    2,0,18,2,15,2,15,1,84,32,70,0,56,0,7,0,112,32,80,1,50,0,10,2,
    15,2,15,1,5,0,0,0,8,0,1,0,1,1,84,15,9,0,2,0,5,0,1,0,
    0,0,0,0,96,0,0,0,84,112,66,0,85,113,65,0,56,1,4,0,18,33,40,2,
    18,17,110,32,40,0,16,0,18,0,1,1,33,130,53,33,80,0,82,114,71,0,33,131,
    177,19,113,32,187,0,50,0,10,2,35,35,134,0,113,82,209,0,24,3,84,114,72,0,
    110,16,254,0,2,0,92,112,69,0,1,2,1,36,19,5,50,0,53,82,29,0,57,4,
    27,0,84,116,66,0,110,32,39,0,52,0,84,116,70,0,84,117,66,0,110,32,36,0,
    84,0,10,4,57,4,7,0,22,5,20,0,113,32,81,1,101,0,216,2,2,1,40,228,
    18,55,15,7,56,4,23,0,22,2,16,39,112,48,73,1,39,3,10,2,56,2,15,0,
    85,114,69,0,57,2,11,0,85,114,67,0,57,2,3,0,40,5,82,114,71,0,176,33,
    40,178,18,103,15,7,15,0,45,0,0,0,20,0,1,0,1,1,97,68,1,0,1,0,
    0,0,0,0,0,0,0,0,3,0,0,0,84,0,74,0,17,0,0,0,1,0,1,0,
    0,0,0,0,0,0,0,0,3,0,0,0,84,0,72,0,17,0,0,0,2,0,2,0,
    0,0,0,0,0,0,0,0,3,0,0,0,92,1,65,0,14,0,0,0,2,0,2,0,
    0,0,0,0,0,0,0,0,3,0,0,0,91,1,66,0,14,0,0,0,2,0,2,0,
    0,0,0,0,0,0,0,0,3,0,0,0,92,1,69,0,14,0,0,0,2,0,2,0,
    0,0,0,0,0,0,0,0,3,0,0,0,91,1,70,0,14,0,0,0,2,0,2,0,
    0,0,0,0,0,0,0,0,3,0,0,0,91,1,73,0,14,0,0,0,2,0,2,0,
    0,0,0,0,0,0,0,0,3,0,0,0,91,1,74,0,14,0,0,0,3,0,1,0,
    2,0,0,0,0,0,0,0,16,0,0,0,112,16,37,0,2,0,19,0,20,0,89,32,
    71,0,34,0,117,0,18,1,112,32,253,0,16,0,91,32,72,0,14,0,2,0,1,0,
    1,0,2,0,0,0,0,0,20,0,0,0,84,16,73,0,56,0,5,0,110,16,44,0,
    0,0,84,16,70,0,56,0,10,0,110,16,32,0,0,0,84,17,70,0,110,16,31,0,
    1,0,14,0,0,0,0,0,7,0,1,0,7,0,0,0,12,0,4,0,2,1,84,7,
    1,97,19,0,4,0,4,0,1,0,0,0,0,0,0,0,18,0,0,0,85,1,69,0,
    57,1,7,0,56,3,3,0,40,3,18,1,40,2,18,17,92,1,69,0,84,0,72,0,
    110,16,255,0,0,0,14,0,4,0,4,0,1,0,0,0,0,0,0,0,23,0,0,0,
    18,17,57,2,7,0,18,34,51,35,4,0,1,18,40,2,18,2,92,2,67,0,85,2,
    67,0,57,2,4,0,92,1,69,0,84,0,72,0,110,16,255,0,0,0,14,0,0,0,
    4,0,4,0,1,0,0,0,0,0,0,0,14,0,0,0,57,3,8,0,18,49,55,18,
    5,0,177,18,89,2,71,0,84,0,72,0,110,16,255,0,0,0,14,0,3,0,3,0,
    1,0,0,0,0,0,0,0,18,0,0,0,85,1,69,0,57,1,7,0,56,2,3,0,
    40,3,18,1,40,2,18,17,92,1,69,0,84,0,72,0,110,16,255,0,0,0,14,0,
    5,0,2,0,2,0,0,0,0,0,0,0,35,0,0,0,26,0,121,1,110,32,4,0,
    3,0,12,3,31,3,16,0,18,0,56,3,7,0,110,16,43,0,3,0,12,3,40,2,
    7,3,18,1,57,3,6,0,18,19,75,3,4,1,17,0,110,16,20,0,3,0,10,2,
    57,2,6,0,18,35,75,3,4,1,17,0,17,3,0,0,6,0,2,0,2,0,0,0,
    0,0,0,0,19,0,0,0,33,80,18,1,1,18,53,2,14,0,70,3,5,2,110,32,
    0,0,52,0,10,3,56,3,3,0,15,1,216,2,2,1,40,243,18,20,15,4,0,0,
    7,0,1,0,2,0,0,0,0,0,0,0,58,0,0,0,110,16,30,0,6,0,12,0,
    18,17,18,2,56,0,28,0,33,3,61,3,25,0,33,6,1,35,53,99,20,0,70,4,
    0,3,98,5,81,0,110,16,90,0,4,0,12,4,110,32,251,0,69,0,10,4,56,4,
    3,0,15,1,216,3,3,1,40,237,15,2,110,16,26,0,6,0,12,6,57,6,3,0,
    15,2,110,16,21,0,6,0,10,6,19,0,0,6,50,6,10,0,56,6,8,0,19,0,
    0,31,51,6,3,0,40,2,15,2,15,1,6,0,1,0,2,0,0,0,0,0,0,0,
    21,0,0,0,98,0,80,0,33,1,18,2,1,35,53,19,15,0,70,4,0,3,110,32,
    251,0,84,0,10,4,56,4,4,0,18,21,15,5,216,3,3,1,40,242,15,2,0,0,
    3,0,1,0,0,0,0,0,0,0,0,0,8,0,0,0,141,34,18,16,35,0,134,0,
    18,1,79,2,0,1,17,0,5,0,1,0,4,0,1,0,0,0,0,0,91,0,0,0,
    18,0,113,32,94,1,4,0,10,1,56,1,7,0,113,16,85,1,1,0,12,4,17,4,
    36,16,138,0,0,0,12,1,113,32,84,1,20,0,12,4,57,4,9,0,68,4,1,0,
    113,16,85,1,4,0,12,4,17,4,34,1,79,0,112,16,159,0,1,0,110,32,161,0,
    1,0,110,16,18,0,4,0,12,4,114,16,250,0,4,0,12,4,114,16,239,0,4,0,
    10,0,56,0,35,0,114,16,240,0,4,0,12,0,31,0,11,0,110,16,29,0,0,0,
    10,2,18,35,50,50,239,255,113,16,95,1,0,0,10,2,56,2,233,255,110,16,25,0,
    0,0,12,2,110,16,28,0,0,0,12,0,19,3,67,0,113,64,97,1,49,2,40,218,
    110,16,160,0,1,0,12,4,17,4,18,52,40,190,0,0,37,0,0,0,46,0,1,0,
    1,1,97,89,13,0,2,0,4,0,3,0,0,0,0,0,171,0,0,0,18,16,113,32,
    94,1,11,0,10,0,56,0,7,0,113,16,85,1,0,0,12,11,17,11,18,0,36,16,
    138,0,0,0,12,1,113,32,84,1,27,0,12,11,57,11,9,0,68,11,1,0,113,16,
    85,1,11,0,12,11,17,11,110,16,17,0,11,0,12,11,18,33,57,11,7,0,113,16,
    85,1,1,0,12,11,17,11,34,2,111,0,112,16,241,0,2,0,34,3,123,0,112,32,
    56,1,35,0,34,4,107,0,112,16,225,0,4,0,98,5,80,0,33,86,1,7,53,103,
    28,0,70,8,5,7,34,9,20,0,112,16,50,0,9,0,34,10,41,0,112,32,89,0,
    138,0,110,32,52,0,169,0,12,8,110,16,51,0,8,0,12,8,114,32,242,0,132,0,
    216,7,7,1,40,229,34,5,24,0,112,16,56,0,5,0,110,32,58,0,21,0,12,1,
    110,16,57,0,1,0,12,1,110,64,47,0,75,49,129,196,113,32,93,1,84,0,110,32,
    48,0,59,0,34,11,79,0,112,16,159,0,11,0,110,32,161,0,11,0,29,2,114,16,
    247,0,2,0,12,12,114,16,250,0,12,0,12,12,114,16,239,0,12,0,10,0,56,0,
    26,0,114,16,240,0,12,0,12,0,31,0,113,0,114,16,244,0,0,0,12,1,31,1,
    98,0,114,16,245,0,0,0,12,0,31,0,98,0,19,3,76,0,113,64,97,1,59,1,
    40,227,30,2,110,16,160,0,11,0,12,11,17,11,13,11,30,2,39,11,18,59,41,0,
    112,255,0,0,102,0,0,0,10,0,1,0,121,0,0,0,39,0,5,0,166,0,0,0,
    1,0,5,0,2,1,97,168,1,0,165,1,10,0,2,0,3,0,0,0,0,0,0,0,
    95,0,0,0,113,16,88,1,9,0,12,9,113,32,86,1,152,0,10,0,18,1,56,0,
    3,0,15,1,113,0,87,0,0,0,12,0,113,0,86,0,0,0,12,2,18,51,51,32,
    3,0,15,3,34,0,122,0,112,48,54,1,128,9,110,32,8,0,8,0,113,0,210,0,
    0,0,11,4,22,6,208,7,187,100,110,16,6,0,8,0,10,0,56,0,16,0,113,0,
    210,0,0,0,11,6,49,0,6,4,59,0,8,0,22,6,50,0,113,32,93,1,118,0,
    40,237,113,0,210,0,0,0,11,4,23,6,96,234,0,0,187,100,110,16,6,0,8,0,
    10,0,57,0,16,0,113,0,210,0,0,0,11,6,49,0,6,4,59,0,8,0,22,6,
    100,0,113,32,93,1,118,0,40,237,113,32,86,1,152,0,10,8,56,8,3,0,15,1,
    15,3,0,0,5,0,1,0,0,0,0,0,0,0,0,0,39,0,0,0,96,0,0,0,
    19,1,31,0,18,18,18,3,52,16,21,0,26,0,97,1,56,4,12,0,18,36,35,68,
    143,0,77,0,4,3,26,0,98,1,77,0,4,2,17,4,35,36,143,0,77,0,4,3,
    17,4,56,4,9,0,35,36,143,0,26,0,96,1,77,0,4,3,17,4,35,52,143,0,
    17,4,0,0,7,0,3,0,3,0,1,0,0,0,0,0,59,0,0,0,34,0,124,0,
    112,16,72,1,0,0,18,1,113,32,94,1,20,0,10,2,89,2,68,0,82,2,68,0,
    56,2,3,0,17,0,36,16,138,0,1,0,12,2,113,32,84,1,36,0,12,3,57,3,
    7,0,68,4,2,1,89,4,68,0,17,0,113,16,16,0,6,0,10,1,57,1,4,0,
    18,68,40,246,110,32,19,0,99,0,12,6,57,5,6,0,113,48,90,1,99,0,17,0,
    113,48,91,1,100,0,17,0,110,16,74,1,0,0,18,52,40,227,0,0,40,0,0,0,
    13,0,1,0,1,1,97,54,2,0,2,0,2,0,0,0,0,0,0,0,4,0,0,0,
    113,32,93,1,16,0,14,0,3,0,0,0,1,0,0,0,0,0,0,0,41,0,0,0,
    26,0,59,0,113,16,252,0,0,0,12,0,105,0,81,0,18,48,35,0,144,0,26,1,
    60,0,113,16,252,0,1,0,12,1,18,2,77,1,0,2,26,1,61,0,113,16,252,0,
    1,0,12,1,18,18,77,1,0,2,26,1,90,0,113,16,252,0,1,0,12,1,18,34,
    77,1,0,2,105,0,80,0,14,0,0,0,1,0,1,0,1,0,0,0,0,0,0,0,
    4,0,0,0,112,16,189,0,0,0,14,0,3,0,2,0,3,0,0,0,0,0,0,0,
    6,0,0,0,19,0,119,1,110,48,7,0,33,0,14,0,5,0,3,0,2,0,2,0,
    0,0,0,0,92,0,0,0,110,16,27,0,3,0,10,0,19,1,12,0,50,16,6,0,
    18,66,89,66,68,0,14,0,110,16,15,0,2,0,18,2,18,32,53,2,65,0,113,16,
    64,1,4,0,12,0,57,0,59,0,57,2,9,0,98,0,81,0,110,32,24,0,3,0,
    12,0,40,7,98,0,81,0,110,32,23,0,3,0,12,0,113,32,69,1,4,0,113,16,
    63,1,4,0,12,0,110,16,45,0,0,0,113,16,63,1,4,0,12,0,110,16,46,0,
    0,0,12,0,113,32,70,1,4,0,40,18,113,16,63,1,4,0,12,0,56,0,9,0,
    113,16,63,1,4,0,12,0,110,16,44,0,0,0,18,0,113,32,69,1,4,0,216,2,
    2,1,40,191,113,16,64,1,4,0,12,2,57,2,5,0,18,82,89,66,68,0,14,0,
    27,0,0,0,34,0,1,0,62,0,0,0,13,0,4,0,2,1,84,62,1,84,75,0,
    10,0,3,0,5,0,0,0,0,0,0,0,201,0,0,0,18,0,18,33,110,81,22,0,
    120,144,12,7,113,32,68,1,121,0,113,16,61,1,9,0,12,7,18,88,56,7,181,0,
    22,2,152,58,113,48,71,1,41,3,10,7,56,7,173,0,113,16,59,1,9,0,10,7,
    57,7,4,0,41,0,165,0,113,16,62,1,9,0,12,7,110,16,254,0,7,0,113,16,
    61,1,9,0,12,7,19,2,185,0,110,32,35,0,39,0,10,7,56,7,7,0,22,2,
    136,19,113,48,71,1,41,3,113,16,62,1,9,0,12,7,110,16,254,0,7,0,113,32,
    67,1,9,0,113,16,61,1,9,0,12,7,110,16,33,0,7,0,10,7,56,7,121,0,
    22,2,16,39,113,48,71,1,41,3,10,7,56,7,113,0,113,16,60,1,9,0,10,7,
    56,7,4,0,41,0,105,0,1,7,53,23,96,0,113,16,58,1,9,0,12,2,57,2,
    90,0,57,7,5,0,19,2,8,0,40,2,18,66,113,16,61,1,9,0,12,3,110,16,
    34,0,3,0,12,3,114,16,243,0,3,0,12,3,114,16,239,0,3,0,10,4,56,4,
    63,0,114,16,240,0,3,0,12,4,31,4,15,0,110,16,42,0,4,0,12,5,113,16,
    96,1,5,0,10,5,57,5,3,0,40,234,110,16,41,0,4,0,12,4,114,16,243,0,
    4,0,12,4,114,16,239,0,4,0,10,5,56,5,26,0,114,16,240,0,4,0,12,5,
    31,5,14,0,110,16,38,0,5,0,10,6,181,38,56,6,239,255,113,32,66,1,89,0,
    57,7,4,0,18,20,40,2,1,4,113,32,65,1,73,0,113,16,58,1,9,0,12,4,
    56,4,191,255,216,7,7,1,40,161,113,16,58,1,9,0,12,7,57,7,7,0,110,16,
    74,1,9,0,89,152,68,0,14,0,0,0,2,0,2,0,2,0,1,0,0,0,0,0,
    12,0,0,0,113,32,215,0,16,0,14,0,113,0,213,0,0,0,12,0,110,16,214,0,
    0,0,14,0,0,0,0,0,3,0,1,0,1,1,91,4,5,0,4,0,4,0,0,0,
    0,0,0,0,41,0,0,0,110,32,161,0,33,0,98,2,12,0,110,32,194,0,35,0,
    12,2,33,35,18,0,110,64,163,0,33,48,110,32,161,0,1,0,26,2,0,0,56,4,
    8,0,26,3,1,0,110,48,199,0,52,2,12,2,98,3,12,0,110,32,194,0,50,0,
    12,2,33,35,110,64,163,0,33,48,110,32,161,0,1,0,14,0,0,0,3,0,3,0,
    2,0,0,0,0,0,0,0,23,0,0,0,84,0,83,0,26,2,0,0,113,32,107,1,
    32,0,114,16,180,0,1,0,12,0,98,1,12,0,110,32,194,0,16,0,12,0,18,1,
    113,32,111,1,1,0,18,16,15,0,0,0,3,0,3,0,1,0,0,0,0,0,0,0,
    12,0,0,0,18,0,1,2,53,18,8,0,113,16,110,1,0,0,216,2,2,1,40,249,
    18,16,15,0,1,0,1,0,1,0,0,0,0,0,0,0,7,0,0,0,84,0,83,0,
    113,16,108,1,0,0,18,16,15,0,0,0,2,0,2,0,1,0,0,0,0,0,0,0,
    10,0,0,0,84,0,83,0,113,16,108,1,0,0,18,16,113,16,110,1,0,0,15,0,
    2,0,2,0,2,0,0,0,0,0,0,0,7,0,0,0,84,0,83,0,113,32,109,1,
    16,0,18,16,15,0,0,0,3,0,3,0,2,0,0,0,0,0,0,0,26,0,0,0,
    84,2,83,0,114,16,180,0,1,0,12,1,113,32,107,1,18,0,84,0,83,0,113,16,
    106,1,0,0,12,0,98,1,12,0,110,32,194,0,16,0,12,0,18,17,113,32,111,1,
    1,0,15,1,3,0,2,0,3,0,0,0,0,0,0,0,7,0,0,0,18,0,112,48,
    153,0,33,0,91,18,83,0,14,0,0,0,2,0,2,0,2,0,0,0,0,0,0,0,
    8,0,0,0,113,0,119,1,0,0,111,32,114,0,16,0,12,0,17,0,4,0,2,0,
    2,0,0,0,0,0,0,0,26,0,0,0,82,32,92,0,89,48,10,0,21,0,0,18,
    82,33,90,0,182,16,89,48,7,0,18,0,89,48,9,0,89,48,8,0,26,3,0,0,
    91,35,91,0,34,3,126,0,112,32,99,1,35,0,17,3,2,0,2,0,2,0,0,0,
    0,0,0,0,5,0,0,0,112,32,118,1,16,0,10,0,15,0,0,0,7,0,2,0,
    3,0,0,0,0,0,0,0,112,0,0,0,110,16,107,0,6,0,10,0,110,16,106,0,
    6,0,10,1,18,2,18,19,57,1,4,0,1,49,40,2,1,33,19,4,67,0,51,64,
    44,0,57,1,3,0,15,3,84,86,91,0,110,16,195,0,6,0,10,6,56,6,6,0,
    113,16,120,1,2,0,40,27,84,86,91,0,110,16,197,0,6,0,10,0,18,241,110,48,
    198,0,6,1,10,0,110,48,203,0,38,0,12,6,91,86,91,0,98,5,12,0,110,32,
    194,0,86,0,12,5,113,32,121,1,83,0,15,3,19,4,66,0,50,64,40,0,19,4,
    160,0,51,64,3,0,40,34,110,16,108,0,6,0,10,6,19,0,32,0,53,6,3,0,
    15,2,56,1,23,0,112,16,115,1,5,0,34,5,98,0,113,16,181,0,6,0,12,6,
    112,32,191,0,101,0,98,6,12,0,110,32,194,0,101,0,12,5,113,32,121,1,82,0,
    15,3,56,1,8,0,112,16,115,1,5,0,113,16,120,1,3,0,15,3,1,0,1,0,
    0,0,0,0,0,0,0,0,2,0,0,0,18,16,15,0,4,0,3,0,3,0,0,0,
    0,0,0,0,20,0,0,0,85,16,93,0,56,0,8,0,112,32,118,1,49,0,10,0,
    57,0,8,0,111,48,115,0,33,3,10,1,56,1,4,0,18,17,15,1,18,1,15,1,
    1,0,1,0,0,0,0,0,0,0,0,0,3,0,0,0,84,0,91,0,17,0,0,0,
    2,0,2,0,0,0,0,0,0,0,0,0,3,0,0,0,91,1,91,0,14,0,0,0,
    1,0,1,0,1,0,0,0,0,0,0,0,4,0,0,0,112,16,115,1,0,0,14,0,
    1,0,1,0,1,0,0,0,0,0,0,0,4,0,0,0,113,16,120,1,0,0,14,0,
    2,0,2,0,2,0,0,0,0,0,0,0,4,0,0,0,113,32,121,1,16,0,14,0,
    2,0,0,0,0,0,0,0,0,0,0,0,48,0,0,0,18,96,35,1,138,0,38,1,
    13,0,0,0,105,1,89,0,35,0,138,0,38,0,22,0,0,0,105,0,84,0,14,0,
    0,3,4,0,6,0,0,0,1,0,0,0,2,0,0,0,2,32,0,0,33,0,0,0,
    17,0,0,0,129,0,0,0,0,3,4,0,6,0,0,0,1,0,0,0,6,0,0,0,
    2,0,0,0,5,0,0,0,3,0,0,0,4,0,0,0,4,0,2,0,2,0,0,0,
    0,0,0,0,39,0,0,0,112,32,112,0,50,0,98,0,89,0,18,1,68,0,0,1,
    89,32,92,0,18,16,89,32,90,0,26,1,0,0,91,33,91,0,110,32,127,1,2,0,
    110,32,128,1,2,0,32,50,5,0,56,2,13,0,31,3,5,0,110,16,5,0,3,0,
    12,2,19,3,48,0,110,32,117,0,50,0,14,0,0,0,8,0,3,0,3,0,0,0,
    0,0,0,0,94,0,0,0,110,16,116,1,5,0,12,0,26,1,245,1,110,32,66,0,
    16,0,12,0,31,0,70,0,213,113,255,0,225,7,7,8,213,119,255,0,98,2,89,0,
    33,35,18,4,53,49,3,0,40,2,1,65,68,1,2,1,98,2,84,0,33,35,53,55,
    3,0,40,2,1,71,68,7,2,7,82,82,92,0,51,33,6,0,82,82,90,0,50,39,
    16,0,89,81,92,0,89,87,90,0,85,87,93,0,56,7,8,0,112,16,115,1,5,0,
    110,32,155,0,80,0,85,87,93,0,56,6,17,0,57,7,11,0,18,22,92,86,93,0,
    110,16,126,1,5,0,110,32,155,0,80,0,110,48,156,0,80,4,14,0,56,7,15,0,
    92,84,93,0,26,6,0,0,91,86,91,0,110,16,117,1,5,0,12,5,110,48,154,0,
    80,4,14,0,3,0,1,0,2,0,0,0,0,0,0,0,25,0,0,0,84,32,91,0,
    110,16,195,0,0,0,10,0,57,0,18,0,84,32,91,0,26,1,0,0,91,33,91,0,
    98,2,12,0,110,32,194,0,32,0,12,2,18,0,113,32,121,1,32,0,14,0,0,0,
    7,0,7,0,1,0,0,0,0,0,0,0,16,0,0,0,91,2,99,0,89,3,97,0,
    91,4,96,0,91,5,98,0,91,6,95,0,91,1,94,0,112,16,189,0,0,0,14,0,
    10,0,1,0,3,0,17,0,0,0,0,0,235,0,0,0,18,0,84,145,99,0,110,16,
    228,0,1,0,10,1,18,2,56,1,6,0,113,32,133,1,2,0,14,0,82,145,97,0,
    18,67,18,20,51,49,62,0,34,1,82,0,84,147,96,0,112,32,169,0,49,0,84,147,
    98,0,84,153,99,0,110,32,227,0,41,0,12,9,31,9,33,0,26,2,170,2,110,48,
    63,0,147,2,12,9,113,32,131,1,145,0,56,9,5,0,110,16,176,0,9,0,110,16,
    173,0,1,0,113,32,133,1,4,0,14,0,13,2,56,9,10,0,110,16,176,0,9,0,
    40,5,13,9,110,32,217,0,146,0,39,2,13,9,110,16,173,0,1,0,40,5,13,1,
    110,32,217,0,25,0,39,9,34,1,79,0,112,16,159,0,1,0,1,35,84,149,99,0,
    110,16,229,0,5,0,10,5,53,83,130,0,34,5,80,0,84,150,95,0,113,0,211,0,
    0,0,11,7,113,32,185,0,135,0,12,7,34,8,99,0,112,16,205,0,8,0,110,32,
    207,0,120,0,26,7,5,0,110,32,207,0,120,0,110,32,206,0,56,0,110,16,208,0,
    8,0,12,7,112,48,164,0,101,7,110,16,168,0,5,0,34,6,80,0,84,151,98,0,
    84,152,99,0,110,32,227,0,56,0,12,8,31,8,33,0,113,32,132,1,135,0,12,7,
    112,48,164,0,86,7,84,149,98,0,84,151,99,0,110,32,227,0,55,0,12,7,31,7,
    33,0,110,32,62,0,117,0,12,5,34,7,83,0,112,32,170,0,103,0,113,32,131,1,
    117,0,110,16,176,0,7,0,56,5,5,0,110,16,173,0,5,0,110,16,166,0,6,0,
    12,5,98,6,12,0,110,32,194,0,101,0,12,5,110,32,162,0,81,0,110,32,161,0,
    33,0,216,3,3,1,40,143,13,9,110,16,176,0,7,0,40,5,13,1,110,32,217,0,
    25,0,39,9,13,9,56,5,10,0,110,16,173,0,5,0,40,5,13,1,110,32,217,0,
    25,0,39,9,110,16,160,0,1,0,12,9,113,32,133,1,148,0,14,0,18,249,113,32,
    133,1,9,0,14,0,0,0,1,0,0,0,26,0,1,0,27,0,0,0,16,0,5,0,
    43,0,0,0,3,0,7,0,48,0,0,0,3,0,5,0,51,0,0,0,6,0,1,0,
    61,0,0,0,3,0,9,0,66,0,0,0,4,0,5,0,71,0,0,0,3,0,11,0,
    76,0,0,0,89,0,1,0,165,0,0,0,5,0,13,0,170,0,0,0,3,0,16,0,
    173,0,0,0,3,0,13,0,178,0,0,0,19,0,1,0,201,0,0,0,3,0,19,0,
    206,0,0,0,4,0,13,0,213,0,0,0,3,0,22,0,218,0,0,0,11,0,1,0,
    9,1,89,230,1,0,70,0,58,0,65,0,75,0,210,1,0,200,1,0,205,1,0,217,
    1,0,0,0,2,0,2,0,2,0,0,0,0,0,0,0,5,0,0,0,113,32,140,1,
    16,0,12,0,17,0,0,0,4,0,2,0,2,0,0,0,0,0,0,0,32,0,0,0,
    19,0,46,0,110,32,196,0,2,0,10,0,59,0,4,0,18,2,40,19,113,0,158,0,
    0,0,12,1,216,0,0,1,110,32,202,0,2,0,12,2,110,16,204,0,2,0,12,2,
    110,32,157,0,33,0,12,2,56,2,3,0,17,2,17,3,10,0,2,0,6,0,5,0,
    0,0,0,0,101,0,0,0,18,1,26,0,79,1,36,16,143,0,0,0,12,4,18,6,
    18,7,18,5,7,130,7,147,116,6,64,0,2,0,12,8,56,8,28,0,114,16,77,0,
    8,0,10,9,56,9,22,0,18,9,114,32,76,0,152,0,12,9,40,16,13,0,7,9,
    56,8,11,0,114,16,75,0,8,0,40,6,13,0,7,8,110,32,217,0,137,0,39,9,
    7,25,56,8,5,0,114,16,75,0,8,0,7,145,40,2,7,147,57,1,6,0,110,16,
    81,0,3,0,12,1,26,8,151,1,57,1,3,0,17,8,19,9,47,0,110,32,196,0,
    145,0,10,9,216,9,9,1,110,32,202,0,145,0,12,9,110,16,195,0,9,0,10,0,
    57,0,20,0,26,0,56,0,110,32,193,0,9,0,10,0,57,0,12,0,26,0,57,0,
    110,32,193,0,9,0,10,0,56,0,3,0,40,2,7,152,17,8,0,0,1,0,0,0,
    6,0,1,0,12,0,0,0,4,0,4,0,18,0,0,0,11,0,7,0,34,0,0,0,
    3,0,9,0,40,0,0,0,10,0,4,0,4,1,96,52,1,96,53,0,30,0,38,0,
    8,0,1,0,2,0,0,0,0,0,0,0,76,0,0,0,34,0,107,0,112,16,225,0,
    0,0,26,1,66,1,110,32,200,0,23,0,12,7,33,113,18,2,1,35,18,4,53,19,
    45,0,70,5,7,3,26,6,56,0,110,32,201,0,101,0,10,6,56,6,7,0,18,22,
    110,32,202,0,101,0,12,5,110,16,195,0,5,0,10,6,56,6,3,0,40,19,113,0,
    158,0,0,0,12,6,110,16,204,0,5,0,12,5,110,32,157,0,86,0,12,5,57,5,
    3,0,17,4,110,32,226,0,80,0,216,3,3,1,40,211,110,16,228,0,0,0,10,7,
    56,7,3,0,17,4,35,39,143,0,110,32,230,0,112,0,12,7,31,7,143,0,17,7,
    2,0,2,0,2,0,0,0,0,0,0,0,4,0,0,0,113,32,136,1,16,0,14,0,
    2,0,2,0,2,0,0,0,0,0,0,0,4,0,0,0,113,32,141,1,16,0,14,0,
    1,0,1,0,1,0,0,0,0,0,0,0,4,0,0,0,112,16,9,0,0,0,14,0,
    2,0,2,0,2,0,0,0,0,0,0,0,4,0,0,0,113,32,141,1,16,0,14,0,
    5,0,2,0,4,0,0,0,0,0,0,0,28,0,0,0,56,3,20,0,56,4,18,0,
    21,0,1,0,35,0,134,0,110,32,174,0,3,0,10,1,61,1,7,0,18,2,110,64,
    179,0,4,18,40,246,14,0,34,3,84,0,26,4,141,2,112,32,172,0,67,0,39,3,
    11,0,4,0,7,0,0,0,0,0,0,0,102,0,0,0,216,3,8,255,110,16,137,1,
    7,0,12,8,18,240,18,1,18,2,51,9,88,0,56,10,86,0,57,8,3,0,40,82,
    1,41,34,2,107,0,112,16,225,0,2,0,110,16,69,0,10,0,12,0,56,0,22,0,
    110,16,61,0,0,0,10,10,53,169,29,0,110,32,60,0,144,0,12,10,110,16,59,0,
    10,0,12,10,110,32,226,0,162,0,216,9,9,1,40,237,110,16,70,0,10,0,12,9,
    56,9,9,0,110,16,70,0,10,0,12,9,110,32,226,0,146,0,84,116,108,0,91,113,
    108,0,110,16,2,0,8,0,12,5,34,6,80,0,110,16,1,0,8,0,12,8,26,9,
    124,1,112,48,164,0,134,9,34,8,101,0,34,0,128,0,7,113,118,7,129,1,0,0,
    26,7,125,1,112,48,212,0,8,7,110,16,216,0,8,0,14,0,1,41,113,32,141,1,
    25,0,14,0,8,0,5,0,3,0,1,0,0,0,0,0,117,0,0,0,26,0,52,2,
    18,81,51,20,6,0,113,48,155,1,99,7,14,0,110,16,3,0,3,0,12,3,110,32,
    12,0,3,0,12,1,31,1,129,0,57,1,21,0,34,1,129,0,112,16,134,1,1,0,
    110,16,10,0,3,0,12,2,110,48,13,0,18,0,12,0,110,16,14,0,0,0,110,16,
    11,0,3,0,18,67,18,16,51,52,26,0,91,23,108,0,34,3,30,0,26,5,90,1,
    112,32,67,0,83,0,26,5,115,1,113,32,138,1,86,0,12,5,110,32,74,0,83,0,
    26,5,95,1,110,48,71,0,83,6,40,34,34,3,30,0,26,6,91,1,112,32,67,0,
    99,0,26,6,3,0,110,32,74,0,99,0,113,16,139,1,5,0,12,5,56,5,7,0,
    26,6,94,1,110,48,73,0,99,5,26,5,93,1,51,4,4,0,1,6,40,2,18,6,
    110,48,72,0,83,6,26,5,92,1,110,32,68,0,83,0,176,4,110,48,144,1,49,4,
    14,0,18,243,18,4,113,32,141,1,67,0,14,0,0,0,5,0,0,0,105,0,1,0,
    1,1,96,111,5,0,5,0,1,0,0,0,0,0,0,0,10,0,0,0,90,2,111,0,
    91,4,110,0,91,1,109,0,112,16,189,0,0,0,14,0,5,0,1,0,4,0,0,0,
    0,0,0,0,33,0,0,0,84,64,109,0,113,16,148,1,0,0,10,0,18,17,51,16,
    20,0,113,0,91,0,0,0,11,0,83,66,111,0,49,0,0,2,59,0,10,0,84,64,
    110,0,22,1,100,0,110,64,85,0,64,33,14,0,84,68,109,0,113,16,147,1,4,0,
    14,0,0,0,6,0,1,0,4,0,5,0,0,0,0,0,75,0,0,0,18,80,35,1,
    134,0,18,2,34,3,82,0,112,32,169,0,83,0,1,37,53,5,26,0,217,4,5,5,
    110,64,175,0,19,69,10,4,60,4,6,0,110,16,173,0,3,0,15,2,176,69,40,241,
    13,5,110,16,173,0,3,0,40,5,13,0,110,32,217,0,5,0,39,5,110,16,173,0,
    3,0,72,5,1,2,19,0,37,0,51,5,31,0,18,21,72,0,1,5,19,3,80,0,
    51,48,24,0,18,32,72,0,1,0,19,3,68,0,51,48,17,0,18,48,72,0,1,0,
    19,3,70,0,51,48,10,0,18,64,72,0,1,0,19,1,45,0,51,16,3,0,15,5,
    15,2,0,0,4,0,0,0,5,0,1,0,14,0,0,0,4,0,4,0,20,0,0,0,
    3,0,1,0,27,0,0,0,3,0,6,0,32,0,0,0,7,0,1,0,3,1,89,74,
    0,26,0,31,1,0,1,0,1,0,0,0,0,0,0,0,5,0,0,0,112,16,156,1,
    0,0,10,0,15,0,0,0,1,0,1,0,1,0,0,0,0,0,0,0,19,0,0,0,
    84,0,118,0,56,0,7,0,110,16,103,0,0,0,12,0,40,2,18,0,56,0,7,0,
    110,16,104,0,0,0,10,0,15,0,18,112,15,0,0,0,1,0,1,0,1,0,0,0,
    0,0,0,0,4,0,0,0,112,16,150,1,0,0,14,0,3,0,3,0,1,0,0,0,
    0,0,0,0,8,0,0,0,112,16,98,0,0,0,91,1,117,0,91,2,119,0,14,0,
    4,0,1,0,2,0,0,0,0,0,0,0,39,0,0,0,85,48,116,0,56,0,3,0,
    14,0,18,16,92,48,116,0,112,16,156,1,3,0,10,3,18,33,18,2,50,19,21,0,
    18,49,50,19,18,0,18,65,50,19,15,0,18,81,50,19,12,0,18,96,50,3,7,0,
    18,3,113,32,135,1,35,0,14,0,18,243,40,251,113,32,135,1,32,0,14,0,0,0,
    6,0,1,0,5,0,0,0,0,0,0,0,25,0,0,0,34,0,37,0,113,0,86,0,
    0,0,12,1,112,32,83,0,16,0,113,0,91,0,0,0,11,1,22,3,48,117,187,49,
    34,3,130,0,112,80,145,1,83,33,110,32,84,0,48,0,14,0,0,0,6,0,6,0,
    3,0,0,0,0,0,0,0,44,0,0,0,110,16,82,0,3,0,10,3,56,3,6,0,
    110,16,93,0,4,0,14,0,34,3,48,0,84,0,119,0,112,32,99,0,3,0,18,0,
    110,32,101,0,3,0,12,3,18,245,110,32,102,0,83,0,12,3,110,16,100,0,3,0,
    12,3,56,1,8,0,110,32,92,0,33,0,10,1,57,1,3,0,18,16,110,48,94,0,
    52,0,14,0,8,0,5,0,4,0,12,0,0,0,0,0,93,0,0,0,34,4,82,0,
    84,51,117,0,112,32,169,0,52,0,34,3,83,0,110,16,88,0,5,0,12,5,112,32,
    171,0,83,0,21,5,1,0,35,85,134,0,110,32,174,0,84,0,10,0,18,1,61,0,
    22,0,110,16,82,0,6,0,10,2,56,2,12,0,110,16,95,0,7,0,110,16,176,0,
    3,0,110,16,173,0,4,0,14,0,110,64,179,0,83,1,40,230,110,16,176,0,3,0,
    110,16,173,0,4,0,18,19,35,51,141,0,98,4,1,0,77,4,3,1,110,32,97,0,
    55,0,14,0,13,5,110,16,176,0,3,0,40,5,13,3,110,32,217,0,53,0,39,5,
    13,3,110,16,173,0,4,0,40,5,13,4,110,32,217,0,67,0,39,3,13,3,110,16,
    182,0,3,0,12,3,110,32,96,0,55,0,14,0,0,0,0,0,0,0,7,0,1,0,
    7,0,0,0,9,0,4,0,18,0,0,0,18,0,6,0,36,0,0,0,3,0,4,0,
    39,0,0,0,3,0,1,0,43,0,0,0,3,0,6,0,47,0,0,0,3,0,4,0,
    50,0,0,0,3,0,1,0,65,0,0,0,3,0,8,0,70,0,0,0,4,0,4,0,
    75,0,0,0,3,0,10,0,80,0,0,0,4,0,1,0,5,1,89,84,0,74,0,64,
    0,69,0,79,9,0,3,0,4,0,0,0,0,0,0,0,82,0,0,0,113,16,151,1,
    8,0,10,0,18,241,18,2,57,0,6,0,113,32,135,1,33,0,14,0,34,0,80,0,
    112,32,165,0,128,0,110,16,167,0,0,0,12,0,110,16,195,0,7,0,10,3,56,3,
    3,0,7,7,34,3,131,0,110,16,204,0,0,0,12,4,26,5,58,0,110,32,192,0,
    84,0,10,4,56,4,3,0,40,16,34,4,99,0,112,16,205,0,4,0,110,32,207,0,
    4,0,110,32,207,0,84,0,110,16,208,0,4,0,12,0,112,48,149,1,131,0,26,8,
    64,2,110,32,4,0,134,0,12,6,31,6,52,0,57,6,6,0,113,32,135,1,33,0,
    14,0,110,64,105,0,118,35,12,6,91,54,118,0,14,0,0,2,0,0,14,0,0,0,
    14,0,0,0,1,0,0,0,95,0,0,0,1,0,0,0,2,0,0,0,1,0,0,0,
    3,0,0,0,2,0,0,0,3,0,3,0,2,0,0,0,5,0,133,0,1,0,0,0,
    98,0,0,0,1,0,0,0,121,0,0,0,1,0,0,0,131,0,0,0,1,0,0,0,
    134,0,0,0,3,0,0,0,134,0,3,0,3,0,0,0,2,0,0,0,6,0,98,0,
    2,0,0,0,5,0,138,0,4,0,0,0,29,0,133,0,13,0,3,0,1,0,0,0,
    124,0,0,0,1,0,0,0,116,0,0,0,1,0,0,0,41,0,0,0,2,0,0,0,
    98,0,98,0,2,0,0,0,98,0,133,0,2,0,0,0,98,0,143,0,5,0,0,0,
    33,0,143,0,98,0,143,0,98,0,0,0,3,0,0,0,98,0,47,0,44,0,0,0,
    1,0,0,0,58,0,0,0,3,0,0,0,3,0,3,0,133,0,0,0,6,0,0,0,
    3,0,3,0,3,0,3,0,133,0,133,0,4,0,0,0,3,0,2,0,2,0,2,0,
    1,0,0,0,55,0,0,0,2,0,0,0,55,0,3,0,1,0,0,0,68,0,0,0,
    1,0,0,0,33,0,0,0,2,0,0,0,33,0,98,0,1,0,0,0,94,0,0,0,
    2,0,0,0,94,0,94,0,1,0,0,0,4,0,0,0,2,0,0,0,28,0,33,0,
    2,0,0,0,87,0,87,0,1,0,0,0,103,0,0,0,2,0,0,0,121,0,3,0,
    1,0,0,0,127,0,0,0,1,0,0,0,104,0,0,0,3,0,0,0,5,0,3,0,
    98,0,0,0,4,0,0,0,3,0,3,0,3,0,3,0,3,0,0,0,3,0,3,0,
    30,0,0,0,2,0,0,0,3,0,23,0,2,0,0,0,3,0,134,0,2,0,0,0,
    4,0,3,0,5,0,0,0,5,0,3,0,98,0,98,0,98,0,0,0,3,0,0,0,
    5,0,11,0,124,0,0,0,3,0,0,0,5,0,98,0,98,0,0,0,2,0,0,0,
    5,0,143,0,3,0,0,0,9,0,11,0,124,0,0,0,2,0,0,0,12,0,3,0,
    3,0,0,0,12,0,3,0,3,0,0,0,3,0,0,0,12,0,14,0,3,0,0,0,
    1,0,0,0,19,0,0,0,1,0,0,0,29,0,0,0,2,0,0,0,30,0,3,0,
    1,0,0,0,32,0,0,0,1,0,0,0,39,0,0,0,5,0,0,0,44,0,44,0,
    36,0,45,0,35,0,0,0,2,0,0,0,49,0,133,0,2,0,0,0,55,0,133,0,
    1,0,0,0,59,0,0,0,1,0,0,0,61,0,0,0,1,0,0,0,62,0,0,0,
    1,0,0,0,63,0,0,0,1,0,0,0,64,0,0,0,1,0,0,0,65,0,0,0,
    4,0,0,0,79,0,0,0,98,0,98,0,1,0,0,0,80,0,0,0,2,0,0,0,
    80,0,98,0,1,0,0,0,81,0,0,0,2,0,0,0,85,0,86,0,1,0,0,0,
    87,0,0,0,5,0,0,0,94,0,3,0,94,0,3,0,3,0,0,0,2,0,0,0,
    95,0,98,0,1,0,0,0,102,0,0,0,3,0,0,0,112,0,25,0,19,0,0,0,
    1,0,0,0,114,0,0,0,3,0,0,0,121,0,3,0,3,0,0,0,2,0,0,0,
    121,0,120,0,2,0,0,0,121,0,133,0,2,0,0,0,124,0,12,0,2,0,0,0,
    124,0,14,0,2,0,0,0,124,0,17,0,2,0,0,0,124,0,86,0,2,0,0,0,
    124,0,133,0,2,0,0,0,127,0,98,0,6,0,0,0,129,0,107,0,3,0,98,0,
    28,0,80,0,3,0,0,0,131,0,4,0,37,0,0,0,1,0,0,0,133,0,0,0,
    2,0,0,0,133,0,3,0,2,0,0,0,134,0,105,0,1,0,0,0,135,0,0,0,
    1,0,0,0,138,0,0,0,1,0,0,0,141,0,0,0,4,0,0,0,141,0,40,0,
    36,0,46,0,2,0,0,0,143,0,3,0,3,0,0,0,3,0,3,0,35,0,0,0,
    2,0,0,0,3,0,35,0,2,0,0,0,3,0,53,0,2,0,0,0,4,0,118,0,
    1,0,0,0,11,0,0,0,1,0,0,0,14,0,0,0,2,0,0,0,38,0,3,0,
    1,0,0,0,53,0,0,0,1,0,0,0,54,0,0,0,2,0,0,0,55,0,59,0,
    2,0,0,0,87,0,3,0,2,0,0,0,95,0,4,0,2,0,0,0,124,0,4,0,
    2,0,0,0,127,0,53,0,1,0,0,0,5,0,0,0,2,0,0,0,5,0,3,0,
    1,0,0,0,105,0,0,0,2,0,0,0,138,0,3,0,2,0,0,0,139,0,3,0,
    1,0,0,0,142,0,0,0,1,192,128,0,3,40,41,86,0,3,42,47,42,0,2,44,
    32,0,1,45,0,17,45,36,36,78,101,115,116,36,102,103,101,116,97,115,107,101,100,0,
    26,45,36,36,78,101,115,116,36,102,103,101,116,99,104,97,114,97,99,116,101,114,105,115,
    116,105,99,0,21,45,36,36,78,101,115,116,36,102,103,101,116,99,111,109,112,111,115,105,
    110,103,0,21,45,36,36,78,101,115,116,36,102,103,101,116,99,111,110,110,101,99,116,101,
    100,0,18,45,36,36,78,101,115,116,36,102,103,101,116,99,111,117,110,116,115,0,18,45,
    36,36,78,101,115,116,36,102,103,101,116,102,97,105,108,101,100,0,17,45,36,36,78,101,
    115,116,36,102,103,101,116,102,108,97,103,115,0,19,45,36,36,78,101,115,116,36,102,103,
    101,116,102,111,99,117,115,101,100,0,16,45,36,36,78,101,115,116,36,102,103,101,116,103,
    97,116,116,0,16,45,36,36,78,101,115,116,36,102,103,101,116,104,101,108,100,0,15,45,
    36,36,78,101,115,116,36,102,103,101,116,105,100,115,0,19,45,36,36,78,101,115,116,36,
    102,103,101,116,105,110,100,101,120,79,102,0,18,45,36,36,78,101,115,116,36,102,103,101,
    116,108,97,98,101,108,115,0,18,45,36,36,78,101,115,116,36,102,103,101,116,112,108,97,
    99,101,115,0,18,45,36,36,78,101,115,116,36,102,103,101,116,114,97,110,103,101,115,0,
    17,45,36,36,78,101,115,116,36,102,103,101,116,114,101,99,116,115,0,17,45,36,36,78,
    101,115,116,36,102,103,101,116,114,111,108,101,115,0,18,45,36,36,78,101,115,116,36,102,
    103,101,116,115,105,103,110,97,108,0,18,45,36,36,78,101,115,116,36,102,103,101,116,115,
    111,99,107,101,116,0,18,45,36,36,78,101,115,116,36,102,103,101,116,115,116,114,101,97,
    109,0,15,45,36,36,78,101,115,116,36,102,103,101,116,116,111,112,0,15,45,36,36,78,
    101,115,116,36,102,103,101,116,117,112,115,0,18,45,36,36,78,101,115,116,36,102,103,101,
    116,118,97,108,117,101,115,0,19,45,36,36,78,101,115,116,36,102,103,101,116,118,105,114,
    116,117,97,108,0,24,45,36,36,78,101,115,116,36,102,112,117,116,97,99,107,110,111,119,
    108,101,100,103,101,100,0,17,45,36,36,78,101,115,116,36,102,112,117,116,97,115,107,101,
    100,0,26,45,36,36,78,101,115,116,36,102,112,117,116,99,104,97,114,97,99,116,101,114,
    105,115,116,105,99,0,21,45,36,36,78,101,115,116,36,102,112,117,116,99,111,109,112,111,
    115,105,110,103,0,18,45,36,36,78,101,115,116,36,102,112,117,116,102,97,105,108,101,100,
    0,19,45,36,36,78,101,115,116,36,102,112,117,116,102,111,99,117,115,101,100,0,16,45,
    36,36,78,101,115,116,36,102,112,117,116,103,97,116,116,0,18,45,36,36,78,101,115,116,
    36,102,112,117,116,115,111,99,107,101,116,0,18,45,36,36,78,101,115,116,36,102,112,117,
    116,115,116,114,101,97,109,0,15,45,36,36,78,101,115,116,36,109,97,110,115,119,101,114,
    0,14,45,36,36,78,101,115,116,36,109,97,119,97,105,116,0,18,45,36,36,78,101,115,
    116,36,109,99,108,97,115,115,78,97,109,101,0,15,45,36,36,78,101,115,116,36,109,102,
    105,110,105,115,104,0,14,45,36,36,78,101,115,116,36,109,104,111,108,100,115,0,12,45,
    36,36,78,101,115,116,36,109,107,101,121,0,14,45,36,36,78,101,115,116,36,109,112,108,
    97,105,110,0,16,45,36,36,78,101,115,116,36,109,115,101,110,100,70,111,114,0,14,45,
    36,36,78,101,115,116,36,109,115,116,97,116,101,0,14,45,36,36,78,101,115,116,36,115,
    109,99,111,112,121,0,14,45,36,36,78,101,115,116,36,115,109,110,97,109,101,0,22,45,
    36,36,78,101,115,116,36,115,109,110,97,116,105,118,101,65,99,99,101,115,115,0,28,45,
    36,36,78,101,115,116,36,115,109,110,97,116,105,118,101,65,99,99,101,115,115,65,99,116,
    105,118,101,0,19,45,36,36,78,101,115,116,36,115,109,110,97,116,105,118,101,75,101,121,
    0,22,45,36,36,78,101,115,116,36,115,109,110,97,116,105,118,101,80,105,99,107,101,100,
    0,20,45,36,36,78,101,115,116,36,115,109,110,97,116,105,118,101,84,101,120,116,0,15,
    45,36,36,78,101,115,116,36,115,109,112,97,117,115,101,0,1,46,0,2,46,46,0,4,
    46,112,100,102,0,36,48,48,48,48,49,49,48,49,45,48,48,48,48,45,49,48,48,48,
    45,56,48,48,48,45,48,48,56,48,53,70,57,66,51,52,70,66,0,36,48,48,48,48,
    49,56,70,48,45,48,48,48,48,45,49,48,48,48,45,56,48,48,48,45,48,48,56,48,
    53,70,57,66,51,52,70,66,0,36,48,48,48,48,70,70,48,48,45,48,48,48,48,45,
    49,48,48,48,45,56,48,48,48,45,48,48,56,48,53,70,57,66,51,52,70,66,0,8,
    60,99,108,105,110,105,116,62,0,6,60,105,110,105,116,62,0,2,62,59,0,7,65,67,
    84,73,79,78,83,0,26,65,67,84,73,79,78,95,65,67,67,69,83,83,73,66,73,76,
    73,84,89,95,70,79,67,85,83,0,32,65,67,84,73,79,78,95,67,76,69,65,82,95,
    65,67,67,69,83,83,73,66,73,76,73,84,89,95,70,79,67,85,83,0,12,65,67,84,
    73,79,78,95,67,76,73,67,75,0,22,65,67,84,73,79,78,95,83,67,82,79,76,76,
    95,66,65,67,75,87,65,82,68,0,21,65,67,84,73,79,78,95,83,67,82,79,76,76,
    95,70,79,82,87,65,82,68,0,9,65,76,76,95,80,65,71,69,83,0,9,66,65,67,
    75,83,80,65,67,69,0,6,66,85,84,84,79,78,0,1,67,0,9,67,65,78,67,69,
    76,76,69,68,0,4,67,69,76,76,0,8,67,72,69,67,75,66,79,88,0,7,67,72,
    69,67,75,69,68,0,7,67,76,65,83,83,69,83,0,13,67,79,76,85,77,78,95,72,
    69,65,68,69,82,0,6,67,79,77,77,73,84,0,7,67,79,77,80,79,83,69,0,10,
    67,111,110,110,101,99,116,105,111,110,0,1,68,0,18,68,56,36,36,83,121,110,116,104,
    101,116,105,99,67,108,97,115,115,0,9,68,69,67,82,69,77,69,78,84,0,6,68,69,
    78,73,69,68,0,8,68,73,83,65,66,76,69,68,0,8,68,82,79,80,68,79,87,78,
    0,36,69,55,56,49,48,65,55,49,45,55,51,65,69,45,52,57,57,68,45,56,67,49,
    53,45,70,65,65,57,65,69,70,48,67,51,70,50,0,5,69,78,84,69,82,0,6,69,
    88,80,79,82,84,0,1,70,0,6,70,65,73,76,69,68,0,4,70,73,76,69,0,5,
    70,73,76,69,83,0,5,71,82,79,85,80,0,1,73,0,2,73,70,0,2,73,73,0,
    3,73,73,73,0,2,73,76,0,4,73,76,73,73,0,3,73,76,90,0,5,73,77,65,
    71,69,0,9,73,78,67,82,69,77,69,78,84,0,1,74,0,1,76,0,2,76,73,0,
    5,76,73,70,70,70,0,3,76,73,73,0,7,76,73,73,73,73,90,90,0,4,76,73,
    73,90,0,4,76,73,83,84,0,9,76,73,83,84,95,73,84,69,77,0,13,76,73,84,
    84,76,69,95,69,78,68,73,65,78,0,2,76,74,0,2,76,76,0,3,76,76,73,0,
    4,76,76,73,76,0,3,76,76,76,0,4,76,76,76,76,0,6,76,76,76,76,76,76,
    0,3,76,76,90,0,5,76,76,90,76,73,0,4,76,79,83,84,0,2,76,90,0,22,
    76,97,110,100,114,111,105,100,47,97,112,112,47,65,99,116,105,118,105,116,121,59,0,22,
    76,97,110,100,114,111,105,100,47,97,112,112,47,70,114,97,103,109,101,110,116,59,0,29,
    76,97,110,100,114,111,105,100,47,97,112,112,47,70,114,97,103,109,101,110,116,77,97,110,
    97,103,101,114,59,0,33,76,97,110,100,114,111,105,100,47,97,112,112,47,70,114,97,103,
    109,101,110,116,84,114,97,110,115,97,99,116,105,111,110,59,0,36,76,97,110,100,114,111,
    105,100,47,98,108,117,101,116,111,111,116,104,47,66,108,117,101,116,111,111,116,104,65,100,
    97,112,116,101,114,59,0,34,76,97,110,100,114,111,105,100,47,98,108,117,101,116,111,111,
    116,104,47,66,108,117,101,116,111,111,116,104,67,108,97,115,115,59,0,35,76,97,110,100,
    114,111,105,100,47,98,108,117,101,116,111,111,116,104,47,66,108,117,101,116,111,111,116,104,
    68,101,118,105,99,101,59,0,33,76,97,110,100,114,111,105,100,47,98,108,117,101,116,111,
    111,116,104,47,66,108,117,101,116,111,111,116,104,71,97,116,116,59,0,41,76,97,110,100,
    114,111,105,100,47,98,108,117,101,116,111,111,116,104,47,66,108,117,101,116,111,111,116,104,
    71,97,116,116,67,97,108,108,98,97,99,107,59,0,47,76,97,110,100,114,111,105,100,47,
    98,108,117,101,116,111,111,116,104,47,66,108,117,101,116,111,111,116,104,71,97,116,116,67,
    104,97,114,97,99,116,101,114,105,115,116,105,99,59,0,40,76,97,110,100,114,111,105,100,
    47,98,108,117,101,116,111,111,116,104,47,66,108,117,101,116,111,111,116,104,71,97,116,116,
    83,101,114,118,105,99,101,59,0,36,76,97,110,100,114,111,105,100,47,98,108,117,101,116,
    111,111,116,104,47,66,108,117,101,116,111,111,116,104,77,97,110,97,103,101,114,59,0,35,
    76,97,110,100,114,111,105,100,47,98,108,117,101,116,111,111,116,104,47,66,108,117,101,116,
    111,111,116,104,83,111,99,107,101,116,59,0,41,76,97,110,100,114,111,105,100,47,98,108,
    117,101,116,111,111,116,104,47,108,101,47,66,108,117,101,116,111,111,116,104,76,101,83,99,
    97,110,110,101,114,59,0,35,76,97,110,100,114,111,105,100,47,98,108,117,101,116,111,111,
    116,104,47,108,101,47,83,99,97,110,67,97,108,108,98,97,99,107,59,0,41,76,97,110,
    100,114,111,105,100,47,98,108,117,101,116,111,111,116,104,47,108,101,47,83,99,97,110,70,
    105,108,116,101,114,36,66,117,105,108,100,101,114,59,0,33,76,97,110,100,114,111,105,100,
    47,98,108,117,101,116,111,111,116,104,47,108,101,47,83,99,97,110,70,105,108,116,101,114,
    59,0,33,76,97,110,100,114,111,105,100,47,98,108,117,101,116,111,111,116,104,47,108,101,
    47,83,99,97,110,82,101,99,111,114,100,59,0,33,76,97,110,100,114,111,105,100,47,98,
    108,117,101,116,111,111,116,104,47,108,101,47,83,99,97,110,82,101,115,117,108,116,59,0,
    43,76,97,110,100,114,111,105,100,47,98,108,117,101,116,111,111,116,104,47,108,101,47,83,
    99,97,110,83,101,116,116,105,110,103,115,36,66,117,105,108,100,101,114,59,0,35,76,97,
    110,100,114,111,105,100,47,98,108,117,101,116,111,111,116,104,47,108,101,47,83,99,97,110,
    83,101,116,116,105,110,103,115,59,0,31,76,97,110,100,114,111,105,100,47,99,111,110,116,
    101,110,116,47,67,108,105,112,68,97,116,97,36,73,116,101,109,59,0,26,76,97,110,100,
    114,111,105,100,47,99,111,110,116,101,110,116,47,67,108,105,112,68,97,116,97,59,0,33,
    76,97,110,100,114,111,105,100,47,99,111,110,116,101,110,116,47,67,111,110,116,101,110,116,
    82,101,115,111,108,118,101,114,59,0,25,76,97,110,100,114,111,105,100,47,99,111,110,116,
    101,110,116,47,67,111,110,116,101,120,116,59,0,24,76,97,110,100,114,111,105,100,47,99,
    111,110,116,101,110,116,47,73,110,116,101,110,116,59,0,25,76,97,110,100,114,111,105,100,
    47,100,97,116,97,98,97,115,101,47,67,117,114,115,111,114,59,0,23,76,97,110,100,114,
    111,105,100,47,103,114,97,112,104,105,99,115,47,82,101,99,116,59,0,17,76,97,110,100,
    114,111,105,100,47,110,101,116,47,85,114,105,59,0,26,76,97,110,100,114,111,105,100,47,
    111,115,47,66,117,105,108,100,36,86,69,82,83,73,79,78,59,0,19,76,97,110,100,114,
    111,105,100,47,111,115,47,66,117,110,100,108,101,59,0,31,76,97,110,100,114,111,105,100,
    47,111,115,47,67,97,110,99,101,108,108,97,116,105,111,110,83,105,103,110,97,108,59,0,
    20,76,97,110,100,114,111,105,100,47,111,115,47,72,97,110,100,108,101,114,59,0,20,76,
    97,110,100,114,111,105,100,47,111,115,47,73,66,105,110,100,101,114,59,0,19,76,97,110,
    100,114,111,105,100,47,111,115,47,76,111,111,112,101,114,59,0,33,76,97,110,100,114,111,
    105,100,47,111,115,47,80,97,114,99,101,108,70,105,108,101,68,101,115,99,114,105,112,116,
    111,114,59,0,23,76,97,110,100,114,111,105,100,47,111,115,47,80,97,114,99,101,108,85,
    117,105,100,59,0,24,76,97,110,100,114,111,105,100,47,111,115,47,83,121,115,116,101,109,
    67,108,111,99,107,59,0,25,76,97,110,100,114,111,105,100,47,112,114,105,110,116,47,80,
    97,103,101,82,97,110,103,101,59,0,31,76,97,110,100,114,111,105,100,47,112,114,105,110,
    116,47,80,114,105,110,116,65,116,116,114,105,98,117,116,101,115,59,0,57,76,97,110,100,
    114,111,105,100,47,112,114,105,110,116,47,80,114,105,110,116,68,111,99,117,109,101,110,116,
    65,100,97,112,116,101,114,36,76,97,121,111,117,116,82,101,115,117,108,116,67,97,108,108,
    98,97,99,107,59,0,56,76,97,110,100,114,111,105,100,47,112,114,105,110,116,47,80,114,
    105,110,116,68,111,99,117,109,101,110,116,65,100,97,112,116,101,114,36,87,114,105,116,101,
    82,101,115,117,108,116,67,97,108,108,98,97,99,107,59,0,36,76,97,110,100,114,111,105,
    100,47,112,114,105,110,116,47,80,114,105,110,116,68,111,99,117,109,101,110,116,65,100,97,
    112,116,101,114,59,0,41,76,97,110,100,114,111,105,100,47,112,114,105,110,116,47,80,114,
    105,110,116,68,111,99,117,109,101,110,116,73,110,102,111,36,66,117,105,108,100,101,114,59,
    0,33,76,97,110,100,114,111,105,100,47,112,114,105,110,116,47,80,114,105,110,116,68,111,
    99,117,109,101,110,116,73,110,102,111,59,0,24,76,97,110,100,114,111,105,100,47,112,114,
    105,110,116,47,80,114,105,110,116,74,111,98,59,0,28,76,97,110,100,114,111,105,100,47,
    112,114,105,110,116,47,80,114,105,110,116,74,111,98,73,110,102,111,59,0,28,76,97,110,
    100,114,111,105,100,47,112,114,105,110,116,47,80,114,105,110,116,77,97,110,97,103,101,114,
    59,0,23,76,97,110,100,114,111,105,100,47,118,105,101,119,47,75,101,121,69,118,101,110,
    116,59,0,26,76,97,110,100,114,111,105,100,47,118,105,101,119,47,77,111,116,105,111,110,
    69,118,101,110,116,59,0,19,76,97,110,100,114,111,105,100,47,118,105,101,119,47,86,105,
    101,119,59,0,25,76,97,110,100,114,111,105,100,47,118,105,101,119,47,86,105,101,119,80,
    97,114,101,110,116,59,0,21,76,97,110,100,114,111,105,100,47,118,105,101,119,47,87,105,
    110,100,111,119,59,0,27,76,97,110,100,114,111,105,100,47,118,105,101,119,47,87,105,110,
    100,111,119,73,110,115,101,116,115,59,0,47,76,97,110,100,114,111,105,100,47,118,105,101,
    119,47,97,99,99,101,115,115,105,98,105,108,105,116,121,47,65,99,99,101,115,115,105,98,
    105,108,105,116,121,69,118,101,110,116,59,0,49,76,97,110,100,114,111,105,100,47,118,105,
    101,119,47,97,99,99,101,115,115,105,98,105,108,105,116,121,47,65,99,99,101,115,115,105,
    98,105,108,105,116,121,77,97,110,97,103,101,114,59,0,70,76,97,110,100,114,111,105,100,
    47,118,105,101,119,47,97,99,99,101,115,115,105,98,105,108,105,116,121,47,65,99,99,101,
    115,115,105,98,105,108,105,116,121,78,111,100,101,73,110,102,111,36,65,99,99,101,115,115,
    105,98,105,108,105,116,121,65,99,116,105,111,110,59,0,65,76,97,110,100,114,111,105,100,
    47,118,105,101,119,47,97,99,99,101,115,115,105,98,105,108,105,116,121,47,65,99,99,101,
    115,115,105,98,105,108,105,116,121,78,111,100,101,73,110,102,111,36,67,111,108,108,101,99,
    116,105,111,110,73,110,102,111,59,0,69,76,97,110,100,114,111,105,100,47,118,105,101,119,
    47,97,99,99,101,115,115,105,98,105,108,105,116,121,47,65,99,99,101,115,115,105,98,105,
    108,105,116,121,78,111,100,101,73,110,102,111,36,67,111,108,108,101,99,116,105,111,110,73,
    116,101,109,73,110,102,111,59,0,60,76,97,110,100,114,111,105,100,47,118,105,101,119,47,
    97,99,99,101,115,115,105,98,105,108,105,116,121,47,65,99,99,101,115,115,105,98,105,108,
    105,116,121,78,111,100,101,73,110,102,111,36,82,97,110,103,101,73,110,102,111,59,0,50,
    76,97,110,100,114,111,105,100,47,118,105,101,119,47,97,99,99,101,115,115,105,98,105,108,
    105,116,121,47,65,99,99,101,115,115,105,98,105,108,105,116,121,78,111,100,101,73,110,102,
    111,59,0,54,76,97,110,100,114,111,105,100,47,118,105,101,119,47,97,99,99,101,115,115,
    105,98,105,108,105,116,121,47,65,99,99,101,115,115,105,98,105,108,105,116,121,78,111,100,
    101,80,114,111,118,105,100,101,114,59,0,46,76,97,110,100,114,111,105,100,47,118,105,101,
    119,47,105,110,112,117,116,109,101,116,104,111,100,47,66,97,115,101,73,110,112,117,116,67,
    111,110,110,101,99,116,105,111,110,59,0,37,76,97,110,100,114,111,105,100,47,118,105,101,
    119,47,105,110,112,117,116,109,101,116,104,111,100,47,69,100,105,116,111,114,73,110,102,111,
    59,0,42,76,97,110,100,114,111,105,100,47,118,105,101,119,47,105,110,112,117,116,109,101,
    116,104,111,100,47,73,110,112,117,116,67,111,110,110,101,99,116,105,111,110,59,0,45,76,
    97,110,100,114,111,105,100,47,118,105,101,119,47,105,110,112,117,116,109,101,116,104,111,100,
    47,73,110,112,117,116,77,101,116,104,111,100,77,97,110,97,103,101,114,59,0,28,76,97,
    110,100,114,111,105,100,47,119,101,98,107,105,116,47,77,105,109,101,84,121,112,101,77,97,
    112,59,0,34,76,100,97,108,118,105,107,47,97,110,110,111,116,97,116,105,111,110,47,69,
    110,99,108,111,115,105,110,103,67,108,97,115,115,59,0,35,76,100,97,108,118,105,107,47,
    97,110,110,111,116,97,116,105,111,110,47,69,110,99,108,111,115,105,110,103,77,101,116,104,
    111,100,59,0,30,76,100,97,108,118,105,107,47,97,110,110,111,116,97,116,105,111,110,47,
    73,110,110,101,114,67,108,97,115,115,59,0,33,76,100,97,108,118,105,107,47,97,110,110,
    111,116,97,116,105,111,110,47,77,101,109,98,101,114,67,108,97,115,115,101,115,59,0,36,
    76,100,97,108,118,105,107,47,97,110,110,111,116,97,116,105,111,110,47,77,101,116,104,111,
    100,80,97,114,97,109,101,116,101,114,115,59,0,29,76,100,97,108,118,105,107,47,97,110,
    110,111,116,97,116,105,111,110,47,83,105,103,110,97,116,117,114,101,59,0,26,76,100,97,
    108,118,105,107,47,97,110,110,111,116,97,116,105,111,110,47,84,104,114,111,119,115,59,0,
    4,76,105,110,107,0,31,76,106,97,118,97,47,105,111,47,66,121,116,101,65,114,114,97,
    121,79,117,116,112,117,116,83,116,114,101,97,109,59,0,14,76,106,97,118,97,47,105,111,
    47,70,105,108,101,59,0,24,76,106,97,118,97,47,105,111,47,70,105,108,101,68,101,115,
    99,114,105,112,116,111,114,59,0,25,76,106,97,118,97,47,105,111,47,70,105,108,101,73,
    110,112,117,116,83,116,114,101,97,109,59,0,26,76,106,97,118,97,47,105,111,47,70,105,
    108,101,79,117,116,112,117,116,83,116,114,101,97,109,59,0,21,76,106,97,118,97,47,105,
    111,47,73,79,69,120,99,101,112,116,105,111,110,59,0,21,76,106,97,118,97,47,105,111,
    47,73,110,112,117,116,83,116,114,101,97,109,59,0,22,76,106,97,118,97,47,105,111,47,
    79,117,116,112,117,116,83,116,114,101,97,109,59,0,24,76,106,97,118,97,47,108,97,110,
    103,47,67,104,97,114,83,101,113,117,101,110,99,101,59,0,21,76,106,97,118,97,47,108,
    97,110,103,47,67,104,97,114,97,99,116,101,114,59,0,21,76,106,97,118,97,47,108,97,
    110,103,47,69,120,99,101,112,116,105,111,110,59,0,19,76,106,97,118,97,47,108,97,110,
    103,47,73,110,116,101,103,101,114,59,0,32,76,106,97,118,97,47,108,97,110,103,47,73,
    110,116,101,114,114,117,112,116,101,100,69,120,99,101,112,116,105,111,110,59,0,16,76,106,
    97,118,97,47,108,97,110,103,47,76,111,110,103,59,0,16,76,106,97,118,97,47,108,97,
    110,103,47,77,97,116,104,59,0,18,76,106,97,118,97,47,108,97,110,103,47,79,98,106,
    101,99,116,59,0,20,76,106,97,118,97,47,108,97,110,103,47,82,117,110,110,97,98,108,
    101,59,0,28,76,106,97,118,97,47,108,97,110,103,47,82,117,110,116,105,109,101,69,120,
    99,101,112,116,105,111,110,59,0,29,76,106,97,118,97,47,108,97,110,103,47,83,101,99,
    117,114,105,116,121,69,120,99,101,112,116,105,111,110,59,0,18,76,106,97,118,97,47,108,
    97,110,103,47,83,116,114,105,110,103,59,0,25,76,106,97,118,97,47,108,97,110,103,47,
    83,116,114,105,110,103,66,117,105,108,100,101,114,59,0,18,76,106,97,118,97,47,108,97,
    110,103,47,83,121,115,116,101,109,59,0,18,76,106,97,118,97,47,108,97,110,103,47,84,
    104,114,101,97,100,59,0,21,76,106,97,118,97,47,108,97,110,103,47,84,104,114,111,119,
    97,98,108,101,59,0,21,76,106,97,118,97,47,110,105,111,47,66,121,116,101,66,117,102,
    102,101,114,59,0,20,76,106,97,118,97,47,110,105,111,47,66,121,116,101,79,114,100,101,
    114,59,0,26,76,106,97,118,97,47,110,105,111,47,99,104,97,114,115,101,116,47,67,104,
    97,114,115,101,116,59,0,35,76,106,97,118,97,47,110,105,111,47,99,104,97,114,115,101,
    116,47,83,116,97,110,100,97,114,100,67,104,97,114,115,101,116,115,59,0,21,76,106,97,
    118,97,47,117,116,105,108,47,65,114,114,97,121,76,105,115,116,59,0,18,76,106,97,118,
    97,47,117,116,105,108,47,65,114,114,97,121,115,59,0,19,76,106,97,118,97,47,117,116,
    105,108,47,72,97,115,104,77,97,112,59,0,19,76,106,97,118,97,47,117,116,105,108,47,
    72,97,115,104,77,97,112,60,0,20,76,106,97,118,97,47,117,116,105,108,47,73,116,101,
    114,97,116,111,114,59,0,25,76,106,97,118,97,47,117,116,105,108,47,76,105,110,107,101,
    100,72,97,115,104,77,97,112,59,0,16,76,106,97,118,97,47,117,116,105,108,47,76,105,
    115,116,59,0,21,76,106,97,118,97,47,117,116,105,108,47,77,97,112,36,69,110,116,114,
    121,59,0,15,76,106,97,118,97,47,117,116,105,108,47,77,97,112,59,0,15,76,106,97,
    118,97,47,117,116,105,108,47,83,101,116,59,0,16,76,106,97,118,97,47,117,116,105,108,
    47,85,85,73,68,59,0,32,76,106,97,118,97,47,117,116,105,108,47,99,111,110,99,117,
    114,114,101,110,116,47,83,101,109,97,112,104,111,114,101,59,0,31,76,106,97,118,97,47,
    117,116,105,108,47,99,111,110,99,117,114,114,101,110,116,47,84,105,109,101,85,110,105,116,
    59,0,40,76,111,114,103,47,99,101,101,115,97,102,101,47,119,105,110,100,111,119,47,65,
    99,99,101,115,115,86,105,101,119,36,80,114,111,118,105,100,101,114,59,0,34,76,111,114,
    103,47,99,101,101,115,97,102,101,47,119,105,110,100,111,119,47,65,99,99,101,115,115,86,
    105,101,119,45,73,65,59,0,31,76,111,114,103,47,99,101,101,115,97,102,101,47,119,105,
    110,100,111,119,47,65,99,99,101,115,115,86,105,101,119,59,0,32,76,111,114,103,47,99,
    101,101,115,97,102,101,47,119,105,110,100,111,119,47,66,108,117,101,116,111,111,116,104,36,
    48,59,0,32,76,111,114,103,47,99,101,101,115,97,102,101,47,119,105,110,100,111,119,47,
    66,108,117,101,116,111,111,116,104,36,49,59,0,35,76,111,114,103,47,99,101,101,115,97,
    102,101,47,119,105,110,100,111,119,47,66,108,117,101,116,111,111,116,104,36,76,105,110,107,
    59,0,30,76,111,114,103,47,99,101,101,115,97,102,101,47,119,105,110,100,111,119,47,66,
    108,117,101,116,111,111,116,104,59,0,41,76,111,114,103,47,99,101,101,115,97,102,101,47,
    119,105,110,100,111,119,47,73,110,112,117,116,86,105,101,119,36,67,111,110,110,101,99,116,
    105,111,110,59,0,30,76,111,114,103,47,99,101,101,115,97,102,101,47,119,105,110,100,111,
    119,47,73,110,112,117,116,86,105,101,119,59,0,29,76,111,114,103,47,99,101,101,115,97,
    102,101,47,119,105,110,100,111,119,47,80,105,99,107,101,114,36,49,59,0,27,76,111,114,
    103,47,99,101,101,115,97,102,101,47,119,105,110,100,111,119,47,80,105,99,107,101,114,59,
    0,31,76,111,114,103,47,99,101,101,115,97,102,101,47,119,105,110,100,111,119,47,80,114,
    105,110,116,105,110,103,36,49,59,0,29,76,111,114,103,47,99,101,101,115,97,102,101,47,
    119,105,110,100,111,119,47,80,114,105,110,116,105,110,103,59,0,4,77,69,78,85,0,8,
    77,69,78,85,95,66,65,82,0,9,77,69,78,85,95,73,84,69,77,0,12,77,73,76,
    76,73,83,69,67,79,78,68,83,0,9,78,79,84,95,70,79,85,78,68,0,3,79,70,
    70,0,5,80,82,69,83,83,0,5,80,82,73,78,84,0,7,80,82,73,78,84,69,68,
    0,8,80,114,111,118,105,100,101,114,0,5,82,65,68,73,79,0,7,82,69,70,85,83,
    69,68,0,3,82,79,87,0,5,83,65,86,69,68,0,7,83,68,75,95,73,78,84,0,
    6,83,69,67,85,82,69,0,8,83,69,76,69,67,84,69,68,0,8,83,69,82,86,73,
    67,69,83,0,9,83,69,84,84,76,69,95,77,83,0,6,83,76,73,68,69,82,0,3,
    83,80,80,0,3,84,65,66,0,5,84,65,66,76,69,0,8,84,65,66,95,76,73,83,
    84,0,3,84,65,71,0,4,84,69,88,84,0,10,84,69,88,84,95,70,73,69,76,68,
    0,5,84,89,80,69,83,0,11,85,78,83,85,80,80,79,82,84,69,68,0,5,85,84,
    70,95,56,0,1,86,0,2,86,73,0,3,86,73,73,0,5,86,73,73,73,73,0,4,
    86,73,73,76,0,3,86,73,76,0,2,86,74,0,3,86,74,73,0,2,86,76,0,5,
    86,76,67,76,76,0,3,86,76,73,0,4,86,76,73,73,0,6,86,76,73,76,73,73,
    0,6,86,76,73,76,76,76,0,4,86,76,74,76,0,3,86,76,76,0,4,86,76,76,
    73,0,7,86,76,76,73,76,76,76,0,4,86,76,76,76,0,5,86,76,76,76,76,0,
    6,86,76,76,76,76,76,0,3,86,76,90,0,2,86,90,0,3,86,90,73,0,1,90,
    0,2,90,73,0,3,90,73,73,0,4,90,73,73,76,0,3,90,73,76,0,2,90,74,
    0,3,90,74,76,0,2,90,76,0,3,90,76,73,0,3,90,76,74,0,3,90,76,76,
    0,5,91,44,32,93,43,0,2,91,66,0,2,91,67,0,2,91,68,0,2,91,70,0,
    2,91,73,0,2,91,74,0,24,91,76,97,110,100,114,111,105,100,47,111,115,47,80,97,
    114,99,101,108,85,117,105,100,59,0,26,91,76,97,110,100,114,111,105,100,47,112,114,105,
    110,116,47,80,97,103,101,82,97,110,103,101,59,0,19,91,76,106,97,118,97,47,108,97,
    110,103,47,79,98,106,101,99,116,59,0,19,91,76,106,97,118,97,47,108,97,110,103,47,
    83,116,114,105,110,103,59,0,17,91,76,106,97,118,97,47,117,116,105,108,47,85,85,73,
    68,59,0,3,91,91,73,0,13,95,100,105,115,112,108,97,121,95,110,97,109,101,0,11,
    97,99,99,101,115,115,70,108,97,103,115,0,13,97,99,99,101,115,115,105,98,105,108,105,
    116,121,0,12,97,99,107,110,111,119,108,101,100,103,101,100,0,6,97,99,116,105,111,110,
    0,7,97,100,97,112,116,101,114,0,3,97,100,100,0,9,97,100,100,65,99,116,105,111,
    110,0,11,97,100,100,67,97,116,101,103,111,114,121,0,8,97,100,100,67,104,105,108,100,
    0,13,97,100,100,83,117,112,112,114,101,115,115,101,100,0,37,97,110,100,114,111,105,100,
    46,105,110,116,101,110,116,46,97,99,116,105,111,110,46,67,82,69,65,84,69,95,68,79,
    67,85,77,69,78,84,0,35,97,110,100,114,111,105,100,46,105,110,116,101,110,116,46,97,
    99,116,105,111,110,46,79,80,69,78,95,68,79,67,85,77,69,78,84,0,32,97,110,100,
    114,111,105,100,46,105,110,116,101,110,116,46,99,97,116,101,103,111,114,121,46,79,80,69,
    78,65,66,76,69,0,35,97,110,100,114,111,105,100,46,105,110,116,101,110,116,46,101,120,
    116,114,97,46,65,76,76,79,87,95,77,85,76,84,73,80,76,69,0,31,97,110,100,114,
    111,105,100,46,105,110,116,101,110,116,46,101,120,116,114,97,46,77,73,77,69,95,84,89,
    80,69,83,0,26,97,110,100,114,111,105,100,46,105,110,116,101,110,116,46,101,120,116,114,
    97,46,84,73,84,76,69,0,39,97,110,100,114,111,105,100,46,112,101,114,109,105,115,115,
    105,111,110,46,65,67,67,69,83,83,95,70,73,78,69,95,76,79,67,65,84,73,79,78,
    0,36,97,110,100,114,111,105,100,46,112,101,114,109,105,115,115,105,111,110,46,66,76,85,
    69,84,79,79,84,72,95,67,79,78,78,69,67,84,0,33,97,110,100,114,111,105,100,46,
    112,101,114,109,105,115,115,105,111,110,46,66,76,85,69,84,79,79,84,72,95,83,67,65,
    78,0,22,97,110,100,114,111,105,100,46,118,105,101,119,46,86,105,101,119,71,114,111,117,
    112,0,21,97,110,100,114,111,105,100,46,119,105,100,103,101,116,46,66,117,116,116,111,110,
    0,23,97,110,100,114,111,105,100,46,119,105,100,103,101,116,46,67,104,101,99,107,66,111,
    120,0,23,97,110,100,114,111,105,100,46,119,105,100,103,101,116,46,69,100,105,116,84,101,
    120,116,0,23,97,110,100,114,111,105,100,46,119,105,100,103,101,116,46,71,114,105,100,86,
    105,101,119,0,24,97,110,100,114,111,105,100,46,119,105,100,103,101,116,46,73,109,97,103,
    101,86,105,101,119,0,23,97,110,100,114,111,105,100,46,119,105,100,103,101,116,46,76,105,
    115,116,86,105,101,119,0,26,97,110,100,114,111,105,100,46,119,105,100,103,101,116,46,82,
    97,100,105,111,66,117,116,116,111,110,0,22,97,110,100,114,111,105,100,46,119,105,100,103,
    101,116,46,83,101,101,107,66,97,114,0,22,97,110,100,114,111,105,100,46,119,105,100,103,
    101,116,46,83,112,105,110,110,101,114,0,23,97,110,100,114,111,105,100,46,119,105,100,103,
    101,116,46,84,101,120,116,86,105,101,119,0,8,97,110,110,111,117,110,99,101,0,24,97,
    110,110,111,117,110,99,101,70,111,114,65,99,99,101,115,115,105,98,105,108,105,116,121,0,
    6,97,110,115,119,101,114,0,8,97,110,115,119,101,114,101,100,0,6,97,112,112,101,110,
    100,0,24,97,112,112,108,105,99,97,116,105,111,110,47,111,99,116,101,116,45,115,116,114,
    101,97,109,0,5,97,112,112,108,121,0,9,97,114,114,97,121,99,111,112,121,0,5,97,
    115,107,101,100,0,5,97,119,97,105,116,0,16,98,101,103,105,110,84,114,97,110,115,97,
    99,116,105,111,110,0,9,98,108,117,101,116,111,111,116,104,0,5,98,117,105,108,100,0,
    15,99,97,110,99,101,108,68,105,115,99,111,118,101,114,121,0,14,99,101,101,115,97,102,
    101,45,112,105,99,107,101,100,0,14,99,101,101,115,97,102,101,45,112,105,99,107,101,114,
    0,14,99,104,97,114,97,99,116,101,114,105,115,116,105,99,0,21,99,104,101,99,107,66,
    108,117,101,116,111,111,116,104,65,100,100,114,101,115,115,0,19,99,104,101,99,107,83,101,
    108,102,80,101,114,109,105,115,115,105,111,110,0,9,99,108,97,115,115,78,97,109,101,0,
    5,99,108,101,97,114,0,5,99,108,111,115,101,0,23,99,111,109,109,105,116,65,108,108,
    111,119,105,110,103,83,116,97,116,101,76,111,115,115,0,10,99,111,109,109,105,116,84,101,
    120,116,0,9,99,111,109,112,111,115,105,110,103,0,7,99,111,110,110,101,99,116,0,11,
    99,111,110,110,101,99,116,71,97,116,116,0,9,99,111,110,110,101,99,116,101,100,0,11,
    99,111,110,116,97,105,110,115,75,101,121,0,4,99,111,112,121,0,6,99,111,112,121,79,
    102,0,6,99,111,117,110,116,115,0,27,99,114,101,97,116,101,65,99,99,101,115,115,105,
    98,105,108,105,116,121,78,111,100,101,73,110,102,111,0,41,99,114,101,97,116,101,73,110,
    115,101,99,117,114,101,82,102,99,111,109,109,83,111,99,107,101,116,84,111,83,101,114,118,
    105,99,101,82,101,99,111,114,100,0,33,99,114,101,97,116,101,82,102,99,111,109,109,83,
    111,99,107,101,116,84,111,83,101,114,118,105,99,101,82,101,99,111,114,100,0,13,99,117,
    114,114,101,110,116,84,104,114,101,97,100,0,17,99,117,114,114,101,110,116,84,105,109,101,
    77,105,108,108,105,115,0,21,100,101,108,101,116,101,83,117,114,114,111,117,110,100,105,110,
    103,84,101,120,116,0,10,100,105,115,99,111,110,110,101,99,116,0,16,100,105,115,99,111,
    118,101,114,83,101,114,118,105,99,101,115,0,18,100,105,115,112,97,116,99,104,72,111,118,
    101,114,69,118,101,110,116,0,8,100,111,99,117,109,101,110,116,0,12,100,114,97,105,110,
    80,101,114,109,105,116,115,0,7,101,110,97,98,108,101,100,0,8,101,110,100,115,87,105,
    116,104,0,8,101,110,116,114,121,83,101,116,0,6,101,113,117,97,108,115,0,5,101,114,
    114,111,114,0,26,101,120,101,99,117,116,101,80,101,110,100,105,110,103,84,114,97,110,115,
    97,99,116,105,111,110,115,0,9,101,120,112,111,114,116,105,110,103,0,3,102,36,48,0,
    3,102,36,49,0,6,102,97,105,108,101,100,0,7,102,97,105,108,117,114,101,0,4,102,
    105,108,101,0,17,102,105,110,100,70,114,97,103,109,101,110,116,66,121,84,97,103,0,6,
    102,105,110,105,115,104,0,19,102,105,110,105,115,104,67,111,109,112,111,115,105,110,103,84,
    101,120,116,0,5,102,108,97,103,115,0,5,102,108,117,115,104,0,7,102,111,99,117,115,
    101,100,0,10,102,114,111,109,83,116,114,105,110,103,0,4,103,97,116,116,0,3,103,101,
    116,0,15,103,101,116,65,98,115,111,108,117,116,101,80,97,116,104,0,28,103,101,116,65,
    99,99,101,115,115,105,98,105,108,105,116,121,78,111,100,101,80,114,111,118,105,100,101,114,
    0,9,103,101,116,65,99,116,105,111,110,0,11,103,101,116,65,99,116,105,118,105,116,121,
    0,10,103,101,116,65,100,97,112,116,101,114,0,10,103,101,116,65,100,100,114,101,115,115,
    0,17,103,101,116,66,108,117,101,116,111,111,116,104,67,108,97,115,115,0,21,103,101,116,
    66,108,117,101,116,111,111,116,104,76,101,83,99,97,110,110,101,114,0,12,103,101,116,66,
    111,110,100,83,116,97,116,101,0,16,103,101,116,66,111,110,100,101,100,68,101,118,105,99,
    101,115,0,8,103,101,116,66,121,116,101,115,0,11,103,101,116,67,97,99,104,101,68,105,
    114,0,18,103,101,116,67,104,97,114,97,99,116,101,114,105,115,116,105,99,115,0,11,103,
    101,116,67,108,105,112,68,97,116,97,0,18,103,101,116,67,111,110,116,101,110,116,82,101,
    115,111,108,118,101,114,0,10,103,101,116,67,111,110,116,101,120,116,0,7,103,101,116,68,
    97,116,97,0,9,103,101,116,68,101,118,105,99,101,0,13,103,101,116,68,101,118,105,99,
    101,78,97,109,101,0,9,103,101,116,68,111,117,98,108,101,0,17,103,101,116,70,105,108,
    101,68,101,115,99,114,105,112,116,111,114,0,8,103,101,116,70,108,111,97,116,0,18,103,
    101,116,70,114,97,103,109,101,110,116,77,97,110,97,103,101,114,0,7,103,101,116,73,110,
    102,111,0,6,103,101,116,73,110,116,0,9,103,101,116,73,116,101,109,65,116,0,12,103,
    101,116,73,116,101,109,67,111,117,110,116,0,6,103,101,116,75,101,121,0,10,103,101,116,
    75,101,121,67,111,100,101,0,18,103,101,116,76,97,115,116,80,97,116,104,83,101,103,109,
    101,110,116,0,19,103,101,116,76,111,99,97,116,105,111,110,79,110,83,99,114,101,101,110,
    0,7,103,101,116,76,111,110,103,0,13,103,101,116,77,97,105,110,76,111,111,112,101,114,
    0,19,103,101,116,77,97,106,111,114,68,101,118,105,99,101,67,108,97,115,115,0,10,103,
    101,116,77,101,115,115,97,103,101,0,24,103,101,116,77,105,109,101,84,121,112,101,70,114,
    111,109,69,120,116,101,110,115,105,111,110,0,7,103,101,116,78,97,109,101,0,15,103,101,
    116,79,117,116,112,117,116,83,116,114,101,97,109,0,14,103,101,116,80,97,99,107,97,103,
    101,78,97,109,101,0,9,103,101,116,80,97,114,101,110,116,0,13,103,101,116,80,114,111,
    112,101,114,116,105,101,115,0,15,103,101,116,82,101,109,111,116,101,68,101,118,105,99,101,
    0,13,103,101,116,83,99,97,110,82,101,99,111,114,100,0,11,103,101,116,83,101,114,118,
    105,99,101,115,0,12,103,101,116,83,105,110,103,108,101,116,111,110,0,8,103,101,116,83,
    116,97,116,101,0,9,103,101,116,83,116,114,105,110,103,0,16,103,101,116,83,121,115,116,
    101,109,83,101,114,118,105,99,101,0,7,103,101,116,84,121,112,101,0,14,103,101,116,85,
    110,105,99,111,100,101,67,104,97,114,0,6,103,101,116,85,114,105,0,7,103,101,116,85,
    117,105,100,0,8,103,101,116,85,117,105,100,115,0,8,103,101,116,86,97,108,117,101,0,
    9,103,101,116,87,105,110,100,111,119,0,14,103,101,116,87,105,110,100,111,119,84,111,107,
    101,110,0,4,103,101,116,88,0,4,103,101,116,89,0,7,103,114,97,110,116,101,100,0,
    7,104,97,115,78,101,120,116,0,14,104,97,115,87,105,110,100,111,119,70,111,99,117,115,
    0,4,104,101,108,100,0,23,104,105,100,101,83,111,102,116,73,110,112,117,116,70,114,111,
    109,87,105,110,100,111,119,0,5,104,111,108,100,115,0,7,104,111,118,101,114,101,100,0,
    3,105,100,115,0,10,105,109,101,79,112,116,105,111,110,115,0,7,105,110,100,101,120,79,
    102,0,13,105,110,105,116,105,97,108,83,101,108,69,110,100,0,15,105,110,105,116,105,97,
    108,83,101,108,83,116,97,114,116,0,9,105,110,112,117,116,84,121,112,101,0,12,105,110,
    112,117,116,95,109,101,116,104,111,100,0,8,105,110,116,86,97,108,117,101,0,9,105,110,
    116,101,114,114,117,112,116,0,10,105,110,118,97,108,105,100,97,116,101,0,10,105,115,67,
    97,110,99,101,108,101,100,0,7,105,115,69,109,112,116,121,0,9,105,115,69,110,97,98,
    108,101,100,0,5,105,115,80,100,102,0,8,105,116,101,114,97,116,111,114,0,3,106,111,
    98,0,3,107,101,121,0,6,108,97,98,101,108,115,0,15,108,97,109,98,100,97,36,112,
    101,114,109,105,116,36,48,0,11,108,97,115,116,73,110,100,101,120,79,102,0,6,108,97,
    121,101,114,115,0,6,108,101,110,103,116,104,0,3,109,97,120,0,4,109,105,109,101,0,
    5,109,105,109,101,115,0,3,109,105,110,0,6,109,107,100,105,114,115,0,11,109,111,118,
    101,84,111,70,105,114,115,116,0,8,109,121,76,111,111,112,101,114,0,4,110,97,109,101,
    0,5,110,97,109,101,115,0,8,110,97,110,111,84,105,109,101,0,12,110,97,116,105,118,
    101,65,99,99,101,115,115,0,18,110,97,116,105,118,101,65,99,99,101,115,115,65,99,116,
    105,118,101,0,12,110,97,116,105,118,101,73,110,115,101,116,115,0,9,110,97,116,105,118,
    101,75,101,121,0,12,110,97,116,105,118,101,80,105,99,107,101,100,0,10,110,97,116,105,
    118,101,84,101,120,116,0,6,110,101,101,100,101,100,0,4,110,101,120,116,0,6,111,98,
    116,97,105,110,0,6,111,102,102,115,101,116,0,18,111,102,102,115,101,116,66,121,67,111,
    100,101,80,111,105,110,116,115,0,16,111,110,65,99,116,105,118,105,116,121,82,101,115,117,
    108,116,0,19,111,110,65,112,112,108,121,87,105,110,100,111,119,73,110,115,101,116,115,0,
    21,111,110,67,104,97,114,97,99,116,101,114,105,115,116,105,99,87,114,105,116,101,0,19,
    111,110,67,104,101,99,107,73,115,84,101,120,116,69,100,105,116,111,114,0,23,111,110,67,
    111,110,110,101,99,116,105,111,110,83,116,97,116,101,67,104,97,110,103,101,0,23,111,110,
    67,114,101,97,116,101,73,110,112,117,116,67,111,110,110,101,99,116,105,111,110,0,8,111,
    110,70,105,110,105,115,104,0,30,111,110,73,110,105,116,105,97,108,105,122,101,65,99,99,
    101,115,115,105,98,105,108,105,116,121,69,118,101,110,116,0,33,111,110,73,110,105,116,105,
    97,108,105,122,101,65,99,99,101,115,115,105,98,105,108,105,116,121,78,111,100,101,73,110,
    102,111,0,9,111,110,75,101,121,68,111,119,110,0,8,111,110,76,97,121,111,117,116,0,
    17,111,110,76,97,121,111,117,116,67,97,110,99,101,108,108,101,100,0,16,111,110,76,97,
    121,111,117,116,70,105,110,105,115,104,101,100,0,12,111,110,77,116,117,67,104,97,110,103,
    101,100,0,12,111,110,83,99,97,110,82,101,115,117,108,116,0,20,111,110,83,101,114,118,
    105,99,101,115,68,105,115,99,111,118,101,114,101,100,0,7,111,110,87,114,105,116,101,0,
    16,111,110,87,114,105,116,101,67,97,110,99,101,108,108,101,100,0,13,111,110,87,114,105,
    116,101,70,97,105,108,101,100,0,15,111,110,87,114,105,116,101,70,105,110,105,115,104,101,
    100,0,4,111,112,101,110,0,11,111,112,101,110,67,108,97,115,115,105,99,0,8,111,112,
    101,110,71,97,116,116,0,15,111,112,101,110,73,110,112,117,116,83,116,114,101,97,109,0,
    16,111,112,101,110,79,117,116,112,117,116,83,116,114,101,97,109,0,5,111,114,100,101,114,
    0,25,111,114,103,46,99,101,101,115,97,102,101,46,119,105,110,100,111,119,46,80,105,99,
    107,101,114,0,6,112,97,105,114,101,100,0,5,112,97,117,115,101,0,26,112,101,114,102,
    111,114,109,65,99,99,101,115,115,105,98,105,108,105,116,121,65,99,116,105,111,110,0,13,
    112,101,114,102,111,114,109,65,99,116,105,111,110,0,19,112,101,114,102,111,114,109,69,100,
    105,116,111,114,65,99,116,105,111,110,0,6,112,101,114,109,105,116,0,5,112,105,101,99,
    101,0,6,112,108,97,99,101,115,0,5,112,108,97,105,110,0,4,112,111,115,116,0,11,
    112,111,115,116,68,101,108,97,121,101,100,0,5,112,114,105,110,116,0,7,112,114,105,110,
    116,101,114,0,14,112,114,105,110,116,101,114,83,101,114,118,105,99,101,0,8,112,114,111,
    118,105,100,101,114,0,3,112,117,116,0,6,112,117,116,65,108,108,0,8,112,117,116,69,
    120,116,114,97,0,5,113,117,101,114,121,0,6,114,97,110,103,101,115,0,4,114,101,97,
    100,0,6,114,101,99,111,114,100,0,5,114,101,99,116,115,0,7,114,101,108,101,97,115,
    101,0,7,114,101,112,108,97,99,101,0,12,114,101,113,117,101,115,116,70,111,99,117,115,
    0,10,114,101,113,117,101,115,116,77,116,117,0,18,114,101,113,117,101,115,116,80,101,114,
    109,105,115,115,105,111,110,115,0,29,114,101,113,117,101,115,116,83,101,110,100,65,99,99,
    101,115,115,105,98,105,108,105,116,121,69,118,101,110,116,0,12,114,101,115,116,97,114,116,
    73,110,112,117,116,0,5,114,111,108,101,115,0,5,114,111,117,110,100,0,3,114,117,110,
    0,13,114,117,110,79,110,85,105,84,104,114,101,97,100,0,4,115,99,97,110,0,4,115,
    101,110,100,0,7,115,101,110,100,70,111,114,0,12,115,101,110,100,75,101,121,69,118,101,
    110,116,0,23,115,101,116,65,99,99,101,115,115,105,98,105,108,105,116,121,70,111,99,117,
    115,101,100,0,17,115,101,116,66,111,117,110,100,115,73,110,80,97,114,101,110,116,0,17,
    115,101,116,66,111,117,110,100,115,73,110,83,99,114,101,101,110,0,12,115,101,116,67,104,
    101,99,107,97,98,108,101,0,10,115,101,116,67,104,101,99,107,101,100,0,12,115,101,116,
    67,108,97,115,115,78,97,109,101,0,12,115,101,116,67,108,105,99,107,97,98,108,101,0,
    17,115,101,116,67,111,108,108,101,99,116,105,111,110,73,110,102,111,0,21,115,101,116,67,
    111,108,108,101,99,116,105,111,110,73,116,101,109,73,110,102,111,0,16,115,101,116,67,111,
    109,112,111,115,105,110,103,84,101,120,116,0,21,115,101,116,67,111,110,116,101,110,116,67,
    104,97,110,103,101,84,121,112,101,115,0,21,115,101,116,67,111,110,116,101,110,116,68,101,
    115,99,114,105,112,116,105,111,110,0,14,115,101,116,67,111,110,116,101,110,116,84,121,112,
    101,0,11,115,101,116,69,100,105,116,97,98,108,101,0,10,115,101,116,69,110,97,98,108,
    101,100,0,12,115,101,116,70,111,99,117,115,97,98,108,101,0,23,115,101,116,70,111,99,
    117,115,97,98,108,101,73,110,84,111,117,99,104,77,111,100,101,0,28,115,101,116,73,109,
    112,111,114,116,97,110,116,70,111,114,65,99,99,101,115,115,105,98,105,108,105,116,121,0,
    14,115,101,116,80,97,99,107,97,103,101,78,97,109,101,0,12,115,101,116,80,97,103,101,
    67,111,117,110,116,0,9,115,101,116,80,97,114,101,110,116,0,11,115,101,116,80,97,115,
    115,119,111,114,100,0,12,115,101,116,82,97,110,103,101,73,110,102,111,0,11,115,101,116,
    83,99,97,110,77,111,100,101,0,11,115,101,116,83,101,108,101,99,116,101,100,0,14,115,
    101,116,83,101,114,118,105,99,101,85,117,105,100,0,16,115,101,116,83,111,102,116,73,110,
    112,117,116,77,111,100,101,0,9,115,101,116,83,111,117,114,99,101,0,7,115,101,116,84,
    101,120,116,0,7,115,101,116,84,121,112,101,0,8,115,101,116,86,97,108,117,101,0,16,
    115,101,116,86,105,115,105,98,108,101,84,111,85,115,101,114,0,14,115,101,116,87,105,108,
    108,78,111,116,68,114,97,119,0,12,115,101,116,87,114,105,116,101,84,121,112,101,0,13,
    115,104,111,119,83,111,102,116,73,110,112,117,116,0,5,115,104,111,119,110,0,6,115,105,
    103,110,97,108,0,4,115,105,122,101,0,5,115,108,101,101,112,0,6,115,111,99,107,101,
    116,0,5,115,112,108,105,116,0,5,115,116,97,114,116,0,22,115,116,97,114,116,65,99,
    116,105,118,105,116,121,70,111,114,82,101,115,117,108,116,0,9,115,116,97,114,116,83,99,
    97,110,0,10,115,116,97,114,116,115,87,105,116,104,0,5,115,116,97,116,101,0,8,115,
    116,111,112,83,99,97,110,0,6,115,116,114,101,97,109,0,9,115,117,98,115,116,114,105,
    110,103,0,4,116,101,120,116,0,29,116,104,101,32,100,111,99,117,109,101,110,116,32,99,
    97,110,110,111,116,32,98,101,32,111,112,101,110,101,100,0,6,116,104,105,115,36,48,0,
    7,116,111,65,114,114,97,121,0,11,116,111,66,121,116,101,65,114,114,97,121,0,7,116,
    111,67,104,97,114,115,0,11,116,111,76,111,119,101,114,67,97,115,101,0,8,116,111,83,
    116,114,105,110,103,0,3,116,111,112,0,10,116,114,121,65,99,113,117,105,114,101,0,6,
    117,112,100,97,116,101,0,3,117,112,115,0,12,117,112,116,105,109,101,77,105,108,108,105,
    115,0,9,118,97,108,36,99,97,99,104,101,0,9,118,97,108,36,102,111,117,110,100,0,
    8,118,97,108,36,102,114,111,109,0,11,118,97,108,36,104,97,110,100,108,101,114,0,8,
    118,97,108,36,107,105,110,100,0,12,118,97,108,36,114,101,115,111,108,118,101,114,0,9,
    118,97,108,36,117,110,116,105,108,0,8,118,97,108,36,117,114,105,115,0,5,118,97,108,
    117,101,0,7,118,97,108,117,101,79,102,0,6,118,97,108,117,101,115,0,4,118,105,101,
    119,0,7,118,105,114,116,117,97,108,0,4,119,114,97,112,0,5,119,114,105,116,101,0,
    19,119,114,105,116,101,67,104,97,114,97,99,116,101,114,105,115,116,105,99,0,9,119,114,
    105,116,101,71,97,116,116,0,2,119,116,0,158,1,126,126,68,56,123,34,98,97,99,107,
    101,110,100,34,58,34,100,101,120,34,44,34,99,111,109,112,105,108,97,116,105,111,110,45,
    109,111,100,101,34,58,34,114,101,108,101,97,115,101,34,44,34,104,97,115,45,99,104,101,
    99,107,115,117,109,115,34,58,102,97,108,115,101,44,34,109,105,110,45,97,112,105,34,58,
    50,53,44,34,115,104,97,45,49,34,58,34,48,56,52,97,56,57,49,50,54,97,101,48,
    53,57,54,99,53,57,57,49,52,51,100,102,53,55,97,54,53,52,54,52,54,97,102,50,
    56,51,49,51,34,44,34,118,101,114,115,105,111,110,34,58,34,57,46,50,46,52,45,100,
    101,118,34,125,0,2,76,2,208,2,28,1,36,16,16,141,4,28,1,30,2,74,2,208,
    2,4,18,140,4,55,10,1,2,72,1,161,5,24,121,2,77,1,161,5,28,4,23,234,
    23,214,23,214,23,64,2,75,1,161,5,28,1,24,119,2,73,1,161,5,58,98,1,2,
    74,2,208,2,4,0,140,4,30,2,74,2,208,2,4,25,140,4,23,202,2,72,1,161,
    5,24,125,2,75,1,161,5,28,1,24,124,2,74,2,208,2,4,26,140,4,23,83,2,
    72,1,161,5,24,127,2,75,1,161,5,28,1,24,126,2,76,2,208,2,28,6,68,16,
    128,0,36,16,16,36,16,16,36,16,16,36,16,16,36,16,16,141,4,28,6,30,30,30,
    30,30,30,2,77,1,161,5,28,1,23,2,2,73,1,161,5,58,142,1,2,78,1,161,
    5,28,1,24,84,2,76,2,208,2,28,3,68,16,128,0,36,16,16,36,16,16,141,4,
    28,3,30,30,30,2,73,1,161,5,58,152,1,0,1,2,2,14,144,32,129,2,130,128,
    4,140,100,1,128,160,4,168,100,131,2,1,232,83,1,1,212,96,28,19,35,4,15,26,
    1,26,1,26,1,26,1,26,1,26,1,26,1,26,1,26,1,26,1,26,1,26,1,26,
    1,26,1,26,1,26,1,26,1,26,1,26,1,26,1,26,1,26,1,26,1,26,1,26,
    1,26,1,26,1,26,43,2,1,2,1,2,1,2,1,2,1,2,1,2,1,18,1,2,
    1,2,1,2,1,18,1,2,1,2,1,2,1,2,1,2,1,2,1,2,133,2,200,32,
    216,100,1,200,32,196,105,1,200,32,220,105,1,200,32,172,105,1,200,32,236,106,1,200,
    32,240,108,1,200,32,216,108,1,200,32,168,108,1,200,32,244,105,1,200,32,252,104,1,
    200,32,148,105,1,200,32,140,106,1,200,32,164,106,1,200,32,188,106,1,200,32,192,108,
    1,200,32,212,106,1,200,32,136,109,1,200,32,160,109,1,200,32,132,107,1,200,32,240,
    100,1,200,32,140,101,1,200,32,184,109,1,200,32,208,109,1,200,32,232,109,1,136,128,
    4,128,110,1,129,128,4,240,110,3,2,160,107,2,2,168,103,5,2,232,103,2,138,2,
    0,1,138,2,0,4,2,168,104,1,2,216,112,1,2,252,112,3,10,244,107,159,2,1,
    176,112,3,1,168,101,2,1,192,100,17,1,140,114,0,2,1,1,62,145,32,1,145,32,
    182,2,129,160,4,184,121,183,2,17,216,121,0,1,1,1,64,144,32,184,2,128,128,4,
    248,121,185,2,1,148,122,0,10,17,6,65,2,1,2,1,66,1,0,1,66,1,2,1,
    2,1,18,1,2,1,2,186,2,200,32,212,123,1,200,32,132,124,1,200,32,156,124,1,
    200,32,188,123,1,200,32,212,127,1,200,32,236,123,1,200,32,188,127,1,200,32,236,127,
    1,200,32,132,128,1,1,200,32,156,128,1,1,200,32,180,128,1,1,200,32,204,128,1,
    1,200,32,228,128,1,1,200,32,180,124,1,129,128,4,252,128,1,1,2,208,124,7,2,
    224,125,202,2,0,172,129,1,1,1,252,129,1,1,1,176,130,1,1,1,240,130,1,1,
    1,156,131,1,1,0,144,125,8,0,18,0,75,24,1,24,1,24,1,24,1,24,1,24,
    1,24,1,24,209,2,200,32,224,142,1,1,136,128,4,248,142,1,1,129,128,4,220,143,
    1,1,10,208,131,1,1,10,160,134,1,1,10,168,132,1,1,136,32,244,143,1,1,10,
    236,140,1,1,8,204,141,1,1,10,144,144,1,1,10,240,145,1,1,8,192,134,1,1,
    10,148,149,1,1,8,156,139,1,1,10,224,132,1,1,10,228,133,1,1,10,200,149,1,
    1,8,148,136,1,0,1,1,6,83,18,227,2,128,128,4,188,152,1,228,2,1,172,150,
    1,1,1,236,150,1,1,1,148,151,1,1,1,180,151,1,1,1,216,151,1,1,1,248,
    151,1,6,4,13,5,84,26,1,26,1,26,1,26,1,26,1,26,90,2,1,2,1,2,
    1,2,234,2,200,32,152,156,1,1,200,32,176,156,1,1,200,32,200,156,1,1,200,32,
    192,153,1,1,200,32,224,156,1,1,200,32,248,156,1,1,136,128,4,144,157,1,1,129,
    128,4,128,158,1,2,2,172,160,1,3,2,220,153,1,1,138,2,0,1,138,2,0,1,
    138,2,0,242,2,1,224,158,1,8,1,220,152,1,1,1,204,155,1,1,1,252,152,1,
    1,1,224,155,1,0,6,1,1,94,144,32,1,144,32,1,144,32,1,144,32,1,144,32,
    1,144,32,129,3,128,128,4,240,160,1,130,3,1,160,161,1,8,1,11,1,100,26,1,
    26,1,26,1,26,1,26,1,26,1,26,1,26,108,2,131,3,200,32,208,170,1,1,200,
    32,172,166,1,1,200,32,232,170,1,1,129,128,4,128,171,1,1,8,152,171,1,1,10,
    176,171,1,2,10,200,166,1,1,10,168,169,1,1,10,152,167,1,1,138,2,0,2,9,
    212,173,1,142,3,1,248,171,1,0,3,1,1,109,144,32,1,144,32,1,144,32,145,3,
    128,128,4,220,175,1,146,3,1,128,176,1,4,4,7,3,112,26,1,26,1,26,1,26,
    116,2,1,18,1,2,1,18,147,3,200,32,128,179,1,1,200,32,172,178,1,1,130,128,
    4,152,179,1,1,2,184,179,1,1,10,212,176,1,4,8,252,183,1,1,2,200,178,1,
    152,3,1,152,180,1,1,1,220,180,1,1,1,196,181,1,28,4,1,4,18,4,2,4,
    1,30,4,19,4,3,4,8,4,9,4,11,4,10,4,2,4,12,4,7,4,13,4,14,
    4,8,4,0,4,3,4,17,4,32,4,2,4,5,4,6,4,16,4,15,4,0,4,4,
    8,4,3,4,6,4,4,4,2,4,5,30,30,4,1,5,30,4,0,4,0,4,1,4,
    1,8,4,0,4,4,4,255,4,0,4,1,4,5,4,1,55,52,2,4,4,0,4,255,
    4,1,38,48,117,0,0,0,1,0,0,0,125,141,0,0,2,0,0,0,152,141,0,0,
    140,141,0,0,1,0,0,0,159,141,0,0,1,0,0,0,174,141,0,0,2,0,0,0,
    183,141,0,0,191,141,0,0,2,0,0,0,212,141,0,0,201,141,0,0,1,0,0,0,
    219,141,0,0,2,0,0,0,239,141,0,0,228,141,0,0,1,0,0,0,246,141,0,0,
    2,0,0,0,255,141,0,0,35,142,0,0,2,0,0,0,44,142,0,0,191,141,0,0,
    1,0,0,0,52,142,0,0,2,0,0,0,61,142,0,0,35,142,0,0,2,0,0,0,
    85,142,0,0,191,141,0,0,200,146,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
    1,1,0,0,192,146,0,0,220,146,0,0,1,0,0,0,0,0,0,0,0,0,0,0,
    50,0,0,0,212,146,0,0,228,146,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    240,146,0,0,0,0,0,0,0,0,0,0,0,0,0,0,252,146,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,4,147,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    16,147,0,0,0,0,0,0,0,0,0,0,0,0,0,0,36,147,0,0,0,0,0,0,
    1,0,0,0,0,0,0,0,129,1,0,0,24,147,0,0,0,0,0,0,0,0,0,0,
    1,0,0,0,0,0,0,0,136,1,0,0,48,147,0,0,68,147,0,0,0,0,0,0,
    1,0,0,0,0,0,0,0,145,1,0,0,56,147,0,0,17,0,0,0,0,0,0,0,
    1,0,0,0,0,0,0,0,1,0,0,0,172,2,0,0,112,0,0,0,2,0,0,0,
    146,0,0,0,32,11,0,0,3,0,0,0,218,0,0,0,104,13,0,0,4,0,0,0,
    120,0,0,0,160,23,0,0,5,0,0,0,157,1,0,0,96,27,0,0,6,0,0,0,
    13,0,0,0,72,40,0,0,1,32,0,0,133,0,0,0,232,41,0,0,3,32,0,0,
    2,0,0,0,176,92,0,0,1,16,0,0,117,0,0,0,188,92,0,0,2,32,0,0,
    172,2,0,0,222,96,0,0,4,32,0,0,19,0,0,0,125,141,0,0,0,32,0,0,
    12,0,0,0,93,142,0,0,5,32,0,0,5,0,0,0,80,146,0,0,3,16,0,0,
    14,0,0,0,192,146,0,0,6,32,0,0,10,0,0,0,80,147,0,0,0,16,0,0,
    1,0,0,0,24,148,0,0,
};
/* END android dex */

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
    int32_t keyboard = 0;
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
            /* The soft keyboard (ADR 0157): WindowInsets.Type.ime(), 8. */
            jobject ime = (*env)->CallObjectMethod(env, insets, get, (jint)8);
            if ((*env)->ExceptionCheck(env)) {
                (*env)->ExceptionClear(env);
                ime = NULL;
            }
            if (ime == NULL || !cee_android_field(env, ime, "bottom", &keyboard)) {
                keyboard = 0;
            }
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
        cee_android.keyboard = keyboard > 0 ? keyboard : 0;
        pthread_mutex_unlock(&cee_android.lock);
    }
}

/* ---- Text input (ADR 0157), on the activity's thread ---- */

/* What the input method typed, queued for the program: a record of a type, a length and the bytes. */
static void cee_android_typed(int type, const char *bytes, size_t length) {
    if (length > 65535u) {
        length = 0; /* no input method sends such a text at once */
    }
    pthread_mutex_lock(&cee_android.lock);
    size_t needed = cee_android.typed_length + 3u + length;
    if (needed > cee_android.typed_capacity) {
        size_t next = cee_android.typed_capacity < 256u ? 256u : cee_android.typed_capacity;
        while (next < needed) {
            next *= 2u;
        }
        char *grown = (char *)ceesafe_platform_realloc(cee_android.typed, next);
        if (grown != NULL) {
            cee_android.typed = grown;
            cee_android.typed_capacity = next;
        }
    }
    if (needed <= cee_android.typed_capacity) {
        char *at = cee_android.typed + cee_android.typed_length;
        at[0] = (char)type;
        at[1] = (char)(length & 0xffu);
        at[2] = (char)(length >> 8);
        if (length > 0) {
            memcpy(at + 3, bytes, length);
        }
        cee_android.typed_length = needed;
    }
    pthread_mutex_unlock(&cee_android.lock);
    cee_android_wake_program();
}

/* InputView.nativeText: text committed (kind 0) or being composed (kind 1), as UTF-8 bytes. */
static void JNICALL cee_android_native_text(JNIEnv *env, jclass type, jint kind, jbyteArray utf8) {
    (void)type;
    jsize length = utf8 != NULL ? (*env)->GetArrayLength(env, utf8) : 0;
    jbyte *bytes = length > 0 ? (*env)->GetByteArrayElements(env, utf8, NULL) : NULL;
    cee_android_typed(kind == 1 ? 1 : 0, (const char *)bytes, bytes != NULL ? (size_t)length : 0u);
    if (bytes != NULL) {
        (*env)->ReleaseByteArrayElements(env, utf8, bytes, JNI_ABORT);
    }
}

/* InputView.nativeKey: the keyboard's Backspace (0) or Enter (1). */
static void JNICALL cee_android_native_key(JNIEnv *env, jclass type, jint key) {
    (void)env;
    (void)type;
    char which = key == 1 ? 1 : 0;
    cee_android_typed(2, &which, 1);
}

/* AccessView.nativeAccess: TalkBack asks `action` of element `id` (ADR 0164): kept for the program, which is woken.
   A full list drops the request. */
static void JNICALL cee_android_native_access(JNIEnv *env, jclass type, jlong id, jint action) {
    (void)env;
    (void)type;
    pthread_mutex_lock(&cee_android.lock);
    if (cee_android.access_asked_count < CEE_ACCESS_QUEUE && action >= 0 && action <= 3) {
        cee_android.access_asked[cee_android.access_asked_count].id = (uint64_t)id;
        cee_android.access_asked[cee_android.access_asked_count].action = (int32_t)action;
        cee_android.access_asked_count++;
    }
    pthread_mutex_unlock(&cee_android.lock);
    cee_android_wake_program();
}

/* AccessView.nativeAccessActive: something asked for the window's elements. */
static void JNICALL cee_android_native_access_active(JNIEnv *env, jclass type) {
    (void)env;
    (void)type;
    pthread_mutex_lock(&cee_android.lock);
    cee_android.access_active = 1;
    pthread_mutex_unlock(&cee_android.lock);
}

/* InputView.nativeInsets: the window's insets were applied again, the keyboard's among them. */
static void JNICALL cee_android_native_insets(JNIEnv *env, jclass type) {
    (void)env;
    (void)type;
    pthread_mutex_lock(&cee_android.lock);
    ANativeActivity *activity = cee_android.activity;
    pthread_mutex_unlock(&cee_android.lock);
    if (activity != NULL) {
        cee_android_read_insets(activity);
        cee_android_wake_program();
    }
}

static int cee_android_failed(JNIEnv *env) {
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return 1;
    }
    return 0;
}

/* Picker.nativePicked: a file dialog's answer (ADR 0158), from the activity's thread or the picker's worker. */
static void JNICALL cee_android_native_picked(JNIEnv *env, jclass type, jint status, jbyteArray paths) {
    (void)type;
    jsize length = paths != NULL ? (*env)->GetArrayLength(env, paths) : 0;
    char *copy = length > 0 ? (char *)ceesafe_platform_alloc((size_t)length) : NULL;
    if (copy != NULL) {
        (*env)->GetByteArrayRegion(env, paths, 0, length, (jbyte *)copy);
    }
    pthread_mutex_lock(&cee_android.lock);
    if (cee_android.pick_state == 2) {
        cee_android.pick_status = length > 0 && copy == NULL ? -CEE_WINDOW_RESOURCES : (int32_t)status;
        cee_android.pick_paths = copy;
        cee_android.pick_length = copy != NULL ? (size_t)length : 0u;
        cee_android.pick_state = 3;
        copy = NULL;
    }
    pthread_mutex_unlock(&cee_android.lock);
    ceesafe_platform_free(copy); /* an answer no dialog is waiting for */
    cee_android_wake_program();
}

/* One of the runtime's classes, by its name with dots: the app's own, when its APK holds the runtime's classes (then
   the system can also make them again by itself), else from `loader`, the embedded dex's. */
static jclass cee_android_class(JNIEnv *env, jobject parent, jobject loader, const char *name) {
    jstring text = (*env)->NewStringUTF(env, name);
    jclass found = NULL;
    for (int k = 0; k < 2 && found == NULL && text != NULL; k++) {
        jobject from = k == 0 ? parent : loader;
        if (from == NULL) {
            continue;
        }
        jmethodID load = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, from), "loadClass",
                                             "(Ljava/lang/String;)Ljava/lang/Class;");
        found = load != NULL ? (jclass)(*env)->CallObjectMethod(env, from, load, text) : NULL;
        if (cee_android_failed(env)) {
            found = NULL;
        }
    }
    return found;
}

/* The class loader of the runtime's embedded dex (ADR 0157, ADR 0188 §2.1), made once for both its users (the window's
   classes and Bluetooth's, on any thread) and kept as a global reference; NULL if it cannot be made. From API level 26
   the dex stays in memory (InMemoryDexClassLoader). Below it that class is missing: the dex is written once to the
   app's code cache, under a name that carries a hash of its bytes (an updated app never loads an old file), made
   read-only, and loaded with DexClassLoader, which every release has. */
static pthread_mutex_t cee_android_loader_lock = PTHREAD_MUTEX_INITIALIZER;
static jobject cee_android_loader_ref;

static jobject cee_android_file_loader(JNIEnv *env, jobject activity_object, jobject parent) {
    jobject dir = cee_android_object(env, activity_object, "getCodeCacheDir", "()Ljava/io/File;");
    jstring folder = (jstring)cee_android_object(env, dir, "getAbsolutePath", "()Ljava/lang/String;");
    const char *where = folder != NULL ? (*env)->GetStringUTFChars(env, folder, NULL) : NULL;
    if (where == NULL) {
        (void)cee_android_failed(env);
        return NULL;
    }
    uint64_t hash = 14695981039346656037ull; /* FNV-1a over the dex */
    for (size_t i = 0; i < sizeof cee_android_dex; i++) {
        hash = (hash ^ cee_android_dex[i]) * 1099511628211ull;
    }
    char path[1024];
    char temporary[1040];
    char compiled[1024];
    int fits = snprintf(path, sizeof path, "%s/ceesafe-runtime-%016llx.dex", where, (unsigned long long)hash);
    (void)snprintf(temporary, sizeof temporary, "%s.part", path);
    /* Where ART puts the compiled dex: a folder of its own, as Android 7 names the .oat file after the dex in it, and
       would write it over the dex in the dex's own folder. */
    int fitsToo = snprintf(compiled, sizeof compiled, "%s/ceesafe-oat", where);
    (*env)->ReleaseStringUTFChars(env, folder, where);
    if (fits <= 0 || (size_t)fits >= sizeof path || fitsToo <= 0 || (size_t)fitsToo >= sizeof compiled) {
        return NULL;
    }
    if (mkdir(compiled, 0700) != 0 && errno != EEXIST) {
        return NULL;
    }
    struct stat known;
    if (stat(path, &known) != 0 || known.st_size != (off_t)sizeof cee_android_dex) {
        int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) {
            return NULL;
        }
        size_t done = 0;
        while (done < sizeof cee_android_dex) {
            ssize_t n = write(fd, cee_android_dex + done, sizeof cee_android_dex - done);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                break;
            }
            done += (size_t)n;
        }
        int closed = close(fd);
        if (done != sizeof cee_android_dex || closed != 0 || chmod(temporary, 0400) != 0 || rename(temporary, path) != 0) {
            (void)unlink(temporary);
            return NULL;
        }
    }
    jclass type = (*env)->FindClass(env, "dalvik/system/DexClassLoader");
    jmethodID make = type != NULL ? (*env)->GetMethodID(env, type, "<init>",
                                                        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
                                                        "Ljava/lang/ClassLoader;)V")
                                  : NULL;
    jstring file = (*env)->NewStringUTF(env, path);
    jstring output = (*env)->NewStringUTF(env, compiled);
    jobject loader = make != NULL && file != NULL && output != NULL
                         ? (*env)->NewObject(env, type, make, file, output, NULL, parent)
                         : NULL;
    if (cee_android_failed(env)) {
        loader = NULL;
    }
    return loader;
}

static jobject cee_android_dex_loader(JNIEnv *env, jobject activity_object, jobject parent) {
    (void)pthread_mutex_lock(&cee_android_loader_lock);
    if (cee_android_loader_ref == NULL && parent != NULL) {
        jobject loader = NULL;
        jclass memory = (*env)->FindClass(env, "dalvik/system/InMemoryDexClassLoader");
        if (memory == NULL) {
            (void)cee_android_failed(env); /* below API level 26 */
            loader = cee_android_file_loader(env, activity_object, parent);
        } else {
            jobject buffer = (*env)->NewDirectByteBuffer(env, cee_android_dex, (jlong)sizeof cee_android_dex);
            jmethodID make = (*env)->GetMethodID(env, memory, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
            loader = make != NULL && buffer != NULL ? (*env)->NewObject(env, memory, make, buffer, parent) : NULL;
            if (cee_android_failed(env)) {
                loader = NULL;
            }
        }
        if (loader != NULL) {
            cee_android_loader_ref = (*env)->NewGlobalRef(env, loader);
        }
    }
    jobject found = cee_android_loader_ref;
    (void)pthread_mutex_unlock(&cee_android_loader_lock);
    return found;
}

/* The runtime's two Java classes, loaded at the first use of either (ADR 0157, ADR 0158): from the embedded dex
   (cee_android_dex_loader, with the activity's class loader as its parent) unless the app holds them, their natives
   registered. A failure is final: text input and file dialogs then do nothing, and the window still works. */
static int cee_android_classes(ANativeActivity *activity) {
    JNIEnv *env = activity->env;
    if (cee_android.classes != 0) {
        return cee_android.classes == 1;
    }
    cee_android.classes = -1;
    if ((*env)->PushLocalFrame(env, 32) != 0) {
        (void)cee_android_failed(env);
        return 0;
    }
    jobject parent = cee_android_object(env, activity->clazz, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = cee_android_dex_loader(env, activity->clazz, parent);
    jclass view_class = cee_android_class(env, parent, loader, "org.ceesafe.window.InputView");
    jclass picker_class = cee_android_class(env, parent, loader, "org.ceesafe.window.Picker");
    jclass access_class = cee_android_class(env, parent, loader, "org.ceesafe.window.AccessView");
    if (view_class != NULL && picker_class != NULL && access_class != NULL) {
        /* ISO C has no function-to-object pointer conversion; a JNINativeMethod's fnPtr is a void *. */
        void (JNICALL *text)(JNIEnv *, jclass, jint, jbyteArray) = cee_android_native_text;
        void (JNICALL *key)(JNIEnv *, jclass, jint) = cee_android_native_key;
        void (JNICALL *applied)(JNIEnv *, jclass) = cee_android_native_insets;
        void (JNICALL *picked)(JNIEnv *, jclass, jint, jbyteArray) = cee_android_native_picked;
        JNINativeMethod natives[3] = {{"nativeText", "(I[B)V", NULL}, {"nativeKey", "(I)V", NULL},
                                      {"nativeInsets", "()V", NULL}};
        JNINativeMethod answer[1] = {{"nativePicked", "(I[B)V", NULL}};
        void (JNICALL *asked)(JNIEnv *, jclass, jlong, jint) = cee_android_native_access;
        void (JNICALL *active)(JNIEnv *, jclass) = cee_android_native_access_active;
        JNINativeMethod access[2] = {{"nativeAccess", "(JI)V", NULL}, {"nativeAccessActive", "()V", NULL}};
        memcpy(&access[0].fnPtr, &asked, sizeof asked);
        memcpy(&access[1].fnPtr, &active, sizeof active);
        memcpy(&natives[0].fnPtr, &text, sizeof text);
        memcpy(&natives[1].fnPtr, &key, sizeof key);
        memcpy(&natives[2].fnPtr, &applied, sizeof applied);
        memcpy(&answer[0].fnPtr, &picked, sizeof picked);
        if ((*env)->RegisterNatives(env, view_class, natives, 3) == 0 &&
            (*env)->RegisterNatives(env, picker_class, answer, 1) == 0 &&
            (*env)->RegisterNatives(env, access_class, access, 2) == 0) {
            cee_android.input_class = (jclass)(*env)->NewGlobalRef(env, view_class);
            cee_android.picker_class = (jclass)(*env)->NewGlobalRef(env, picker_class);
            cee_android.access_class = (jclass)(*env)->NewGlobalRef(env, access_class);
            if (cee_android.input_class != NULL && cee_android.picker_class != NULL &&
                cee_android.access_class != NULL) {
                cee_android.classes = 1;
            }
        }
    }
    (void)cee_android_failed(env);
    (void)(*env)->PopLocalFrame(env, NULL);
    if (cee_android.classes != 1) {
        __android_log_write(ANDROID_LOG_WARN, "ceesafe", "the runtime's Java classes could not be loaded");
    }
    return cee_android.classes == 1;
}

/* For the Bluetooth runtime (ADR 0177; ceesafe_rt_bluetooth.c, which declares these weak, so a program without a
   window has none): the activity's VM, NULL before the activity is made; and, for a thread attached to it, the
   activity's Java object and org.ceesafe.window.Bluetooth, loaded from the embedded dex at the first call (a global
   reference, kept). The Bluetooth runtime makes one call at a time. */
JavaVM *ceesafe_android_vm(void);
jclass ceesafe_android_bluetooth(JNIEnv *env, jobject *activity);

JavaVM *ceesafe_android_vm(void) {
    ANativeActivity *a = cee_android.activity;
    return a != NULL ? a->vm : NULL;
}

/* For a host program (ceesafe/android.h, ADR 0200): the activity's Java object, the global reference NativeActivity
   holds for the activity's life. */
jobject ceesafe_android_activity(void);

jobject ceesafe_android_activity(void) {
    ANativeActivity *a = cee_android.activity;
    return a != NULL ? a->clazz : NULL;
}

jclass ceesafe_android_bluetooth(JNIEnv *env, jobject *activity) {
    static jclass loaded;
    ANativeActivity *a = cee_android.activity;
    if (a == NULL) {
        return NULL;
    }
    *activity = a->clazz;
    if (loaded != NULL || (*env)->PushLocalFrame(env, 16) != 0) {
        (void)cee_android_failed(env);
        return loaded;
    }
    jobject parent = cee_android_object(env, a->clazz, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = cee_android_dex_loader(env, a->clazz, parent);
    jclass found = cee_android_class(env, parent, loader, "org.ceesafe.window.Bluetooth");
    if (found != NULL) {
        loaded = (jclass)(*env)->NewGlobalRef(env, found);
    }
    (void)cee_android_failed(env);
    (void)(*env)->PopLocalFrame(env, NULL);
    return loaded;
}

/* The input view, made at the first use: one view added to the activity's window, 1 by 1. A failure is final for the
   activity: text input then does nothing. */
static int cee_android_input_load(ANativeActivity *activity) {
    JNIEnv *env = activity->env;
    if (cee_android.input_view != NULL) {
        return 1;
    }
    if (cee_android.input_failed || !cee_android_classes(activity) || (*env)->PushLocalFrame(env, 16) != 0) {
        (void)cee_android_failed(env);
        return 0;
    }
    cee_android.input_failed = 1;
    jclass view_class = cee_android.input_class;
    jmethodID create = (*env)->GetMethodID(env, view_class, "<init>", "(Landroid/content/Context;)V");
    jmethodID apply = (*env)->GetMethodID(env, view_class, "apply", "(ZI)V");
    jclass params_class = (*env)->FindClass(env, "android/view/ViewGroup$LayoutParams");
    jmethodID params_make = params_class != NULL ? (*env)->GetMethodID(env, params_class, "<init>", "(II)V") : NULL;
    jmethodID add = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, activity->clazz), "addContentView",
                                        "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V");
    if (!cee_android_failed(env) && create != NULL && apply != NULL && params_make != NULL && add != NULL) {
        jobject view = (*env)->NewObject(env, view_class, create, activity->clazz);
        jobject params = (*env)->NewObject(env, params_class, params_make, (jint)1, (jint)1);
        if (!cee_android_failed(env) && view != NULL && params != NULL) {
            (*env)->CallVoidMethod(env, activity->clazz, add, view, params);
            if (!cee_android_failed(env)) {
                cee_android.input_view = (*env)->NewGlobalRef(env, view);
                cee_android.input_apply = apply;
                cee_android.input_failed = cee_android.input_view == NULL;
            }
        }
    }
    (void)cee_android_failed(env);
    (void)(*env)->PopLocalFrame(env, NULL);
    if (cee_android.input_failed) {
        __android_log_write(ANDROID_LOG_WARN, "ceesafe", "text input: the input view could not be made");
    }
    return cee_android.input_view != NULL;
}

/* Starts the file dialog the program asked for (ADR 0158): Picker.start, with the dialog's texts as Java strings.
   When it cannot be started, the dialog is answered at once as unsupported. */
static void cee_android_pick_start(ANativeActivity *activity) {
    JNIEnv *env = activity->env;
    pthread_mutex_lock(&cee_android.lock);
    int kind = cee_android.pick_kind;
    const char *extensions = cee_android.pick_extensions, *name = cee_android.pick_name,
               *file = cee_android.pick_file;
    cee_android.pick_state = 2;
    pthread_mutex_unlock(&cee_android.lock);
    int started = 0;
    if (cee_android_classes(activity) && (*env)->PushLocalFrame(env, 16) == 0) {
        jmethodID start = (*env)->GetStaticMethodID(
            env, cee_android.picker_class, "start",
            "(Landroid/app/Activity;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
        jstring a = (*env)->NewStringUTF(env, extensions);
        jstring b = (*env)->NewStringUTF(env, name);
        jstring c = (*env)->NewStringUTF(env, file);
        if (!cee_android_failed(env) && start != NULL && a != NULL && b != NULL && c != NULL) {
            (*env)->CallStaticVoidMethod(env, cee_android.picker_class, start, activity->clazz, (jint)kind, a, b, c);
            started = !cee_android_failed(env);
        }
        (void)(*env)->PopLocalFrame(env, NULL);
    }
    (void)cee_android_failed(env);
    if (!started) {
        pthread_mutex_lock(&cee_android.lock);
        if (cee_android.pick_state == 2) {
            cee_android.pick_status = -CEE_WINDOW_UNSUPPORTED;
            cee_android.pick_state = 3;
        }
        pthread_mutex_unlock(&cee_android.lock);
        cee_android_wake_program();
    }
}

/* The activity's looper's callback for `ui_pipe`: applies what the program wants of text input. */
/* The access view (ADR 0164), made when the program first gives a list: a view the size of the window, which draws
   nothing. A failure is final for the activity: no reader is told then. */
static int cee_android_access_load(ANativeActivity *activity) {
    JNIEnv *env = activity->env;
    if (cee_android.access_view != NULL) {
        return 1;
    }
    if (cee_android.access_failed || !cee_android_classes(activity) || (*env)->PushLocalFrame(env, 16) != 0) {
        (void)cee_android_failed(env);
        return 0;
    }
    cee_android.access_failed = 1;
    jclass view_class = cee_android.access_class;
    jmethodID create = (*env)->GetMethodID(env, view_class, "<init>", "(Landroid/content/Context;)V");
    jmethodID update = (*env)->GetMethodID(env, view_class, "update", "([B)V");
    jmethodID announce = (*env)->GetMethodID(env, view_class, "announce", "([B)V");
    jclass params_class = (*env)->FindClass(env, "android/view/ViewGroup$LayoutParams");
    jmethodID params_make = params_class != NULL ? (*env)->GetMethodID(env, params_class, "<init>", "(II)V") : NULL;
    jmethodID add = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, activity->clazz), "addContentView",
                                        "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V");
    if (!cee_android_failed(env) && create != NULL && update != NULL && announce != NULL && params_make != NULL &&
        add != NULL) {
        jobject view = (*env)->NewObject(env, view_class, create, activity->clazz);
        jobject params = (*env)->NewObject(env, params_class, params_make, (jint)-1, (jint)-1); /* MATCH_PARENT */
        if (!cee_android_failed(env) && view != NULL && params != NULL) {
            (*env)->CallVoidMethod(env, activity->clazz, add, view, params);
            if (!cee_android_failed(env)) {
                cee_android.access_view = (*env)->NewGlobalRef(env, view);
                cee_android.access_update = update;
                cee_android.access_announce = announce;
                cee_android.access_failed = cee_android.access_view == NULL;
            }
        }
    }
    (void)cee_android_failed(env);
    (void)(*env)->PopLocalFrame(env, NULL);
    if (cee_android.access_failed) {
        __android_log_write(ANDROID_LOG_WARN, "ceesafe", "accessibility: the access view could not be made");
    }
    return !cee_android.access_failed;
}

/* On the activity's thread: the program's list goes to the access view if it changed, and a text to say is said. */
static void cee_android_access_give(ANativeActivity *activity) {
    JNIEnv *env = activity->env;
    pthread_mutex_lock(&cee_android.lock);
    int wanted = (cee_android.access_dirty && cee_android.access_packed != NULL) || cee_android.access_said != NULL;
    pthread_mutex_unlock(&cee_android.lock);
    if (!wanted || !cee_android_access_load(activity) || (*env)->PushLocalFrame(env, 8) != 0) {
        (void)cee_android_failed(env);
        return;
    }
    pthread_mutex_lock(&cee_android.lock);
    jbyteArray packed = NULL, said = NULL;
    if (cee_android.access_dirty && cee_android.access_packed != NULL) {
        packed = (*env)->NewByteArray(env, (jsize)cee_android.access_length);
        if (packed != NULL) {
            (*env)->SetByteArrayRegion(env, packed, 0, (jsize)cee_android.access_length,
                                       (const jbyte *)cee_android.access_packed);
            cee_android.access_dirty = 0;
        }
    }
    if (cee_android.access_said != NULL) {
        said = (*env)->NewByteArray(env, (jsize)cee_android.access_said_length);
        if (said != NULL) {
            (*env)->SetByteArrayRegion(env, said, 0, (jsize)cee_android.access_said_length,
                                       (const jbyte *)cee_android.access_said);
        }
        ceesafe_platform_free(cee_android.access_said);
        cee_android.access_said = NULL;
        cee_android.access_said_length = 0;
    }
    pthread_mutex_unlock(&cee_android.lock);
    if (packed != NULL) {
        (*env)->CallVoidMethod(env, cee_android.access_view, cee_android.access_update, packed);
        (void)cee_android_failed(env);
    }
    if (said != NULL) {
        (*env)->CallVoidMethod(env, cee_android.access_view, cee_android.access_announce, said);
        (void)cee_android_failed(env);
    }
    (void)(*env)->PopLocalFrame(env, NULL);
}

static int cee_android_ui(int fd, int events, void *data) {
    (void)events;
    (void)data;
    char drained[16];
    while (read(fd, drained, sizeof drained) == (ssize_t)sizeof drained) {
        /* every byte asks for the same thing: the latest wish */
    }
    pthread_mutex_lock(&cee_android.lock);
    ANativeActivity *activity = cee_android.activity;
    int on = cee_android.want_input, kind = cee_android.want_kind;
    int pick = cee_android.pick_state == 1;
    pthread_mutex_unlock(&cee_android.lock);
    if (activity == NULL || activity->env == NULL) {
        return 1;
    }
    if (pick) {
        cee_android_pick_start(activity);
    }
    cee_android_access_give(activity);
    if (!on && cee_android.input_view == NULL) {
        return 1;
    }
    if (cee_android_input_load(activity)) {
        JNIEnv *env = activity->env;
        (*env)->CallVoidMethod(env, cee_android.input_view, cee_android.input_apply, (jboolean)(on != 0), (jint)kind);
        (void)cee_android_failed(env);
    }
    return 1; /* stay registered */
}

/* The activity ends: its view goes with it, and a new activity makes a new one. */
static void cee_android_input_drop(ANativeActivity *activity) {
    JNIEnv *env = activity->env;
    if (env != NULL && cee_android.input_view != NULL) {
        (*env)->DeleteGlobalRef(env, cee_android.input_view);
    }
    cee_android.input_view = NULL;
    cee_android.input_apply = NULL;
    cee_android.input_failed = 0;
    /* The access view goes too (ADR 0164): a new activity's is given the list again. */
    if (env != NULL && cee_android.access_view != NULL) {
        (*env)->DeleteGlobalRef(env, cee_android.access_view);
    }
    cee_android.access_view = NULL;
    cee_android.access_failed = 0;
    pthread_mutex_lock(&cee_android.lock);
    cee_android.access_dirty = cee_android.access_packed != NULL;
    pthread_mutex_unlock(&cee_android.lock);
    /* A file dialog the activity will not answer now is cancelled (ADR 0158). The classes stay loaded. */
    pthread_mutex_lock(&cee_android.lock);
    if (cee_android.pick_state == 1 || cee_android.pick_state == 2) {
        cee_android.pick_status = 0;
        cee_android.pick_state = 3;
    }
    pthread_mutex_unlock(&cee_android.lock);
}

/* From the program's thread: the wish recorded, and the activity's thread asked to apply it. */
/* What InputView.apply takes as its kind: the kind of text, and the Return key's face above it (ADR 0161). */
static int cee_android_kind(const struct cee_window *w) { return w->text_kind | (w->return_key << 8); }

static void cee_android_wish(int on, int kind) {
    pthread_mutex_lock(&cee_android.lock);
    cee_android.want_input = on;
    cee_android.want_kind = kind;
    int fd = cee_android.ui_pipe[1];
    pthread_mutex_unlock(&cee_android.lock);
    if (fd >= 0) {
        char byte = 1;
        if (write(fd, &byte, 1) < 0) {
            /* the pipe is full of wishes already: the activity's thread will read the latest */
        }
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

/* The activity is off the screen. For a program that takes app events, this returns only once the program has taken
   BACKGROUND and waited or polled again, which is its word that it has saved, two seconds at most: the system does
   not end a process inside onStop (ADR 0159 §2.2). */
static void cee_android_stop(ANativeActivity *activity) {
    (void)activity;
    pthread_mutex_lock(&cee_android.lock);
    cee_android_life.background = 1;
    cee_android_life.saved = 0;
    cee_android_told(CEE_APP_BACKGROUND);
    /* Not while the program waits in a file dialog, whose picker is what covers the activity (ADR 0158). */
    int hold = cee_android_life.listening && cee_android.looper != NULL && cee_android.pick_state == 0;
    pthread_mutex_unlock(&cee_android.lock);
    cee_android_wake_program();
    for (int k = 0; hold && k < 400; k++) {
        usleep(5000);
        pthread_mutex_lock(&cee_android.lock);
        hold = !cee_android_life.saved && cee_android.looper != NULL;
        pthread_mutex_unlock(&cee_android.lock);
    }
}

static void cee_android_start(ANativeActivity *activity) {
    (void)activity;
    pthread_mutex_lock(&cee_android.lock);
    cee_android_life.background = 0;
    cee_android_told(CEE_APP_FOREGROUND);
    pthread_mutex_unlock(&cee_android.lock);
    cee_android_wake_program();
}

static void cee_android_low_memory(ANativeActivity *activity) {
    (void)activity;
    pthread_mutex_lock(&cee_android.lock);
    cee_android_told(CEE_APP_MEMORY);
    pthread_mutex_unlock(&cee_android.lock);
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
    cee_android_input_drop(activity);
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
    activity->callbacks->onStop = cee_android_stop;
    activity->callbacks->onStart = cee_android_start;
    activity->callbacks->onLowMemory = cee_android_low_memory;
    cee_android_read_scale(activity);
    /* The activity's thread's looper takes what the program asks of text input (ADR 0157). */
    ALooper *ui = ALooper_forThread();
    if (ui != NULL && cee_android.ui_pipe[0] < 0 && pipe(cee_android.ui_pipe) == 0) {
        (void)fcntl(cee_android.ui_pipe[0], F_SETFL, O_NONBLOCK);
        (void)fcntl(cee_android.ui_pipe[1], F_SETFL, O_NONBLOCK);
        ALooper_addFd(ui, cee_android.ui_pipe[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, cee_android_ui, NULL);
    }
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

/* Set while the input events are read, under the lock: the keyboard is to be asked for again once it is released. */
static int cee_android_again;

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
        if (touchscreen && (kind == AMOTION_EVENT_ACTION_HOVER_ENTER || kind == AMOTION_EVENT_ACTION_HOVER_MOVE ||
                            kind == AMOTION_EVENT_ACTION_HOVER_EXIT)) {
            /* A finger exploring the screen with TalkBack on (ADR 0164): not taken, so the views get it, and the
               access view says what is under the finger. */
            w->touches_on = on;
            return 0;
        }
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
            if (w->text_input == 1 && w->keyboard == 0.0f) {
                /* Text input is on and no keyboard shows: the user hid it (Back), and a tap asks for it again
                   (ADR 0157). With a hardware keyboard this shows nothing. */
                cee_android_again = 1;
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
    int32_t keyboard = cee_android.keyboard;
    uint8_t told[sizeof cee_android_life.told];
    unsigned told_count = cee_android_life.told_count;
    memcpy(told, cee_android_life.told, told_count);
    cee_android_life.told_count = 0;
    struct cee_access_request asked[CEE_ACCESS_QUEUE]; /* what TalkBack asked (ADR 0164) */
    unsigned asked_count = cee_android.access_asked_count;
    memcpy(asked, cee_android.access_asked, asked_count * sizeof asked[0]);
    cee_android.access_asked_count = 0;
    int access_active = cee_android.access_active;
    char *typed = cee_android.typed;
    size_t typed_length = cee_android.typed_length;
    cee_android.typed = NULL; /* taken: the activity's thread starts a new queue */
    cee_android.typed_length = 0;
    cee_android.typed_capacity = 0;
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
        ceesafe_platform_free(typed);
        return;
    }
    if (cee_android_again) {
        cee_android_again = 0;
        cee_android_wish(1, cee_android_kind(w));
    }
    for (unsigned k = 0; k < asked_count; k++) {
        cee_access_ask(w, asked[k].id, asked[k].action);
    }
    if (access_active) {
        w->access_active = 1;
    }
    /* What the input method typed (ADR 0157): committed text, the composition, Backspace and Enter. */
    for (size_t at = 0; at + 3u <= typed_length;) {
        int type = typed[at];
        size_t length = (size_t)(unsigned char)typed[at + 1] | ((size_t)(unsigned char)typed[at + 2] << 8);
        const char *bytes = typed + at + 3;
        if (at + 3u + length > typed_length) {
            break;
        }
        if (type == 0) {
            cee_window_commit(w, bytes, length);
        } else if (type == 1) {
            cee_window_compose(w, bytes, length, length);
        } else if (length == 1) {
            cee_window_key_tap(w, bytes[0] == 1 ? CEE_KEY_ENTER : CEE_KEY_BACKSPACE);
        }
        at += 3u + length;
    }
    ceesafe_platform_free(typed);
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
    cee_window_keyboard(w, (float)keyboard / scale);
    /* The app's life (ADR 0159). */
    for (unsigned k = 0; k < told_count; k++) {
        cee_window_app(w, told[k]);
    }
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
/* Text input on shows the soft keyboard on the window's input view, off hides it; a new kind of text shows the
   keyboard for it (ADR 0157). */
/* ---- Accessibility on Android (ADR 0164) ---- */
#define CEE_WINDOW_ACCESS 1

static void cee_access_put(char **at, const void *bytes, size_t length) {
    memcpy(*at, bytes, length);
    *at += length;
}

/* The window's list changed: packed for AccessView.update (a count; then each node's id, role, flags, layer,
   container as its index among those packed or -1 for the window, row, column, rows, columns, rectangle in pixels,
   range, and its label and value with their lengths before them, little-endian as every Android is), and the
   activity's thread asked to give it to the view. What a row read whole holds is not packed (ADR 0207): the row
   says it. Out of memory, the view keeps the last list. */
static void cee_native_access(struct cee_window *w, const struct cee_access_node *old, uint32_t old_count,
                              const char *old_text) {
    (void)old;
    (void)old_count;
    (void)old_text;
    uint32_t count = w->access_count;
    /* Where each node is among those packed, or CEE_ACCESS_TOP for one left out: what a row read whole holds, and
       what that holds. A container is before what it holds. */
    uint32_t *packed_as = (uint32_t *)ceesafe_platform_alloc(((size_t)count + 1u) * sizeof *packed_as);
    if (packed_as == NULL) {
        return;
    }
    uint32_t kept = 0;
    size_t length = 4u;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t up = w->access[k].up;
        int out = up != CEE_ACCESS_TOP && up < k &&
                  (packed_as[up] == CEE_ACCESS_TOP || cee_access_whole_row(w->access, count, up));
        packed_as[k] = out ? CEE_ACCESS_TOP : kept++;
        if (!out) {
            length += 8u + 4u + 4u + 4u + 20u + 16u + 24u + 4u + w->access[k].label_length + 4u + w->access[k].value_length;
        }
    }
    char *packed = (char *)ceesafe_platform_alloc(length);
    if (packed == NULL) {
        ceesafe_platform_free(packed_as);
        return;
    }
    float scale = w->scale > 0.0f ? w->scale : 1.0f;
    char *at = packed;
    cee_access_put(&at, &kept, 4u);
    for (uint32_t k = 0; k < count; k++) {
        const struct cee_access_node *n = &w->access[k];
        if (packed_as[k] == CEE_ACCESS_TOP) {
            continue;
        }
        float box[4] = {n->x * scale, n->y * scale, n->width * scale, n->height * scale};
        double range[3] = {n->min, n->max, n->now};
        int32_t tree[5] = {n->up != CEE_ACCESS_TOP && n->up < k ? (int32_t)packed_as[n->up] : -1, (int32_t)n->row,
                           (int32_t)n->column, (int32_t)n->rows, (int32_t)n->columns};
        cee_access_put(&at, &n->id, 8u);
        cee_access_put(&at, &n->role, 4u);
        cee_access_put(&at, &n->flags, 4u);
        cee_access_put(&at, &n->layer, 4u);
        cee_access_put(&at, tree, 20u);
        cee_access_put(&at, box, 16u);
        cee_access_put(&at, range, 24u);
        cee_access_put(&at, &n->label_length, 4u);
        cee_access_put(&at, w->access_text + n->label_start, n->label_length);
        cee_access_put(&at, &n->value_length, 4u);
        cee_access_put(&at, w->access_text + n->value_start, n->value_length);
    }
    ceesafe_platform_free(packed_as);
    pthread_mutex_lock(&cee_android.lock);
    ceesafe_platform_free(cee_android.access_packed);
    cee_android.access_packed = packed;
    cee_android.access_length = length;
    cee_android.access_dirty = 1;
    int fd = cee_android.ui_pipe[1];
    pthread_mutex_unlock(&cee_android.lock);
    if (fd >= 0) {
        char byte = 1;
        if (write(fd, &byte, 1) < 0) {
            /* the pipe is full: the activity's thread will come by */
        }
    }
}

/* Says `text` through TalkBack: the latest text not yet said, on the activity's thread. */
static void cee_native_announce(struct cee_window *w, const char *text, size_t length) {
    (void)w;
    char *copy = (char *)ceesafe_platform_alloc(length);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, text, length);
    pthread_mutex_lock(&cee_android.lock);
    ceesafe_platform_free(cee_android.access_said);
    cee_android.access_said = copy;
    cee_android.access_said_length = length;
    int fd = cee_android.ui_pipe[1];
    pthread_mutex_unlock(&cee_android.lock);
    if (fd >= 0) {
        char byte = 1;
        if (write(fd, &byte, 1) < 0) {
            /* the pipe is full: the activity's thread will come by */
        }
    }
}

static void cee_native_text_input(struct cee_window *w, int on) { cee_android_wish(on, cee_android_kind(w)); }
static void cee_native_text_kind(struct cee_window *w) { cee_android_wish(w->text_input == 1, cee_android_kind(w)); }
static void cee_native_text_area(struct cee_window *w) { (void)w; } /* the input view has no place of its own */

/* The program has saved (it waits or polls again after BACKGROUND): the activity's onStop, held for it, goes on
   (ADR 0159). */
static void cee_native_saved(struct cee_window *w) {
    (void)w;
    pthread_mutex_lock(&cee_android.lock);
    cee_android_life.saved = 1;
    pthread_mutex_unlock(&cee_android.lock);
}

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

/* A file dialog (ADR 0158): asked of the activity's thread, which starts the system's picker, and waited for here,
   on the program's thread, until the picker's answer comes (the activity's end is a cancel). 1 answered with
   `*paths` (the program's to free) and its length, 0 cancelled, less an error. */
static int32_t cee_android_pick(int kind, const char *extensions, const char *name, const char *file, char **paths,
                                size_t *length) {
    *paths = NULL;
    *length = 0;
    pthread_mutex_lock(&cee_android.lock);
    int fd = cee_android.ui_pipe[1];
    if (cee_android.activity == NULL || fd < 0 || cee_android.pick_state != 0) {
        pthread_mutex_unlock(&cee_android.lock);
        return -CEE_WINDOW_UNSUPPORTED;
    }
    cee_android.pick_kind = kind;
    cee_android.pick_extensions = extensions;
    cee_android.pick_name = name;
    cee_android.pick_file = file;
    cee_android.pick_state = 1;
    pthread_mutex_unlock(&cee_android.lock);
    char byte = 1;
    if (write(fd, &byte, 1) < 0) {
        /* the pipe is full of wishes: the activity's thread is about to read them, and sees this one */
    }
    int32_t status = 0;
    for (;;) {
        (void)ALooper_pollOnce(100, NULL, NULL, NULL);
        pthread_mutex_lock(&cee_android.lock);
        int answered = cee_android.pick_state == 3;
        if (answered) {
            status = cee_android.pick_status;
            *paths = cee_android.pick_paths;
            *length = cee_android.pick_length;
            cee_android.pick_paths = NULL;
            cee_android.pick_length = 0;
            cee_android.pick_state = 0;
        }
        pthread_mutex_unlock(&cee_android.lock);
        if (answered) {
            return status;
        }
        usleep(20000); /* the looper may have input waiting that this wait does not take */
    }
}

/* Open: each answer is a copy of the picked document in the app's cache folder (ADR 0158 §2.1). */
static int32_t cee_native_dialog(struct cee_window *w, int kind, const char *title, const char *name,
                                 const char *extensions, size_t extensions_length) {
    (void)title; /* the system's picker has its own */
    (void)name;
    (void)extensions_length;
    if (kind != CEE_DIALOG_FILE && kind != CEE_DIALOG_FILES) {
        return -CEE_WINDOW_UNSUPPORTED; /* a save's path and a folder: not a phone's to give (ADR 0158 §2.2) */
    }
    char *paths = NULL;
    size_t length = 0;
    int32_t status = cee_android_pick(kind, extensions, "", "", &paths, &length);
    int32_t result = status < 0 ? (status == -1 ? -CEE_WINDOW_RESOURCES : status) : 0;
    for (size_t start = 0, k = 0; status == 1 && result == 0 && k < length; k++) {
        if (paths[k] == '\0') {
            if (!cee_window_answer_add(w, paths + start, k - start)) {
                result = -CEE_WINDOW_RESOURCES;
            }
            start = k + 1;
            if (kind == CEE_DIALOG_FILE) {
                break;
            }
        }
    }
    ceesafe_platform_free(paths);
    return result;
}

/* Export (ADR 0158 §2.2): the system's picker creates a document and the file's bytes are written to it. 1 saved, 0
   cancelled. */
static int32_t cee_native_export(struct cee_window *w, const char *title, const char *name, const char *file) {
    (void)w;
    (void)title;
    char *paths = NULL;
    size_t length = 0;
    int32_t status = cee_android_pick(CEE_DIALOG_EXPORT, "", name, file, &paths, &length);
    ceesafe_platform_free(paths);
    return status == -1 ? -CEE_WINDOW_RESOURCES : status;
}

/* Printing a PDF (ADR 0178): PrintManager with the runtime's document adapter (Printing.java), started and waited
   for as a picker is; its answer is the job's state when the dialog is gone. 1 printed, 0 cancelled; a file that is
   not a PDF, or a job that failed, Resources. */
#define CEE_HAS_NATIVE_PRINT 1
static int32_t cee_native_print(struct cee_window *w, const char *title, const char *file) {
    (void)w;
    char *paths = NULL;
    size_t length = 0;
    int32_t status = cee_android_pick(CEE_DIALOG_PRINT, "", title, file, &paths, &length);
    ceesafe_platform_free(paths);
    return status == -1 ? -CEE_WINDOW_RESOURCES : status;
}

#elif defined(__linux__) || defined(__FreeBSD__)
/* X11 through libX11.so.6, loaded with dlopen (Linux, and FreeBSD with Xorg: ADR 0149): the declarations below are
   Xlib's (X11/Xlib.h, LP64), so no headers are needed to build. A frame is converted to the visual's pixel layout,
   scaled to the window's size, and drawn with XPutImage. */
#include <dlfcn.h>
#include <locale.h>
#include <sys/select.h>

/* KeyPress, KeyRelease, ButtonPress, ButtonRelease, PointerMotion, Exposure, StructureNotify, FocusChange. */
#define CEE_X_EVENTS (1L | 2L | 4L | 8L | 64L | (1L << 15) | (1L << 17) | (1L << 21) | (1L << 22))

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
typedef struct { /* XPropertyEvent */
    int type;
    unsigned long serial;
    int send_event;
    cee_x_display *display;
    cee_xid window;
    unsigned long atom, time;
    int state;
} cee_x_property;
typedef union {
    int type;
    cee_x_any any;
    cee_x_property property;
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
    int (*ungrab_pointer)(cee_x_display *, unsigned long); /* ADR 0206 */
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
    /* Accessibility (ADR 0164, AT-SPI 2): the title, which names the window's frame; the frame's serial in its path;
       whether the window has the keyboard (the frame is active). */
    char *title;
    uint32_t atspi_serial;
    int atspi_active;
    /* A title bar the program draws (ADR 0206): the window manager's atoms for it, the pointer's last position and
       the edge whose resize shape it shows (0 none), those shapes, and when the bar was last pressed (a double press
       maximizes). */
    unsigned long wm_state, wm_max_vert, wm_max_horz, wm_moveresize, wm_menu, wm_change_state;
    float pointer_x, pointer_y;
    uint32_t edge_shown;
    cee_xid edge_cursors[16];
    double bar_pressed_at;
    /* A Wayland window's own record (ADR 0205): this one then has no display, and only the fields above. */
    void *wl;
};

typedef void (*cee_x_fn)(void);
static cee_x_fn cee_x_symbol(void *handle, const char *name) {
    void *p = dlsym(handle, name);
    cee_x_fn f;
    memcpy(&f, &p, sizeof f); /* ISO C has no object-to-function pointer conversion; POSIX guarantees this one */
    return f;
}

/* Xlib's error event: what failed, and on what. */
typedef struct {
    int type;
    cee_x_display *display;
    unsigned long resource;
    unsigned long serial;
    unsigned char error_code, request_code, minor_code;
} cee_x_error_event;
typedef int (*cee_x_error_handler)(cee_x_display *, cee_x_error_event *);
static cee_x_error_handler cee_x_error_before;

/* An X error that the window's own requests did not cause: BadWindow (3), a window of another client gone before the
   request reached it (a clipboard's requestor that closed, as GNOME's Xwayland bridge's do), is no failure of the
   program's, and Xlib's default handler would end the process for it. Every other error goes to the handler there
   was before (the program's own, or Xlib's). */
static int cee_x_error(cee_x_display *display, cee_x_error_event *e) {
    if (e->error_code == 3 /* BadWindow */) {
        return 0;
    }
    return cee_x_error_before != NULL ? cee_x_error_before(display, e) : 0;
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
    CEE_X(ungrab_pointer, int (*)(cee_x_display *, unsigned long), "XUngrabPointer")
    cee_x_error_handler (*set_error_handler)(cee_x_error_handler) =
        (cee_x_error_handler(*)(cee_x_error_handler))cee_x_symbol(x, "XSetErrorHandler");
#undef CEE_X
    if (set_error_handler != NULL) {
        cee_x_error_before = set_error_handler(cee_x_error);
    }
    cee_x.state = 1;
    return 1;
}

/* ---- Accessibility: AT-SPI 2 (ADR 0164 §3 step 5) ---------------------------------------------------------------
   The application is on the accessibility bus as AT-SPI 2 describes it: its root, /org/a11y/atspi/accessible/root
   (role application), embedded in the registry's desktop at the first open; each window a frame under it
   (.../w<serial>); each element of the window's list a child of the frame, flat, in reading order
   (.../w<serial>_<id>). libdbus-1.so.3 is loaded with dlopen, as libX11 is, so nothing links it; without it, or
   without an accessibility bus, windows work as before and no reader sees them. Every request is answered on the
   window thread, from the pump and the waits, which take the connection's descriptor too. A request makes the
   window's accessActive true, as on the other systems: something reads it. The roles, states and signatures are
   at-spi2-core's (atspi-constants.h and xml/). Containers (ADR 0207) are objects too: a node's parent is its
   container's object, or the frame's; the Table and TableCell interfaces describe a table. */
#define CEE_WINDOW_ACCESS 1

typedef struct {
    const char *name;
    const char *message;
    unsigned int dummy1 : 1, dummy2 : 1, dummy3 : 1, dummy4 : 1, dummy5 : 1;
    void *padding1;
} cee_dbus_error;
typedef union {
    unsigned char bytes[128]; /* DBusMessageIter is 72 bytes on LP64; room to spare */
    void *align;
} cee_dbus_iter;
typedef struct cee_dbus_connection cee_dbus_connection;
typedef struct cee_dbus_message cee_dbus_message;

static struct {
    int state; /* 0 not tried, 1 loaded, -1 failed */
    void (*error_init)(cee_dbus_error *);
    void (*error_free)(cee_dbus_error *);
    cee_dbus_connection *(*bus_get_private)(int, cee_dbus_error *);
    cee_dbus_connection *(*open_private)(const char *, cee_dbus_error *);
    uint32_t (*bus_register)(cee_dbus_connection *, cee_dbus_error *);
    const char *(*unique_name)(cee_dbus_connection *);
    void (*set_exit_on_disconnect)(cee_dbus_connection *, uint32_t);
    uint32_t (*get_unix_fd)(cee_dbus_connection *, int *);
    uint32_t (*read_write)(cee_dbus_connection *, int);
    cee_dbus_message *(*pop_message)(cee_dbus_connection *);
    uint32_t (*send)(cee_dbus_connection *, cee_dbus_message *, uint32_t *);
    void (*flush)(cee_dbus_connection *);
    cee_dbus_message *(*send_with_reply_and_block)(cee_dbus_connection *, cee_dbus_message *, int, cee_dbus_error *);
    void (*connection_close)(cee_dbus_connection *);
    void (*connection_unref)(cee_dbus_connection *);
    cee_dbus_message *(*new_method_call)(const char *, const char *, const char *, const char *);
    cee_dbus_message *(*new_method_return)(cee_dbus_message *);
    cee_dbus_message *(*new_error)(cee_dbus_message *, const char *, const char *);
    cee_dbus_message *(*new_signal)(const char *, const char *, const char *);
    void (*message_unref)(cee_dbus_message *);
    int (*message_type)(cee_dbus_message *);
    const char *(*message_path)(cee_dbus_message *);
    const char *(*message_interface)(cee_dbus_message *);
    const char *(*message_member)(cee_dbus_message *);
    uint32_t (*iter_init)(cee_dbus_message *, cee_dbus_iter *);
    void (*iter_init_append)(cee_dbus_message *, cee_dbus_iter *);
    uint32_t (*append_basic)(cee_dbus_iter *, int, const void *);
    uint32_t (*open_container)(cee_dbus_iter *, int, const char *, cee_dbus_iter *);
    uint32_t (*close_container)(cee_dbus_iter *, cee_dbus_iter *);
    int (*arg_type)(cee_dbus_iter *);
    void (*get_basic)(cee_dbus_iter *, void *);
    uint32_t (*iter_next)(cee_dbus_iter *);
    void (*recurse)(cee_dbus_iter *, cee_dbus_iter *);
} cee_dbus;

/* The application's one connection to the accessibility bus, made at the first open; its unique name; the desktop the
   registry embedded the root in; and the serial the next window's frame takes. */
static struct {
    int state; /* 0 not tried, 1 connected, -1 none */
    cee_dbus_connection *bus;
    char name[256];
    char desktop_name[256];
    char desktop_path[256];
    char app_name[64]; /* the process's name, the application's (as a GTK application's is its program's) */
    uint32_t next_serial;
} cee_atspi;

static int cee_dbus_load(void) {
    if (cee_dbus.state != 0) {
        return cee_dbus.state == 1;
    }
    cee_dbus.state = -1;
    void *d = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
    if (d == NULL) {
        return 0;
    }
#define CEE_DBUS(field, type, name)                                                                                    \
    cee_dbus.field = (type)cee_x_symbol(d, name);                                                                      \
    if (cee_dbus.field == NULL) {                                                                                      \
        return 0;                                                                                                      \
    }
    CEE_DBUS(error_init, void (*)(cee_dbus_error *), "dbus_error_init")
    CEE_DBUS(error_free, void (*)(cee_dbus_error *), "dbus_error_free")
    CEE_DBUS(bus_get_private, cee_dbus_connection * (*)(int, cee_dbus_error *), "dbus_bus_get_private")
    CEE_DBUS(open_private, cee_dbus_connection * (*)(const char *, cee_dbus_error *), "dbus_connection_open_private")
    CEE_DBUS(bus_register, uint32_t(*)(cee_dbus_connection *, cee_dbus_error *), "dbus_bus_register")
    CEE_DBUS(unique_name, const char *(*)(cee_dbus_connection *), "dbus_bus_get_unique_name")
    CEE_DBUS(set_exit_on_disconnect, void (*)(cee_dbus_connection *, uint32_t),
             "dbus_connection_set_exit_on_disconnect")
    CEE_DBUS(get_unix_fd, uint32_t(*)(cee_dbus_connection *, int *), "dbus_connection_get_unix_fd")
    CEE_DBUS(read_write, uint32_t(*)(cee_dbus_connection *, int), "dbus_connection_read_write")
    CEE_DBUS(pop_message, cee_dbus_message * (*)(cee_dbus_connection *), "dbus_connection_pop_message")
    CEE_DBUS(send, uint32_t(*)(cee_dbus_connection *, cee_dbus_message *, uint32_t *), "dbus_connection_send")
    CEE_DBUS(flush, void (*)(cee_dbus_connection *), "dbus_connection_flush")
    CEE_DBUS(send_with_reply_and_block,
             cee_dbus_message * (*)(cee_dbus_connection *, cee_dbus_message *, int, cee_dbus_error *),
             "dbus_connection_send_with_reply_and_block")
    CEE_DBUS(connection_close, void (*)(cee_dbus_connection *), "dbus_connection_close")
    CEE_DBUS(connection_unref, void (*)(cee_dbus_connection *), "dbus_connection_unref")
    CEE_DBUS(new_method_call, cee_dbus_message * (*)(const char *, const char *, const char *, const char *),
             "dbus_message_new_method_call")
    CEE_DBUS(new_method_return, cee_dbus_message * (*)(cee_dbus_message *), "dbus_message_new_method_return")
    CEE_DBUS(new_error, cee_dbus_message * (*)(cee_dbus_message *, const char *, const char *),
             "dbus_message_new_error")
    CEE_DBUS(new_signal, cee_dbus_message * (*)(const char *, const char *, const char *), "dbus_message_new_signal")
    CEE_DBUS(message_unref, void (*)(cee_dbus_message *), "dbus_message_unref")
    CEE_DBUS(message_type, int (*)(cee_dbus_message *), "dbus_message_get_type")
    CEE_DBUS(message_path, const char *(*)(cee_dbus_message *), "dbus_message_get_path")
    CEE_DBUS(message_interface, const char *(*)(cee_dbus_message *), "dbus_message_get_interface")
    CEE_DBUS(message_member, const char *(*)(cee_dbus_message *), "dbus_message_get_member")
    CEE_DBUS(iter_init, uint32_t(*)(cee_dbus_message *, cee_dbus_iter *), "dbus_message_iter_init")
    CEE_DBUS(iter_init_append, void (*)(cee_dbus_message *, cee_dbus_iter *), "dbus_message_iter_init_append")
    CEE_DBUS(append_basic, uint32_t(*)(cee_dbus_iter *, int, const void *), "dbus_message_iter_append_basic")
    CEE_DBUS(open_container, uint32_t(*)(cee_dbus_iter *, int, const char *, cee_dbus_iter *),
             "dbus_message_iter_open_container")
    CEE_DBUS(close_container, uint32_t(*)(cee_dbus_iter *, cee_dbus_iter *), "dbus_message_iter_close_container")
    CEE_DBUS(arg_type, int (*)(cee_dbus_iter *), "dbus_message_iter_get_arg_type")
    CEE_DBUS(get_basic, void (*)(cee_dbus_iter *, void *), "dbus_message_iter_get_basic")
    CEE_DBUS(iter_next, uint32_t(*)(cee_dbus_iter *), "dbus_message_iter_next")
    CEE_DBUS(recurse, void (*)(cee_dbus_iter *, cee_dbus_iter *), "dbus_message_iter_recurse")
#undef CEE_DBUS
    cee_dbus.state = 1;
    return 1;
}

#define CEE_ATSPI_ROOT "/org/a11y/atspi/accessible/root"
#define CEE_ATSPI_PREFIX "/org/a11y/atspi/accessible/w"

/* What a path names: the root (window NULL), a window's frame (index -1), or an element of a window (its index in the
   list); 0 for no object of ours. */
static int cee_atspi_find(const char *path, struct cee_window **window, int64_t *index) {
    *window = NULL;
    *index = -1;
    if (path == NULL) {
        return 0;
    }
    if (strcmp(path, CEE_ATSPI_ROOT) == 0) {
        return 1;
    }
    size_t prefix = sizeof(CEE_ATSPI_PREFIX) - 1u;
    if (strncmp(path, CEE_ATSPI_PREFIX, prefix) != 0) {
        return 0;
    }
    char *end = NULL;
    unsigned long serial = strtoul(path + prefix, &end, 10);
    for (struct cee_window *w = cee_wake_windows; w != NULL; w = w->wake_link) {
        if (!w->scripted && w->native != NULL && ((struct cee_x11 *)w->native)->atspi_serial == (uint32_t)serial) {
            *window = w;
            if (*end == '\0') {
                return 1;
            }
            if (*end != '_') {
                return 0;
            }
            unsigned long long id = strtoull(end + 1, NULL, 10);
            for (uint32_t k = 0; k < w->access_count; k++) {
                if (w->access[k].id == (uint64_t)id) {
                    *index = (int64_t)k;
                    return 1;
                }
            }
            return 0;
        }
    }
    return 0;
}

/* An object's path: the root, a frame (index -1) or an element. */
static void cee_atspi_path(char *out, size_t size, struct cee_window *w, int64_t index) {
    if (w == NULL) {
        snprintf(out, size, "%s", CEE_ATSPI_ROOT);
    } else if (index < 0) {
        snprintf(out, size, "%s%u", CEE_ATSPI_PREFIX, ((struct cee_x11 *)w->native)->atspi_serial);
    } else {
        snprintf(out, size, "%s%u_%llu", CEE_ATSPI_PREFIX, ((struct cee_x11 *)w->native)->atspi_serial,
                 (unsigned long long)w->access[index].id);
    }
}

/* ---- Writing replies */

static void cee_atspi_string(cee_dbus_iter *it, const char *s) {
    if (s == NULL) {
        s = "";
    }
    cee_dbus.append_basic(it, 's', &s);
}

/* A run of the list's text as a string (copied, so it ends; invalid UTF-8 is libdbus's to refuse, which the list never
   has: lib/std's are StringViews). */
static void cee_atspi_text(cee_dbus_iter *it, const char *text, uint32_t start, uint32_t length) {
    char few[256];
    char *s = length < sizeof few ? few : (char *)ceesafe_platform_alloc((size_t)length + 1u);
    if (s == NULL) {
        cee_atspi_string(it, "");
        return;
    }
    if (length > 0) {
        memcpy(s, text + start, length);
    }
    s[length] = '\0';
    cee_atspi_string(it, s);
    if (s != few) {
        ceesafe_platform_free(s);
    }
}

static void cee_atspi_int(cee_dbus_iter *it, int32_t v) { cee_dbus.append_basic(it, 'i', &v); }
static void cee_atspi_uint(cee_dbus_iter *it, uint32_t v) { cee_dbus.append_basic(it, 'u', &v); }
static void cee_atspi_bool(cee_dbus_iter *it, int v) {
    uint32_t b = v ? 1u : 0u;
    cee_dbus.append_basic(it, 'b', &b);
}
static void cee_atspi_double(cee_dbus_iter *it, double v) { cee_dbus.append_basic(it, 'd', &v); }

/* An object reference, (so): ours (window NULL with index -2 for "none", the null object) or the desktop. */
static void cee_atspi_ref(cee_dbus_iter *it, struct cee_window *w, int64_t index) {
    cee_dbus_iter s;
    char path[160];
    cee_dbus.open_container(it, 'r', NULL, &s);
    if (index == -2) {
        cee_atspi_string(&s, cee_atspi.name);
        const char *none = "/org/a11y/atspi/null";
        cee_dbus.append_basic(&s, 'o', &none);
    } else {
        cee_atspi_path(path, sizeof path, w, index);
        const char *p = path;
        cee_atspi_string(&s, cee_atspi.name);
        cee_dbus.append_basic(&s, 'o', &p);
    }
    cee_dbus.close_container(it, &s);
}

static void cee_atspi_desktop(cee_dbus_iter *it) {
    cee_dbus_iter s;
    cee_dbus.open_container(it, 'r', NULL, &s);
    cee_atspi_string(&s, cee_atspi.desktop_name);
    const char *p = cee_atspi.desktop_path;
    cee_dbus.append_basic(&s, 'o', &p);
    cee_dbus.close_container(it, &s);
}

/* ---- What an object is */

/* An element's AT-SPI role (ADR 0164's roles in lib/std's order). */
static uint32_t cee_atspi_role(const struct cee_access_node *n) {
    switch (n->role) {
    case 1: return 43; /* PUSH_BUTTON */
    case 2: return 7;  /* CHECK_BOX */
    case 3: return 44; /* RADIO_BUTTON */
    case 4: return (n->flags & CEE_ACCESS_SECURE) != 0 ? 40u : 79u; /* PASSWORD_TEXT, ENTRY */
    case 5: return 51; /* SLIDER */
    case 6: return 37; /* PAGE_TAB */
    case 7: return 32; /* LIST_ITEM */
    case 8: return 35; /* MENU_ITEM */
    case 9: return 11; /* COMBO_BOX */
    case 10: return 27; /* IMAGE */
    case CEE_ACCESS_GROUP: return 39;         /* PANEL */
    case CEE_ACCESS_LIST: return 31;          /* LIST */
    case CEE_ACCESS_MENU: return 33;          /* MENU */
    case CEE_ACCESS_MENU_BAR: return 34;      /* MENU_BAR */
    case CEE_ACCESS_TAB_LIST: return 38;      /* PAGE_TAB_LIST */
    case CEE_ACCESS_TABLE: return 55;         /* TABLE */
    case CEE_ACCESS_ROW: return 90;           /* TABLE_ROW */
    case CEE_ACCESS_CELL: return 56;          /* TABLE_CELL */
    case CEE_ACCESS_COLUMN_HEADER: return 57; /* TABLE_COLUMN_HEADER */
    default: return 29; /* LABEL */
    }
}

static const char *cee_atspi_role_name(uint32_t role) {
    switch (role) {
    case 75: return "application";
    case 23: return "frame";
    case 43: return "push button";
    case 7: return "check box";
    case 44: return "radio button";
    case 40: return "password text";
    case 79: return "entry";
    case 51: return "slider";
    case 37: return "page tab";
    case 32: return "list item";
    case 35: return "menu item";
    case 11: return "combo box";
    case 27: return "image";
    case 39: return "panel";
    case 31: return "list";
    case 33: return "menu";
    case 34: return "menu bar";
    case 38: return "page tab list";
    case 55: return "table";
    case 90: return "table row";
    case 56: return "table cell";
    case 57: return "table column header";
    default: return "label";
    }
}

/* Whether an element has Action (a press), Value (a slider) or Text (a field). */
static int cee_atspi_acts(const struct cee_access_node *n) { return n->role != 0 && n->role != 4 && n->role != 5 && n->role != 10; }
static int cee_atspi_valued(const struct cee_access_node *n) { return n->role == 5; }
static int cee_atspi_texted(const struct cee_access_node *n) { return n->role == 4; }
/* A table and its cells and column headers (ADR 0207). */
static int cee_atspi_tabled(const struct cee_access_node *n) { return n->role == CEE_ACCESS_TABLE; }
static int cee_atspi_celled(const struct cee_access_node *n) {
    return n->role == CEE_ACCESS_CELL || n->role == CEE_ACCESS_COLUMN_HEADER;
}

/* The tree (ADR 0207), in the list: the children of node `up` (CEE_ACCESS_TOP for the frame) are the nodes whose
   container it is, in list order. */
static int32_t cee_atspi_child_count(struct cee_window *w, uint32_t up) {
    int32_t count = 0;
    for (uint32_t k = 0; k < w->access_count; k++) {
        count += w->access[k].up == up;
    }
    return count;
}
static int64_t cee_atspi_child_at(struct cee_window *w, uint32_t up, int32_t at) {
    for (uint32_t k = 0; k < w->access_count && at >= 0; k++) {
        if (w->access[k].up == up && at-- == 0) {
            return (int64_t)k;
        }
    }
    return -2;
}
/* Node k's index among its container's children, in `nodes`. */
static int32_t cee_atspi_place(const struct cee_access_node *nodes, uint32_t k) {
    int32_t at = 0;
    for (uint32_t j = 0; j < k; j++) {
        at += nodes[j].up == nodes[k].up;
    }
    return at;
}
/* The table a cell or column header is in (its row's container), or -2. */
static int64_t cee_atspi_table_of(struct cee_window *w, uint32_t k) {
    uint32_t row = w->access[k].up;
    if (row == CEE_ACCESS_TOP || w->access[row].role != CEE_ACCESS_ROW || w->access[row].up == CEE_ACCESS_TOP ||
        w->access[w->access[row].up].role != CEE_ACCESS_TABLE) {
        return -2;
    }
    return (int64_t)w->access[row].up;
}
/* The cell or column header of table `t` at `row` and `column`, from 0; -2 for none. */
static int64_t cee_atspi_cell_at(struct cee_window *w, uint32_t t, int32_t row, int32_t column) {
    for (uint32_t k = t + 1u; k < w->access_count && row >= 0 && column >= 0; k++) {
        const struct cee_access_node *c = &w->access[k];
        if (cee_atspi_celled(c) && c->row == (uint32_t)row + 1u && c->column == (uint32_t)column + 1u &&
            cee_atspi_table_of(w, k) == (int64_t)t) {
            return (int64_t)k;
        }
    }
    return -2;
}

/* The state set, two words of AtspiStateType bits. */
static void cee_atspi_states(struct cee_window *w, int64_t index, uint32_t words[2]) {
    words[0] = words[1] = 0;
#define CEE_ATSPI_STATE(bit) (words[(bit) / 32] |= 1u << ((bit) % 32))
    if (w == NULL) {
        return; /* the application has none */
    }
    CEE_ATSPI_STATE(25); /* SHOWING */
    CEE_ATSPI_STATE(30); /* VISIBLE */
    if (index < 0) {
        CEE_ATSPI_STATE(8);  /* ENABLED */
        CEE_ATSPI_STATE(24); /* SENSITIVE */
        if (((struct cee_x11 *)w->native)->atspi_active) {
            CEE_ATSPI_STATE(1); /* ACTIVE */
        }
        return;
    }
    const struct cee_access_node *n = &w->access[index];
    if ((n->flags & CEE_ACCESS_DISABLED) == 0) {
        CEE_ATSPI_STATE(8);
        CEE_ATSPI_STATE(24);
    }
    if (n->role != 0 && n->role != 10) {
        CEE_ATSPI_STATE(11); /* FOCUSABLE */
    }
    if ((n->flags & CEE_ACCESS_FOCUSED) != 0) {
        CEE_ATSPI_STATE(12); /* FOCUSED */
    }
    if (n->role == 2 || n->role == 3) {
        CEE_ATSPI_STATE(41); /* CHECKABLE */
        if ((n->flags & CEE_ACCESS_CHECKED) != 0) {
            CEE_ATSPI_STATE(4); /* CHECKED */
        }
    }
    if (n->role == 6 || n->role == 7) {
        CEE_ATSPI_STATE(22); /* SELECTABLE */
        if ((n->flags & CEE_ACCESS_SELECTED) != 0) {
            CEE_ATSPI_STATE(23); /* SELECTED */
        }
    }
    if (n->role == 9) {
        CEE_ATSPI_STATE(9); /* EXPANDABLE */
        if ((n->flags & CEE_ACCESS_EXPANDED) != 0) {
            CEE_ATSPI_STATE(10); /* EXPANDED */
        }
    }
    if (n->role == 4) {
        CEE_ATSPI_STATE(7);  /* EDITABLE */
        CEE_ATSPI_STATE(26); /* SINGLE_LINE */
    }
#undef CEE_ATSPI_STATE
}

/* The window's content origin on the screen, in pixels. */
static void cee_atspi_origin(struct cee_window *w, int *x, int *y) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    cee_xid child = 0;
    *x = *y = 0;
    if (n->wl != NULL) {
        return; /* a Wayland window does not know where it is: screen coordinates are the window's (ADR 0205) */
    }
    cee_x.translate_coordinates(n->display, n->window, cee_x.root_window(n->display, n->screen), 0, 0, x, y, &child);
}

/* An object's box: screen (coord 0) or window and parent (1, 2) coordinates, in pixels. */
static void cee_atspi_extents(struct cee_window *w, int64_t index, uint32_t coord, int32_t box[4]) {
    box[0] = box[1] = box[2] = box[3] = 0;
    if (w == NULL) {
        return;
    }
    float scale = w->scale > 0.0f ? w->scale : 1.0f;
    int ox = 0, oy = 0;
    if (coord == 0) {
        cee_atspi_origin(w, &ox, &oy);
    }
    if (index < 0) {
        box[0] = ox;
        box[1] = oy;
        box[2] = (int32_t)w->width;
        box[3] = (int32_t)w->height;
        return;
    }
    const struct cee_access_node *n = &w->access[index];
    box[0] = ox + (int32_t)(n->x * scale);
    box[1] = oy + (int32_t)(n->y * scale);
    box[2] = (int32_t)(n->width * scale + 0.5f);
    box[3] = (int32_t)(n->height * scale + 0.5f);
}

/* ---- Text: a field's value, in characters as AT-SPI counts them (code points), and the list's bytes */

static uint32_t cee_atspi_chars(const char *s, uint32_t length) {
    uint32_t count = 0;
    for (uint32_t k = 0; k < length; k++) {
        if (((unsigned char)s[k] & 0xC0u) != 0x80u) {
            count++;
        }
    }
    return count;
}

/* The byte offset of character `chars` (clamped to the end). */
static uint32_t cee_atspi_byte(const char *s, uint32_t length, int32_t chars) {
    if (chars <= 0) {
        return 0;
    }
    uint32_t seen = 0;
    for (uint32_t k = 0; k < length; k++) {
        if (((unsigned char)s[k] & 0xC0u) != 0x80u) {
            if (seen == (uint32_t)chars) {
                return k;
            }
            seen++;
        }
    }
    return length;
}

/* The [start, end) characters of a field around `offset` for boundary `kind` (AtspiTextBoundaryType): one character,
   or a word (letters and digits, with what follows up to the next word for WORD_START), or the whole one-line text
   for a sentence or a line. */
static void cee_atspi_around(const char *s, uint32_t length, int32_t offset, uint32_t kind, int32_t *start,
                             int32_t *end) {
    int32_t count = (int32_t)cee_atspi_chars(s, length);
    if (offset < 0) {
        offset = 0;
    }
    if (offset > count) {
        offset = count;
    }
    if (kind == 0) {
        *start = offset;
        *end = offset < count ? offset + 1 : offset;
        return;
    }
    if (kind != 1 && kind != 2) {
        *start = 0;
        *end = count;
        return;
    }
    /* Words: runs that are not spaces, by character. */
    int32_t a = offset, b = offset;
    while (a > 0) {
        uint32_t at = cee_atspi_byte(s, length, a - 1);
        if (s[at] == ' ' || s[at] == '\t') {
            break;
        }
        a--;
    }
    while (b < count) {
        uint32_t at = cee_atspi_byte(s, length, b);
        if (s[at] == ' ' || s[at] == '\t') {
            break;
        }
        b++;
    }
    while (kind == 1 && b < count) { /* WORD_START: the spaces after it too */
        uint32_t at = cee_atspi_byte(s, length, b);
        if (s[at] != ' ' && s[at] != '\t') {
            break;
        }
        b++;
    }
    *start = a;
    *end = b;
}

/* ---- Signals */

/* An Event.<kind> signal on an object, AT-SPI's (siiva{sv}): its detail, two numbers and a value written by `value`
   (an int 0 when NULL). */
typedef void (*cee_atspi_any)(cee_dbus_iter *, const void *);
static void cee_atspi_signal(struct cee_window *w, int64_t index, const char *interface, const char *member,
                             const char *detail, int32_t one, int32_t two, const char *signature, cee_atspi_any value,
                             const void *data) {
    if (cee_atspi.state != 1) {
        return;
    }
    char path[160];
    cee_atspi_path(path, sizeof path, w, index);
    cee_dbus_message *m = cee_dbus.new_signal(path, interface, member);
    if (m == NULL) {
        return;
    }
    cee_dbus_iter it, v, d;
    cee_dbus.iter_init_append(m, &it);
    cee_atspi_string(&it, detail);
    cee_atspi_int(&it, one);
    cee_atspi_int(&it, two);
    cee_dbus.open_container(&it, 'v', value != NULL ? signature : "i", &v);
    if (value != NULL) {
        value(&v, data);
    } else {
        cee_atspi_int(&v, 0);
    }
    cee_dbus.close_container(&it, &v);
    cee_dbus.open_container(&it, 'a', "{sv}", &d);
    cee_dbus.close_container(&it, &d);
    cee_dbus.send(cee_atspi.bus, m, NULL);
    cee_dbus.message_unref(m);
}

static void cee_atspi_any_string(cee_dbus_iter *it, const void *data) { cee_atspi_string(it, (const char *)data); }
static void cee_atspi_any_double(cee_dbus_iter *it, const void *data) { cee_atspi_double(it, *(const double *)data); }

/* A child added or removed: the frame tells, with the child's reference. */
struct cee_atspi_child {
    struct cee_window *w;
    int64_t index;
};
static void cee_atspi_any_child(cee_dbus_iter *it, const void *data) {
    const struct cee_atspi_child *c = (const struct cee_atspi_child *)data;
    cee_atspi_ref(it, c->w, c->index);
}

static void cee_atspi_state_changed(struct cee_window *w, int64_t index, const char *state, int on) {
    cee_atspi_signal(w, index, "org.a11y.atspi.Event.Object", "StateChanged", state, on ? 1 : 0, 0, NULL, NULL, NULL);
}

/* ---- Answering requests */

static void cee_atspi_reply_error(cee_dbus_message *m, const char *name, const char *text) {
    cee_dbus_message *r = cee_dbus.new_error(m, name, text);
    if (r != NULL) {
        cee_dbus.send(cee_atspi.bus, r, NULL);
        cee_dbus.message_unref(r);
    }
}

/* The arguments of a request, in order: ints, uints and a string. */
static int cee_atspi_args(cee_dbus_message *m, const char *types, void *out[]) {
    cee_dbus_iter it;
    if (!cee_dbus.iter_init(m, &it)) {
        return types[0] == '\0';
    }
    for (size_t k = 0; types[k] != '\0'; k++) {
        if (cee_dbus.arg_type(&it) != types[k]) {
            return 0;
        }
        cee_dbus.get_basic(&it, out[k]);
        if (types[k + 1] != '\0' && !cee_dbus.iter_next(&it)) {
            return 0;
        }
    }
    return 1;
}

/* One property of an object as a variant: 1 written, 0 not one of its properties. */
static int cee_atspi_property(cee_dbus_iter *out, struct cee_window *w, int64_t index, const char *interface,
                              const char *name) {
    const struct cee_access_node *n = w != NULL && index >= 0 ? &w->access[index] : NULL;
    cee_dbus_iter v;
#define CEE_ATSPI_VARIANT(sig, write)                                                                                  \
    do {                                                                                                               \
        cee_dbus.open_container(out, 'v', sig, &v);                                                                    \
        write;                                                                                                         \
        cee_dbus.close_container(out, &v);                                                                             \
        return 1;                                                                                                      \
    } while (0)
    if (strcmp(interface, "org.a11y.atspi.Accessible") == 0) {
        if (strcmp(name, "Name") == 0) {
            if (n != NULL) {
                CEE_ATSPI_VARIANT("s", cee_atspi_text(&v, w->access_text, n->label_start, n->label_length));
            }
            CEE_ATSPI_VARIANT("s", cee_atspi_string(&v, w != NULL ? ((struct cee_x11 *)w->native)->title
                                                                    : cee_atspi.app_name));
        }
        if (strcmp(name, "Description") == 0 || strcmp(name, "Locale") == 0 || strcmp(name, "AccessibleId") == 0 ||
            strcmp(name, "HelpText") == 0) {
            CEE_ATSPI_VARIANT("s", cee_atspi_string(&v, ""));
        }
        if (strcmp(name, "Parent") == 0) {
            if (w == NULL) {
                CEE_ATSPI_VARIANT("(so)", cee_atspi_desktop(&v));
            }
            if (n != NULL && n->up != CEE_ACCESS_TOP) {
                CEE_ATSPI_VARIANT("(so)", cee_atspi_ref(&v, w, (int64_t)n->up)); /* its container (ADR 0207) */
            }
            CEE_ATSPI_VARIANT("(so)", cee_atspi_ref(&v, n != NULL ? w : NULL, -1));
        }
        if (strcmp(name, "ChildCount") == 0) {
            int32_t count = 0;
            if (w == NULL) {
                for (struct cee_window *e = cee_wake_windows; e != NULL; e = e->wake_link) {
                    count += !e->scripted && e->native != NULL;
                }
            } else {
                count = cee_atspi_child_count(w, n == NULL ? CEE_ACCESS_TOP : (uint32_t)index);
            }
            CEE_ATSPI_VARIANT("i", cee_atspi_int(&v, count));
        }
        if (strcmp(name, "version") == 0) {
            CEE_ATSPI_VARIANT("u", cee_atspi_uint(&v, 1));
        }
    } else if (strcmp(interface, "org.a11y.atspi.Application") == 0 && w == NULL) {
        if (strcmp(name, "ToolkitName") == 0) {
            CEE_ATSPI_VARIANT("s", cee_atspi_string(&v, "CeeSafe"));
        }
        if (strcmp(name, "Version") == 0 || strcmp(name, "ToolkitVersion") == 0) {
            CEE_ATSPI_VARIANT("s", cee_atspi_string(&v, "1"));
        }
        if (strcmp(name, "AtspiVersion") == 0) {
            CEE_ATSPI_VARIANT("s", cee_atspi_string(&v, "2.1"));
        }
        if (strcmp(name, "InterfaceVersion") == 0) {
            CEE_ATSPI_VARIANT("u", cee_atspi_uint(&v, 1));
        }
        if (strcmp(name, "Id") == 0) {
            CEE_ATSPI_VARIANT("i", cee_atspi_int(&v, 0));
        }
    } else if (strcmp(interface, "org.a11y.atspi.Value") == 0 && n != NULL && cee_atspi_valued(n)) {
        if (strcmp(name, "MinimumValue") == 0) {
            CEE_ATSPI_VARIANT("d", cee_atspi_double(&v, n->min));
        }
        if (strcmp(name, "MaximumValue") == 0) {
            CEE_ATSPI_VARIANT("d", cee_atspi_double(&v, n->max));
        }
        if (strcmp(name, "MinimumIncrement") == 0) {
            CEE_ATSPI_VARIANT("d", cee_atspi_double(&v, (n->max - n->min) / 100.0));
        }
        if (strcmp(name, "CurrentValue") == 0) {
            CEE_ATSPI_VARIANT("d", cee_atspi_double(&v, n->now));
        }
        if (strcmp(name, "Text") == 0) {
            CEE_ATSPI_VARIANT("s", cee_atspi_text(&v, w->access_text, n->value_start, n->value_length));
        }
        if (strcmp(name, "version") == 0) {
            CEE_ATSPI_VARIANT("u", cee_atspi_uint(&v, 1));
        }
    } else if (strcmp(interface, "org.a11y.atspi.Text") == 0 && n != NULL && cee_atspi_texted(n)) {
        const char *s = w->access_text + n->value_start;
        if (strcmp(name, "CharacterCount") == 0) {
            CEE_ATSPI_VARIANT("i", cee_atspi_int(&v, (int32_t)cee_atspi_chars(s, n->value_length)));
        }
        if (strcmp(name, "CaretOffset") == 0) {
            CEE_ATSPI_VARIANT("i", cee_atspi_int(&v, (int32_t)cee_atspi_chars(s, n->caret)));
        }
        if (strcmp(name, "version") == 0) {
            CEE_ATSPI_VARIANT("u", cee_atspi_uint(&v, 1));
        }
    } else if (strcmp(interface, "org.a11y.atspi.Action") == 0 && n != NULL && cee_atspi_acts(n)) {
        if (strcmp(name, "NActions") == 0) {
            CEE_ATSPI_VARIANT("i", cee_atspi_int(&v, 1));
        }
        if (strcmp(name, "version") == 0) {
            CEE_ATSPI_VARIANT("u", cee_atspi_uint(&v, 1));
        }
    } else if (strcmp(interface, "org.a11y.atspi.Table") == 0 && n != NULL && cee_atspi_tabled(n)) {
        if (strcmp(name, "NRows") == 0) {
            CEE_ATSPI_VARIANT("i", cee_atspi_int(&v, (int32_t)n->rows));
        }
        if (strcmp(name, "NColumns") == 0) {
            CEE_ATSPI_VARIANT("i", cee_atspi_int(&v, (int32_t)n->columns));
        }
        if (strcmp(name, "NSelectedRows") == 0 || strcmp(name, "NSelectedColumns") == 0) {
            CEE_ATSPI_VARIANT("i", cee_atspi_int(&v, 0));
        }
        if (strcmp(name, "Caption") == 0 || strcmp(name, "Summary") == 0) {
            CEE_ATSPI_VARIANT("(so)", cee_atspi_ref(&v, NULL, -2));
        }
        if (strcmp(name, "version") == 0) {
            CEE_ATSPI_VARIANT("u", cee_atspi_uint(&v, 1));
        }
    } else if (strcmp(interface, "org.a11y.atspi.TableCell") == 0 && n != NULL && cee_atspi_celled(n)) {
        if (strcmp(name, "RowSpan") == 0 || strcmp(name, "ColumnSpan") == 0) {
            CEE_ATSPI_VARIANT("i", cee_atspi_int(&v, 1));
        }
        if (strcmp(name, "Position") == 0) {
            cee_dbus_iter r;
            cee_dbus.open_container(out, 'v', "(ii)", &v);
            cee_dbus.open_container(&v, 'r', NULL, &r);
            cee_atspi_int(&r, n->row > 0 ? (int32_t)n->row - 1 : 0);
            cee_atspi_int(&r, n->column > 0 ? (int32_t)n->column - 1 : 0);
            cee_dbus.close_container(&v, &r);
            cee_dbus.close_container(out, &v);
            return 1;
        }
        if (strcmp(name, "Table") == 0) {
            CEE_ATSPI_VARIANT("(so)", cee_atspi_ref(&v, w, cee_atspi_table_of(w, (uint32_t)index)));
        }
        if (strcmp(name, "version") == 0) {
            CEE_ATSPI_VARIANT("u", cee_atspi_uint(&v, 1));
        }
    } else if (strcmp(interface, "org.a11y.atspi.Component") == 0 && w != NULL && strcmp(name, "version") == 0) {
        CEE_ATSPI_VARIANT("u", cee_atspi_uint(&v, 1));
    }
#undef CEE_ATSPI_VARIANT
    return 0;
}

static const char *const cee_atspi_accessible_properties[] = {"Name", "Description", "Parent", "ChildCount", "Locale",
                                                              "AccessibleId", "HelpText", "version"};

/* The request `m` to an object of ours: answered, and 1; 0 when it is not one we know (the caller answers
   UnknownMethod). */
static int cee_atspi_answer(cee_dbus_message *m, cee_dbus_message *r, struct cee_window *w, int64_t index,
                            const char *interface, const char *member) {
    cee_dbus_iter it, s, e;
    cee_dbus.iter_init_append(r, &it);
    const struct cee_access_node *n = w != NULL && index >= 0 ? &w->access[index] : NULL;
    if (strcmp(interface, "org.freedesktop.DBus.Properties") == 0) {
        if (strcmp(member, "Get") == 0) {
            const char *iface = NULL, *name = NULL;
            void *args[] = {&iface, &name};
            return cee_atspi_args(m, "ss", args) && cee_atspi_property(&it, w, index, iface, name);
        }
        if (strcmp(member, "GetAll") == 0) {
            const char *iface = NULL;
            void *args[] = {&iface};
            if (!cee_atspi_args(m, "s", args)) {
                return 0;
            }
            cee_dbus.open_container(&it, 'a', "{sv}", &s);
            if (strcmp(iface, "org.a11y.atspi.Accessible") == 0) {
                for (size_t k = 0; k < sizeof cee_atspi_accessible_properties / sizeof *cee_atspi_accessible_properties;
                     k++) {
                    cee_dbus.open_container(&s, 'e', NULL, &e);
                    cee_atspi_string(&e, cee_atspi_accessible_properties[k]);
                    cee_atspi_property(&e, w, index, iface, cee_atspi_accessible_properties[k]);
                    cee_dbus.close_container(&s, &e);
                }
            }
            cee_dbus.close_container(&it, &s);
            return 1;
        }
        if (strcmp(member, "Set") == 0) {
            return 1; /* the registry sets Application.Id; nothing else is writable here */
        }
        return 0;
    }
    if (strcmp(interface, "org.a11y.atspi.Accessible") == 0) {
        if (strcmp(member, "GetRole") == 0) {
            cee_atspi_uint(&it, w == NULL ? 75u : n == NULL ? 23u : cee_atspi_role(n));
            return 1;
        }
        if (strcmp(member, "GetRoleName") == 0 || strcmp(member, "GetLocalizedRoleName") == 0) {
            cee_atspi_string(&it, cee_atspi_role_name(w == NULL ? 75u : n == NULL ? 23u : cee_atspi_role(n)));
            return 1;
        }
        if (strcmp(member, "GetState") == 0) {
            uint32_t words[2];
            cee_atspi_states(w, index, words);
            cee_dbus.open_container(&it, 'a', "u", &s);
            cee_atspi_uint(&s, words[0]);
            cee_atspi_uint(&s, words[1]);
            cee_dbus.close_container(&it, &s);
            return 1;
        }
        if (strcmp(member, "GetChildren") == 0) {
            cee_dbus.open_container(&it, 'a', "(so)", &s);
            if (w == NULL) {
                for (struct cee_window *f = cee_wake_windows; f != NULL; f = f->wake_link) {
                    if (!f->scripted && f->native != NULL) {
                        cee_atspi_ref(&s, f, -1);
                    }
                }
            } else {
                uint32_t up = n == NULL ? CEE_ACCESS_TOP : (uint32_t)index; /* ADR 0207: a container's too */
                for (uint32_t k = 0; k < w->access_count; k++) {
                    if (w->access[k].up == up) {
                        cee_atspi_ref(&s, w, (int64_t)k);
                    }
                }
            }
            cee_dbus.close_container(&it, &s);
            return 1;
        }
        if (strcmp(member, "GetChildAtIndex") == 0) {
            int32_t at = 0;
            void *args[] = {&at};
            if (!cee_atspi_args(m, "i", args)) {
                return 0;
            }
            if (w == NULL) {
                int32_t k = 0;
                for (struct cee_window *f = cee_wake_windows; f != NULL; f = f->wake_link) {
                    if (!f->scripted && f->native != NULL && k++ == at) {
                        cee_atspi_ref(&it, f, -1);
                        return 1;
                    }
                }
            } else {
                int64_t k = cee_atspi_child_at(w, n == NULL ? CEE_ACCESS_TOP : (uint32_t)index, at);
                cee_atspi_ref(&it, k >= 0 ? w : NULL, k);
                return 1;
            }
            cee_atspi_ref(&it, NULL, -2);
            return 1;
        }
        if (strcmp(member, "GetIndexInParent") == 0) {
            int32_t at = -1;
            if (n != NULL) {
                at = cee_atspi_place(w->access, (uint32_t)index); /* among its container's children */
            } else if (w != NULL) {
                int32_t k = 0;
                for (struct cee_window *f = cee_wake_windows; f != NULL; f = f->wake_link) {
                    if (!f->scripted && f->native != NULL) {
                        if (f == w) {
                            at = k;
                        }
                        k++;
                    }
                }
            }
            cee_atspi_int(&it, at);
            return 1;
        }
        if (strcmp(member, "GetRelationSet") == 0) {
            cee_dbus.open_container(&it, 'a', "(ua(so))", &s);
            cee_dbus.close_container(&it, &s);
            return 1;
        }
        if (strcmp(member, "GetAttributes") == 0) {
            cee_dbus.open_container(&it, 'a', "{ss}", &s);
            cee_dbus.open_container(&s, 'e', NULL, &e);
            cee_atspi_string(&e, "toolkit");
            cee_atspi_string(&e, "CeeSafe");
            cee_dbus.close_container(&s, &e);
            if (n != NULL && n->up != CEE_ACCESS_TOP && n->row > 0 && n->column == 0) {
                /* An item's place in its container, "3 of 40" (ADR 0207): ARIA's attributes, which Orca reads. */
                char number[16];
                snprintf(number, sizeof number, "%u", n->row);
                cee_dbus.open_container(&s, 'e', NULL, &e);
                cee_atspi_string(&e, "posinset");
                cee_atspi_string(&e, number);
                cee_dbus.close_container(&s, &e);
                snprintf(number, sizeof number, "%u", w->access[n->up].rows);
                cee_dbus.open_container(&s, 'e', NULL, &e);
                cee_atspi_string(&e, "setsize");
                cee_atspi_string(&e, number);
                cee_dbus.close_container(&s, &e);
            }
            cee_dbus.close_container(&it, &s);
            return 1;
        }
        if (strcmp(member, "GetApplication") == 0) {
            cee_atspi_ref(&it, NULL, -1);
            return 1;
        }
        if (strcmp(member, "GetInterfaces") == 0) {
            cee_dbus.open_container(&it, 'a', "s", &s);
            cee_atspi_string(&s, "org.a11y.atspi.Accessible");
            if (w == NULL) {
                cee_atspi_string(&s, "org.a11y.atspi.Application");
            } else {
                cee_atspi_string(&s, "org.a11y.atspi.Component");
                if (n != NULL && cee_atspi_acts(n)) {
                    cee_atspi_string(&s, "org.a11y.atspi.Action");
                }
                if (n != NULL && cee_atspi_valued(n)) {
                    cee_atspi_string(&s, "org.a11y.atspi.Value");
                }
                if (n != NULL && cee_atspi_texted(n)) {
                    cee_atspi_string(&s, "org.a11y.atspi.Text");
                }
                if (n != NULL && cee_atspi_tabled(n)) {
                    cee_atspi_string(&s, "org.a11y.atspi.Table");
                }
                if (n != NULL && cee_atspi_celled(n)) {
                    cee_atspi_string(&s, "org.a11y.atspi.TableCell");
                }
            }
            cee_dbus.close_container(&it, &s);
            return 1;
        }
        return 0;
    }
    if (strcmp(interface, "org.a11y.atspi.Table") == 0 && n != NULL && cee_atspi_tabled(n)) {
        int32_t row = 0, column = 0;
        int32_t columns = n->columns > 0 ? (int32_t)n->columns : 1;
        if (strcmp(member, "GetAccessibleAt") == 0 || strcmp(member, "GetIndexAt") == 0 ||
            strcmp(member, "GetRowExtentAt") == 0 || strcmp(member, "GetColumnExtentAt") == 0 ||
            strcmp(member, "IsSelected") == 0) {
            void *args[] = {&row, &column};
            if (!cee_atspi_args(m, "ii", args)) {
                return 0;
            }
            if (strcmp(member, "GetAccessibleAt") == 0) {
                int64_t k = cee_atspi_cell_at(w, (uint32_t)index, row, column);
                cee_atspi_ref(&it, k >= 0 ? w : NULL, k);
            } else if (strcmp(member, "GetIndexAt") == 0) {
                cee_atspi_int(&it, row * columns + column);
            } else if (strcmp(member, "IsSelected") == 0) {
                cee_atspi_bool(&it, 0);
            } else {
                cee_atspi_int(&it, 1);
            }
            return 1;
        }
        if (strcmp(member, "GetRowAtIndex") == 0 || strcmp(member, "GetColumnAtIndex") == 0 ||
            strcmp(member, "GetColumnHeader") == 0 || strcmp(member, "GetRowHeader") == 0 ||
            strcmp(member, "GetRowDescription") == 0 || strcmp(member, "GetColumnDescription") == 0 ||
            strcmp(member, "IsRowSelected") == 0 || strcmp(member, "IsColumnSelected") == 0) {
            int32_t at = 0;
            void *args[] = {&at};
            if (!cee_atspi_args(m, "i", args)) {
                return 0;
            }
            if (strcmp(member, "GetRowAtIndex") == 0) {
                cee_atspi_int(&it, at / columns);
            } else if (strcmp(member, "GetColumnAtIndex") == 0) {
                cee_atspi_int(&it, at % columns);
            } else if (strcmp(member, "GetColumnHeader") == 0) {
                /* the column's header: the column header of that column, in whatever row has them */
                int64_t found = -2;
                for (uint32_t k = (uint32_t)index + 1u; k < w->access_count && found < 0; k++) {
                    if (w->access[k].role == CEE_ACCESS_COLUMN_HEADER && w->access[k].column == (uint32_t)at + 1u &&
                        cee_atspi_table_of(w, k) == index) {
                        found = (int64_t)k;
                    }
                }
                cee_atspi_ref(&it, found >= 0 ? w : NULL, found);
            } else if (strcmp(member, "GetRowHeader") == 0) {
                cee_atspi_ref(&it, NULL, -2);
            } else if (strcmp(member, "GetRowDescription") == 0 || strcmp(member, "GetColumnDescription") == 0) {
                cee_atspi_string(&it, "");
            } else {
                cee_atspi_bool(&it, 0);
            }
            return 1;
        }
        if (strcmp(member, "GetSelectedRows") == 0 || strcmp(member, "GetSelectedColumns") == 0) {
            cee_dbus.open_container(&it, 'a', "i", &s);
            cee_dbus.close_container(&it, &s);
            return 1;
        }
        return 0;
    }
    if (strcmp(interface, "org.a11y.atspi.TableCell") == 0 && n != NULL && cee_atspi_celled(n)) {
        if (strcmp(member, "GetRowColumnSpan") == 0) {
            cee_atspi_int(&it, n->row > 0 ? (int32_t)n->row - 1 : 0);
            cee_atspi_int(&it, n->column > 0 ? (int32_t)n->column - 1 : 0);
            cee_atspi_int(&it, 1);
            cee_atspi_int(&it, 1);
            return 1;
        }
        if (strcmp(member, "GetColumnHeaderCells") == 0 || strcmp(member, "GetRowHeaderCells") == 0) {
            cee_dbus.open_container(&it, 'a', "(so)", &s);
            int64_t t = cee_atspi_table_of(w, (uint32_t)index);
            for (uint32_t k = t >= 0 ? (uint32_t)t + 1u : w->access_count;
                 strcmp(member, "GetColumnHeaderCells") == 0 && n->role == CEE_ACCESS_CELL && k < w->access_count; k++) {
                if (w->access[k].role == CEE_ACCESS_COLUMN_HEADER && w->access[k].column == n->column &&
                    cee_atspi_table_of(w, k) == t) {
                    cee_atspi_ref(&s, w, (int64_t)k);
                }
            }
            cee_dbus.close_container(&it, &s);
            return 1;
        }
        return 0;
    }
    if (strcmp(interface, "org.a11y.atspi.Application") == 0 && w == NULL) {
        if (strcmp(member, "GetLocale") == 0) {
            const char *l = getenv("LANG");
            cee_atspi_string(&it, l != NULL ? l : "C");
            return 1;
        }
        return 0;
    }
    if (strcmp(interface, "org.a11y.atspi.Component") == 0 && w != NULL) {
        int32_t box[4];
        if (strcmp(member, "GetExtents") == 0 || strcmp(member, "GetPosition") == 0) {
            uint32_t coord = 0;
            void *args[] = {&coord};
            if (!cee_atspi_args(m, "u", args)) {
                return 0;
            }
            cee_atspi_extents(w, index, coord, box);
            if (strcmp(member, "GetPosition") == 0) {
                cee_atspi_int(&it, box[0]);
                cee_atspi_int(&it, box[1]);
                return 1;
            }
            cee_dbus.open_container(&it, 'r', NULL, &s);
            for (int k = 0; k < 4; k++) {
                cee_atspi_int(&s, box[k]);
            }
            cee_dbus.close_container(&it, &s);
            return 1;
        }
        if (strcmp(member, "GetSize") == 0) {
            cee_atspi_extents(w, index, 1, box);
            cee_atspi_int(&it, box[2]);
            cee_atspi_int(&it, box[3]);
            return 1;
        }
        if (strcmp(member, "Contains") == 0 || strcmp(member, "GetAccessibleAtPoint") == 0) {
            int32_t x = 0, y = 0;
            uint32_t coord = 0;
            void *args[] = {&x, &y, &coord};
            if (!cee_atspi_args(m, "iiu", args)) {
                return 0;
            }
            cee_atspi_extents(w, index, coord, box);
            int inside = x >= box[0] && y >= box[1] && x < box[0] + box[2] && y < box[1] + box[3];
            if (strcmp(member, "Contains") == 0) {
                cee_atspi_bool(&it, inside);
                return 1;
            }
            /* The frame's child at the point: of those that hold it, the highest layer's, then the latest. */
            int64_t found = -1;
            if (n == NULL && inside) {
                float scale = w->scale > 0.0f ? w->scale : 1.0f;
                found = cee_access_at(w->access, w->access_count, (double)(x - box[0]) / scale,
                                      (double)(y - box[1]) / scale);
            }
            cee_atspi_ref(&it, w, found >= 0 ? found : -2);
            return 1;
        }
        if (strcmp(member, "GetLayer") == 0) {
            cee_atspi_uint(&it, n == NULL ? 7u : 3u); /* WINDOW, WIDGET */
            return 1;
        }
        if (strcmp(member, "GetMDIZOrder") == 0) {
            int16_t z = 0;
            cee_dbus.append_basic(&it, 'n', &z);
            return 1;
        }
        if (strcmp(member, "GetAlpha") == 0) {
            cee_atspi_double(&it, 1.0);
            return 1;
        }
        if (strcmp(member, "GrabFocus") == 0) {
            cee_atspi_bool(&it, n != NULL && cee_access_ask(w, n->id, CEE_ACCESS_FOCUS));
            return 1;
        }
        return 0;
    }
    if (strcmp(interface, "org.a11y.atspi.Action") == 0 && n != NULL && cee_atspi_acts(n)) {
        const char *name = n->role == 2 || n->role == 3 ? "toggle" : "press";
        if (strcmp(member, "GetName") == 0 || strcmp(member, "GetLocalizedName") == 0) {
            cee_atspi_string(&it, name);
            return 1;
        }
        if (strcmp(member, "GetDescription") == 0 || strcmp(member, "GetKeyBinding") == 0) {
            cee_atspi_string(&it, "");
            return 1;
        }
        if (strcmp(member, "GetActions") == 0) {
            cee_dbus.open_container(&it, 'a', "(sss)", &s);
            cee_dbus.open_container(&s, 'r', NULL, &e);
            cee_atspi_string(&e, name);
            cee_atspi_string(&e, "");
            cee_atspi_string(&e, "");
            cee_dbus.close_container(&s, &e);
            cee_dbus.close_container(&it, &s);
            return 1;
        }
        if (strcmp(member, "DoAction") == 0) {
            int32_t which = 0;
            void *args[] = {&which};
            cee_atspi_bool(&it, cee_atspi_args(m, "i", args) && which == 0 &&
                                    cee_access_ask(w, n->id, CEE_ACCESS_PRESS));
            return 1;
        }
        return 0;
    }
    if (strcmp(interface, "org.a11y.atspi.Text") == 0 && n != NULL && cee_atspi_texted(n)) {
        const char *text = w->access_text + n->value_start;
        uint32_t length = n->value_length;
        int32_t count = (int32_t)cee_atspi_chars(text, length);
        /* A run of the text: answered by cee_atspi_text_reply, which reads its own arguments. */
        if (strcmp(member, "GetText") == 0 || strcmp(member, "GetStringAtOffset") == 0 ||
            strcmp(member, "GetTextAtOffset") == 0 ||
            strcmp(member, "GetTextBeforeOffset") == 0 || strcmp(member, "GetTextAfterOffset") == 0) {
            return 2;
        }
        if (strcmp(member, "GetCharacterAtOffset") == 0) {
            int32_t at = 0;
            void *args[] = {&at};
            if (!cee_atspi_args(m, "i", args)) {
                return 0;
            }
            uint32_t b = cee_atspi_byte(text, length, at);
            int32_t cp = 0;
            if (at >= 0 && at < count) {
                unsigned char c0 = (unsigned char)text[b];
                int extra = c0 >= 0xF0 ? 3 : c0 >= 0xE0 ? 2 : c0 >= 0xC0 ? 1 : 0;
                cp = extra == 0 ? c0 : (int32_t)(c0 & (0x3F >> extra));
                for (int k = 1; k <= extra && b + (uint32_t)k < length; k++) {
                    cp = (cp << 6) | ((unsigned char)text[b + (uint32_t)k] & 0x3F);
                }
            }
            cee_atspi_int(&it, cp);
            return 1;
        }
        if (strcmp(member, "GetNSelections") == 0) {
            cee_atspi_int(&it, n->caret != n->anchor ? 1 : 0);
            return 1;
        }
        if (strcmp(member, "GetSelection") == 0) {
            uint32_t lo = n->caret < n->anchor ? n->caret : n->anchor;
            uint32_t hi = n->caret < n->anchor ? n->anchor : n->caret;
            cee_atspi_int(&it, (int32_t)cee_atspi_chars(text, lo));
            cee_atspi_int(&it, (int32_t)cee_atspi_chars(text, hi));
            return 1;
        }
        if (strcmp(member, "SetCaretOffset") == 0) {
            cee_atspi_bool(&it, 0); /* the program moves its caret; a reader reads where it is */
            return 1;
        }
        if (strcmp(member, "GetCharacterExtents") == 0 || strcmp(member, "GetRangeExtents") == 0) {
            int32_t box[4];
            cee_atspi_extents(w, index, 0, box);
            for (int k = 0; k < 4; k++) {
                cee_atspi_int(&it, box[k]);
            }
            return 1;
        }
        if (strcmp(member, "GetOffsetAtPoint") == 0) {
            cee_atspi_int(&it, -1);
            return 1;
        }
        if (strcmp(member, "GetAttributes") == 0 || strcmp(member, "GetAttributeRun") == 0) {
            cee_dbus.open_container(&it, 'a', "{ss}", &s);
            cee_dbus.close_container(&it, &s);
            cee_atspi_int(&it, 0);
            cee_atspi_int(&it, count);
            return 1;
        }
        if (strcmp(member, "GetDefaultAttributes") == 0 || strcmp(member, "GetDefaultAttributeSet") == 0) {
            cee_dbus.open_container(&it, 'a', "{ss}", &s);
            cee_dbus.close_container(&it, &s);
            return 1;
        }
        return 0;
    }
    return 0;
}

/* Text requests that answer a run of the field's text: GetText (s), and the boundary ones (s, start, end). */
static cee_dbus_message *cee_atspi_text_reply(cee_dbus_message *m, struct cee_window *w, int64_t index,
                                              const char *member) {
    const struct cee_access_node *n = &w->access[index];
    const char *text = w->access_text + n->value_start;
    uint32_t length = n->value_length;
    int32_t count = (int32_t)cee_atspi_chars(text, length);
    int32_t a = 0, b = 0;
    int boundary = strcmp(member, "GetText") != 0;
    if (!boundary) {
        void *args[] = {&a, &b};
        if (!cee_atspi_args(m, "ii", args)) {
            return NULL;
        }
        if (b < 0 || b > count) {
            b = count;
        }
        if (a < 0) {
            a = 0;
        }
        if (a > b) {
            a = b;
        }
    } else {
        int32_t offset = 0;
        uint32_t kind = 0;
        void *args[] = {&offset, &kind};
        if (!cee_atspi_args(m, "iu", args)) {
            return NULL;
        }
        if (strcmp(member, "GetTextBeforeOffset") == 0) {
            int32_t s0 = 0, e0 = 0;
            cee_atspi_around(text, length, offset, kind, &s0, &e0);
            offset = s0 - 1;
            if (offset < 0) {
                a = b = 0;
                goto reply;
            }
        } else if (strcmp(member, "GetTextAfterOffset") == 0) {
            int32_t s0 = 0, e0 = 0;
            cee_atspi_around(text, length, offset, kind, &s0, &e0);
            offset = e0;
            if (offset >= count) {
                a = b = count;
                goto reply;
            }
        }
        cee_atspi_around(text, length, offset, kind == 0 ? 0u : kind <= 2 ? 1u : 5u, &a, &b);
    }
reply:;
    cee_dbus_message *r = cee_dbus.new_method_return(m);
    if (r == NULL) {
        return NULL;
    }
    cee_dbus_iter it;
    cee_dbus.iter_init_append(r, &it);
    uint32_t from = cee_atspi_byte(text, length, a), to = cee_atspi_byte(text, length, b);
    cee_atspi_text(&it, text, from, to - from);
    if (boundary) {
        cee_atspi_int(&it, a);
        cee_atspi_int(&it, b);
    }
    return r;
}

/* Every request the bus has for us, answered; called from the pump and after the waits. */
static void cee_atspi_dispatch(void) {
    if (cee_atspi.state != 1) {
        return;
    }
    cee_dbus.read_write(cee_atspi.bus, 0);
    cee_dbus_message *m;
    while ((m = cee_dbus.pop_message(cee_atspi.bus)) != NULL) {
        if (cee_dbus.message_type(m) == 1 /* METHOD_CALL */) {
            const char *path = cee_dbus.message_path(m);
            const char *interface = cee_dbus.message_interface(m);
            const char *member = cee_dbus.message_member(m);
            struct cee_window *w = NULL;
            int64_t index = -1;
            if (interface != NULL && strcmp(interface, "org.freedesktop.DBus.Peer") == 0) {
                /* On any path, as libdbus's own dispatch answers it: the registry pings each application every 30
                   s and hides one that does not answer ("Disabling unresponsive app"). */
                cee_dbus_message *r = cee_dbus.new_method_return(m);
                if (r != NULL && member != NULL && strcmp(member, "GetMachineId") == 0) {
                    cee_dbus_iter it;
                    cee_dbus.iter_init_append(r, &it);
                    cee_atspi_string(&it, "");
                }
                if (r != NULL) {
                    cee_dbus.send(cee_atspi.bus, r, NULL);
                    cee_dbus.message_unref(r);
                }
            } else if (path != NULL && strcmp(path, "/org/a11y/atspi/cache") == 0 && interface != NULL &&
                       member != NULL && strcmp(interface, "org.a11y.atspi.Cache") == 0 &&
                       strcmp(member, "GetItems") == 0) {
                /* Every client asks for the cache first; an empty one has it ask each object, which is what is here
                   (the list changes each frame, so a cache's updates would be most of the traffic). */
                cee_dbus_message *r = cee_dbus.new_method_return(m);
                if (r != NULL) {
                    cee_dbus_iter it, a;
                    cee_dbus.iter_init_append(r, &it);
                    cee_dbus.open_container(&it, 'a', "((so)(so)(so)iiassusau)", &a);
                    cee_dbus.close_container(&it, &a);
                    cee_dbus.send(cee_atspi.bus, r, NULL);
                    cee_dbus.message_unref(r);
                }
            } else if (interface == NULL || member == NULL || !cee_atspi_find(path, &w, &index)) {
                cee_atspi_reply_error(m, "org.freedesktop.DBus.Error.UnknownObject", "no such object");
            } else {
                if (w != NULL) {
                    w->access_active = 1; /* something reads the window: the program sends its list (ADR 0164) */
                } else {
                    for (struct cee_window *f = cee_wake_windows; f != NULL; f = f->wake_link) {
                        if (!f->scripted) {
                            f->access_active = 1;
                        }
                    }
                }
                cee_dbus_message *r = cee_dbus.new_method_return(m);
                int answered = r != NULL ? cee_atspi_answer(m, r, w, index, interface, member) : 0;
                if (answered == 2) {
                    cee_dbus.message_unref(r);
                    r = cee_atspi_text_reply(m, w, index, member);
                    answered = r != NULL;
                }
                if (answered) {
                    cee_dbus.send(cee_atspi.bus, r, NULL);
                } else {
                    cee_atspi_reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", "not answered");
                }
                if (r != NULL) {
                    cee_dbus.message_unref(r);
                }
            }
        }
        cee_dbus.message_unref(m);
    }
    cee_dbus.flush(cee_atspi.bus);
}

/* The connection's descriptor, for the waits; -1 without one. */
static int cee_atspi_fd(void) {
    int fd = -1;
    if (cee_atspi.state == 1 && !cee_dbus.get_unix_fd(cee_atspi.bus, &fd)) {
        fd = -1;
    }
    return fd;
}

/* The first open's: the accessibility bus's address from the session bus (org.a11y.Bus), a connection to it, and the
   root embedded in the registry's desktop (Socket.Embed). Any failure leaves the program without a reader. */
static void cee_atspi_connect(void) {
    if (cee_atspi.state != 0) {
        return;
    }
    cee_atspi.state = -1;
    if (getenv("CEESAFE_NO_ATSPI") != NULL || !cee_dbus_load()) {
        return;
    }
#if defined(__FreeBSD__)
    snprintf(cee_atspi.app_name, sizeof cee_atspi.app_name, "%s", getprogname());
#else
    FILE *comm = fopen("/proc/self/comm", "r");
    if (comm != NULL) {
        if (fgets(cee_atspi.app_name, sizeof cee_atspi.app_name, comm) != NULL) {
            cee_atspi.app_name[strcspn(cee_atspi.app_name, "\n")] = '\0';
        }
        fclose(comm);
    }
#endif
    cee_dbus_error err;
    cee_dbus.error_init(&err);
    cee_dbus_connection *session = cee_dbus.bus_get_private(0 /* DBUS_BUS_SESSION */, &err);
    if (session == NULL) {
        cee_dbus.error_free(&err);
        return;
    }
    cee_dbus.set_exit_on_disconnect(session, 0);
    char address[512];
    address[0] = '\0';
    cee_dbus_message *call =
        cee_dbus.new_method_call("org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress");
    cee_dbus_message *reply = call != NULL ? cee_dbus.send_with_reply_and_block(session, call, 2000, &err) : NULL;
    if (reply != NULL) {
        cee_dbus_iter it;
        const char *a = NULL;
        if (cee_dbus.iter_init(reply, &it) && cee_dbus.arg_type(&it) == 's') {
            cee_dbus.get_basic(&it, &a);
            snprintf(address, sizeof address, "%s", a != NULL ? a : "");
        }
        cee_dbus.message_unref(reply);
    }
    if (call != NULL) {
        cee_dbus.message_unref(call);
    }
    cee_dbus.error_free(&err);
    cee_dbus.connection_close(session);
    cee_dbus.connection_unref(session);
    if (address[0] == '\0') {
        return;
    }
    cee_dbus.error_init(&err);
    cee_dbus_connection *bus = cee_dbus.open_private(address, &err);
    if (bus == NULL || !cee_dbus.bus_register(bus, &err)) {
        cee_dbus.error_free(&err);
        if (bus != NULL) {
            cee_dbus.connection_close(bus);
            cee_dbus.connection_unref(bus);
        }
        return;
    }
    cee_dbus.set_exit_on_disconnect(bus, 0);
    const char *unique = cee_dbus.unique_name(bus);
    snprintf(cee_atspi.name, sizeof cee_atspi.name, "%s", unique != NULL ? unique : "");
    cee_atspi.bus = bus;
    cee_atspi.state = 1;
    snprintf(cee_atspi.desktop_name, sizeof cee_atspi.desktop_name, "%s", "org.a11y.atspi.Registry");
    snprintf(cee_atspi.desktop_path, sizeof cee_atspi.desktop_path, "%s", "/org/a11y/atspi/accessible/root");
    call = cee_dbus.new_method_call("org.a11y.atspi.Registry", CEE_ATSPI_ROOT, "org.a11y.atspi.Socket", "Embed");
    if (call == NULL) {
        return;
    }
    cee_dbus_iter it, s;
    cee_dbus.iter_init_append(call, &it);
    cee_dbus.open_container(&it, 'r', NULL, &s);
    cee_atspi_string(&s, cee_atspi.name);
    const char *root = CEE_ATSPI_ROOT;
    cee_dbus.append_basic(&s, 'o', &root);
    cee_dbus.close_container(&it, &s);
    reply = cee_dbus.send_with_reply_and_block(bus, call, 2000, &err);
    cee_dbus.message_unref(call);
    if (reply != NULL) {
        cee_dbus_iter r, ref;
        if (cee_dbus.iter_init(reply, &r) && cee_dbus.arg_type(&r) == 'r') {
            const char *name = NULL, *path = NULL;
            cee_dbus.recurse(&r, &ref);
            if (cee_dbus.arg_type(&ref) == 's') {
                cee_dbus.get_basic(&ref, &name);
                cee_dbus.iter_next(&ref);
                if (cee_dbus.arg_type(&ref) == 'o') {
                    cee_dbus.get_basic(&ref, &path);
                    snprintf(cee_atspi.desktop_name, sizeof cee_atspi.desktop_name, "%s", name);
                    snprintf(cee_atspi.desktop_path, sizeof cee_atspi.desktop_path, "%s", path);
                }
            }
        }
        cee_dbus.message_unref(reply);
    }
    cee_dbus.error_free(&err);
}

/* A window opened (its frame is the root's newest child) or closed. */
static void cee_atspi_frame_added(struct cee_window *w) {
    if (cee_atspi.state != 1) {
        return;
    }
    struct cee_atspi_child c = {w, -1};
    int32_t at = 0;
    for (struct cee_window *f = cee_wake_windows; f != NULL; f = f->wake_link) {
        at += !f->scripted && f->native != NULL && f != w;
    }
    cee_atspi_signal(NULL, -1, "org.a11y.atspi.Event.Object", "ChildrenChanged", "add", at, 0, "(so)",
                     cee_atspi_any_child, &c);
    cee_dbus.flush(cee_atspi.bus);
}

static void cee_atspi_frame_removed(struct cee_window *w) {
    if (cee_atspi.state != 1) {
        return;
    }
    struct cee_atspi_child c = {w, -1};
    cee_atspi_state_changed(w, -1, "defunct", 1);
    cee_atspi_signal(NULL, -1, "org.a11y.atspi.Event.Object", "ChildrenChanged", "remove", 0, 0, "(so)",
                     cee_atspi_any_child, &c);
    cee_dbus.flush(cee_atspi.bus);
}

/* The window took or lost the keyboard: the frame is activated or deactivated. */
static void cee_atspi_frame_focus(struct cee_window *w, int on) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    if (cee_atspi.state != 1 || n == NULL || n->atspi_active == on) {
        return;
    }
    n->atspi_active = on;
    const char *title = n->title != NULL ? n->title : "";
    cee_atspi_signal(w, -1, "org.a11y.atspi.Event.Window", on ? "Activate" : "Deactivate", "", 0, 0, "s",
                     cee_atspi_any_string, title);
    cee_atspi_state_changed(w, -1, "active", on);
    if (on) {
        for (uint32_t k = 0; k < w->access_count; k++) {
            if ((w->access[k].flags & CEE_ACCESS_FOCUSED) != 0) {
                cee_atspi_state_changed(w, (int64_t)k, "focused", 1);
            }
        }
    }
    cee_dbus.flush(cee_atspi.bus);
}

/* Equal runs of two lists' texts. */
static int cee_atspi_same(const char *a, uint32_t a_start, uint32_t a_length, const char *b, uint32_t b_start,
                          uint32_t b_length) {
    return a_length == b_length && (a_length == 0 || memcmp(a + a_start, b + b_start, a_length) == 0);
}

/* The window's list changed (w->access is the new one; `old` the last, with its text): readers are told what was added
   or removed, renamed, changed value, checked, selected, expanded or focused, and where a field's caret went. */
static void cee_native_access(struct cee_window *w, const struct cee_access_node *old, uint32_t old_count,
                              const char *old_text) {
    if (cee_atspi.state != 1 || w->native == NULL) {
        return;
    }
    /* Elements gone: told before those that came, with their old place (their path is their id, the same). */
    for (uint32_t j = 0; j < old_count; j++) {
        int kept = 0;
        for (uint32_t k = 0; k < w->access_count && !kept; k++) {
            kept = w->access[k].id == old[j].id;
        }
        if (!kept) {
            char path[160];
            snprintf(path, sizeof path, "%s%u_%llu", CEE_ATSPI_PREFIX, ((struct cee_x11 *)w->native)->atspi_serial,
                     (unsigned long long)old[j].id);
            char frame[160]; /* its container's object (ADR 0207), or the frame's */
            if (old[j].parent != 0) {
                snprintf(frame, sizeof frame, "%s%u_%llu", CEE_ATSPI_PREFIX, ((struct cee_x11 *)w->native)->atspi_serial,
                         (unsigned long long)old[j].parent);
            } else {
                cee_atspi_path(frame, sizeof frame, w, -1);
            }
            cee_dbus_message *s = cee_dbus.new_signal(frame, "org.a11y.atspi.Event.Object", "ChildrenChanged");
            if (s != NULL) {
                cee_dbus_iter it, v, r, d;
                cee_dbus.iter_init_append(s, &it);
                cee_atspi_string(&it, "remove");
                cee_atspi_int(&it, cee_atspi_place(old, j));
                cee_atspi_int(&it, 0);
                cee_dbus.open_container(&it, 'v', "(so)", &v);
                cee_dbus.open_container(&v, 'r', NULL, &r);
                cee_atspi_string(&r, cee_atspi.name);
                const char *p = path;
                cee_dbus.append_basic(&r, 'o', &p);
                cee_dbus.close_container(&v, &r);
                cee_dbus.close_container(&it, &v);
                cee_dbus.open_container(&it, 'a', "{sv}", &d);
                cee_dbus.close_container(&it, &d);
                cee_dbus.send(cee_atspi.bus, s, NULL);
                cee_dbus.message_unref(s);
            }
        }
    }
    for (uint32_t k = 0; k < w->access_count; k++) {
        const struct cee_access_node *n = &w->access[k];
        uint32_t j = old_count;
        if (k < old_count && old[k].id == n->id) {
            j = k;
        } else {
            for (uint32_t i = 0; i < old_count; i++) {
                if (old[i].id == n->id) {
                    j = i;
                    break;
                }
            }
        }
        if (j == old_count) {
            struct cee_atspi_child c = {w, (int64_t)k};
            /* told to its container (ADR 0207), with its index among the container's children */
            cee_atspi_signal(w, n->up == CEE_ACCESS_TOP ? -1 : (int64_t)n->up, "org.a11y.atspi.Event.Object",
                             "ChildrenChanged", "add", cee_atspi_place(w->access, k), 0, "(so)", cee_atspi_any_child,
                             &c);
            if ((n->flags & CEE_ACCESS_FOCUSED) != 0) {
                cee_atspi_state_changed(w, (int64_t)k, "focused", 1);
            }
            continue;
        }
        const struct cee_access_node *o = &old[j];
        if (n->role != 0 && !cee_atspi_same(w->access_text, n->label_start, n->label_length, old_text, o->label_start,
                                            o->label_length)) {
            char few[256];
            uint32_t len = n->label_length < sizeof few - 1u ? n->label_length : (uint32_t)sizeof few - 1u;
            memcpy(few, w->access_text + n->label_start, len);
            few[len] = '\0';
            cee_atspi_signal(w, (int64_t)k, "org.a11y.atspi.Event.Object", "PropertyChange", "accessible-name", 0, 0,
                             "s", cee_atspi_any_string, few);
        }
        if (n->role == 5 && n->now != o->now) {
            double now = n->now;
            cee_atspi_signal(w, (int64_t)k, "org.a11y.atspi.Event.Object", "PropertyChange", "accessible-value", 0, 0,
                             "d", cee_atspi_any_double, &now);
        }
        if (n->role == 4 && !cee_atspi_same(w->access_text, n->value_start, n->value_length, old_text, o->value_start,
                                            o->value_length)) {
            /* The old text deleted and the new inserted, whole: a reader speaks what was typed from the difference. */
            const char *was = old_text + o->value_start;
            const char *now = w->access_text + n->value_start;
            uint32_t lead = 0;
            while (lead < o->value_length && lead < n->value_length && was[lead] == now[lead]) {
                lead++;
            }
            while (lead > 0 && ((unsigned char)now[lead] & 0xC0u) == 0x80u) {
                lead--;
            }
            int32_t at = (int32_t)cee_atspi_chars(now, lead);
            uint32_t gone = o->value_length - lead, came = n->value_length - lead;
            if (gone > 0) {
                char *s = (char *)ceesafe_platform_alloc((size_t)gone + 1u);
                if (s != NULL) {
                    memcpy(s, was + lead, gone);
                    s[gone] = '\0';
                    cee_atspi_signal(w, (int64_t)k, "org.a11y.atspi.Event.Object", "TextChanged", "delete", at,
                                     (int32_t)cee_atspi_chars(s, gone), "s", cee_atspi_any_string, s);
                    ceesafe_platform_free(s);
                }
            }
            if (came > 0) {
                char *s = (char *)ceesafe_platform_alloc((size_t)came + 1u);
                if (s != NULL) {
                    memcpy(s, now + lead, came);
                    s[came] = '\0';
                    cee_atspi_signal(w, (int64_t)k, "org.a11y.atspi.Event.Object", "TextChanged", "insert", at,
                                     (int32_t)cee_atspi_chars(s, came), "s", cee_atspi_any_string, s);
                    ceesafe_platform_free(s);
                }
            }
        }
        if (n->role == 4 && (n->caret != o->caret || n->value_length != o->value_length) &&
            (n->flags & CEE_ACCESS_FOCUSED) != 0) {
            cee_atspi_signal(w, (int64_t)k, "org.a11y.atspi.Event.Object", "TextCaretMoved", "",
                             (int32_t)cee_atspi_chars(w->access_text + n->value_start, n->caret), 0, NULL, NULL, NULL);
        }
        uint32_t changed = n->flags ^ o->flags;
        if ((n->role == 2 || n->role == 3) && (changed & CEE_ACCESS_CHECKED) != 0) {
            cee_atspi_state_changed(w, (int64_t)k, "checked", (n->flags & CEE_ACCESS_CHECKED) != 0);
        }
        if ((changed & CEE_ACCESS_SELECTED) != 0) {
            cee_atspi_state_changed(w, (int64_t)k, "selected", (n->flags & CEE_ACCESS_SELECTED) != 0);
        }
        if ((changed & CEE_ACCESS_EXPANDED) != 0) {
            cee_atspi_state_changed(w, (int64_t)k, "expanded", (n->flags & CEE_ACCESS_EXPANDED) != 0);
        }
        if ((changed & CEE_ACCESS_DISABLED) != 0) {
            cee_atspi_state_changed(w, (int64_t)k, "enabled", (n->flags & CEE_ACCESS_DISABLED) == 0);
        }
        if ((changed & CEE_ACCESS_FOCUSED) != 0) {
            cee_atspi_state_changed(w, (int64_t)k, "focused", (n->flags & CEE_ACCESS_FOCUSED) != 0);
        }
    }
    cee_dbus.flush(cee_atspi.bus);
}

/* A message for a reader to speak (ADR 0164's announce): the frame's Announcement. */
static void cee_native_announce(struct cee_window *w, const char *text, size_t length) {
    if (cee_atspi.state != 1 || w->native == NULL) {
        return;
    }
    char *s = (char *)ceesafe_platform_alloc(length + 1u);
    if (s == NULL) {
        return;
    }
    memcpy(s, text, length);
    s[length] = '\0';
    cee_atspi_signal(w, -1, "org.a11y.atspi.Event.Object", "Announcement", "", 1 /* polite */, 0, "s",
                     cee_atspi_any_string, s);
    ceesafe_platform_free(s);
    cee_dbus.flush(cee_atspi.bus);
}

/* The title, UTF-8 as the program gives it: _NET_WM_NAME (EWMH), which window managers show, and WM_NAME, which
   XStoreName fills and which ICCCM reads as Latin-1, so a "—" there is "â" and two more. */
static void cee_x_title(cee_x_display *display, cee_xid window, unsigned long utf8, const char *title) {
    cee_x.store_name(display, window, title);
    cee_x.change_property(display, window, cee_x.intern_atom(display, "_NET_WM_NAME", 0), utf8, 8,
                          0 /* PropModeReplace */, (const unsigned char *)title, (int)strlen(title));
}

/* ---- Wayland (ADR 0205) -------------------------------------------------------------------------------------------
   A window on a Wayland compositor, beside the X11 one: chosen at the first open by CEESAFE_WINDOW_SYSTEM=wayland
   (step 1; X11 stays the default until step 2 makes Wayland it). libwayland-client.so.0 and libxkbcommon.so.0 are
   loaded with dlopen, and libdecor-0.so.0 when the compositor draws no title bar (GNOME); nothing is linked. One
   connection serves every window of the process; its events are read in the pump and in the waits, on the window
   thread. A Wayland window keeps the X11 one's record (struct cee_x11, display NULL) for what both have: its title,
   its accessibility serial and focus (ADR 0164 §11); the rest is its struct cee_wl_window. The protocols' tables are
   written out here from their XML (xdg-shell, xdg-decoration, fractional-scale, viewporter, cursor-shape); the core
   ones are libwayland-client's own symbols. */
#include <sys/mman.h>
#include <sys/stat.h>

static int32_t cee_x_key(unsigned long sym);

typedef struct cee_wl_proxy cee_wl_proxy;
struct cee_wl_interface;
struct cee_wl_message {
    const char *name;
    const char *signature;
    const struct cee_wl_interface **types;
};
struct cee_wl_interface {
    const char *name;
    int version;
    int method_count;
    const struct cee_wl_message *methods;
    int event_count;
    const struct cee_wl_message *events;
};
struct cee_wl_array {
    size_t size;
    size_t alloc;
    void *data;
};
typedef int32_t cee_wl_fixed; /* 24.8 */
typedef void (*cee_wl_fn)(void);

static float cee_wl_float(cee_wl_fixed f) { return (float)f / 256.0f; }

/* The staged and xdg protocols' tables; their arguments' interfaces are filled at load, the core ones being the
   library's data. */
static const struct cee_wl_interface *cee_wl_types[32];
#define CEE_WLT(k) (&cee_wl_types[k])
/* cee_wl_types: 0 NULL, 1-2 (xdg_surface, wl_surface), 3 xdg_toplevel, 4-6 (xdg_popup, xdg_surface, xdg_positioner),
   7 xdg_toplevel, 8-9 wl_seat, 10 wl_output, 11-12 (zxdg_toplevel_decoration_v1, xdg_toplevel), 13-14
   (wp_fractional_scale_v1, wl_surface), 15-16 (wp_viewport, wl_surface), 17-18 (wp_cursor_shape_device_v1, wl_pointer),
   19-20 (wp_cursor_shape_device_v1, NULL), 21 xdg_positioner, 22 xdg_activation_token_v1, 23-24 (NULL, wl_surface),
   25-26 (zwp_text_input_v3, wl_seat), 27 wl_surface */
static const struct cee_wl_interface cee_xdg_positioner_interface = {"xdg_positioner", 6, 0, NULL, 0, NULL};
static const struct cee_wl_interface cee_xdg_popup_interface = {"xdg_popup", 6, 0, NULL, 0, NULL};
static const struct cee_wl_message cee_xdg_wm_base_requests[] = {
    {"destroy", "", CEE_WLT(0)},
    {"create_positioner", "n", CEE_WLT(21)},
    {"get_xdg_surface", "no", CEE_WLT(1)},
    {"pong", "u", CEE_WLT(0)},
};
static const struct cee_wl_message cee_xdg_wm_base_events[] = {{"ping", "u", CEE_WLT(0)}};
static const struct cee_wl_interface cee_xdg_wm_base_interface = {"xdg_wm_base", 6, 4, cee_xdg_wm_base_requests, 1,
                                                                  cee_xdg_wm_base_events};
static const struct cee_wl_message cee_xdg_surface_requests[] = {
    {"destroy", "", CEE_WLT(0)},
    {"get_toplevel", "n", CEE_WLT(3)},
    {"get_popup", "n?oo", CEE_WLT(4)},
    {"set_window_geometry", "iiii", CEE_WLT(0)},
    {"ack_configure", "u", CEE_WLT(0)},
};
static const struct cee_wl_message cee_xdg_surface_events[] = {{"configure", "u", CEE_WLT(0)}};
static const struct cee_wl_interface cee_xdg_surface_interface = {"xdg_surface", 6, 5, cee_xdg_surface_requests, 1,
                                                                  cee_xdg_surface_events};
static const struct cee_wl_message cee_xdg_toplevel_requests[] = {
    {"destroy", "", CEE_WLT(0)},
    {"set_parent", "?o", CEE_WLT(7)},
    {"set_title", "s", CEE_WLT(0)},
    {"set_app_id", "s", CEE_WLT(0)},
    {"show_window_menu", "ouii", CEE_WLT(8)},
    {"move", "ou", CEE_WLT(8)},
    {"resize", "ouu", CEE_WLT(8)},
    {"set_max_size", "ii", CEE_WLT(0)},
    {"set_min_size", "ii", CEE_WLT(0)},
    {"set_maximized", "", CEE_WLT(0)},
    {"unset_maximized", "", CEE_WLT(0)},
    {"set_fullscreen", "?o", CEE_WLT(10)},
    {"unset_fullscreen", "", CEE_WLT(0)},
    {"set_minimized", "", CEE_WLT(0)},
};
static const struct cee_wl_message cee_xdg_toplevel_events[] = {
    {"configure", "iia", CEE_WLT(0)},
    {"close", "", CEE_WLT(0)},
    {"configure_bounds", "4ii", CEE_WLT(0)},
    {"wm_capabilities", "5a", CEE_WLT(0)},
};
static const struct cee_wl_interface cee_xdg_toplevel_interface = {"xdg_toplevel", 6, 14, cee_xdg_toplevel_requests, 4,
                                                                   cee_xdg_toplevel_events};
static const struct cee_wl_message cee_decoration_manager_requests[] = {
    {"destroy", "", CEE_WLT(0)},
    {"get_toplevel_decoration", "no", CEE_WLT(11)},
};
static const struct cee_wl_interface cee_decoration_manager_interface = {
    "zxdg_decoration_manager_v1", 1, 2, cee_decoration_manager_requests, 0, NULL};
static const struct cee_wl_message cee_decoration_requests[] = {
    {"destroy", "", CEE_WLT(0)},
    {"set_mode", "u", CEE_WLT(0)},
    {"unset_mode", "", CEE_WLT(0)},
};
static const struct cee_wl_message cee_decoration_events[] = {{"configure", "u", CEE_WLT(0)}};
static const struct cee_wl_interface cee_decoration_interface = {"zxdg_toplevel_decoration_v1", 1, 3,
                                                                 cee_decoration_requests, 1, cee_decoration_events};
static const struct cee_wl_message cee_fractional_manager_requests[] = {
    {"destroy", "", CEE_WLT(0)},
    {"get_fractional_scale", "no", CEE_WLT(13)},
};
static const struct cee_wl_interface cee_fractional_manager_interface = {
    "wp_fractional_scale_manager_v1", 1, 2, cee_fractional_manager_requests, 0, NULL};
static const struct cee_wl_message cee_fractional_requests[] = {{"destroy", "", CEE_WLT(0)}};
static const struct cee_wl_message cee_fractional_events[] = {{"preferred_scale", "u", CEE_WLT(0)}};
static const struct cee_wl_interface cee_fractional_interface = {"wp_fractional_scale_v1", 1, 1, cee_fractional_requests,
                                                                 1, cee_fractional_events};
static const struct cee_wl_message cee_viewporter_requests[] = {
    {"destroy", "", CEE_WLT(0)},
    {"get_viewport", "no", CEE_WLT(15)},
};
static const struct cee_wl_interface cee_viewporter_interface = {"wp_viewporter", 1, 2, cee_viewporter_requests, 0,
                                                                 NULL};
static const struct cee_wl_message cee_viewport_requests[] = {
    {"destroy", "", CEE_WLT(0)},
    {"set_source", "ffff", CEE_WLT(0)},
    {"set_destination", "ii", CEE_WLT(0)},
};
static const struct cee_wl_interface cee_viewport_interface = {"wp_viewport", 1, 3, cee_viewport_requests, 0, NULL};
static const struct cee_wl_message cee_cursor_manager_requests[] = {
    {"destroy", "", CEE_WLT(0)},
    {"get_pointer", "no", CEE_WLT(17)},
    {"get_tablet_tool_v2", "no", CEE_WLT(19)},
};
static const struct cee_wl_interface cee_cursor_manager_interface = {"wp_cursor_shape_manager_v1", 1, 3,
                                                                     cee_cursor_manager_requests, 0, NULL};
static const struct cee_wl_message cee_cursor_device_requests[] = {
    {"destroy", "", CEE_WLT(0)},
    {"set_shape", "uu", CEE_WLT(0)},
};
static const struct cee_wl_interface cee_cursor_device_interface = {"wp_cursor_shape_device_v1", 1, 2,
                                                                    cee_cursor_device_requests, 0, NULL};
static const struct cee_wl_interface cee_activation_token_interface = {"xdg_activation_token_v1", 1, 0, NULL, 0, NULL};
static const struct cee_wl_message cee_activation_requests[] = {
    {"destroy", "", CEE_WLT(0)},
    {"get_activation_token", "n", CEE_WLT(22)},
    {"activate", "so", CEE_WLT(23)},
};
static const struct cee_wl_interface cee_activation_interface = {"xdg_activation_v1", 1, 3, cee_activation_requests, 0,
                                                                 NULL};
static const struct cee_wl_message cee_text_manager_requests[] = {
    {"destroy", "", CEE_WLT(0)},
    {"get_text_input", "no", CEE_WLT(25)},
};
static const struct cee_wl_interface cee_text_manager_interface = {"zwp_text_input_manager_v3", 1, 2,
                                                                   cee_text_manager_requests, 0, NULL};
static const struct cee_wl_message cee_text_input_requests[] = {
    {"destroy", "", CEE_WLT(0)},
    {"enable", "", CEE_WLT(0)},
    {"disable", "", CEE_WLT(0)},
    {"set_surrounding_text", "sii", CEE_WLT(0)},
    {"set_text_change_cause", "u", CEE_WLT(0)},
    {"set_content_type", "uu", CEE_WLT(0)},
    {"set_cursor_rectangle", "iiii", CEE_WLT(0)},
    {"commit", "", CEE_WLT(0)},
};
static const struct cee_wl_message cee_text_input_events[] = {
    {"enter", "o", CEE_WLT(27)},
    {"leave", "o", CEE_WLT(27)},
    {"preedit_string", "?sii", CEE_WLT(0)},
    {"commit_string", "?s", CEE_WLT(0)},
    {"delete_surrounding_text", "uu", CEE_WLT(0)},
    {"done", "u", CEE_WLT(0)},
};
static const struct cee_wl_interface cee_text_input_interface = {"zwp_text_input_v3", 1, 8, cee_text_input_requests, 6,
                                                                 cee_text_input_events};

/* libdecor's, as its header has them (0.2): the context's and a frame's callbacks, with room for later ones. */
struct cee_decor_interface {
    void (*error)(void *context, int error, const char *message);
    cee_wl_fn reserved[10];
};
struct cee_decor_frame_interface {
    void (*configure)(void *frame, void *configuration, void *data);
    void (*close)(void *frame, void *data);
    void (*commit)(void *frame, void *data);
    void (*dismiss_popup)(void *frame, const char *seat, void *data);
    cee_wl_fn reserved[12];
};

static struct {
    int state; /* 0 not tried, 1 loaded, -1 failed */
    cee_wl_proxy *(*connect)(const char *);
    void (*disconnect)(cee_wl_proxy *);
    int (*get_fd)(cee_wl_proxy *);
    int (*dispatch_pending)(cee_wl_proxy *);
    int (*prepare_read)(cee_wl_proxy *);
    int (*read_events)(cee_wl_proxy *);
    void (*cancel_read)(cee_wl_proxy *);
    int (*flush)(cee_wl_proxy *);
    int (*roundtrip)(cee_wl_proxy *);
    cee_wl_proxy *(*marshal_flags)(cee_wl_proxy *, uint32_t, const struct cee_wl_interface *, uint32_t, uint32_t, ...);
    int (*add_listener)(cee_wl_proxy *, cee_wl_fn *, void *);
    uint32_t (*get_version)(cee_wl_proxy *);
    void (*destroy)(cee_wl_proxy *);
    void *(*get_user_data)(cee_wl_proxy *);
    const struct cee_wl_interface *data_manager, *data_device, *data_source, *data_offer;
    const struct cee_wl_interface *registry, *compositor, *surface, *shm, *shm_pool, *buffer, *seat, *pointer,
        *keyboard, *touch, *callback, *output, *region;
    /* xkbcommon */
    void *(*xkb_context_new)(int);
    void *(*xkb_keymap_new_from_string)(void *, const char *, int, int);
    void *(*xkb_state_new)(void *);
    void (*xkb_keymap_unref)(void *);
    void (*xkb_state_unref)(void *);
    int (*xkb_state_update_mask)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    uint32_t (*xkb_state_key_get_one_sym)(void *, uint32_t);
    uint32_t (*xkb_state_key_get_utf32)(void *, uint32_t);
    uint32_t (*xkb_state_key_get_layout)(void *, uint32_t);
    int (*xkb_keymap_key_get_syms_by_level)(void *, uint32_t, uint32_t, uint32_t, const uint32_t **);
    int (*xkb_keymap_key_repeats)(void *, uint32_t);
    int (*xkb_state_mod_name_is_active)(void *, const char *, int);
    /* libdecor, when it loads */
    void *(*decor_new)(cee_wl_proxy *, struct cee_decor_interface *);
    void *(*decor_decorate)(void *, cee_wl_proxy *, struct cee_decor_frame_interface *, void *);
    void (*decor_frame_unref)(void *);
    void (*decor_frame_set_title)(void *, const char *);
    void (*decor_frame_set_app_id)(void *, const char *);
    void (*decor_frame_map)(void *);
    void (*decor_frame_commit)(void *, void *, void *);
    int (*decor_configuration_get_content_size)(void *, void *, int *, int *);
    void *(*decor_state_new)(int, int);
    void (*decor_state_free)(void *);
    int (*decor_dispatch)(void *, int);
} cee_wl;

/* The connection and what the compositor offers, the seat's devices, the keymap, and the keys' repeat. */
static struct {
    int state; /* 0 not tried, 1 connected, -1 none */
    cee_wl_proxy *display, *registry, *compositor, *shm, *wm_base, *seat, *pointer, *keyboard, *touch;
    cee_wl_proxy *decoration_manager, *fractional_manager, *viewporter, *cursor_manager, *cursor_device, *activation;
    cee_wl_proxy *data_manager, *data_device, *text_manager, *text_input;
    uint32_t serial; /* the last input event's, which a selection change carries */
    uint32_t compositor_version, seat_version, seat_caps;
    int xbgr; /* the compositor takes XBGR8888, which is the frames' own byte order */
    void *decor; /* libdecor's context, for compositors that draw no title bar */
    struct cee_window *pointer_window, *keyboard_window;
    uint32_t pointer_serial;
    float pointer_x, pointer_y;
    float wheel_x, wheel_y; /* this pointer frame's scroll, in lines */
    int wheel_discrete;
    void *xkb_context, *xkb_keymap, *xkb_state;
    int32_t repeat_rate, repeat_delay; /* per second, ms; a rate of 0 is no repeat */
    uint32_t repeat_key;               /* the evdev code + 8 of the key held, 0 for none */
    double repeat_at;
    char app_id[64];
} cee_wls;

struct cee_wl_buffer {
    cee_wl_proxy *buffer;
    uint32_t *data;
    size_t size;
    uint32_t width, height;
    int busy;
};

struct cee_wl_window {
    cee_wl_proxy *surface, *xdg_surface, *toplevel, *decoration, *fractional, *viewport;
    void *frame; /* libdecor's, when it draws the title bar */
    int configured;
    int32_t pending_width, pending_height;
    struct cee_wl_buffer buffers[3];
};

static struct cee_wl_window *cee_wl_of(struct cee_window *w) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    return n != NULL ? (struct cee_wl_window *)n->wl : NULL;
}

/* The window whose surface this is: ours, not libdecor's title bar. */
static struct cee_window *cee_wl_window_of(cee_wl_proxy *surface) {
    for (struct cee_window *w = cee_wake_windows; w != NULL; w = w->wake_link) {
        struct cee_wl_window *ww = !w->scripted ? cee_wl_of(w) : NULL;
        if (ww != NULL && ww->surface == surface) {
            return w;
        }
    }
    return NULL;
}

static int cee_wl_load(void) {
    if (cee_wl.state != 0) {
        return cee_wl.state == 1;
    }
    cee_wl.state = -1;
    void *c = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
    void *k = c != NULL ? dlopen("libxkbcommon.so.0", RTLD_NOW | RTLD_LOCAL) : NULL;
    if (c == NULL || k == NULL) {
        return 0;
    }
#define CEE_WL(handle, field, type, name)                                                                              \
    cee_wl.field = (type)cee_x_symbol(handle, name);                                                                   \
    if (cee_wl.field == NULL) {                                                                                        \
        return 0;                                                                                                      \
    }
#define CEE_WL_DATA(field, name)                                                                                       \
    cee_wl.field = (const struct cee_wl_interface *)dlsym(c, name);                                                    \
    if (cee_wl.field == NULL) {                                                                                        \
        return 0;                                                                                                      \
    }
    CEE_WL(c, connect, cee_wl_proxy * (*)(const char *), "wl_display_connect")
    CEE_WL(c, disconnect, void (*)(cee_wl_proxy *), "wl_display_disconnect")
    CEE_WL(c, get_fd, int (*)(cee_wl_proxy *), "wl_display_get_fd")
    CEE_WL(c, dispatch_pending, int (*)(cee_wl_proxy *), "wl_display_dispatch_pending")
    CEE_WL(c, prepare_read, int (*)(cee_wl_proxy *), "wl_display_prepare_read")
    CEE_WL(c, read_events, int (*)(cee_wl_proxy *), "wl_display_read_events")
    CEE_WL(c, cancel_read, void (*)(cee_wl_proxy *), "wl_display_cancel_read")
    CEE_WL(c, flush, int (*)(cee_wl_proxy *), "wl_display_flush")
    CEE_WL(c, roundtrip, int (*)(cee_wl_proxy *), "wl_display_roundtrip")
    CEE_WL(c, marshal_flags,
           cee_wl_proxy * (*)(cee_wl_proxy *, uint32_t, const struct cee_wl_interface *, uint32_t, uint32_t, ...),
           "wl_proxy_marshal_flags")
    CEE_WL(c, add_listener, int (*)(cee_wl_proxy *, cee_wl_fn *, void *), "wl_proxy_add_listener")
    CEE_WL(c, get_version, uint32_t(*)(cee_wl_proxy *), "wl_proxy_get_version")
    CEE_WL(c, destroy, void (*)(cee_wl_proxy *), "wl_proxy_destroy")
    CEE_WL(c, get_user_data, void *(*)(cee_wl_proxy *), "wl_proxy_get_user_data")
    CEE_WL_DATA(data_manager, "wl_data_device_manager_interface")
    CEE_WL_DATA(data_device, "wl_data_device_interface")
    CEE_WL_DATA(data_source, "wl_data_source_interface")
    CEE_WL_DATA(data_offer, "wl_data_offer_interface")
    CEE_WL_DATA(registry, "wl_registry_interface")
    CEE_WL_DATA(compositor, "wl_compositor_interface")
    CEE_WL_DATA(surface, "wl_surface_interface")
    CEE_WL_DATA(shm, "wl_shm_interface")
    CEE_WL_DATA(shm_pool, "wl_shm_pool_interface")
    CEE_WL_DATA(buffer, "wl_buffer_interface")
    CEE_WL_DATA(seat, "wl_seat_interface")
    CEE_WL_DATA(pointer, "wl_pointer_interface")
    CEE_WL_DATA(keyboard, "wl_keyboard_interface")
    CEE_WL_DATA(touch, "wl_touch_interface")
    CEE_WL_DATA(callback, "wl_callback_interface")
    CEE_WL_DATA(output, "wl_output_interface")
    CEE_WL_DATA(region, "wl_region_interface")
    CEE_WL(k, xkb_context_new, void *(*)(int), "xkb_context_new")
    CEE_WL(k, xkb_keymap_new_from_string, void *(*)(void *, const char *, int, int), "xkb_keymap_new_from_string")
    CEE_WL(k, xkb_state_new, void *(*)(void *), "xkb_state_new")
    CEE_WL(k, xkb_keymap_unref, void (*)(void *), "xkb_keymap_unref")
    CEE_WL(k, xkb_state_unref, void (*)(void *), "xkb_state_unref")
    CEE_WL(k, xkb_state_update_mask, int (*)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t),
           "xkb_state_update_mask")
    CEE_WL(k, xkb_state_key_get_one_sym, uint32_t(*)(void *, uint32_t), "xkb_state_key_get_one_sym")
    CEE_WL(k, xkb_state_key_get_utf32, uint32_t(*)(void *, uint32_t), "xkb_state_key_get_utf32")
    CEE_WL(k, xkb_state_key_get_layout, uint32_t(*)(void *, uint32_t), "xkb_state_key_get_layout")
    CEE_WL(k, xkb_keymap_key_get_syms_by_level, int (*)(void *, uint32_t, uint32_t, uint32_t, const uint32_t **),
           "xkb_keymap_key_get_syms_by_level")
    CEE_WL(k, xkb_keymap_key_repeats, int (*)(void *, uint32_t), "xkb_keymap_key_repeats")
    CEE_WL(k, xkb_state_mod_name_is_active, int (*)(void *, const char *, int), "xkb_state_mod_name_is_active")
#undef CEE_WL_DATA
    cee_wl_types[1] = &cee_xdg_surface_interface;
    cee_wl_types[2] = cee_wl.surface;
    cee_wl_types[3] = &cee_xdg_toplevel_interface;
    cee_wl_types[4] = &cee_xdg_popup_interface;
    cee_wl_types[5] = &cee_xdg_surface_interface;
    cee_wl_types[6] = &cee_xdg_positioner_interface;
    cee_wl_types[7] = &cee_xdg_toplevel_interface;
    cee_wl_types[8] = cee_wl.seat;
    cee_wl_types[10] = cee_wl.output;
    cee_wl_types[11] = &cee_decoration_interface;
    cee_wl_types[12] = &cee_xdg_toplevel_interface;
    cee_wl_types[13] = &cee_fractional_interface;
    cee_wl_types[14] = cee_wl.surface;
    cee_wl_types[15] = &cee_viewport_interface;
    cee_wl_types[16] = cee_wl.surface;
    cee_wl_types[17] = &cee_cursor_device_interface;
    cee_wl_types[18] = cee_wl.pointer;
    cee_wl_types[19] = &cee_cursor_device_interface;
    cee_wl_types[21] = &cee_xdg_positioner_interface;
    cee_wl_types[22] = &cee_activation_token_interface;
    cee_wl_types[24] = cee_wl.surface;
    cee_wl_types[25] = &cee_text_input_interface;
    cee_wl_types[26] = cee_wl.seat;
    cee_wl_types[27] = cee_wl.surface;
#undef CEE_WL
    cee_wl.state = 1;
    return 1;
}

/* libdecor, at the first window that wants a title bar the compositor does not draw (ADR 0205 §7): a window with the
   program's own (ADR 0206) loads none of it. Optional: without it such a window has no title bar. */
static int cee_wl_decor_state; /* 0 not tried, 1 loaded, -1 not there */
static int cee_wl_load_decor(void) {
    if (cee_wl_decor_state != 0) {
        return cee_wl_decor_state == 1;
    }
    cee_wl_decor_state = -1;
    /* libdecor is optional: without it a window on a compositor that draws no title bar has none. */
    void *d = dlopen("libdecor-0.so.0", RTLD_NOW | RTLD_LOCAL);
    if (d != NULL) {
        int ok = 1;
#define CEE_DECOR(field, type, name)                                                                                   \
    cee_wl.field = (type)cee_x_symbol(d, name);                                                                        \
    ok = ok && cee_wl.field != NULL;
        CEE_DECOR(decor_new, void *(*)(cee_wl_proxy *, struct cee_decor_interface *), "libdecor_new")
        CEE_DECOR(decor_decorate, void *(*)(void *, cee_wl_proxy *, struct cee_decor_frame_interface *, void *),
                  "libdecor_decorate")
        CEE_DECOR(decor_frame_unref, void (*)(void *), "libdecor_frame_unref")
        CEE_DECOR(decor_frame_set_title, void (*)(void *, const char *), "libdecor_frame_set_title")
        CEE_DECOR(decor_frame_set_app_id, void (*)(void *, const char *), "libdecor_frame_set_app_id")
        CEE_DECOR(decor_frame_map, void (*)(void *), "libdecor_frame_map")
        CEE_DECOR(decor_frame_commit, void (*)(void *, void *, void *), "libdecor_frame_commit")
        CEE_DECOR(decor_configuration_get_content_size, int (*)(void *, void *, int *, int *),
                  "libdecor_configuration_get_content_size")
        CEE_DECOR(decor_state_new, void *(*)(int, int), "libdecor_state_new")
        CEE_DECOR(decor_state_free, void (*)(void *), "libdecor_state_free")
        CEE_DECOR(decor_dispatch, int (*)(void *, int), "libdecor_dispatch")
#undef CEE_DECOR
        if (!ok) {
            cee_wl.decor_new = NULL;
        }
    }
    if (cee_wl.decor_new == NULL) {
        return 0;
    }
    cee_wl_decor_state = 1;
    return 1;
}

/* ---- The globals */

static void cee_wl_ping(void *data, cee_wl_proxy *base, uint32_t serial) {
    (void)data;
    cee_wl.marshal_flags(base, 3 /* pong */, NULL, cee_wl.get_version(base), 0, serial);
}
static cee_wl_fn cee_wl_wm_base_listener[] = {(cee_wl_fn)cee_wl_ping};

static void cee_wl_shm_format(void *data, cee_wl_proxy *shm, uint32_t format) {
    (void)data;
    (void)shm;
    if (format == 0x34324258u) { /* XBGR8888: R, G, B, X in memory, as a frame's pixels are */
        cee_wls.xbgr = 1;
    }
}
static cee_wl_fn cee_wl_shm_listener[] = {(cee_wl_fn)cee_wl_shm_format};

static void cee_wl_seat_caps(void *data, cee_wl_proxy *seat, uint32_t caps);
static void cee_wl_seat_name(void *data, cee_wl_proxy *seat, const char *name) {
    (void)data;
    (void)seat;
    (void)name;
}
static cee_wl_fn cee_wl_seat_listener[] = {(cee_wl_fn)cee_wl_seat_caps, (cee_wl_fn)cee_wl_seat_name};

static cee_wl_proxy *cee_wl_bind(cee_wl_proxy *registry, uint32_t name, const struct cee_wl_interface *interface,
                                 uint32_t offered, uint32_t wanted) {
    uint32_t version = offered < wanted ? offered : wanted;
    return cee_wl.marshal_flags(registry, 0 /* bind */, interface, version, 0, name, interface->name, version, NULL);
}

static void cee_wl_global(void *data, cee_wl_proxy *registry, uint32_t name, const char *interface,
                          uint32_t version) {
    (void)data;
    if (strcmp(interface, "wl_compositor") == 0 && cee_wls.compositor == NULL) {
        cee_wls.compositor_version = version < 6 ? version : 6;
        cee_wls.compositor = cee_wl_bind(registry, name, cee_wl.compositor, version, 6);
    } else if (strcmp(interface, "wl_shm") == 0 && cee_wls.shm == NULL) {
        cee_wls.shm = cee_wl_bind(registry, name, cee_wl.shm, version, 1);
        cee_wl.add_listener(cee_wls.shm, cee_wl_shm_listener, NULL);
    } else if (strcmp(interface, "xdg_wm_base") == 0 && cee_wls.wm_base == NULL) {
        cee_wls.wm_base = cee_wl_bind(registry, name, &cee_xdg_wm_base_interface, version, 6);
        cee_wl.add_listener(cee_wls.wm_base, cee_wl_wm_base_listener, NULL);
    } else if (strcmp(interface, "wl_seat") == 0 && cee_wls.seat == NULL) {
        /* At most 9: from 10 a compositor may repeat keys itself, and the runtime repeats them (ADR 0205 §2.2). */
        cee_wls.seat_version = version < 9 ? version : 9;
        cee_wls.seat = cee_wl_bind(registry, name, cee_wl.seat, version, 9);
        cee_wl.add_listener(cee_wls.seat, cee_wl_seat_listener, NULL);
    } else if (strcmp(interface, "zxdg_decoration_manager_v1") == 0) {
        cee_wls.decoration_manager = cee_wl_bind(registry, name, &cee_decoration_manager_interface, version, 1);
    } else if (strcmp(interface, "wp_fractional_scale_manager_v1") == 0) {
        cee_wls.fractional_manager = cee_wl_bind(registry, name, &cee_fractional_manager_interface, version, 1);
    } else if (strcmp(interface, "wp_viewporter") == 0) {
        cee_wls.viewporter = cee_wl_bind(registry, name, &cee_viewporter_interface, version, 1);
    } else if (strcmp(interface, "wl_data_device_manager") == 0 && cee_wls.data_manager == NULL) {
        cee_wls.data_manager = cee_wl_bind(registry, name, cee_wl.data_manager, version, 3);
    } else if (strcmp(interface, "zwp_text_input_manager_v3") == 0 && cee_wls.text_manager == NULL) {
        cee_wls.text_manager = cee_wl_bind(registry, name, &cee_text_manager_interface, version, 1);
    } else if (strcmp(interface, "xdg_activation_v1") == 0) {
        cee_wls.activation = cee_wl_bind(registry, name, &cee_activation_interface, version, 1);
    } else if (strcmp(interface, "wp_cursor_shape_manager_v1") == 0) {
        cee_wls.cursor_manager = cee_wl_bind(registry, name, &cee_cursor_manager_interface, version, 1);
    }
}
static void cee_wl_global_remove(void *data, cee_wl_proxy *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}
static cee_wl_fn cee_wl_registry_listener[] = {(cee_wl_fn)cee_wl_global, (cee_wl_fn)cee_wl_global_remove};

/* ---- The pointer */

static uint32_t cee_wl_shape(int32_t cursor) {
    /* WindowCursor's order: Default, Pointer, Text, Crosshair, Grab, ResizeHorizontal, ResizeVertical. */
    static const uint32_t shapes[] = {1 /* default */, 4 /* pointer */, 9 /* text */, 8 /* crosshair */, 16 /* grab */,
                                      26 /* ew-resize */, 27 /* ns-resize */};
    return shapes[cursor >= 0 && cursor < 7 ? cursor : 0];
}

/* The edge of a window with the program's title bar that (x, y) is within 6 points of, as xdg_toplevel's resize edge
   (top 1, bottom 2, left 4, right 8, and their sums for corners); 0 for none, and always for one maximized or tiled
   (ADR 0206). */
static uint32_t cee_wl_edge(struct cee_window *w, float x, float y) {
    if (!w->program_bar || w->maximized || w->tiled) {
        return 0;
    }
    const float m = 6.0f;
    uint32_t edge = 0;
    edge |= y < m ? 1u : 0u;
    edge |= y >= (float)w->height - m ? 2u : 0u;
    edge |= x < m ? 4u : 0u;
    edge |= x >= (float)w->width - m ? 8u : 0u;
    return edge;
}

/* cursor-shape-v1's resize shapes for an edge: n 19, s 22, w 25, e 18, nw 21, ne 20, sw 24, se 23. */
static uint32_t cee_wl_edge_shape(uint32_t edge) {
    switch (edge) {
    case 1: return 19;
    case 2: return 22;
    case 4: return 25;
    case 8: return 18;
    case 5: return 21;
    case 9: return 20;
    case 6: return 24;
    case 10: return 23;
    default: return 1;
    }
}

static void cee_wl_set_cursor(struct cee_window *w) {
    if (cee_wls.cursor_device != NULL && w != NULL && cee_wls.pointer_window == w) {
        uint32_t edge = cee_wl_edge(w, cee_wls.pointer_x, cee_wls.pointer_y);
        cee_wl.marshal_flags(cee_wls.cursor_device, 1 /* set_shape */, NULL, 1, 0, cee_wls.pointer_serial,
                             edge != 0 ? cee_wl_edge_shape(edge) : cee_wl_shape(w->cursor));
    }
}

/* Whether (x, y) is in box `k` of the program's title bar (0 minimize, 1 maximize, 2 close, 3 menu). */
static int cee_wl_in_box(const struct cee_window *w, int k, float x, float y) {
    const float *b = &w->bar[1 + 4 * k];
    return b[2] > 0.0f && b[3] > 0.0f && x >= b[0] && y >= b[1] && x < b[0] + b[2] && y < b[1] + b[3];
}

/* A press the runtime takes for a window with the program's title bar (ADR 0206): at an edge it resizes; in the bar
   but in none of its boxes, the primary button moves the window and a second press within 0.4 s toggles maximized,
   and the secondary shows the window menu. 1 when taken: the program sees no press then (the compositor's grab would
   take its release). */
static double cee_wl_bar_pressed_at;
static int cee_wl_bar_press(struct cee_window *w, int32_t button, uint32_t serial, float x, float y) {
    struct cee_wl_window *ww = cee_wl_of(w);
    if (!w->program_bar || ww == NULL || ww->toplevel == NULL) {
        return 0;
    }
    uint32_t version = cee_wl.get_version(ww->toplevel);
    uint32_t edge = cee_wl_edge(w, x, y);
    if (edge != 0 && button == 0) {
        cee_wl.marshal_flags(ww->toplevel, 6 /* resize */, NULL, version, 0, cee_wls.seat, serial, edge);
        return 1;
    }
    float height = w->bar[0];
    if (y >= height || cee_wl_in_box(w, 0, x, y) || cee_wl_in_box(w, 1, x, y) || cee_wl_in_box(w, 2, x, y) ||
        cee_wl_in_box(w, 3, x, y)) {
        return 0;
    }
    if (button == 1) {
        cee_wl.marshal_flags(ww->toplevel, 4 /* show_window_menu */, NULL, version, 0, cee_wls.seat, serial,
                             (int32_t)x, (int32_t)y);
        return 1;
    }
    if (button != 0) {
        return 0;
    }
    double now = cee_window_now();
    if (now - cee_wl_bar_pressed_at < 0.4) {
        cee_wl_bar_pressed_at = 0.0;
        cee_wl.marshal_flags(ww->toplevel, w->maximized ? 10 /* unset_maximized */ : 9 /* set_maximized */, NULL,
                             version, 0);
        return 1;
    }
    cee_wl_bar_pressed_at = now;
    cee_wl.marshal_flags(ww->toplevel, 5 /* move */, NULL, version, 0, cee_wls.seat, serial);
    return 1;
}

static void cee_wl_pointer_enter(void *data, cee_wl_proxy *p, uint32_t serial, cee_wl_proxy *surface, cee_wl_fixed x,
                                 cee_wl_fixed y) {
    (void)data;
    (void)p;
    cee_wls.pointer_window = surface != NULL ? cee_wl_window_of(surface) : NULL;
    cee_wls.pointer_serial = serial;
    cee_wls.pointer_x = cee_wl_float(x);
    cee_wls.pointer_y = cee_wl_float(y);
    if (cee_wls.pointer_window != NULL) {
        cee_wl_set_cursor(cee_wls.pointer_window);
        cee_window_push(cee_wls.pointer_window, CEE_EVENT_MOVED, 0, 0, 0, 0, cee_wls.pointer_x, cee_wls.pointer_y);
    }
}
static void cee_wl_pointer_leave(void *data, cee_wl_proxy *p, uint32_t serial, cee_wl_proxy *surface) {
    (void)data;
    (void)p;
    (void)serial;
    (void)surface;
    cee_wls.pointer_window = NULL;
}
static void cee_wl_pointer_motion(void *data, cee_wl_proxy *p, uint32_t time, cee_wl_fixed x, cee_wl_fixed y) {
    (void)data;
    (void)p;
    (void)time;
    struct cee_window *w = cee_wls.pointer_window;
    uint32_t was = w != NULL ? cee_wl_edge(w, cee_wls.pointer_x, cee_wls.pointer_y) : 0;
    cee_wls.pointer_x = cee_wl_float(x);
    cee_wls.pointer_y = cee_wl_float(y);
    if (w != NULL) {
        if (cee_wl_edge(w, cee_wls.pointer_x, cee_wls.pointer_y) != was) {
            cee_wl_set_cursor(w); /* onto an edge or off it: the resize shape, or the program's own */
        }
        cee_window_push(w, CEE_EVENT_MOVED, 0, 0, 0, 0, cee_wls.pointer_x, cee_wls.pointer_y);
    }
}
static void cee_wl_pointer_button(void *data, cee_wl_proxy *p, uint32_t serial, uint32_t time, uint32_t button,
                                  uint32_t state) {
    (void)data;
    (void)p;
    (void)time;
    cee_wls.serial = serial;
    /* evdev's codes: BTN_LEFT 0x110, BTN_RIGHT 0x111, BTN_MIDDLE 0x112 */
    int32_t b = button == 0x110 ? 0 : button == 0x111 ? 1 : button == 0x112 ? 2 : -1;
    if (b >= 0 && state == 1 && cee_wls.pointer_window != NULL &&
        cee_wl_bar_press(cee_wls.pointer_window, b, serial, cee_wls.pointer_x, cee_wls.pointer_y)) {
        cee_wl.flush(cee_wls.display);
        return; /* the window's move, resize or menu: no press for the program */
    }
    if (b >= 0 && cee_wls.pointer_window != NULL) {
        cee_window_push(cee_wls.pointer_window, CEE_EVENT_BUTTON, b, state == 1, 0, 0, cee_wls.pointer_x,
                        cee_wls.pointer_y);
    }
}
/* Scroll as WHEEL events in lines, one a notch (as X11's buttons 4 to 7 are): axis_value120 for a wheel, a touchpad's
   pixels at 10 a line; positive Wayland values are down and right, a WHEEL's up and left. */
static void cee_wl_pointer_axis(void *data, cee_wl_proxy *p, uint32_t time, uint32_t axis, cee_wl_fixed value) {
    (void)data;
    (void)p;
    (void)time;
    if (cee_wls.wheel_discrete) {
        return; /* the frame's value120 says it */
    }
    float lines = -cee_wl_float(value) / 10.0f;
    if (axis == 0) {
        cee_wls.wheel_y += lines;
    } else {
        cee_wls.wheel_x += lines;
    }
}
static void cee_wl_pointer_frame(void *data, cee_wl_proxy *p) {
    (void)data;
    (void)p;
    if ((cee_wls.wheel_x != 0.0f || cee_wls.wheel_y != 0.0f) && cee_wls.pointer_window != NULL) {
        cee_window_push(cee_wls.pointer_window, CEE_EVENT_WHEEL, 0, 0, 0, 0, -cee_wls.wheel_x, cee_wls.wheel_y);
    }
    cee_wls.wheel_x = cee_wls.wheel_y = 0.0f;
    cee_wls.wheel_discrete = 0;
}
static void cee_wl_pointer_axis_source(void *data, cee_wl_proxy *p, uint32_t source) {
    (void)data;
    (void)p;
    (void)source;
}
static void cee_wl_pointer_axis_stop(void *data, cee_wl_proxy *p, uint32_t time, uint32_t axis) {
    (void)data;
    (void)p;
    (void)time;
    (void)axis;
}
static void cee_wl_pointer_axis_discrete(void *data, cee_wl_proxy *p, uint32_t axis, int32_t discrete) {
    (void)data;
    (void)p;
    (void)axis;
    (void)discrete; /* before seat 8; value120 replaces it, and axis gives the same */
}
static void cee_wl_pointer_axis_value120(void *data, cee_wl_proxy *p, uint32_t axis, int32_t value120) {
    (void)data;
    (void)p;
    if (!cee_wls.wheel_discrete) {
        cee_wls.wheel_x = cee_wls.wheel_y = 0.0f; /* the frame's continuous value, if it came first, is the same */
    }
    cee_wls.wheel_discrete = 1;
    float lines = -(float)value120 / 120.0f;
    if (axis == 0) {
        cee_wls.wheel_y += lines;
    } else {
        cee_wls.wheel_x += lines;
    }
}
static void cee_wl_pointer_axis_direction(void *data, cee_wl_proxy *p, uint32_t axis, uint32_t direction) {
    (void)data;
    (void)p;
    (void)axis;
    (void)direction;
}
static cee_wl_fn cee_wl_pointer_listener[] = {
    (cee_wl_fn)cee_wl_pointer_enter,       (cee_wl_fn)cee_wl_pointer_leave,
    (cee_wl_fn)cee_wl_pointer_motion,      (cee_wl_fn)cee_wl_pointer_button,
    (cee_wl_fn)cee_wl_pointer_axis,        (cee_wl_fn)cee_wl_pointer_frame,
    (cee_wl_fn)cee_wl_pointer_axis_source, (cee_wl_fn)cee_wl_pointer_axis_stop,
    (cee_wl_fn)cee_wl_pointer_axis_discrete, (cee_wl_fn)cee_wl_pointer_axis_value120,
    (cee_wl_fn)cee_wl_pointer_axis_direction,
};

/* ---- Keys: xkbcommon over the compositor's keymap, and the repeat the runtime does */

static int32_t cee_wl_modifiers(void) {
    if (cee_wls.xkb_state == NULL) {
        return 0;
    }
    const int effective = 8; /* XKB_STATE_MODS_EFFECTIVE */
    return (cee_wl.xkb_state_mod_name_is_active(cee_wls.xkb_state, "Shift", effective) > 0 ? CEE_MOD_SHIFT : 0) |
           (cee_wl.xkb_state_mod_name_is_active(cee_wls.xkb_state, "Control", effective) > 0 ? CEE_MOD_CONTROL : 0) |
           (cee_wl.xkb_state_mod_name_is_active(cee_wls.xkb_state, "Mod1", effective) > 0 ? CEE_MOD_ALT : 0) |
           (cee_wl.xkb_state_mod_name_is_active(cee_wls.xkb_state, "Mod4", effective) > 0 ? CEE_MOD_COMMAND : 0);
}

/* A key's Key, from its keysym at the first level of its layout (unshifted, as X11's is: Shift and 1 is Key 1). */
static int32_t cee_wl_key_of(uint32_t code) {
    const uint32_t *syms = NULL;
    uint32_t layout = cee_wl.xkb_state_key_get_layout(cee_wls.xkb_state, code);
    int count = cee_wl.xkb_keymap_key_get_syms_by_level(cee_wls.xkb_keymap, code, layout, 0, &syms);
    uint32_t sym = count > 0 && syms != NULL ? syms[0] : cee_wl.xkb_state_key_get_one_sym(cee_wls.xkb_state, code);
    return cee_x_key(sym); /* xkbcommon's keysyms are X11's */
}

/* A key down (first or repeated): KEY, and its text unless Control or the command key is held. */
static void cee_wl_key_down(struct cee_window *w, uint32_t code, int32_t repeat) {
    int32_t modifiers = cee_wl_modifiers();
    cee_window_push(w, CEE_EVENT_KEY, cee_wl_key_of(code), 1, repeat, modifiers, 0.0f, 0.0f);
    if (!(modifiers & (CEE_MOD_CONTROL | CEE_MOD_COMMAND))) {
        uint32_t c = cee_wl.xkb_state_key_get_utf32(cee_wls.xkb_state, code);
        if (c >= 0x20 && c != 0x7f && !(c >= 0x80 && c < 0xa0)) {
            cee_window_text(w, c);
        }
    }
}

static void cee_wl_keymap(void *data, cee_wl_proxy *k, uint32_t format, int32_t fd, uint32_t size) {
    (void)data;
    (void)k;
    if (format != 1 /* XKB_V1 */ || size == 0) {
        close(fd);
        return;
    }
    char *text = (char *)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (text == MAP_FAILED) {
        return;
    }
    if (cee_wls.xkb_context == NULL) {
        cee_wls.xkb_context = cee_wl.xkb_context_new(0);
    }
    void *keymap = cee_wls.xkb_context != NULL
                       ? cee_wl.xkb_keymap_new_from_string(cee_wls.xkb_context, text, 1 /* TEXT_V1 */, 0)
                       : NULL;
    munmap(text, size);
    void *state = keymap != NULL ? cee_wl.xkb_state_new(keymap) : NULL;
    if (state == NULL) {
        if (keymap != NULL) {
            cee_wl.xkb_keymap_unref(keymap);
        }
        return;
    }
    if (cee_wls.xkb_state != NULL) {
        cee_wl.xkb_state_unref(cee_wls.xkb_state);
        cee_wl.xkb_keymap_unref(cee_wls.xkb_keymap);
    }
    cee_wls.xkb_keymap = keymap;
    cee_wls.xkb_state = state;
}
static void cee_wl_keyboard_enter(void *data, cee_wl_proxy *k, uint32_t serial, cee_wl_proxy *surface,
                                  struct cee_wl_array *keys) {
    (void)data;
    (void)k;
    (void)keys;
    struct cee_window *w = surface != NULL ? cee_wl_window_of(surface) : NULL;
    cee_wls.keyboard_window = w;
    cee_wls.serial = serial;
    if (w != NULL) {
        cee_window_focus(w, 1);
        cee_atspi_frame_focus(w, 1);
    }
}
static void cee_wl_keyboard_leave(void *data, cee_wl_proxy *k, uint32_t serial, cee_wl_proxy *surface) {
    (void)data;
    (void)k;
    (void)serial;
    (void)surface;
    struct cee_window *w = cee_wls.keyboard_window;
    cee_wls.keyboard_window = NULL;
    cee_wls.repeat_key = 0;
    if (w != NULL) {
        cee_window_focus(w, 0);
        cee_atspi_frame_focus(w, 0);
    }
}
static void cee_wl_keyboard_key(void *data, cee_wl_proxy *k, uint32_t serial, uint32_t time, uint32_t key,
                                uint32_t state) {
    (void)data;
    (void)k;
    (void)time;
    cee_wls.serial = serial;
    struct cee_window *w = cee_wls.keyboard_window;
    if (w == NULL || cee_wls.xkb_state == NULL) {
        return;
    }
    uint32_t code = key + 8u; /* evdev to XKB */
    if (state == 1) {
        cee_wl_key_down(w, code, 0);
        if (cee_wls.repeat_rate > 0 && cee_wl.xkb_keymap_key_repeats(cee_wls.xkb_keymap, code)) {
            cee_wls.repeat_key = code;
            cee_wls.repeat_at = cee_window_now() + (double)cee_wls.repeat_delay / 1000.0;
        }
    } else {
        if (cee_wls.repeat_key == code) {
            cee_wls.repeat_key = 0;
        }
        cee_window_push(w, CEE_EVENT_KEY, cee_wl_key_of(code), 0, 0, cee_wl_modifiers(), 0.0f, 0.0f);
    }
}
static void cee_wl_keyboard_modifiers(void *data, cee_wl_proxy *k, uint32_t serial, uint32_t depressed,
                                      uint32_t latched, uint32_t locked, uint32_t group) {
    (void)data;
    (void)k;
    (void)serial;
    if (cee_wls.xkb_state != NULL) {
        cee_wl.xkb_state_update_mask(cee_wls.xkb_state, depressed, latched, locked, 0, 0, group);
    }
}
static void cee_wl_keyboard_repeat(void *data, cee_wl_proxy *k, int32_t rate, int32_t delay) {
    (void)data;
    (void)k;
    cee_wls.repeat_rate = rate > 0 ? rate : 0;
    cee_wls.repeat_delay = delay > 0 ? delay : 0;
    if (rate <= 0) {
        cee_wls.repeat_key = 0;
    }
}
static cee_wl_fn cee_wl_keyboard_listener[] = {
    (cee_wl_fn)cee_wl_keymap,         (cee_wl_fn)cee_wl_keyboard_enter,     (cee_wl_fn)cee_wl_keyboard_leave,
    (cee_wl_fn)cee_wl_keyboard_key,   (cee_wl_fn)cee_wl_keyboard_modifiers, (cee_wl_fn)cee_wl_keyboard_repeat,
};

/* The held key's repeats that are due, from the pump. */
static void cee_wl_repeat(void) {
    struct cee_window *w = cee_wls.keyboard_window;
    if (cee_wls.repeat_key == 0 || w == NULL || cee_wls.repeat_rate <= 0) {
        return;
    }
    double now = cee_window_now(), step = 1.0 / (double)cee_wls.repeat_rate;
    for (int k = 0; k < 64 && now >= cee_wls.repeat_at; k++) {
        cee_wl_key_down(w, cee_wls.repeat_key, 1);
        cee_wls.repeat_at += step;
    }
    if (now >= cee_wls.repeat_at) {
        cee_wls.repeat_at = now + step; /* the program was away: no burst */
    }
}

/* ---- Touch (ADR 0155): a finger's key is its id + 1, its place in the window's points */

static struct cee_window *cee_wl_touch_window;
static void cee_wl_touch_down(void *data, cee_wl_proxy *t, uint32_t serial, uint32_t time, cee_wl_proxy *surface,
                              int32_t id, cee_wl_fixed x, cee_wl_fixed y) {
    (void)data;
    (void)t;
    (void)serial;
    struct cee_window *w = surface != NULL ? cee_wl_window_of(surface) : NULL;
    if (w != NULL) {
        cee_wl_touch_window = w;
        cee_touch_began(w, (uintptr_t)(uint32_t)id + 1u, cee_wl_float(x), cee_wl_float(y), (uint64_t)time * 1000000u);
    }
}
static void cee_wl_touch_up(void *data, cee_wl_proxy *t, uint32_t serial, uint32_t time, int32_t id) {
    (void)data;
    (void)t;
    (void)serial;
    struct cee_window *w = cee_wl_touch_window;
    if (w != NULL) {
        struct cee_window_finger *f = cee_touch_finger(w, (uintptr_t)(uint32_t)id + 1u);
        cee_touch_ended(w, (uintptr_t)(uint32_t)id + 1u, f != NULL ? f->x : 0.0f, f != NULL ? f->y : 0.0f,
                        (uint64_t)time * 1000000u, 0);
    }
}
static void cee_wl_touch_motion(void *data, cee_wl_proxy *t, uint32_t time, int32_t id, cee_wl_fixed x,
                                cee_wl_fixed y) {
    (void)data;
    (void)t;
    if (cee_wl_touch_window != NULL) {
        cee_touch_moved(cee_wl_touch_window, (uintptr_t)(uint32_t)id + 1u, cee_wl_float(x), cee_wl_float(y),
                        (uint64_t)time * 1000000u);
    }
}
static void cee_wl_touch_frame(void *data, cee_wl_proxy *t) {
    (void)data;
    (void)t;
}
static void cee_wl_touch_cancel(void *data, cee_wl_proxy *t) {
    (void)data;
    (void)t;
    if (cee_wl_touch_window != NULL) {
        cee_touch_cancel_all(cee_wl_touch_window);
    }
}
static void cee_wl_touch_shape(void *data, cee_wl_proxy *t, int32_t id, cee_wl_fixed major, cee_wl_fixed minor) {
    (void)data;
    (void)t;
    (void)id;
    (void)major;
    (void)minor;
}
static void cee_wl_touch_orientation(void *data, cee_wl_proxy *t, int32_t id, cee_wl_fixed orientation) {
    (void)data;
    (void)t;
    (void)id;
    (void)orientation;
}
static cee_wl_fn cee_wl_touch_listener[] = {
    (cee_wl_fn)cee_wl_touch_down,   (cee_wl_fn)cee_wl_touch_up,    (cee_wl_fn)cee_wl_touch_motion,
    (cee_wl_fn)cee_wl_touch_frame,  (cee_wl_fn)cee_wl_touch_cancel, (cee_wl_fn)cee_wl_touch_shape,
    (cee_wl_fn)cee_wl_touch_orientation,
};

static void cee_wl_seat_caps(void *data, cee_wl_proxy *seat, uint32_t caps) {
    (void)data;
    cee_wls.seat_caps = caps;
    uint32_t version = cee_wl.get_version(seat);
    if ((caps & 1u) && cee_wls.pointer == NULL) {
        cee_wls.pointer = cee_wl.marshal_flags(seat, 0 /* get_pointer */, cee_wl.pointer, version, 0, NULL);
        cee_wl.add_listener(cee_wls.pointer, cee_wl_pointer_listener, NULL);
        if (cee_wls.cursor_manager != NULL) {
            cee_wls.cursor_device = cee_wl.marshal_flags(cee_wls.cursor_manager, 1 /* get_pointer */,
                                                         &cee_cursor_device_interface, 1, 0, NULL, cee_wls.pointer);
        }
    }
    if ((caps & 2u) && cee_wls.keyboard == NULL) {
        cee_wls.keyboard = cee_wl.marshal_flags(seat, 1 /* get_keyboard */, cee_wl.keyboard, version, 0, NULL);
        cee_wl.add_listener(cee_wls.keyboard, cee_wl_keyboard_listener, NULL);
    }
    if ((caps & 4u) && cee_wls.touch == NULL) {
        cee_wls.touch = cee_wl.marshal_flags(seat, 2 /* get_touch */, cee_wl.touch, version, 0, NULL);
        cee_wl.add_listener(cee_wls.touch, cee_wl_touch_listener, NULL);
    }
}

/* ---- The input method: zwp_text_input_v3 (ADR 0205 step 2; ADRs 0141, 0157) ---------------------------------------
   The seat's text input is enabled while the window it entered has text input on (setTextInput), with the field's
   kind as its content type and the caret's area (setTextInputArea) as its cursor rectangle; the input method's
   composition is COMPOSITION, and what it commits is TEXT, scalar by scalar, as on X11. Its events come in groups that
   `done` applies, as the protocol has them. */


/* The window the text input entered, whether it is enabled, and the group of events `done` has not applied. */
static struct {
    struct cee_window *window;
    int enabled;
    char *preedit, *commit;
    int32_t preedit_caret;
} cee_wl_ti;

/* The text input as the window it entered wants it: enabled with the field's kind and the caret's area while its text
   input is on, disabled when not. */
static void cee_wl_text_update(void) {
    cee_wl_proxy *t = cee_wls.text_input;
    struct cee_window *w = cee_wl_ti.window;
    if (t == NULL) {
        return;
    }
    int wanted = w != NULL && w->text_input == 1;
    if (wanted) {
        /* WindowTextKind to the protocol's purpose: Text normal, Number digits, Decimal number, Email email, Url url,
           Password password with the hints sensitive data and hidden text. */
        static const uint32_t purposes[] = {0, 2, 3, 6, 5, 8};
        int32_t kind = w->text_kind >= 0 && w->text_kind <= 5 ? w->text_kind : 0;
        uint32_t hints = kind == 5 ? 0x80u /* sensitive_data */ | 0x40u /* hidden_text */ : 0u;
        if (!cee_wl_ti.enabled) {
            cee_wl.marshal_flags(t, 1 /* enable */, NULL, 1, 0);
        }
        cee_wl.marshal_flags(t, 5 /* set_content_type */, NULL, 1, 0, hints, purposes[kind]);
        cee_wl.marshal_flags(t, 6 /* set_cursor_rectangle */, NULL, 1, 0, (int32_t)w->text_area.x,
                             (int32_t)w->text_area.y, (int32_t)w->text_area.w, (int32_t)w->text_area.h);
        cee_wl.marshal_flags(t, 7 /* commit */, NULL, 1, 0);
        cee_wl_ti.enabled = 1;
    } else if (cee_wl_ti.enabled) {
        cee_wl.marshal_flags(t, 2 /* disable */, NULL, 1, 0);
        cee_wl.marshal_flags(t, 7 /* commit */, NULL, 1, 0);
        cee_wl_ti.enabled = 0;
        if (w != NULL) {
            cee_window_compose(w, "", 0, 0); /* no input method, no composition */
        }
    }
    cee_wl.flush(cee_wls.display);
}

static void cee_wl_text_enter(void *data, cee_wl_proxy *t, cee_wl_proxy *surface) {
    (void)data;
    (void)t;
    cee_wl_ti.window = surface != NULL ? cee_wl_window_of(surface) : NULL;
    cee_wl_ti.enabled = 0;
    cee_wl_text_update();
}
static void cee_wl_text_leave(void *data, cee_wl_proxy *t, cee_wl_proxy *surface) {
    (void)data;
    (void)surface;
    if (cee_wl_ti.enabled) {
        cee_wl.marshal_flags(t, 2 /* disable */, NULL, 1, 0);
        cee_wl.marshal_flags(t, 7 /* commit */, NULL, 1, 0);
    }
    if (cee_wl_ti.window != NULL) {
        cee_window_compose(cee_wl_ti.window, "", 0, 0);
    }
    cee_wl_ti.window = NULL;
    cee_wl_ti.enabled = 0;
}

static char *cee_wl_copy_string(const char *text) {
    if (text == NULL) {
        return NULL;
    }
    size_t length = strlen(text);
    char *copy = (char *)ceesafe_platform_alloc(length + 1u);
    if (copy != NULL) {
        memcpy(copy, text, length + 1u);
    }
    return copy;
}

static void cee_wl_text_preedit(void *data, cee_wl_proxy *t, const char *text, int32_t begin, int32_t end) {
    (void)data;
    (void)t;
    (void)end;
    ceesafe_platform_free(cee_wl_ti.preedit);
    cee_wl_ti.preedit = cee_wl_copy_string(text);
    cee_wl_ti.preedit_caret = begin; /* a byte offset, or -1: no caret shown */
}
static void cee_wl_text_commit(void *data, cee_wl_proxy *t, const char *text) {
    (void)data;
    (void)t;
    ceesafe_platform_free(cee_wl_ti.commit);
    cee_wl_ti.commit = cee_wl_copy_string(text);
}
static void cee_wl_text_delete(void *data, cee_wl_proxy *t, uint32_t before, uint32_t after) {
    (void)data;
    (void)t;
    (void)before;
    (void)after; /* the runtime sends no surrounding text, so an input method has none to delete */
}
/* A group of the input method's events, applied: what it committed, as TEXT, then its composition (none if it sent
   none in the group, as the protocol has it). */
static void cee_wl_text_done(void *data, cee_wl_proxy *t, uint32_t serial) {
    (void)data;
    (void)t;
    (void)serial;
    struct cee_window *w = cee_wl_ti.window;
    if (w != NULL && cee_wl_ti.commit != NULL) {
        size_t length = strlen(cee_wl_ti.commit), i = 0;
        while (i < length) {
            cee_window_text(w, cee_window_utf8((const unsigned char *)cee_wl_ti.commit, length, &i));
        }
    }
    if (w != NULL) {
        const char *p = cee_wl_ti.preedit != NULL ? cee_wl_ti.preedit : "";
        size_t length = strlen(p);
        int32_t caret = cee_wl_ti.preedit_caret;
        cee_window_compose(w, p, length, caret >= 0 && (size_t)caret <= length ? (size_t)caret : length);
    }
    ceesafe_platform_free(cee_wl_ti.preedit);
    ceesafe_platform_free(cee_wl_ti.commit);
    cee_wl_ti.preedit = NULL;
    cee_wl_ti.commit = NULL;
}
static cee_wl_fn cee_wl_text_listener[] = {
    (cee_wl_fn)cee_wl_text_enter,  (cee_wl_fn)cee_wl_text_leave,  (cee_wl_fn)cee_wl_text_preedit,
    (cee_wl_fn)cee_wl_text_commit, (cee_wl_fn)cee_wl_text_delete, (cee_wl_fn)cee_wl_text_done,
};

/* ---- The clipboard and drops: wl_data_device (ADR 0205 step 2; ADRs 0141, 0150) ---------------------------------
   The seat's data device carries both. The clipboard: setClipboardText offers the text as a data source and makes it
   the selection, with the serial of the last input the window took (the compositor gives the selection only to the
   client the user is working in); clipboardText reads the selection's offer through a pipe, or the window's own text
   when the selection is still this process's. Drops: a drag that offers `text/uri-list` is accepted with the copy
   action, its list read once when it enters (its files counted for dragOver, ADR 0150) and again at the drop (their
   paths); other drags are refused. */

static uint32_t cee_x_uri_list(const char *text, size_t length, struct cee_window_paths *p);

/* What an offer carries: which of the types the runtime reads, as the offer's events listed them. */
struct cee_wl_offer {
    cee_wl_proxy *offer;
    int utf8, utf8_string, plain, uris;
};

static void cee_wl_offer_type(void *data, cee_wl_proxy *offer, const char *type) {
    (void)offer;
    struct cee_wl_offer *o = (struct cee_wl_offer *)data;
    if (strcmp(type, "text/plain;charset=utf-8") == 0) {
        o->utf8 = 1;
    } else if (strcmp(type, "UTF8_STRING") == 0) {
        o->utf8_string = 1;
    } else if (strcmp(type, "text/plain") == 0) {
        o->plain = 1;
    } else if (strcmp(type, "text/uri-list") == 0) {
        o->uris = 1;
    }
}
static void cee_wl_offer_actions(void *data, cee_wl_proxy *offer, uint32_t actions) {
    (void)data;
    (void)offer;
    (void)actions;
}
static void cee_wl_offer_action(void *data, cee_wl_proxy *offer, uint32_t action) {
    (void)data;
    (void)offer;
    (void)action;
}
static cee_wl_fn cee_wl_offer_listener[] = {(cee_wl_fn)cee_wl_offer_type, (cee_wl_fn)cee_wl_offer_actions,
                                            (cee_wl_fn)cee_wl_offer_action};

static struct cee_wl_offer *cee_wl_offer_of(cee_wl_proxy *offer) {
    return offer != NULL ? (struct cee_wl_offer *)cee_wl.get_user_data(offer) : NULL;
}

static void cee_wl_offer_drop(struct cee_wl_offer *o) {
    if (o != NULL) {
        cee_wl.marshal_flags(o->offer, 2 /* destroy */, NULL, cee_wl.get_version(o->offer), 1 /* DESTROY */);
        ceesafe_platform_free(o);
    }
}

/* An offer's bytes in `type`, through a pipe its source writes (up to 16 MiB, a second at most); NULL when it gave
   none. The caller frees them. */
static char *cee_wl_receive(struct cee_wl_offer *o, const char *type, size_t *length) {
    *length = 0;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        return NULL;
    }
    cee_wl.marshal_flags(o->offer, 1 /* receive */, NULL, cee_wl.get_version(o->offer), 0, type, fds[1]);
    close(fds[1]);
    cee_wl.flush(cee_wls.display);
    size_t capacity = 0;
    char *text = NULL;
    double deadline = cee_window_now() + 1.0;
    for (;;) {
        double left = deadline - cee_window_now();
        struct pollfd p = {fds[0], POLLIN, 0};
        if (left <= 0.0 || poll(&p, 1, (int)(left * 1000.0) + 1) <= 0) {
            break;
        }
        if (*length + 4096u > capacity) {
            if (capacity >= 16u * 1024u * 1024u) {
                break;
            }
            size_t grown = capacity == 0 ? 8192u : capacity * 2u;
            char *bigger = (char *)ceesafe_platform_realloc(text, grown);
            if (bigger == NULL) {
                break;
            }
            text = bigger;
            capacity = grown;
        }
        ssize_t got = read(fds[0], text + *length, capacity - *length);
        if (got <= 0) {
            break; /* the source's end is closed: all of it */
        }
        *length += (size_t)got;
    }
    close(fds[0]);
    return text;
}

static void cee_wl_data_offer(void *data, cee_wl_proxy *device, cee_wl_proxy *offer) {
    (void)data;
    (void)device;
    struct cee_wl_offer *o = (struct cee_wl_offer *)ceesafe_platform_alloc_zeroed(sizeof *o);
    if (o == NULL) {
        cee_wl.marshal_flags(offer, 2 /* destroy */, NULL, cee_wl.get_version(offer), 1);
        return;
    }
    o->offer = offer;
    cee_wl.add_listener(offer, cee_wl_offer_listener, o);
}

/* The drag over a window: its offer, the window, the files its list names, and where it is. */
static struct {
    struct cee_wl_offer *offer;
    struct cee_window *window;
    uint32_t files, serial;
    float x, y;
} cee_wl_drag;

static void cee_wl_drag_enter(void *data, cee_wl_proxy *device, uint32_t serial, cee_wl_proxy *surface,
                              cee_wl_fixed x, cee_wl_fixed y, cee_wl_proxy *offer) {
    (void)data;
    (void)device;
    cee_wl_offer_drop(cee_wl_drag.offer);
    memset(&cee_wl_drag, 0, sizeof cee_wl_drag);
    struct cee_wl_offer *o = cee_wl_offer_of(offer);
    struct cee_window *w = surface != NULL ? cee_wl_window_of(surface) : NULL;
    if (o == NULL) {
        return;
    }
    cee_wl_drag.offer = o;
    cee_wl_drag.window = w;
    cee_wl_drag.serial = serial;
    cee_wl_drag.x = cee_wl_float(x);
    cee_wl_drag.y = cee_wl_float(y);
    uint32_t version = cee_wl.get_version(offer);
    if (w == NULL || !o->uris) {
        cee_wl.marshal_flags(offer, 0 /* accept */, NULL, version, 0, serial, (const char *)NULL);
        return; /* not files, or not over a window of ours: refused */
    }
    cee_wl.marshal_flags(offer, 0 /* accept */, NULL, version, 0, serial, "text/uri-list");
    if (version >= 3) {
        cee_wl.marshal_flags(offer, 4 /* set_actions */, NULL, version, 0, 1u /* copy */, 1u /* copy */);
    }
    size_t length = 0;
    char *list = cee_wl_receive(o, "text/uri-list", &length);
    cee_wl_drag.files = list != NULL ? cee_x_uri_list(list, length, NULL) : 0;
    ceesafe_platform_free(list);
    cee_window_drag(w, cee_wl_drag.files, cee_wl_drag.x, cee_wl_drag.y);
}
static void cee_wl_drag_leave(void *data, cee_wl_proxy *device) {
    (void)data;
    (void)device;
    if (cee_wl_drag.window != NULL) {
        cee_window_drag(cee_wl_drag.window, 0, 0.0f, 0.0f);
    }
    cee_wl_offer_drop(cee_wl_drag.offer);
    memset(&cee_wl_drag, 0, sizeof cee_wl_drag);
}
static void cee_wl_drag_motion(void *data, cee_wl_proxy *device, uint32_t time, cee_wl_fixed x, cee_wl_fixed y) {
    (void)data;
    (void)device;
    (void)time;
    cee_wl_drag.x = cee_wl_float(x);
    cee_wl_drag.y = cee_wl_float(y);
    if (cee_wl_drag.window != NULL && cee_wl_drag.files > 0) {
        cee_window_drag(cee_wl_drag.window, cee_wl_drag.files, cee_wl_drag.x, cee_wl_drag.y);
    }
}
static void cee_wl_drag_drop(void *data, cee_wl_proxy *device) {
    (void)data;
    (void)device;
    struct cee_wl_offer *o = cee_wl_drag.offer;
    struct cee_window *w = cee_wl_drag.window;
    if (o != NULL && w != NULL && o->uris && cee_wl_drag.files > 0) {
        size_t length = 0;
        char *list = cee_wl_receive(o, "text/uri-list", &length);
        struct cee_window_paths p = {NULL, 0, 0};
        if (list != NULL) {
            cee_x_uri_list(list, length, &p);
        }
        ceesafe_platform_free(list);
        cee_window_dropped(w, cee_wl_drag.x, cee_wl_drag.y, &p);
        if (cee_wl.get_version(o->offer) >= 3) {
            cee_wl.marshal_flags(o->offer, 3 /* finish */, NULL, cee_wl.get_version(o->offer), 0);
        }
    } else if (w != NULL) {
        cee_window_drag(w, 0, 0.0f, 0.0f);
    }
    cee_wl_offer_drop(o);
    memset(&cee_wl_drag, 0, sizeof cee_wl_drag);
}

/* The selection's offer (NULL when the clipboard is empty, or the selection is this process's own source). */
static struct cee_wl_offer *cee_wl_selection;
static void cee_wl_data_selection(void *data, cee_wl_proxy *device, cee_wl_proxy *offer) {
    (void)data;
    (void)device;
    if (cee_wl_selection != NULL && cee_wl_selection != cee_wl_drag.offer) {
        cee_wl_offer_drop(cee_wl_selection);
    }
    cee_wl_selection = cee_wl_offer_of(offer);
}
static cee_wl_fn cee_wl_data_device_listener[] = {
    (cee_wl_fn)cee_wl_data_offer,  (cee_wl_fn)cee_wl_drag_enter, (cee_wl_fn)cee_wl_drag_leave,
    (cee_wl_fn)cee_wl_drag_motion, (cee_wl_fn)cee_wl_drag_drop,  (cee_wl_fn)cee_wl_data_selection,
};

/* The text this process offers as the selection, and its source; NULL once another client's selection replaced it. */
static cee_wl_proxy *cee_wl_source;
static char *cee_wl_owned;
static size_t cee_wl_owned_length;

static void cee_wl_source_forget(void) {
    if (cee_wl_source != NULL) {
        cee_wl.marshal_flags(cee_wl_source, 1 /* destroy */, NULL, cee_wl.get_version(cee_wl_source), 1);
        cee_wl_source = NULL;
    }
    ceesafe_platform_free(cee_wl_owned);
    cee_wl_owned = NULL;
    cee_wl_owned_length = 0;
}

static void cee_wl_source_target(void *data, cee_wl_proxy *source, const char *type) {
    (void)data;
    (void)source;
    (void)type;
}
/* A client pastes: the text, written whole to its pipe. */
static void cee_wl_source_send(void *data, cee_wl_proxy *source, const char *type, int32_t fd) {
    (void)data;
    (void)type;
    if (source == cee_wl_source && cee_wl_owned != NULL) {
        int flags = fcntl(fd, F_GETFL);
        if (flags >= 0) {
            fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        }
        size_t done = 0;
        while (done < cee_wl_owned_length) {
            ssize_t put = write(fd, cee_wl_owned + done, cee_wl_owned_length - done);
            if (put <= 0) {
                break;
            }
            done += (size_t)put;
        }
    }
    close(fd);
}
static void cee_wl_source_cancelled(void *data, cee_wl_proxy *source) {
    (void)data;
    if (source == cee_wl_source) {
        cee_wl_source_forget(); /* another client's selection now */
    } else {
        cee_wl.marshal_flags(source, 1 /* destroy */, NULL, cee_wl.get_version(source), 1);
    }
}
static void cee_wl_source_dnd(void *data, cee_wl_proxy *source) {
    (void)data;
    (void)source;
}
static void cee_wl_source_action(void *data, cee_wl_proxy *source, uint32_t action) {
    (void)data;
    (void)source;
    (void)action;
}
static cee_wl_fn cee_wl_source_listener[] = {
    (cee_wl_fn)cee_wl_source_target, (cee_wl_fn)cee_wl_source_send,   (cee_wl_fn)cee_wl_source_cancelled,
    (cee_wl_fn)cee_wl_source_dnd,    (cee_wl_fn)cee_wl_source_dnd,    (cee_wl_fn)cee_wl_source_action,
};

static int32_t cee_wl_clipboard_set(struct cee_window *w, const char *text, size_t length) {
    (void)w;
    if (cee_wls.data_manager == NULL || cee_wls.data_device == NULL || cee_wls.serial == 0) {
        return -CEE_WINDOW_RESOURCES; /* no data device, or no input yet to vouch for the change */
    }
    char *copy = (char *)ceesafe_platform_alloc(length + 1u);
    if (copy == NULL) {
        return -CEE_WINDOW_RESOURCES;
    }
    if (length > 0) {
        memcpy(copy, text, length);
    }
    copy[length] = '\0';
    uint32_t version = cee_wl.get_version(cee_wls.data_manager);
    cee_wl_proxy *source = cee_wl.marshal_flags(cee_wls.data_manager, 0 /* create_data_source */, cee_wl.data_source,
                                                version, 0, NULL);
    if (source == NULL) {
        ceesafe_platform_free(copy);
        return -CEE_WINDOW_RESOURCES;
    }
    cee_wl.add_listener(source, cee_wl_source_listener, NULL);
    static const char *const types[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING"};
    for (size_t k = 0; k < sizeof types / sizeof *types; k++) {
        cee_wl.marshal_flags(source, 0 /* offer */, NULL, version, 0, types[k]);
    }
    cee_wl_source_forget();
    cee_wl_source = source;
    cee_wl_owned = copy;
    cee_wl_owned_length = length;
    cee_wl.marshal_flags(cee_wls.data_device, 1 /* set_selection */, NULL, cee_wl.get_version(cee_wls.data_device), 0,
                         source, cee_wls.serial);
    cee_wl.flush(cee_wls.display);
    return 0;
}

static int cee_wl_clipboard_get(struct cee_window *w) {
    if (cee_wl_source != NULL && cee_wl_owned != NULL) {
        return cee_window_paste(w, cee_wl_owned, cee_wl_owned_length); /* still ours */
    }
    struct cee_wl_offer *o = cee_wl_selection;
    if (o == NULL || (!o->utf8 && !o->utf8_string && !o->plain)) {
        return 0;
    }
    size_t length = 0;
    char *text = cee_wl_receive(o, o->utf8 ? "text/plain;charset=utf-8" : o->utf8_string ? "UTF8_STRING" : "text/plain",
                                &length);
    int found = text != NULL ? cee_window_paste(w, text, length) : 0;
    ceesafe_platform_free(text);
    return found;
}

/* libdecor's cairo plugin rather than its GTK one (ADR 0205 §2.2, the owner's choice): the GTK one runs GTK 3 in the
   program, which aborts it when it cannot load an icon (Fedora 45's GTK loads them through a program found on PATH).
   libdecor takes every plugin in LIBDECOR_PLUGIN_DIR and chooses among them, so that is a folder of the user's own,
   $XDG_RUNTIME_DIR/ceesafe-libdecor, holding only a link to the cairo plugin beside libdecor's others. Nothing is done
   when the program's environment names a plugin folder already, or there is no cairo plugin. */
static void cee_wl_cairo_plugin(void) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    Dl_info info;
    void *address = NULL; /* the function's address as an object pointer, which ISO C does not convert; POSIX's
                             function and object pointers have one representation */
    memcpy(&address, &cee_wl.decor_new, sizeof address);
    if (getenv("LIBDECOR_PLUGIN_DIR") != NULL || runtime == NULL || runtime[0] == '\0' ||
        dladdr(address, &info) == 0 || info.dli_fname == NULL) {
        return;
    }
    char plugin[512], folder[512], link[600];
    const char *slash = strrchr(info.dli_fname, '/');
    if (slash == NULL) {
        return;
    }
    int at = snprintf(plugin, sizeof plugin, "%.*s/libdecor/plugins-1/libdecor-cairo.so",
                      (int)(slash - info.dli_fname), info.dli_fname);
    if (at <= 0 || (size_t)at >= sizeof plugin || access(plugin, R_OK) != 0) {
        return;
    }
    at = snprintf(folder, sizeof folder, "%s/ceesafe-libdecor", runtime);
    if (at <= 0 || (size_t)at >= sizeof folder || (mkdir(folder, 0700) != 0 && errno != EEXIST)) {
        return;
    }
    snprintf(link, sizeof link, "%s/libdecor-cairo.so", folder);
    char target[512];
    ssize_t length = readlink(link, target, sizeof target - 1u);
    if (length < 0 || (size_t)length != strlen(plugin) || memcmp(target, plugin, (size_t)length) != 0) {
        unlink(link); /* another libdecor's, or none yet */
        if (symlink(plugin, link) != 0 && errno != EEXIST) {
            return;
        }
    }
    setenv("LIBDECOR_PLUGIN_DIR", folder, 1);
}

static void cee_wl_decor_error(void *context, int error, const char *message) {
    (void)context;
    (void)error;
    (void)message; /* libdecor's own failure: the window stays, without its title bar */
}
static struct cee_decor_interface cee_wl_decor_interface = {cee_wl_decor_error, {NULL}};

/* The process's connection, at the first Wayland open: the compositor's globals and one roundtrip for the seat's
   devices. 0 when there is none or it lacks what a window needs (ADR 0205 §2.1). */
static int cee_wl_connect(void) {
    if (cee_wls.state != 0) {
        return cee_wls.state == 1;
    }
    cee_wls.state = -1;
    if ((getenv("WAYLAND_DISPLAY") == NULL && getenv("WAYLAND_SOCKET") == NULL) || !cee_wl_load()) {
        return 0;
    }
    cee_wl_proxy *display = cee_wl.connect(NULL);
    if (display == NULL) {
        return 0;
    }
    cee_wls.display = display;
    cee_wls.registry = cee_wl.marshal_flags(display, 1 /* get_registry */, cee_wl.registry,
                                            cee_wl.get_version(display), 0, NULL);
    cee_wl.add_listener(cee_wls.registry, cee_wl_registry_listener, NULL);
    cee_wl.roundtrip(display); /* the globals */
    cee_wl.roundtrip(display); /* the seat's capabilities, the shm's formats, the keymap */
    if (cee_wls.compositor == NULL || cee_wls.compositor_version < 4 || cee_wls.shm == NULL ||
        cee_wls.wm_base == NULL || cee_wls.seat == NULL) {
        cee_wl.disconnect(display);
        memset(&cee_wls, 0, sizeof cee_wls);
        cee_wls.state = -1;
        return 0;
    }
    if (cee_wls.data_manager != NULL) {
        cee_wls.data_device = cee_wl.marshal_flags(cee_wls.data_manager, 1 /* get_data_device */, cee_wl.data_device,
                                                   cee_wl.get_version(cee_wls.data_manager), 0, NULL, cee_wls.seat);
        cee_wl.add_listener(cee_wls.data_device, cee_wl_data_device_listener, NULL);
    }
    if (cee_wls.text_manager != NULL) {
        cee_wls.text_input = cee_wl.marshal_flags(cee_wls.text_manager, 1 /* get_text_input */,
                                                  &cee_text_input_interface, 1, 0, NULL, cee_wls.seat);
        cee_wl.add_listener(cee_wls.text_input, cee_wl_text_listener, NULL);
    }
#if defined(__FreeBSD__)
    snprintf(cee_wls.app_id, sizeof cee_wls.app_id, "%s", getprogname());
#else
    FILE *comm = fopen("/proc/self/comm", "r");
    if (comm != NULL) {
        if (fgets(cee_wls.app_id, sizeof cee_wls.app_id, comm) != NULL) {
            cee_wls.app_id[strcspn(cee_wls.app_id, "\n")] = '\0';
        }
        fclose(comm);
    }
#endif
    cee_wls.state = 1;
    return 1;
}

/* ---- A window */

static void cee_wl_resize_to(struct cee_window *w, int32_t width, int32_t height) {
    if (width > 0 && height > 0) {
        cee_window_resized(w, (uint32_t)width, (uint32_t)height);
    }
}

static void cee_wl_xdg_configure(void *data, cee_wl_proxy *xdg_surface, uint32_t serial) {
    struct cee_window *w = (struct cee_window *)data;
    struct cee_wl_window *ww = cee_wl_of(w);
    cee_wl.marshal_flags(xdg_surface, 4 /* ack_configure */, NULL, cee_wl.get_version(xdg_surface), 0, serial);
    if (ww != NULL) {
        cee_wl_resize_to(w, ww->pending_width, ww->pending_height);
        ww->configured = 1;
    }
}
static cee_wl_fn cee_wl_xdg_surface_listener[] = {(cee_wl_fn)cee_wl_xdg_configure};

static void cee_wl_toplevel_configure(void *data, cee_wl_proxy *t, int32_t width, int32_t height,
                                      struct cee_wl_array *states) {
    (void)t;
    struct cee_window *w = (struct cee_window *)data;
    struct cee_wl_window *ww = cee_wl_of(w);
    if (ww != NULL) {
        ww->pending_width = width; /* 0: the window's own choice */
        ww->pending_height = height;
    }
    /* xdg_toplevel's states: maximized 1, fullscreen 2, tiled left, right, top, bottom 5 to 8 (ADR 0206). */
    int maximized = 0, tiled = 0;
    const uint32_t *state = states != NULL ? (const uint32_t *)states->data : NULL;
    for (size_t k = 0; state != NULL && k < states->size / sizeof(uint32_t); k++) {
        maximized |= state[k] == 1u;
        tiled |= state[k] == 2u || (state[k] >= 5u && state[k] <= 8u);
    }
    w->tiled = tiled;
    if (maximized != w->maximized) {
        w->maximized = maximized;
        cee_window_push(w, CEE_EVENT_RESIZED, (int32_t)w->width, (int32_t)w->height, 0, 0, 0.0f, 0.0f);
    }
}
static void cee_wl_toplevel_close(void *data, cee_wl_proxy *t) {
    (void)t;
    cee_window_push((struct cee_window *)data, CEE_EVENT_CLOSED, 0, 0, 0, 0, 0.0f, 0.0f);
}
static void cee_wl_toplevel_bounds(void *data, cee_wl_proxy *t, int32_t width, int32_t height) {
    (void)data;
    (void)t;
    (void)width;
    (void)height;
}
static void cee_wl_toplevel_capabilities(void *data, cee_wl_proxy *t, struct cee_wl_array *caps) {
    (void)data;
    (void)t;
    (void)caps;
}
static cee_wl_fn cee_wl_toplevel_listener[] = {(cee_wl_fn)cee_wl_toplevel_configure, (cee_wl_fn)cee_wl_toplevel_close,
                                               (cee_wl_fn)cee_wl_toplevel_bounds,
                                               (cee_wl_fn)cee_wl_toplevel_capabilities};

static void cee_wl_decoration_configure(void *data, cee_wl_proxy *d, uint32_t mode) {
    (void)d;
    struct cee_window *w = (struct cee_window *)data;
    /* The compositor's choice: a program's bar only where it draws none (client-side, 1); asked for server-side, a
       window it leaves bare draws nothing (no libdecor here). */
    w->program_bar = w->wants_bar && mode == 1u;
}
static cee_wl_fn cee_wl_decoration_listener[] = {(cee_wl_fn)cee_wl_decoration_configure};

static void cee_wl_preferred_scale(void *data, cee_wl_proxy *f, uint32_t scale120) {
    (void)f;
    if (scale120 > 0) {
        cee_window_rescaled((struct cee_window *)data, (float)scale120 / 120.0f);
    }
}
static cee_wl_fn cee_wl_fractional_listener[] = {(cee_wl_fn)cee_wl_preferred_scale};

static void cee_wl_surface_enter(void *data, cee_wl_proxy *s, cee_wl_proxy *output) {
    (void)data;
    (void)s;
    (void)output;
}
static void cee_wl_surface_leave(void *data, cee_wl_proxy *s, cee_wl_proxy *output) {
    (void)data;
    (void)s;
    (void)output;
}
static void cee_wl_surface_buffer_scale(void *data, cee_wl_proxy *s, int32_t factor) {
    (void)s;
    if (factor > 0 && cee_wls.fractional_manager == NULL) { /* the fraction, when there is one, says it better */
        cee_window_rescaled((struct cee_window *)data, (float)factor);
    }
}
static void cee_wl_surface_buffer_transform(void *data, cee_wl_proxy *s, uint32_t transform) {
    (void)data;
    (void)s;
    (void)transform;
}
static cee_wl_fn cee_wl_surface_listener[] = {(cee_wl_fn)cee_wl_surface_enter, (cee_wl_fn)cee_wl_surface_leave,
                                              (cee_wl_fn)cee_wl_surface_buffer_scale,
                                              (cee_wl_fn)cee_wl_surface_buffer_transform};

/* libdecor's frame: its configure is the xdg one, with the title bar's room taken off. */
static void cee_wl_frame_configure(void *frame, void *configuration, void *data) {
    struct cee_window *w = (struct cee_window *)data;
    struct cee_wl_window *ww = cee_wl_of(w);
    int width = 0, height = 0;
    if (!cee_wl.decor_configuration_get_content_size(configuration, frame, &width, &height) || width <= 0 ||
        height <= 0) {
        width = (int)w->width;
        height = (int)w->height;
    }
    void *state = cee_wl.decor_state_new(width, height);
    cee_wl.decor_frame_commit(frame, state, configuration);
    cee_wl.decor_state_free(state);
    cee_wl_resize_to(w, width, height);
    if (ww != NULL) {
        ww->configured = 1;
    }
}
static void cee_wl_frame_close(void *frame, void *data) {
    (void)frame;
    cee_window_push((struct cee_window *)data, CEE_EVENT_CLOSED, 0, 0, 0, 0, 0.0f, 0.0f);
}
static void cee_wl_frame_commit(void *frame, void *data) {
    (void)frame;
    struct cee_wl_window *ww = cee_wl_of((struct cee_window *)data);
    if (ww != NULL) {
        cee_wl.marshal_flags(ww->surface, 6 /* commit */, NULL, cee_wl.get_version(ww->surface), 0);
    }
}
static void cee_wl_frame_dismiss(void *frame, const char *seat, void *data) {
    (void)frame;
    (void)seat;
    (void)data;
}
static struct cee_decor_frame_interface cee_wl_frame_interface = {cee_wl_frame_configure, cee_wl_frame_close,
                                                                  cee_wl_frame_commit, cee_wl_frame_dismiss, {NULL}};

/* What the connection has read, given to the listeners (libdecor's dispatch, which also serves its plugin, when it
   draws the title bars). */
static void cee_wl_dispatch(void) {
    if (cee_wls.decor != NULL) {
        cee_wl.decor_dispatch(cee_wls.decor, 0);
        return;
    }
    while (cee_wl.prepare_read(cee_wls.display) != 0) {
        cee_wl.dispatch_pending(cee_wls.display);
    }
    cee_wl.flush(cee_wls.display);
    struct pollfd p = {cee_wl.get_fd(cee_wls.display), POLLIN, 0};
    if (poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
        cee_wl.read_events(cee_wls.display);
    } else {
        cee_wl.cancel_read(cee_wls.display);
    }
    cee_wl.dispatch_pending(cee_wls.display);
}

/* Until the compositor has events, one of `fds` is readable, or `seconds` pass; a held key's next repeat ends it too. */
static void cee_wl_wait(struct pollfd *fds, nfds_t count, double seconds) {
    if (cee_wls.repeat_key != 0 && cee_wls.keyboard_window != NULL) {
        double left = cee_wls.repeat_at - cee_window_now();
        seconds = left < seconds ? (left > 0.0 ? left : 0.0) : seconds;
    }
    int reading = 0;
    if (cee_wls.decor == NULL) {
        if (cee_wl.prepare_read(cee_wls.display) != 0) {
            return; /* events already read: the pump gives them */
        }
        reading = 1;
    }
    cee_wl.flush(cee_wls.display);
    fds[0].fd = cee_wl.get_fd(cee_wls.display);
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    double ms = seconds * 1000.0 + 0.999;
    int ready = poll(fds, count, ms <= 0.0 ? 0 : ms >= 2147483647.0 ? 2147483647 : (int)ms);
    if (reading) {
        if (ready > 0 && (fds[0].revents & POLLIN)) {
            cee_wl.read_events(cee_wls.display);
        } else {
            cee_wl.cancel_read(cee_wls.display);
        }
    }
}

static int cee_wl_open(struct cee_window *w, const char *title) {
    if (!cee_wl_connect()) {
        return CEE_WINDOW_NO_DISPLAY;
    }
    struct cee_x11 *n = (struct cee_x11 *)ceesafe_platform_alloc_zeroed(sizeof *n);
    struct cee_wl_window *ww = (struct cee_wl_window *)ceesafe_platform_alloc_zeroed(sizeof *ww);
    if (n == NULL || ww == NULL) {
        ceesafe_platform_free(n);
        ceesafe_platform_free(ww);
        return CEE_WINDOW_RESOURCES;
    }
    n->wl = ww;
    w->native = n;
    w->scale = 1.0f;
    /* The compositor says when the window has the keyboard (wl_keyboard.enter), which a new one need not: Focus(true)
       comes then, not before. */
    w->focused = 0;
    ww->surface = cee_wl.marshal_flags(cee_wls.compositor, 0 /* create_surface */, cee_wl.surface,
                                       cee_wl.get_version(cee_wls.compositor), 0, NULL);
    if (ww->surface == NULL) {
        w->native = NULL;
        ceesafe_platform_free(n);
        ceesafe_platform_free(ww);
        return CEE_WINDOW_RESOURCES;
    }
    cee_wl.add_listener(ww->surface, cee_wl_surface_listener, w);
    if (cee_wls.fractional_manager != NULL) {
        ww->fractional = cee_wl.marshal_flags(cee_wls.fractional_manager, 1 /* get_fractional_scale */,
                                              &cee_fractional_interface, 1, 0, NULL, ww->surface);
        cee_wl.add_listener(ww->fractional, cee_wl_fractional_listener, w);
    }
    if (cee_wls.viewporter != NULL) {
        ww->viewport = cee_wl.marshal_flags(cee_wls.viewporter, 1 /* get_viewport */, &cee_viewport_interface, 1, 0,
                                            NULL, ww->surface);
    }
    /* A program's own title bar (ADR 0206): no libdecor frame; a compositor that draws decorations is asked for none. */
    w->program_bar = w->wants_bar;
    if (cee_wls.decor == NULL && !w->wants_bar && cee_wls.decoration_manager == NULL && cee_wl_load_decor()) {
        cee_wl_cairo_plugin();
        /* Should libdecor still take its GTK plugin, GTK's accessibility bridge would make this process a second
           application for readers beside the runtime's own (ADR 0164 §11). */
        setenv("NO_AT_BRIDGE", "1", 0);
        cee_wls.decor = cee_wl.decor_new(cee_wls.display, &cee_wl_decor_interface);
    }
    if (cee_wls.decor != NULL && !w->wants_bar) {
        ww->frame = cee_wl.decor_decorate(cee_wls.decor, ww->surface, &cee_wl_frame_interface, w);
    }
    if (ww->frame != NULL) {
        cee_wl.decor_frame_set_title(ww->frame, title);
        cee_wl.decor_frame_set_app_id(ww->frame, cee_wls.app_id);
        cee_wl.decor_frame_map(ww->frame);
    } else {
        uint32_t base = cee_wl.get_version(cee_wls.wm_base);
        ww->xdg_surface = cee_wl.marshal_flags(cee_wls.wm_base, 2 /* get_xdg_surface */, &cee_xdg_surface_interface,
                                               base, 0, NULL, ww->surface);
        cee_wl.add_listener(ww->xdg_surface, cee_wl_xdg_surface_listener, w);
        ww->toplevel = cee_wl.marshal_flags(ww->xdg_surface, 1 /* get_toplevel */, &cee_xdg_toplevel_interface, base,
                                            0, NULL);
        cee_wl.add_listener(ww->toplevel, cee_wl_toplevel_listener, w);
        cee_wl.marshal_flags(ww->toplevel, 2 /* set_title */, NULL, base, 0, title);
        cee_wl.marshal_flags(ww->toplevel, 3 /* set_app_id */, NULL, base, 0, cee_wls.app_id);
        if (cee_wls.decoration_manager != NULL) {
            ww->decoration = cee_wl.marshal_flags(cee_wls.decoration_manager, 1 /* get_toplevel_decoration */,
                                                  &cee_decoration_interface, 1, 0, NULL, ww->toplevel);
            cee_wl.add_listener(ww->decoration, cee_wl_decoration_listener, w);
            cee_wl.marshal_flags(ww->decoration, 1 /* set_mode */, NULL, 1, 0,
                                 w->wants_bar ? 1u /* client-side */ : 2u /* server-side */);
        }
        cee_wl.marshal_flags(ww->surface, 6 /* commit */, NULL, cee_wl.get_version(ww->surface), 0);
    }
    /* Until the first configure, which says the size: a window is shown only once it has one. */
    double deadline = cee_window_now() + 2.0;
    while (!ww->configured && cee_window_now() < deadline) {
        cee_wl.roundtrip(cee_wls.display);
        if (cee_wls.decor != NULL) {
            cee_wl.decor_dispatch(cee_wls.decor, 0);
        }
    }
    /* The launcher's activation token (a terminal's, the desktop's): with it the compositor gives the new window the
       keyboard, as it would not a window that just appeared. It is the first window's; the variable is consumed, as
       the protocol asks, so the program's children do not use it again. */
    const char *token = getenv("XDG_ACTIVATION_TOKEN");
    if (token != NULL && token[0] != '\0' && cee_wls.activation != NULL) {
        cee_wl.marshal_flags(cee_wls.activation, 2 /* activate */, NULL, 1, 0, token, ww->surface);
        cee_wl.flush(cee_wls.display);
    }
    unsetenv("XDG_ACTIVATION_TOKEN");
    size_t title_length = strlen(title);
    n->title = (char *)ceesafe_platform_alloc(title_length + 1u);
    if (n->title != NULL) {
        memcpy(n->title, title, title_length + 1u);
    }
    n->atspi_serial = ++cee_atspi.next_serial;
    cee_atspi_connect(); /* AT-SPI is D-Bus, whatever shows the window (ADR 0164 §11) */
    cee_atspi_frame_added(w);
    return 0;
}

static void cee_wl_buffer_release(void *data, cee_wl_proxy *buffer) {
    (void)buffer;
    ((struct cee_wl_buffer *)data)->busy = 0;
}
static cee_wl_fn cee_wl_buffer_listener[] = {(cee_wl_fn)cee_wl_buffer_release};

static void cee_wl_buffer_free(struct cee_wl_buffer *b) {
    if (b->buffer != NULL) {
        cee_wl.marshal_flags(b->buffer, 0 /* destroy */, NULL, cee_wl.get_version(b->buffer), 1 /* DESTROY */);
    }
    if (b->data != NULL) {
        munmap(b->data, b->size);
    }
    memset(b, 0, sizeof *b);
}

/* A shared-memory buffer of width x height, its memory a memfd (shm_open's anonymous one on FreeBSD). */
static int cee_wl_buffer_make(struct cee_wl_buffer *b, uint32_t width, uint32_t height) {
    size_t stride = (size_t)width * 4u, size = stride * (size_t)height;
    if (size == 0 || size > 0x7fffffffu) {
        return 0;
    }
#if defined(__FreeBSD__)
    int fd = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
#else
    int fd = memfd_create("ceesafe-frame", MFD_CLOEXEC);
#endif
    if (fd < 0) {
        return 0;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return 0;
    }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return 0;
    }
    cee_wl_proxy *pool = cee_wl.marshal_flags(cee_wls.shm, 0 /* create_pool */, cee_wl.shm_pool,
                                              cee_wl.get_version(cee_wls.shm), 0, NULL, fd, (int32_t)size);
    close(fd);
    if (pool == NULL) {
        munmap(data, size);
        return 0;
    }
    uint32_t format = cee_wls.xbgr ? 0x34324258u /* XBGR8888 */ : 1u /* XRGB8888 */;
    b->buffer = cee_wl.marshal_flags(pool, 0 /* create_buffer */, cee_wl.buffer, cee_wl.get_version(pool), 0, NULL, 0,
                                     (int32_t)width, (int32_t)height, (int32_t)stride, format);
    cee_wl.marshal_flags(pool, 1 /* destroy */, NULL, cee_wl.get_version(pool), 1 /* DESTROY */);
    if (b->buffer == NULL) {
        munmap(data, size);
        return 0;
    }
    b->data = (uint32_t *)data;
    b->size = size;
    b->width = width;
    b->height = height;
    b->busy = 0;
    cee_wl.add_listener(b->buffer, cee_wl_buffer_listener, b);
    return 1;
}

/* A buffer of that size the compositor is not reading: a free one of the size, a free one remade, or, all three being
   read, the connection read until one is released (a quarter of a second at most). */
static struct cee_wl_buffer *cee_wl_buffer_for(struct cee_wl_window *ww, uint32_t width, uint32_t height) {
    double deadline = cee_window_now() + 0.25;
    for (;;) {
        for (int k = 0; k < 3; k++) {
            struct cee_wl_buffer *b = &ww->buffers[k];
            if (!b->busy && b->buffer != NULL && b->width == width && b->height == height) {
                return b;
            }
        }
        for (int k = 0; k < 3; k++) {
            struct cee_wl_buffer *b = &ww->buffers[k];
            if (!b->busy) {
                cee_wl_buffer_free(b);
                return cee_wl_buffer_make(b, width, height) ? b : NULL;
            }
        }
        if (cee_window_now() >= deadline) {
            return NULL;
        }
        struct pollfd p[1];
        cee_wl_wait(p, 1, 0.01);
        if (cee_wls.decor != NULL) {
            cee_wl.decor_dispatch(cee_wls.decor, 0);
        } else {
            cee_wl.dispatch_pending(cee_wls.display);
        }
    }
}

/* A frame of width x height pixels shown over the window's size in points: the viewport maps it (any scale), or an
   integral buffer scale does; `boxes` (NULL for all) are what changed. */
static int32_t cee_wl_present(struct cee_window *w, const uint32_t *pixels, uint32_t width, uint32_t height,
                              const struct cee_window_box *boxes, size_t count) {
    struct cee_wl_window *ww = cee_wl_of(w);
    struct cee_wl_buffer *b = cee_wl_buffer_for(ww, width, height);
    if (b == NULL) {
        return -CEE_WINDOW_RESOURCES;
    }
    size_t total = (size_t)width * (size_t)height;
    if (cee_wls.xbgr) {
        memcpy(b->data, pixels, total * sizeof(uint32_t));
    } else {
        for (size_t k = 0; k < total; k++) { /* R and B change places: XRGB8888 is B, G, R, X in memory */
            uint32_t p = pixels[k];
            b->data[k] = (p & 0xff00ff00u) | ((p & 0xffu) << 16) | ((p >> 16) & 0xffu);
        }
    }
    uint32_t sv = cee_wl.get_version(ww->surface);
    cee_wl.marshal_flags(ww->surface, 1 /* attach */, NULL, sv, 0, b->buffer, 0, 0);
    if (ww->viewport != NULL) {
        cee_wl.marshal_flags(ww->viewport, 2 /* set_destination */, NULL, 1, 0, (int32_t)w->width,
                             (int32_t)w->height);
    } else if (w->width > 0 && width % w->width == 0 && width / w->width >= 1) {
        cee_wl.marshal_flags(ww->surface, 8 /* set_buffer_scale */, NULL, sv, 0, (int32_t)(width / w->width));
    }
    if (boxes == NULL) {
        cee_wl.marshal_flags(ww->surface, 9 /* damage_buffer */, NULL, sv, 0, 0, 0, (int32_t)width, (int32_t)height);
    } else {
        for (size_t k = 0; k < count; k++) {
            cee_wl.marshal_flags(ww->surface, 9 /* damage_buffer */, NULL, sv, 0, (int32_t)boxes[k].x,
                                 (int32_t)boxes[k].y, (int32_t)boxes[k].w, (int32_t)boxes[k].h);
        }
    }
    cee_wl.marshal_flags(ww->surface, 6 /* commit */, NULL, sv, 0);
    b->busy = 1;
    cee_wl.flush(cee_wls.display);
    return 0;
}

static void cee_wl_close(struct cee_window *w) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    struct cee_wl_window *ww = cee_wl_of(w);
    if (n == NULL || ww == NULL) {
        return;
    }
    cee_atspi_frame_removed(w);
    if (cee_wls.pointer_window == w) {
        cee_wls.pointer_window = NULL;
    }
    if (cee_wls.keyboard_window == w) {
        cee_wls.keyboard_window = NULL;
        cee_wls.repeat_key = 0;
    }
    if (cee_wl_touch_window == w) {
        cee_wl_touch_window = NULL;
    }
    if (cee_wl_drag.window == w) {
        cee_wl_drag.window = NULL;
    }
    if (cee_wl_ti.window == w) {
        cee_wl_ti.window = NULL;
        cee_wl_ti.enabled = 0;
    }
    for (int k = 0; k < 3; k++) {
        cee_wl_buffer_free(&ww->buffers[k]);
    }
    if (ww->frame != NULL) {
        cee_wl.decor_frame_unref(ww->frame);
    }
    if (ww->decoration != NULL) {
        cee_wl.marshal_flags(ww->decoration, 0, NULL, 1, 1);
    }
    if (ww->toplevel != NULL) {
        cee_wl.marshal_flags(ww->toplevel, 0, NULL, cee_wl.get_version(ww->toplevel), 1);
    }
    if (ww->xdg_surface != NULL) {
        cee_wl.marshal_flags(ww->xdg_surface, 0, NULL, cee_wl.get_version(ww->xdg_surface), 1);
    }
    if (ww->viewport != NULL) {
        cee_wl.marshal_flags(ww->viewport, 0, NULL, 1, 1);
    }
    if (ww->fractional != NULL) {
        cee_wl.marshal_flags(ww->fractional, 0, NULL, 1, 1);
    }
    cee_wl.marshal_flags(ww->surface, 0, NULL, cee_wl.get_version(ww->surface), 1);
    cee_wl.flush(cee_wls.display);
    ceesafe_platform_free(n->title);
    ceesafe_platform_free(ww);
    ceesafe_platform_free(n);
    w->native = NULL;
}

static void cee_wl_title(struct cee_window *w, const char *title) {
    struct cee_wl_window *ww = cee_wl_of(w);
    if (ww->frame != NULL) {
        cee_wl.decor_frame_set_title(ww->frame, title);
    } else {
        cee_wl.marshal_flags(ww->toplevel, 2 /* set_title */, NULL, cee_wl.get_version(ww->toplevel), 0, title);
    }
    cee_wl.flush(cee_wls.display);
}

static void cee_wl_pump(struct cee_window *w) {
    (void)w;
    cee_atspi_dispatch();
    cee_wl_dispatch();
    cee_wl_repeat();
    cee_wl.flush(cee_wls.display);
}

/* ADR 0206's actions: xdg_toplevel's set_minimized, and set_maximized or unset_maximized (a libdecor frame has none
   of these here: a window with libdecor's bar has the system's buttons). */
static void cee_wl_minimize(struct cee_window *w) {
    struct cee_wl_window *ww = cee_wl_of(w);
    if (ww != NULL && ww->toplevel != NULL) {
        cee_wl.marshal_flags(ww->toplevel, 13 /* set_minimized */, NULL, cee_wl.get_version(ww->toplevel), 0);
        cee_wl.flush(cee_wls.display);
    }
}
static void cee_wl_toggle_maximized(struct cee_window *w) {
    struct cee_wl_window *ww = cee_wl_of(w);
    if (ww != NULL && ww->toplevel != NULL) {
        cee_wl.marshal_flags(ww->toplevel, w->maximized ? 10 /* unset_maximized */ : 9 /* set_maximized */, NULL,
                             cee_wl.get_version(ww->toplevel), 0);
        cee_wl.flush(cee_wls.display);
    }
}

/* ---- A title bar the program draws, on X11 (ADR 0206 step 3): the window manager draws no frame (_MOTIF_WM_HINTS,
   decorations none) and does what only it can when the runtime asks it, as EWMH says: moving and resizing by the
   pointer (_NET_WM_MOVERESIZE), the window menu (_GTK_SHOW_WINDOW_MENU, GNOME's and KDE's), maximizing
   (_NET_WM_STATE), minimizing (WM_CHANGE_STATE). Only with a window manager that moves windows so: without one, or
   with one that does not say it does (_NET_SUPPORTED), the window keeps the system's frame. */
static int cee_x_supports(struct cee_x11 *n, unsigned long atom) {
    unsigned long supported = cee_x.intern_atom(n->display, "_NET_SUPPORTED", 0), type = 0, count = 0, after = 0;
    int format = 0;
    unsigned char *data = NULL;
    int found = 0;
    if (cee_x.get_window_property(n->display, cee_x.root_window(n->display, n->screen), supported, 0, 4096, 0,
                                  4 /* XA_ATOM */, &type, &format, &count, &after, &data) == 0 &&
        data != NULL) {
        const unsigned long *atoms = (const unsigned long *)data; /* format 32 is longs */
        for (unsigned long k = 0; k < count && format == 32; k++) {
            found |= atoms[k] == atom;
        }
    }
    if (data != NULL) {
        cee_x.release(data);
    }
    return found;
}

static void cee_x_program_bar(struct cee_window *w, struct cee_x11 *n) {
    n->wm_state = cee_x.intern_atom(n->display, "_NET_WM_STATE", 0);
    n->wm_max_vert = cee_x.intern_atom(n->display, "_NET_WM_STATE_MAXIMIZED_VERT", 0);
    n->wm_max_horz = cee_x.intern_atom(n->display, "_NET_WM_STATE_MAXIMIZED_HORZ", 0);
    n->wm_moveresize = cee_x.intern_atom(n->display, "_NET_WM_MOVERESIZE", 0);
    n->wm_menu = cee_x.intern_atom(n->display, "_GTK_SHOW_WINDOW_MENU", 0);
    n->wm_change_state = cee_x.intern_atom(n->display, "WM_CHANGE_STATE", 0);
    w->program_bar = w->wants_bar && cee_x_supports(n, n->wm_moveresize);
    if (w->program_bar) {
        unsigned long hints[5] = {2ul /* decorations */, 0ul, 0ul /* none */, 0ul, 0ul};
        unsigned long motif = cee_x.intern_atom(n->display, "_MOTIF_WM_HINTS", 0);
        cee_x.change_property(n->display, n->window, motif, motif, 32, 0 /* PropModeReplace */,
                              (const unsigned char *)hints, 5);
    }
}

/* A client message about this window to the root window, as EWMH's requests are sent. */
static void cee_x_ask_wm(struct cee_x11 *n, unsigned long type, long a, long b, long c, long d, long e) {
    cee_x_event message;
    memset(&message, 0, sizeof message);
    message.client.type = 33; /* ClientMessage */
    message.client.window = n->window;
    message.client.message_type = type;
    message.client.format = 32;
    message.client.data[0] = a;
    message.client.data[1] = b;
    message.client.data[2] = c;
    message.client.data[3] = d;
    message.client.data[4] = e;
    cee_x.send_event(n->display, cee_x.root_window(n->display, n->screen), 0,
                     (1L << 19) | (1L << 20) /* SubstructureNotify | SubstructureRedirect */, &message);
    cee_x.flush(n->display);
}

static void cee_x_toggle_maximized(struct cee_window *w) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    cee_x_ask_wm(n, n->wm_state, 2 /* toggle */, (long)n->wm_max_vert, (long)n->wm_max_horz, 1 /* an application */, 0);
}

static void cee_x_minimize(struct cee_window *w) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    cee_x_ask_wm(n, n->wm_change_state, 3 /* IconicState */, 0, 0, 0, 0);
}

/* Whether the window is maximized, from its _NET_WM_STATE, kept for every window; a change is a Resized. */
static void cee_x_read_state(struct cee_window *w) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    unsigned long type = 0, count = 0, after = 0;
    int format = 0, vertical = 0, horizontal = 0;
    unsigned char *data = NULL;
    if (cee_x.get_window_property(n->display, n->window, n->wm_state, 0, 64, 0, 4 /* XA_ATOM */, &type, &format,
                                  &count, &after, &data) == 0 &&
        data != NULL) {
        const unsigned long *atoms = (const unsigned long *)data;
        for (unsigned long k = 0; k < count && format == 32; k++) {
            vertical |= atoms[k] == n->wm_max_vert;
            horizontal |= atoms[k] == n->wm_max_horz;
        }
    }
    if (data != NULL) {
        cee_x.release(data);
    }
    int maximized = vertical && horizontal;
    if (maximized != w->maximized) {
        w->maximized = maximized;
        cee_window_push(w, CEE_EVENT_RESIZED, (int32_t)w->width, (int32_t)w->height, 0, 0, 0.0f, 0.0f);
    }
}

/* _NET_WM_MOVERESIZE's direction for an edge (cee_wl_edge's bits): 0 top left, then clockwise to 7 left. */
static long cee_x_direction(uint32_t edge) {
    switch (edge) {
    case 5: return 0;
    case 1: return 1;
    case 9: return 2;
    case 8: return 3;
    case 10: return 4;
    case 2: return 5;
    case 6: return 6;
    default: return 7;
    }
}

/* A press the runtime takes for a window with the program's title bar (ADR 0206), as on Wayland: at an edge the
   primary button resizes; in the bar but in none of its boxes, the primary button moves the window and a second
   press within 0.4 s toggles maximized, and the secondary shows the window menu. The press's implicit grab is let go
   so that the window manager can take the pointer. 1 when taken: the program sees no press then. */
static int cee_x_bar_press(struct cee_window *w, const cee_x_input *press) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    float x = (float)press->x, y = (float)press->y;
    if (!w->program_bar || (press->code != 1 && press->code != 3)) {
        return 0;
    }
    uint32_t edge = cee_wl_edge(w, x, y);
    long direction = -1;
    if (edge != 0) {
        if (press->code != 1) {
            return 0;
        }
        direction = cee_x_direction(edge);
    } else {
        if (y >= w->bar[0]) {
            return 0;
        }
        for (int k = 0; k < 4; k++) {
            const float *b = &w->bar[1 + 4 * k];
            if (b[2] > 0.0f && b[3] > 0.0f && x >= b[0] && y >= b[1] && x < b[0] + b[2] && y < b[1] + b[3]) {
                return 0;
            }
        }
        if (press->code == 3) {
            cee_x.ungrab_pointer(n->display, press->time);
            cee_x_ask_wm(n, n->wm_menu, 0 /* the core pointer */, press->x_root, press->y_root, 0, 0);
            return 1;
        }
        double now = cee_window_now();
        if (now - n->bar_pressed_at < 0.4) {
            n->bar_pressed_at = 0.0;
            cee_x_toggle_maximized(w);
            return 1;
        }
        n->bar_pressed_at = now;
        direction = 8; /* _NET_WM_MOVERESIZE_MOVE */
    }
    cee_x.ungrab_pointer(n->display, press->time);
    cee_x_ask_wm(n, n->wm_moveresize, press->x_root, press->y_root, direction, 1 /* the button */,
                 1 /* an application */);
    return 1;
}

/* The pointer's shape: a resize shape at an edge of a window with the program's title bar, else the program's. */
static void cee_x_show_cursor(struct cee_window *w) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    uint32_t edge = cee_wl_edge(w, n->pointer_x, n->pointer_y);
    n->edge_shown = edge;
    if (edge != 0) {
        /* XC_top_left_corner, top_side, top_right_corner, right_side, bottom_right_corner, bottom_side,
           bottom_left_corner, left_side, by cee_x_direction's order. */
        static const unsigned int shapes[] = {134, 138, 136, 96, 14, 16, 12, 70};
        long k = cee_x_direction(edge);
        if (n->edge_cursors[k] == 0) {
            n->edge_cursors[k] = cee_x.create_font_cursor(n->display, shapes[k]);
        }
        cee_x.define_cursor(n->display, n->window, n->edge_cursors[k]);
        cee_x.flush(n->display);
        return;
    }
    static const unsigned int shapes[] = {68, 60, 152, 34, 52, 108, 116}; /* XC_left_ptr, hand2, xterm, ... */
    int32_t k = w->cursor >= 0 && w->cursor < 7 ? w->cursor : 0;
    if (n->cursors[k] == 0) {
        n->cursors[k] = cee_x.create_font_cursor(n->display, shapes[k]);
    }
    cee_x.define_cursor(n->display, n->window, n->cursors[k]);
    cee_x.flush(n->display);
}

/* Whether this process's windows are Wayland's, which the first open decides (ADR 0205 §2.1): CEESAFE_WINDOW_SYSTEM
   when it says x11 or wayland; else Wayland when there is a compositor the connection takes (step 2 made it the
   default), and X11 when not. */
static int cee_wl_chosen = -1;
static int cee_wl_on(void) {
    if (cee_wl_chosen < 0) {
        const char *system = getenv("CEESAFE_WINDOW_SYSTEM");
        if (system != NULL && strcmp(system, "wayland") == 0) {
            cee_wl_chosen = 1; /* asked for: without a compositor, open is NoDisplay */
        } else if (system != NULL && strcmp(system, "x11") == 0) {
            cee_wl_chosen = 0;
        } else {
            cee_wl_chosen = cee_wl_connect();
        }
    }
    return cee_wl_chosen;
}

static int cee_native_open(struct cee_window *w, const char *title) {
    if (cee_wl_on()) {
        return cee_wl_open(w, title);
    }
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
    cee_x_title(display, n->window, n->utf8_atom, title);
    cee_x_program_bar(w, n);
    cee_x.map_window(display, n->window);
    cee_x.flush(display);
    size_t title_length = strlen(title);
    n->title = (char *)ceesafe_platform_alloc(title_length + 1u);
    if (n->title != NULL) {
        memcpy(n->title, title, title_length + 1u);
    }
    n->atspi_serial = ++cee_atspi.next_serial;
    w->native = n;
    w->scale = 1.0f;
    cee_atspi_connect(); /* the first open's: the accessibility bus, if there is one (ADR 0164) */
    cee_atspi_frame_added(w);
    return 0;
}

static int32_t cee_native_handles(struct cee_window *w, uint64_t *first, uint64_t *second) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    if (n->wl != NULL) {
        *first = (uint64_t)(uintptr_t)cee_wls.display;
        *second = (uint64_t)(uintptr_t)((struct cee_wl_window *)n->wl)->surface;
        return CEE_HANDLE_WAYLAND;
    }
    *first = (uint64_t)(uintptr_t)n->display;
    *second = (uint64_t)n->window;
    return CEE_HANDLE_XLIB;
}

static void cee_native_close(struct cee_window *w) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    if (n == NULL) {
        return;
    }
    if (n->wl != NULL) {
        cee_wl_close(w);
        return;
    }
    cee_atspi_frame_removed(w);
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
    ceesafe_platform_free(n->title);
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
    if (n->wl != NULL) {
        cee_wl_pump(w);
        return;
    }
    cee_atspi_dispatch(); /* what readers asked (ADR 0164) */
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
                if (e.type == 4 && cee_x_bar_press(w, &e.input)) {
                    break; /* the runtime's (ADR 0206) */
                }
                int32_t button = b == 1 ? 0 : b == 3 ? 1 : 2;
                cee_window_push(w, CEE_EVENT_BUTTON, button, e.type == 4, 0, 0, x, y);
            }
            break;
        }
        case 6: /* MotionNotify */
            n->pointer_x = (float)e.input.x;
            n->pointer_y = (float)e.input.y;
            if (w->program_bar && cee_wl_edge(w, n->pointer_x, n->pointer_y) != n->edge_shown) {
                cee_x_show_cursor(w); /* an edge's resize shape, or the program's again (ADR 0206) */
            }
            cee_window_push(w, CEE_EVENT_MOVED, 0, 0, 0, 0, (float)e.input.x, (float)e.input.y);
            break;
        case 28: /* PropertyNotify: the window manager's state of the window (ADR 0206) */
            if (e.property.atom == n->wm_state) {
                cee_x_read_state(w);
            }
            break;
        case 9: /* FocusIn */
            cee_window_focus(w, 1);
            cee_atspi_frame_focus(w, 1);
            if (n->ic != NULL && w->text_input == 1) {
                cee_x.set_ic_focus(n->ic);
            }
            break;
        case 10: /* FocusOut */
            cee_window_focus(w, 0);
            cee_atspi_frame_focus(w, 0);
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
    if (n->wl != NULL) {
        return cee_wl_present(w, pixels, width, height, NULL, 0);
    }
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
    if (n->wl != NULL) {
        return cee_wl_present(w, pixels, width, height, boxes, count);
    }
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
    if (n->wl != NULL) {
        cee_wl_set_cursor(w);
        cee_wl.flush(cee_wls.display);
        return;
    }
    cee_x_show_cursor(w);
}

static void cee_native_title(struct cee_window *w, const char *title) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    if (n->wl != NULL) {
        cee_wl_title(w, title);
    } else {
        cee_x_title(n->display, n->window, n->utf8_atom, title);
        cee_x.flush(n->display);
    }
    size_t length = strlen(title);
    char *copy = (char *)ceesafe_platform_alloc(length + 1u);
    if (copy != NULL) {
        memcpy(copy, title, length + 1u);
        ceesafe_platform_free(n->title);
        n->title = copy;
        cee_atspi_signal(w, -1, "org.a11y.atspi.Event.Object", "PropertyChange", "accessible-name", 0, 0, "s",
                         cee_atspi_any_string, copy);
    }
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
    if (n->wl != NULL) {
        (void)on; /* w->text_input says it */
        cee_wl_text_update();
        return;
    }
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
    if (n->wl != NULL) {
        if (cee_wl_ti.window == w) {
            cee_wl_text_update();
        }
        return;
    }
    cee_x_spot(w);
    cee_x.flush(n->display);
}

/* The CLIPBOARD selection, as UTF8_STRING (ADR 0141): this window owns it and answers in the pump. */
static int32_t cee_native_clipboard_set(struct cee_window *w, const char *text, size_t length) {
    struct cee_x11 *n = (struct cee_x11 *)w->native;
    if (n->wl != NULL) {
        return cee_wl_clipboard_set(w, text, length);
    }
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
    if (n->wl != NULL) {
        return cee_wl_clipboard_get(w);
    }
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
    if (n->wl != NULL) {
        int bus = cee_atspi_fd();
        struct pollfd p[3] = {{-1, POLLIN, 0}, {w->wake_read, POLLIN, 0}, {bus, POLLIN, 0}};
        cee_wl_wait(p, bus >= 0 ? 3 : 2, seconds);
        return;
    }
    if (cee_x.pending(n->display) > 0) {
        return;
    }
    double ms = seconds * 1000.0 + 0.999;
    /* The accessibility bus too (ADR 0164): a reader's request ends the wait, and the pump answers it. */
    int bus = cee_atspi_fd();
    struct pollfd p[3] = {{cee_x.connection_number(n->display), POLLIN, 0}, {w->wake_read, POLLIN, 0}, {bus, POLLIN, 0}};
    poll(p, bus >= 0 ? 3 : 2, ms <= 0.0 ? 0 : ms >= 2147483647.0 ? 2147483647 : (int)ms);
}

/* waitAny's wait (ADR 0145): every window has its own connection, so `poll` takes each one's descriptor, after Xlib's
   queues, with the process's pipe. Past 16 windows the descriptors are allocated; if that fails, the first 15 are
   polled for at most 10 ms at a time, so the loop around this still sees the others. */
static void cee_native_wait_any(double seconds) {
    if (cee_wls.state == 1) { /* one connection for every window (ADR 0205) */
        int bus = cee_atspi_fd();
        struct pollfd q[3] = {{-1, POLLIN, 0}, {cee_wake_any_read, POLLIN, 0}, {bus, POLLIN, 0}};
        cee_wl_wait(q, bus >= 0 ? 3 : 2, seconds);
        return;
    }
    struct pollfd few[16];
    struct pollfd *p = few;
    int bus = cee_atspi_fd(); /* the accessibility bus (ADR 0164) */
    size_t count = bus >= 0 ? 2 : 1;
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
    if (bus >= 0 && n < count) {
        p[n].fd = bus;
        p[n].events = POLLIN;
        p[n++].revents = 0;
    }
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
            if (((struct cee_x11 *)w->native)->wl == NULL) { /* a Wayland window has no X id to attach to */
                argv[n++] = "--attach";
                argv[n++] = xid;
            }
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
    uint32_t style = surface >> 8; /* WindowStyle (ADR 0206), above the surface's number */
    surface &= 0xffu;
    if (style > 1u) {
        *error = CEE_WINDOW_UNSUPPORTED;
        return NULL;
    }
    if (width == 0 || height == 0 || width > CEE_WINDOW_MAX_SIDE || height > CEE_WINDOW_MAX_SIDE ||
        (surface != CEE_SURFACE_FRAMES && surface != CEE_SURFACE_EXTERNAL)) {
        *error = CEE_WINDOW_UNSUPPORTED;
        return NULL;
    }
#if defined(CEE_WINDOW_CLAIMS_THREAD)
    if (!cee_window_claim_thread()) { /* ADR 0201: the first open makes its thread the window thread */
#else
    if (!cee_window_on_main_thread()) {
#endif
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
    w->wants_bar = style == 1u;
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
    ceesafe_platform_free(w->access);
    ceesafe_platform_free(w->access_text);
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

/* The program waits or polls again after it took a BACKGROUND: it has saved what it had to, and the time the system
   was asked for is given back (ADR 0159 §2.2). */
static void cee_window_saved(struct cee_window *w) {
    if (w->saving) {
        w->saving = 0;
#if defined(CEE_WINDOW_UIKIT) || defined(__ANDROID__)
        if (!w->scripted) {
            cee_native_saved(w);
        }
#endif
    }
}

int32_t ceesafe_window_poll(void *window, int32_t *fields, float *position) {
    struct cee_window *w = (struct cee_window *)window;
    cee_window_saved(w);
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
    } else if (w->background) {
        return 0; /* off the screen: nothing is shown, and nothing is drawn for no one (ADR 0159 §2.2) */
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
    if (!w->scripted && w->background) {
        return 0; /* as ceesafe_window_present */
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
    cee_window_saved(w);
    double target = w->last_wait + 1.0 / 60.0, now = cee_window_now();
    if (w->scripted || now >= target || w->count > 0 || w->choice_count > 0 || w->drop_count > 0 || w->drag_changed ||
        w->touch_count > 0 || w->app_count > 0 || w->access_requests_count > 0 || cee_wake_pending(w)) {
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
   over the window changed (ADR 0150), or nextTouch a touch (ADR 0155), or nextAppEvent an app event (ADR 0159). */
static int cee_window_has_event(struct cee_window *w) {
    if (w->drop_count > 0 || w->drag_changed || w->touch_count > 0 || w->app_count > 0 ||
        w->access_requests_count > 0) {
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
    cee_window_saved(w);
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
    for (struct cee_window *each = cee_wake_windows; each != NULL && cee_window_on_main_thread();
         each = each->wake_link) {
        cee_window_saved(each);
    }
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

/* The soft keyboard's inset, in points (ADR 0157). */
void ceesafe_window_keyboard_inset(void *window, float *inset) { *inset = ((struct cee_window *)window)->keyboard; }

/* The kind of keyboard text input shows (ADR 0157): WindowTextKind's number, 0 to 5. Only iOS and Android have one. */
void ceesafe_window_set_text_kind(void *window, int32_t kind) {
    struct cee_window *w = (struct cee_window *)window;
    if (kind < 0 || kind > 5 || kind == w->text_kind) {
        return;
    }
    w->text_kind = kind;
#if defined(CEE_WINDOW_UIKIT) || defined(__ANDROID__)
    if (!w->scripted) {
        cee_native_text_kind(w);
    }
#elif (defined(__linux__) || defined(__FreeBSD__)) && !defined(__ANDROID__)
    if (!w->scripted && cee_wl_of(w) != NULL && cee_wl_ti.window == w) {
        cee_wl_text_update(); /* Wayland's input method takes the field's kind too (ADR 0205) */
    }
#endif
}

/* ---- A title bar the program draws (ADR 0206): the layout, kept for every window; the actions, on the systems that
   implement them (Wayland, step 1). */
int32_t ceesafe_window_program_title_bar(void *window) { return ((struct cee_window *)window)->program_bar; }

void ceesafe_window_system_buttons(void *window, float *area) {
    memcpy(area, ((struct cee_window *)window)->sys_buttons, 4u * sizeof(float));
}

void ceesafe_window_set_title_bar(void *window, const float *bar) {
    struct cee_window *w = (struct cee_window *)window;
    float height = w->bar[0];
    for (int k = 0; k < 17; k++) {
        w->bar[k] = bar[k] > 0.0f && bar[k] < 1e6f ? bar[k] : 0.0f; /* NaN, negative or absurd: none */
    }
#if defined(__APPLE__) && !defined(CEE_WINDOW_UIKIT)
    if (!w->scripted && w->bar[0] != height) {
        void *pool = cee_objc.pool_push();
        cee_mac_place_buttons(w); /* the traffic lights centered in the new height */
        cee_objc.pool_pop(pool);
    }
#else
    (void)height;
#endif
}

int32_t ceesafe_window_maximized(void *window) { return ((struct cee_window *)window)->maximized; }

void ceesafe_window_minimize(void *window) {
    struct cee_window *w = (struct cee_window *)window;
#if (defined(__linux__) || defined(__FreeBSD__)) && !defined(__ANDROID__)
    if (!w->scripted && cee_wl_of(w) != NULL) {
        cee_wl_minimize(w);
    } else if (!w->scripted && w->native != NULL) {
        cee_x_minimize(w);
    }
#elif defined(_WIN32)
    if (!w->scripted) {
        cee_win32_minimize(w);
    }
#elif defined(__APPLE__) && !defined(CEE_WINDOW_UIKIT)
    if (!w->scripted) {
        void *pool = cee_objc.pool_push();
        cee_send_v_id(((struct cee_mac *)w->native)->window, "miniaturize:", NULL);
        cee_objc.pool_pop(pool);
    }
#else
    (void)w;
#endif
}

void ceesafe_window_toggle_maximized(void *window) {
    struct cee_window *w = (struct cee_window *)window;
#if (defined(__linux__) || defined(__FreeBSD__)) && !defined(__ANDROID__)
    if (!w->scripted && cee_wl_of(w) != NULL) {
        cee_wl_toggle_maximized(w);
    } else if (!w->scripted && w->native != NULL) {
        cee_x_toggle_maximized(w);
    }
#elif defined(_WIN32)
    if (!w->scripted) {
        cee_win32_toggle_maximized(w);
    }
#elif defined(__APPLE__) && !defined(CEE_WINDOW_UIKIT)
    if (!w->scripted) {
        void *pool = cee_objc.pool_push();
        cee_send_v_id(((struct cee_mac *)w->native)->window, "zoom:", NULL);
        cee_objc.pool_pop(pool);
    }
#else
    (void)w;
#endif
}

/* What the soft keyboard's Return key says (ADR 0161): WindowReturnKey's number, 0 to 5. Only iOS and Android have
   one; a new one is a new kind of keyboard to them. */
void ceesafe_window_set_return_key(void *window, int32_t key) {
    struct cee_window *w = (struct cee_window *)window;
    if (key < 0 || key > 5 || key == w->return_key) {
        return;
    }
    w->return_key = key;
#if defined(CEE_WINDOW_UIKIT) || defined(__ANDROID__)
    if (!w->scripted) {
        cee_native_text_kind(w);
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
    if (kind < CEE_DIALOG_FILE || kind > CEE_DIALOG_PRINT || title_length > 0x7fffffffu ||
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
        if (t == NULL || n == NULL || x == NULL) {
            result = -CEE_WINDOW_RESOURCES;
        } else if (kind == CEE_DIALOG_PRINT) {
#if defined(CEE_HAS_NATIVE_PRINT)
            result = cee_native_print(w, t, x);
            if (result == 1) {
                result = cee_window_reserve(&w->answer, &w->answer_capacity, 1u) ? 0 : -CEE_WINDOW_RESOURCES;
                if (result == 0) {
                    w->answer[0] = '\0'; /* one empty path: printed */
                    w->answer_length = 1;
                }
            }
#else
            result = -CEE_WINDOW_UNSUPPORTED; /* not yet on this system (ADR 0178 §4) */
#endif
        } else if (kind != CEE_DIALOG_EXPORT) {
            result = cee_native_dialog(w, (int)kind, t, n, x, (size_t)extensions_length);
        } else {
#if defined(CEE_WINDOW_UIKIT) || defined(__ANDROID__)
            /* The system saves the file where the user chooses (ADR 0158): 1 saved, 0 cancelled. */
            result = cee_native_export(w, t, n, x);
            if (result == 1) {
                result = cee_window_reserve(&w->answer, &w->answer_capacity, 1u) ? 0 : -CEE_WINDOW_RESOURCES;
                if (result == 0) {
                    w->answer[0] = '\0'; /* one empty path: saved, and nothing for lib/std to copy */
                    w->answer_length = 1;
                }
            }
#else
            result = cee_native_dialog(w, CEE_DIALOG_SAVE, t, n, "", 0); /* lib/std copies the file to the answer */
#endif
        }
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

/* The next app event (ADR 0159): WindowAppEvent's number, or -1. With none queued, the window takes the system's (or
   its script's) events first, as ceesafe_window_poll does. Taking a BACKGROUND begins the program's time to save. */
int32_t ceesafe_window_app_event(void *window) {
    struct cee_window *w = (struct cee_window *)window;
#if defined(__ANDROID__)
    if (!w->scripted) {
        pthread_mutex_lock(&cee_android.lock);
        cee_android_life.listening = 1; /* a program that asks is one the activity's onStop waits for */
        pthread_mutex_unlock(&cee_android.lock);
    }
#endif
    if (w->app_count == 0) {
        if (w->scripted) {
            cee_window_script_pump(w);
        }
#if !defined(CEE_WINDOW_NO_NATIVE)
        else {
            cee_native_pump(w);
        }
#endif
    }
    if (w->app_count == 0) {
        return -1;
    }
    int32_t kind = w->app_events[w->app_head];
    w->app_head = (w->app_head + 1u) % CEE_APP_QUEUE;
    w->app_count--;
    if (kind == CEE_APP_BACKGROUND) {
        w->saving = 1;
    }
    return kind;
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

/* ---- Accessibility (ADR 0164) ------------------------------------------------------------------------------------ */

struct cee_access_key {
    uint64_t id;
    uint32_t index;
};

static int cee_access_id_order(const void *a, const void *b) {
    uint64_t x = ((const struct cee_access_key *)a)->id, y = ((const struct cee_access_key *)b)->id;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Containers (ADR 0207): each node's container found by id in the sorted keys, and what the program left at 0
   numbered. A container comes before what it holds, so one pass in list order sees a container's place before its
   children's. An item's `row` is its place among its container's children; a cell's or a column header's `column`
   its place in its row, and its `row` its row's; a container's `rows` its children (a table's, its rows) and a
   table's `columns` its longest row. 0, or -2 when a node's container is not in the list, comes after it, or holds
   nothing (lib/std panics). */
static int32_t cee_access_tree(struct cee_access_node *nodes, uint32_t count, const struct cee_access_key *keys,
                               uint32_t *children) {
    for (uint32_t k = 0; k < count; k++) {
        children[k] = 0;
    }
    for (uint32_t k = 0; k < count; k++) {
        struct cee_access_node *n = &nodes[k];
        n->up = CEE_ACCESS_TOP;
        if (n->parent == 0) {
            continue;
        }
        uint32_t lo = 0, hi = count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2u;
            if (keys[mid].id < n->parent) {
                lo = mid + 1u;
            } else {
                hi = mid;
            }
        }
        if (lo == count || keys[lo].id != n->parent || keys[lo].index >= k ||
            !CEE_ACCESS_CONTAINER(nodes[keys[lo].index].role)) {
            return -2;
        }
        struct cee_access_node *p = &nodes[keys[lo].index];
        n->up = keys[lo].index;
        uint32_t place = ++children[n->up];
        if (p->role == CEE_ACCESS_ROW) {
            if (n->column == 0) {
                n->column = place;
            }
            if (n->row == 0) {
                n->row = p->row;
            }
        } else if (n->row == 0) {
            n->row = place;
        }
    }
    /* Counts: a container's children; a table's longest row. A table's explicit columns stay. */
    for (uint32_t k = 0; k < count; k++) {
        struct cee_access_node *n = &nodes[k];
        if (CEE_ACCESS_CONTAINER(n->role) && n->rows == 0 && n->role != CEE_ACCESS_ROW && n->role != CEE_ACCESS_CELL) {
            n->rows = children[k];
        }
    }
    uint32_t *widest = children; /* reused: a table's longest row so far, by the table's index */
    for (uint32_t k = 0; k < count; k++) {
        if (nodes[k].role == CEE_ACCESS_TABLE) {
            widest[k] = 0;
        }
    }
    for (uint32_t k = 0; k < count; k++) {
        const struct cee_access_node *n = &nodes[k];
        if ((n->role == CEE_ACCESS_CELL || n->role == CEE_ACCESS_COLUMN_HEADER) && n->up != CEE_ACCESS_TOP &&
            nodes[n->up].role == CEE_ACCESS_ROW && nodes[n->up].up != CEE_ACCESS_TOP) {
            uint32_t t = nodes[n->up].up;
            if (nodes[t].role == CEE_ACCESS_TABLE && n->column > widest[t]) {
                widest[t] = n->column;
            }
        }
    }
    for (uint32_t k = 0; k < count; k++) {
        if (nodes[k].role == CEE_ACCESS_TABLE && nodes[k].columns == 0) {
            nodes[k].columns = widest[k];
        }
    }
    return 0;
}

/* The window's elements, replacing the last list: 1, 0 when memory ran out (the last list stays), -1 when an id is
   given twice and -2 when a container is not before what it holds (lib/std panics). lib/std has checked the ranges
   and that no id is 0. The system is told what changed where its part is implemented (CEE_WINDOW_ACCESS). */
int32_t ceesafe_window_set_access(void *window, const void *nodes, uint32_t count, const uint8_t *text, uint32_t length) {
    struct cee_window *w = (struct cee_window *)window;
    struct cee_access_node *copy = NULL;
    char *text_copy = NULL;
    if (count > 0) {
        struct cee_access_key *ids = (struct cee_access_key *)ceesafe_platform_alloc((size_t)count * sizeof *ids);
        uint32_t *children = (uint32_t *)ceesafe_platform_alloc((size_t)count * sizeof *children);
        copy = (struct cee_access_node *)ceesafe_platform_alloc_zeroed((size_t)count * sizeof *copy);
        if (ids == NULL || copy == NULL || children == NULL) {
            ceesafe_platform_free(ids);
            ceesafe_platform_free(children);
            ceesafe_platform_free(copy);
            return 0;
        }
        const uint64_t *words = (const uint64_t *)nodes;
        for (uint32_t k = 0; k < count; k++, words += 14) {
            /* id; min, max, now as f64 bits; x|y, width|height as f32 bits; role|flags; the label's and the
               value's start|length; caret|anchor; the layer; the container's id (ADR 0207); row|column;
               rows|columns. */
            uint32_t halves[12];
            for (int j = 0; j < 6; j++) {
                halves[2 * j] = (uint32_t)words[4 + j];
                halves[2 * j + 1] = (uint32_t)(words[4 + j] >> 32);
            }
            copy[k].id = words[0];
            memcpy(&copy[k].min, &words[1], sizeof(double));
            memcpy(&copy[k].max, &words[2], sizeof(double));
            memcpy(&copy[k].now, &words[3], sizeof(double));
            memcpy(&copy[k].x, &halves[0], sizeof(float));
            memcpy(&copy[k].y, &halves[1], sizeof(float));
            memcpy(&copy[k].width, &halves[2], sizeof(float));
            memcpy(&copy[k].height, &halves[3], sizeof(float));
            copy[k].role = halves[4];
            copy[k].flags = halves[5];
            copy[k].label_start = halves[6];
            copy[k].label_length = halves[7];
            copy[k].value_start = halves[8];
            copy[k].value_length = halves[9];
            copy[k].caret = halves[10] <= halves[9] ? halves[10] : halves[9];
            copy[k].anchor = halves[11] <= halves[9] ? halves[11] : halves[9];
            copy[k].layer = (uint32_t)words[10];
            copy[k].parent = words[11];
            copy[k].row = (uint32_t)words[12];
            copy[k].column = (uint32_t)(words[12] >> 32);
            copy[k].rows = (uint32_t)words[13];
            copy[k].columns = (uint32_t)(words[13] >> 32);
            ids[k].id = copy[k].id;
            ids[k].index = k;
            if (copy[k].role >= CEE_ACCESS_ROLES) {
                copy[k].role = 0;
            }
        }
        qsort(ids, count, sizeof *ids, cee_access_id_order);
        int twice = 0;
        for (uint32_t k = 1; k < count; k++) {
            twice = twice || ids[k].id == ids[k - 1].id;
        }
        int32_t tree = twice ? 0 : cee_access_tree(copy, count, ids, children);
        ceesafe_platform_free(ids);
        ceesafe_platform_free(children);
        if (twice || tree < 0) {
            ceesafe_platform_free(copy);
            return twice ? -1 : -2;
        }
    }
    if (length > 0) {
        text_copy = (char *)ceesafe_platform_alloc(length);
        if (text_copy == NULL) {
            ceesafe_platform_free(copy);
            return 0;
        }
        memcpy(text_copy, text, length);
    }
    struct cee_access_node *old = w->access;
    uint32_t old_count = w->access_count;
    char *old_text = w->access_text;
    w->access = copy;
    w->access_count = count;
    w->access_text = text_copy;
    w->access_text_length = length;
#if defined(CEE_WINDOW_ACCESS)
    if (!w->scripted) {
        cee_native_access(w, old, old_count, old_text);
    }
#else
    (void)old_count;
#endif
    ceesafe_platform_free(old);
    ceesafe_platform_free(old_text);
    return 1;
}

/* The oldest request not taken: 1 with its fields, or 0. With none queued, the window takes the system's (or its
   script's) events first, as ceesafe_window_touch does. */
int32_t ceesafe_window_access_request(void *window, uint64_t *id, int32_t *action) {
    struct cee_window *w = (struct cee_window *)window;
    if (w->access_requests_count == 0) {
        if (w->scripted) {
            cee_window_script_pump(w);
        }
#if !defined(CEE_WINDOW_NO_NATIVE)
        else {
            cee_native_pump(w);
        }
#endif
    }
    if (w->access_requests_count == 0) {
        return 0;
    }
    struct cee_access_request r = w->access_requests[w->access_head];
    w->access_head = (w->access_head + 1u) % CEE_ACCESS_QUEUE;
    w->access_requests_count--;
    *id = r.id;
    *action = r.action;
    return 1;
}

int32_t ceesafe_window_access_active(void *window) { return ((struct cee_window *)window)->access_active; }

void ceesafe_window_announce(void *window, const uint8_t *text, uint64_t length) {
    struct cee_window *w = (struct cee_window *)window;
#if defined(CEE_WINDOW_ACCESS)
    if (!w->scripted && length > 0 && length < 0x7fffffffu) {
        cee_native_announce(w, (const char *)text, (size_t)length);
    }
#else
    (void)w;
    (void)text;
    (void)length;
#endif
}
