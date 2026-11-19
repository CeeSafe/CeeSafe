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

/* What lib/std calls (lib/std/bluetooth.cee declares them). */
int64_t ceesafe_bluetooth_list(uint32_t scan, uint32_t milliseconds, const uint8_t **out);
void ceesafe_bluetooth_free(const uint8_t *list);
int64_t ceesafe_bluetooth_open(uint32_t transport, const uint8_t *address, uint64_t length);
int32_t ceesafe_bluetooth_write(int64_t handle, const uint8_t *bytes, uint64_t length);
void ceesafe_bluetooth_close(int64_t handle);

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
    uint32_t transport;
    /* The system's connection: Classic, the device and its channel; Low Energy, the peripheral and its
       characteristic. */
    void *device;
    void *channel;
    size_t piece;      /* the most bytes one write takes */
    int acknowledged;  /* Low Energy: the characteristic acknowledges writes */
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
/* Each defines cee_bt_native_list (the paired printers, or a scan's), cee_bt_native_open (fills the link's device,
   channel and piece: 0 or an error), cee_bt_native_write (0 or minus an error) and cee_bt_native_close. */

#if defined(__APPLE__)
/* macOS and iOS. Apple's frameworks are loaded with dlopen and driven through the Objective-C runtime, as AppKit and
   UIKit are (ceesafe_rt_window.c), so no program links a framework. Classic (macOS only): IOBluetooth, whose
   synchronous calls run the calling thread's run loop while they wait. Low Energy: CoreBluetooth, one central manager
   whose delegate runs on a private serial queue of the runtime's; a call waits on a semaphore that the delegate
   signals. One Bluetooth call runs at a time (cee_bt_lock). */
#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

typedef void *cee_bt_id;
typedef void *cee_bt_sel;
typedef void (*cee_bt_fn)(void);

static pthread_mutex_t cee_bt_lock = PTHREAD_MUTEX_INITIALIZER;

static struct {
    int state; /* 0 not tried, 1 loaded, -1 failed */
    cee_bt_id (*cls)(const char *);
    cee_bt_sel (*sel)(const char *);
    cee_bt_fn send;
    cee_bt_id (*allocate_class)(cee_bt_id, const char *, size_t);
    signed char (*add_method)(cee_bt_id, cee_bt_sel, cee_bt_fn, const char *);
    void (*register_class)(cee_bt_id);
    void *(*pool_push)(void);
    void (*pool_pop)(void *);
    int32_t (*run_in_mode)(const void *, double, unsigned char);
    const void *const *default_mode;
} cee_bt_objc;

static int cee_bt_objc_load(void) {
    if (cee_bt_objc.state != 0) {
        return cee_bt_objc.state == 1;
    }
    cee_bt_objc.state = -1;
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL);
    void *foundation = dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW | RTLD_GLOBAL);
    void *core = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW | RTLD_GLOBAL);
    if (objc == NULL || foundation == NULL || core == NULL) {
        return 0;
    }
    cee_bt_objc.cls = (cee_bt_id(*)(const char *))dlsym(objc, "objc_getClass");
    cee_bt_objc.sel = (cee_bt_sel(*)(const char *))dlsym(objc, "sel_registerName");
    cee_bt_objc.send = (cee_bt_fn)dlsym(objc, "objc_msgSend");
    cee_bt_objc.allocate_class = (cee_bt_id(*)(cee_bt_id, const char *, size_t))dlsym(objc, "objc_allocateClassPair");
    cee_bt_objc.add_method = (signed char (*)(cee_bt_id, cee_bt_sel, cee_bt_fn, const char *))dlsym(objc,
                                                                                                  "class_addMethod");
    cee_bt_objc.register_class = (void (*)(cee_bt_id))dlsym(objc, "objc_registerClassPair");
    cee_bt_objc.pool_push = (void *(*)(void))dlsym(objc, "objc_autoreleasePoolPush");
    cee_bt_objc.pool_pop = (void (*)(void *))dlsym(objc, "objc_autoreleasePoolPop");
    cee_bt_objc.run_in_mode = (int32_t(*)(const void *, double, unsigned char))dlsym(core, "CFRunLoopRunInMode");
    cee_bt_objc.default_mode = (const void *const *)dlsym(core, "kCFRunLoopDefaultMode");
    if (cee_bt_objc.cls == NULL || cee_bt_objc.sel == NULL || cee_bt_objc.send == NULL ||
        cee_bt_objc.allocate_class == NULL || cee_bt_objc.add_method == NULL || cee_bt_objc.register_class == NULL ||
        cee_bt_objc.pool_push == NULL || cee_bt_objc.pool_pop == NULL || cee_bt_objc.run_in_mode == NULL ||
        cee_bt_objc.default_mode == NULL) {
        return 0;
    }
    cee_bt_objc.state = 1;
    return 1;
}

/* Messages, each through objc_msgSend cast to its exact type (arm64 passes variadic arguments differently). */
#define CEE_BT_SEL(name) cee_bt_objc.sel(name)
static cee_bt_id cee_bt_cls(const char *name) { return cee_bt_objc.cls(name); }
static cee_bt_id cee_bt_get(cee_bt_id o, const char *s) {
    return o == NULL ? NULL : ((cee_bt_id(*)(cee_bt_id, cee_bt_sel))cee_bt_objc.send)(o, CEE_BT_SEL(s));
}
static cee_bt_id cee_bt_get_id(cee_bt_id o, const char *s, cee_bt_id a) {
    return o == NULL ? NULL
                     : ((cee_bt_id(*)(cee_bt_id, cee_bt_sel, cee_bt_id))cee_bt_objc.send)(o, CEE_BT_SEL(s), a);
}
static cee_bt_id cee_bt_get_index(cee_bt_id o, const char *s, unsigned long k) {
    return o == NULL ? NULL
                     : ((cee_bt_id(*)(cee_bt_id, cee_bt_sel, unsigned long))cee_bt_objc.send)(o, CEE_BT_SEL(s), k);
}
static void cee_bt_do(cee_bt_id o, const char *s) {
    if (o != NULL) {
        ((void (*)(cee_bt_id, cee_bt_sel))cee_bt_objc.send)(o, CEE_BT_SEL(s));
    }
}
static void cee_bt_do_id(cee_bt_id o, const char *s, cee_bt_id a) {
    if (o != NULL) {
        ((void (*)(cee_bt_id, cee_bt_sel, cee_bt_id))cee_bt_objc.send)(o, CEE_BT_SEL(s), a);
    }
}
static void cee_bt_do_id_id(cee_bt_id o, const char *s, cee_bt_id a, cee_bt_id b) {
    if (o != NULL) {
        ((void (*)(cee_bt_id, cee_bt_sel, cee_bt_id, cee_bt_id))cee_bt_objc.send)(o, CEE_BT_SEL(s), a, b);
    }
}
static long cee_bt_long(cee_bt_id o, const char *s) {
    return o == NULL ? 0 : ((long (*)(cee_bt_id, cee_bt_sel))cee_bt_objc.send)(o, CEE_BT_SEL(s));
}
static unsigned long cee_bt_count(cee_bt_id array) {
    return array == NULL ? 0
                         : ((unsigned long (*)(cee_bt_id, cee_bt_sel))cee_bt_objc.send)(array, CEE_BT_SEL("count"));
}
static signed char cee_bt_bool(cee_bt_id o, const char *s) {
    return o == NULL ? 0 : ((signed char (*)(cee_bt_id, cee_bt_sel))cee_bt_objc.send)(o, CEE_BT_SEL(s));
}
static cee_bt_id cee_bt_string(const char *text) {
    return ((cee_bt_id(*)(cee_bt_id, cee_bt_sel, const char *))cee_bt_objc.send)(
        cee_bt_cls("NSString"), CEE_BT_SEL("stringWithUTF8String:"), text);
}
static const char *cee_bt_utf8(cee_bt_id string) {
    return string == NULL
               ? NULL
               : ((const char *(*)(cee_bt_id, cee_bt_sel))cee_bt_objc.send)(string, CEE_BT_SEL("UTF8String"));
}
static cee_bt_id cee_bt_array(const cee_bt_id *items, unsigned long count) {
    return ((cee_bt_id(*)(cee_bt_id, cee_bt_sel, const cee_bt_id *, unsigned long))cee_bt_objc.send)(
        cee_bt_cls("NSArray"), CEE_BT_SEL("arrayWithObjects:count:"), items, count);
}

/* Whether the program is an app bundle without NSBluetoothAlwaysUsageDescription, which the system would end at the
   first use of Bluetooth: Denied instead (ADR 0177 §2.2). A program run from a terminal has no bundle of its own. */
static int cee_bt_undeclared(void) {
    cee_bt_id bundle = cee_bt_get(cee_bt_cls("NSBundle"), "mainBundle");
    const char *path = cee_bt_utf8(cee_bt_get(bundle, "bundlePath"));
    size_t n = path != NULL ? strlen(path) : 0;
    if (n < 4 || strcmp(path + n - 4, ".app") != 0) {
        return 0;
    }
    return cee_bt_get_id(bundle, "objectForInfoDictionaryKey:", cee_bt_string("NSBluetoothAlwaysUsageDescription")) ==
           NULL;
}

/* ---- Classic: IOBluetooth (macOS) ---- */

#if !TARGET_OS_IPHONE
/* "00-11-22-aa-bb-cc" (IOBluetooth's form) as "00:11:22:AA:BB:CC", into `out` (18 bytes). */
static void cee_bt_address_from_apple(const char *apple, char *out) {
    size_t k = 0;
    for (; apple[k] != '\0' && k < 17; k++) {
        char c = apple[k];
        out[k] = c == '-' ? ':' : (c >= 'a' && c <= 'f') ? (char)(c - 'a' + 'A') : c;
    }
    out[k] = '\0';
}

static int cee_bt_io_load(void) {
    static int state; /* 0 not tried, 1 loaded, -1 not there */
    if (state == 0) {
        void *io = dlopen("/System/Library/Frameworks/IOBluetooth.framework/IOBluetooth", RTLD_NOW | RTLD_GLOBAL);
        state = io != NULL && cee_bt_cls("IOBluetoothDevice") != NULL ? 1 : -1;
    }
    return state == 1;
}

/* 0 when the adapter is there and on, else the error. */
static int cee_bt_io_host(void) {
    cee_bt_id host = cee_bt_get(cee_bt_cls("IOBluetoothHostController"), "defaultController");
    if (host == NULL) {
        return CEE_BT_UNSUPPORTED;
    }
    int power = ((int (*)(cee_bt_id, cee_bt_sel))cee_bt_objc.send)(host, CEE_BT_SEL("powerState"));
    return power == 1 ? 0 : CEE_BT_OFF; /* kBluetoothHCIPowerStateON */
}

static cee_bt_id cee_bt_spp_uuid(void) {
    return ((cee_bt_id(*)(cee_bt_id, cee_bt_sel, uint16_t))cee_bt_objc.send)(cee_bt_cls("IOBluetoothSDPUUID"),
                                                                            CEE_BT_SEL("uuid16:"), 0x1101);
}

/* Whether a paired device may be a printer: it has a Serial Port Profile record; or its records were never read and
   its class, which it gave when it was paired, is imaging (a printer's), miscellaneous or unclassified, as receipt
   printers say. A device paired by Low Energy has no class (0) and is not one; nor is one whose records are known and
   have no serial port (headphones, a phone, a watch). */
static int cee_bt_io_printer(cee_bt_id device) {
    if (cee_bt_get_id(device, "getServiceRecordForUUID:", cee_bt_spp_uuid()) != NULL) {
        return 1;
    }
    if (cee_bt_count(cee_bt_get(device, "services")) > 0) {
        return 0;
    }
    uint32_t whole = ((uint32_t(*)(cee_bt_id, cee_bt_sel))cee_bt_objc.send)(device, CEE_BT_SEL("classOfDevice"));
    uint32_t major = (whole >> 8) & 0x1F;
    return whole != 0 && (major == 6 || major == 0 || major == 0x1F); /* Imaging, Miscellaneous, Unclassified */
}

static int64_t cee_bt_io_paired(uint8_t **out) {
    if (!cee_bt_io_load()) {
        return -CEE_BT_UNSUPPORTED;
    }
    int host = cee_bt_io_host();
    if (host != 0) {
        return -host;
    }
    struct cee_bt_list l;
    memset(&l, 0, sizeof l);
    cee_bt_id devices = cee_bt_get(cee_bt_cls("IOBluetoothDevice"), "pairedDevices");
    for (unsigned long k = 0; k < cee_bt_count(devices); k++) {
        cee_bt_id device = cee_bt_get_index(devices, "objectAtIndex:", k);
        const char *apple = cee_bt_utf8(cee_bt_get(device, "addressString"));
        if (apple == NULL || !cee_bt_io_printer(device)) {
            continue;
        }
        char address[18];
        cee_bt_address_from_apple(apple, address);
        const char *name = cee_bt_utf8(cee_bt_get(device, "name"));
        cee_bt_add(&l, CEE_BT_CLASSIC, address, strlen(address), name != NULL ? name : "",
                   name != NULL ? strlen(name) : 0);
    }
    return cee_bt_finish(&l, out);
}

static int cee_bt_io_open(struct cee_bt_link *link, const char *address) {
    if (!cee_bt_io_load()) {
        return CEE_BT_UNSUPPORTED;
    }
    int host = cee_bt_io_host();
    if (host != 0) {
        return host;
    }
    char apple[18];
    size_t n = 0;
    for (; address[n] != '\0' && n < 17; n++) {
        apple[n] = address[n] == ':' ? '-' : address[n];
    }
    apple[n] = '\0';
    cee_bt_id device =
        cee_bt_get_id(cee_bt_cls("IOBluetoothDevice"), "deviceWithAddressString:", cee_bt_string(apple));
    if (device == NULL || !cee_bt_bool(device, "isPaired")) {
        return CEE_BT_NOT_FOUND;
    }
    if (((int (*)(cee_bt_id, cee_bt_sel))cee_bt_objc.send)(device, CEE_BT_SEL("openConnection")) != 0) {
        return CEE_BT_REFUSED;
    }
    /* The Serial Port Profile's channel, from the device's records: asked for when they are not known, five seconds
       at most, the run loop run meanwhile for the answer. */
    cee_bt_id uuid = cee_bt_spp_uuid();
    cee_bt_id record = cee_bt_get_id(device, "getServiceRecordForUUID:", uuid);
    if (record == NULL &&
        ((int (*)(cee_bt_id, cee_bt_sel, cee_bt_id))cee_bt_objc.send)(device, CEE_BT_SEL("performSDPQuery:"), NULL) ==
            0) {
        for (int k = 0; k < 50 && record == NULL; k++) {
            (void)cee_bt_objc.run_in_mode(*cee_bt_objc.default_mode, 0.1, 1);
            record = cee_bt_get_id(device, "getServiceRecordForUUID:", uuid);
        }
    }
    uint8_t channel_id = 0;
    if (record == NULL || ((int (*)(cee_bt_id, cee_bt_sel, uint8_t *))cee_bt_objc.send)(
                              record, CEE_BT_SEL("getRFCOMMChannelID:"), &channel_id) != 0) {
        (void)((int (*)(cee_bt_id, cee_bt_sel))cee_bt_objc.send)(device, CEE_BT_SEL("closeConnection"));
        return CEE_BT_REFUSED; /* not a serial port */
    }
    cee_bt_id channel = NULL;
    int opened = ((int (*)(cee_bt_id, cee_bt_sel, cee_bt_id *, uint8_t, cee_bt_id))cee_bt_objc.send)(
        device, CEE_BT_SEL("openRFCOMMChannelSync:withChannelID:delegate:"), &channel, channel_id, NULL);
    if (opened != 0 || channel == NULL) {
        (void)((int (*)(cee_bt_id, cee_bt_sel))cee_bt_objc.send)(device, CEE_BT_SEL("closeConnection"));
        return CEE_BT_REFUSED;
    }
    cee_bt_do(device, "retain");
    cee_bt_do(channel, "retain");
    uint16_t mtu = ((uint16_t(*)(cee_bt_id, cee_bt_sel))cee_bt_objc.send)(channel, CEE_BT_SEL("getMTU"));
    link->device = device;
    link->channel = channel;
    link->piece = mtu > 0 ? mtu : 127;
    return 0;
}

static int32_t cee_bt_io_write(struct cee_bt_link *link, const uint8_t *bytes, size_t length) {
    while (length > 0) {
        size_t n = length < link->piece ? length : link->piece;
        int written = ((int (*)(cee_bt_id, cee_bt_sel, void *, uint16_t))cee_bt_objc.send)(
            (cee_bt_id)link->channel, CEE_BT_SEL("writeSync:length:"), (void *)(uintptr_t)bytes, (uint16_t)n);
        if (written != 0) {
            return -CEE_BT_LOST;
        }
        bytes += n;
        length -= n;
    }
    return 0;
}

static void cee_bt_io_close(struct cee_bt_link *link) {
    (void)((int (*)(cee_bt_id, cee_bt_sel))cee_bt_objc.send)((cee_bt_id)link->channel, CEE_BT_SEL("closeChannel"));
    cee_bt_do((cee_bt_id)link->channel, "release");
    (void)((int (*)(cee_bt_id, cee_bt_sel))cee_bt_objc.send)((cee_bt_id)link->device, CEE_BT_SEL("closeConnection"));
    cee_bt_do((cee_bt_id)link->device, "release");
}
#endif

/* ---- Low Energy: CoreBluetooth (macOS and iOS) ---- */

/* The printers' services (ADR 0177 §1). */
static const char *const cee_bt_services[] = {"18F0", "FF00", "E7810A71-73AE-499D-8C15-FAA9AEF0C3F2"};
#define CEE_BT_SERVICES 3
/* The peripherals seen, kept (retained) so that a later open finds them; the scan's own are marked. */
#define CEE_BT_SEEN 64

static struct {
    int state; /* 0 not made, 1 made, -1 failed */
    dispatch_queue_t queue;
    dispatch_semaphore_t signal;
    cee_bt_id central;
    cee_bt_id delegate;
    const cee_bt_id *name_key; /* CBAdvertisementDataLocalNameKey */
    /* What the delegate says, read by the waiting call after the signal. */
    long manager_state;
    int failed;        /* a connect, a discovery or a write failed */
    int disconnected;  /* the peripheral of `current` went away */
    cee_bt_id current; /* the peripheral a call is working with */
    int scanning;
    int seen;
    struct {
        char address[40];
        char name[64];
        cee_bt_id peripheral;
        int in_scan;
    } peripherals[CEE_BT_SEEN];
} cee_bt_le;

static void cee_bt_le_signal(void) { dispatch_semaphore_signal(cee_bt_le.signal); }

/* Takes signals left from before; then a wait for the delegate's signal, `seconds` at most: 1 when it came. */
static void cee_bt_le_drain(void) {
    while (dispatch_semaphore_wait(cee_bt_le.signal, DISPATCH_TIME_NOW) == 0) {
    }
}
static int cee_bt_le_wait(double seconds) {
    return dispatch_semaphore_wait(cee_bt_le.signal, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * 1e9))) == 0;
}

static void cee_bt_le_did_update_state(cee_bt_id self, cee_bt_sel cmd, cee_bt_id central) {
    (void)self;
    (void)cmd;
    cee_bt_le.manager_state = cee_bt_long(central, "state");
    cee_bt_le_signal();
}

static void cee_bt_le_did_discover(cee_bt_id self, cee_bt_sel cmd, cee_bt_id central, cee_bt_id peripheral,
                                   cee_bt_id advertisement, cee_bt_id rssi) {
    (void)self;
    (void)cmd;
    (void)central;
    (void)rssi;
    if (!cee_bt_le.scanning) {
        return;
    }
    const char *address = cee_bt_utf8(cee_bt_get(cee_bt_get(peripheral, "identifier"), "UUIDString"));
    if (address == NULL) {
        return;
    }
    int k = 0;
    while (k < cee_bt_le.seen && strcmp(cee_bt_le.peripherals[k].address, address) != 0) {
        k++;
    }
    if (k == cee_bt_le.seen) {
        if (k == CEE_BT_SEEN) {
            return;
        }
        cee_bt_le.seen++;
        snprintf(cee_bt_le.peripherals[k].address, sizeof cee_bt_le.peripherals[k].address, "%s", address);
        cee_bt_do(peripheral, "retain");
        cee_bt_le.peripherals[k].peripheral = peripheral;
        cee_bt_le.peripherals[k].name[0] = '\0';
    }
    cee_bt_le.peripherals[k].in_scan = 1;
    const char *name = NULL;
    if (cee_bt_le.name_key != NULL) {
        name = cee_bt_utf8(cee_bt_get_id(advertisement, "objectForKey:", *cee_bt_le.name_key));
    }
    if (name == NULL) {
        name = cee_bt_utf8(cee_bt_get(peripheral, "name"));
    }
    if (name != NULL) {
        snprintf(cee_bt_le.peripherals[k].name, sizeof cee_bt_le.peripherals[k].name, "%s", name);
    }
}

static void cee_bt_le_did_connect(cee_bt_id self, cee_bt_sel cmd, cee_bt_id central, cee_bt_id peripheral) {
    (void)self;
    (void)cmd;
    (void)central;
    if (peripheral == cee_bt_le.current) {
        cee_bt_le_signal();
    }
}

static void cee_bt_le_did_fail(cee_bt_id self, cee_bt_sel cmd, cee_bt_id central, cee_bt_id peripheral,
                               cee_bt_id error) {
    (void)self;
    (void)cmd;
    (void)central;
    (void)error;
    if (peripheral == cee_bt_le.current) {
        cee_bt_le.failed = 1;
        cee_bt_le_signal();
    }
}

static void cee_bt_le_did_disconnect(cee_bt_id self, cee_bt_sel cmd, cee_bt_id central, cee_bt_id peripheral,
                                     cee_bt_id error) {
    (void)self;
    (void)cmd;
    (void)central;
    (void)error;
    if (peripheral == cee_bt_le.current) {
        cee_bt_le.disconnected = 1;
        cee_bt_le_signal();
    }
}

/* peripheral:didDiscoverServices:, peripheral:didDiscoverCharacteristicsForService:error: and
   peripheral:didWriteValueForCharacteristic:error: each end a wait, an error failing it. */
static void cee_bt_le_did_services(cee_bt_id self, cee_bt_sel cmd, cee_bt_id peripheral, cee_bt_id error) {
    (void)self;
    (void)cmd;
    (void)peripheral;
    cee_bt_le.failed = error != NULL;
    cee_bt_le_signal();
}
static void cee_bt_le_did_answer(cee_bt_id self, cee_bt_sel cmd, cee_bt_id peripheral, cee_bt_id about,
                                 cee_bt_id error) {
    (void)self;
    (void)cmd;
    (void)peripheral;
    (void)about;
    cee_bt_le.failed = error != NULL;
    cee_bt_le_signal();
}
static void cee_bt_le_ready(cee_bt_id self, cee_bt_sel cmd, cee_bt_id peripheral) {
    (void)self;
    (void)cmd;
    (void)peripheral;
    cee_bt_le_signal();
}

/* The central manager, made once, and its state: 0 when it is on, else the error. */
static int cee_bt_le_start(void) {
    if (cee_bt_le.state == 0) {
        cee_bt_le.state = -1;
        void *cb = dlopen("/System/Library/Frameworks/CoreBluetooth.framework/CoreBluetooth", RTLD_NOW | RTLD_GLOBAL);
        cee_bt_id base = cee_bt_cls("NSObject");
        if (cb == NULL || base == NULL || cee_bt_cls("CBCentralManager") == NULL) {
            return CEE_BT_UNSUPPORTED;
        }
        cee_bt_le.name_key = (const cee_bt_id *)dlsym(cb, "CBAdvertisementDataLocalNameKey");
        cee_bt_id cls = cee_bt_cls("CeeSafeBluetoothDelegate");
        if (cls == NULL) {
            cls = cee_bt_objc.allocate_class(base, "CeeSafeBluetoothDelegate", 0);
            if (cls == NULL) {
                return CEE_BT_RESOURCES;
            }
            cee_bt_objc.add_method(cls, CEE_BT_SEL("centralManagerDidUpdateState:"),
                                   (cee_bt_fn)cee_bt_le_did_update_state, "v@:@");
            cee_bt_objc.add_method(cls, CEE_BT_SEL("centralManager:didDiscoverPeripheral:advertisementData:RSSI:"),
                                   (cee_bt_fn)cee_bt_le_did_discover, "v@:@@@@");
            cee_bt_objc.add_method(cls, CEE_BT_SEL("centralManager:didConnectPeripheral:"),
                                   (cee_bt_fn)cee_bt_le_did_connect, "v@:@@");
            cee_bt_objc.add_method(cls, CEE_BT_SEL("centralManager:didFailToConnectPeripheral:error:"),
                                   (cee_bt_fn)cee_bt_le_did_fail, "v@:@@@");
            cee_bt_objc.add_method(cls, CEE_BT_SEL("centralManager:didDisconnectPeripheral:error:"),
                                   (cee_bt_fn)cee_bt_le_did_disconnect, "v@:@@@");
            cee_bt_objc.add_method(cls, CEE_BT_SEL("peripheral:didDiscoverServices:"),
                                   (cee_bt_fn)cee_bt_le_did_services, "v@:@@");
            cee_bt_objc.add_method(cls, CEE_BT_SEL("peripheral:didDiscoverCharacteristicsForService:error:"),
                                   (cee_bt_fn)cee_bt_le_did_answer, "v@:@@@");
            cee_bt_objc.add_method(cls, CEE_BT_SEL("peripheral:didWriteValueForCharacteristic:error:"),
                                   (cee_bt_fn)cee_bt_le_did_answer, "v@:@@@");
            cee_bt_objc.add_method(cls, CEE_BT_SEL("peripheralIsReadyToSendWriteWithoutResponse:"),
                                   (cee_bt_fn)cee_bt_le_ready, "v@:@");
            cee_bt_objc.register_class(cls);
        }
        cee_bt_le.signal = dispatch_semaphore_create(0);
        cee_bt_le.queue = dispatch_queue_create("org.ceesafe.bluetooth", NULL);
        cee_bt_le.delegate = cee_bt_get(cee_bt_get(cls, "alloc"), "init");
        cee_bt_le.manager_state = 0;
        cee_bt_id manager = cee_bt_get(cee_bt_cls("CBCentralManager"), "alloc");
        cee_bt_le.central = ((cee_bt_id(*)(cee_bt_id, cee_bt_sel, cee_bt_id, void *, cee_bt_id))cee_bt_objc.send)(
            manager, CEE_BT_SEL("initWithDelegate:queue:options:"), cee_bt_le.delegate, (void *)cee_bt_le.queue, NULL);
        if (cee_bt_le.central == NULL || cee_bt_le.delegate == NULL) {
            return CEE_BT_RESOURCES;
        }
        cee_bt_le.state = 1;
        /* The first state comes from the delegate; the system may ask the user meanwhile, a minute at most. */
        for (int k = 0; k < 600 && cee_bt_le.manager_state <= 1; k++) {
            (void)cee_bt_le_wait(0.1);
        }
    }
    if (cee_bt_le.state != 1) {
        return CEE_BT_UNSUPPORTED;
    }
    switch (cee_bt_long(cee_bt_le.central, "state")) {
    case 5:
        return 0; /* CBManagerStatePoweredOn */
    case 4:
        return CEE_BT_OFF;
    case 3:
        return CEE_BT_DENIED; /* unauthorized */
    case 2:
        return CEE_BT_UNSUPPORTED;
    default:
        return CEE_BT_OFF; /* resetting, or not known yet */
    }
}

static cee_bt_id cee_bt_le_service_uuids(void) {
    cee_bt_id uuids[CEE_BT_SERVICES];
    for (int k = 0; k < CEE_BT_SERVICES; k++) {
        uuids[k] = cee_bt_get_id(cee_bt_cls("CBUUID"), "UUIDWithString:", cee_bt_string(cee_bt_services[k]));
    }
    return cee_bt_array(uuids, CEE_BT_SERVICES);
}

/* Runs `f(context)` on the delegate's queue, so what the delegate keeps is read between its callbacks. */
static void cee_bt_le_sync(void *context, void (*f)(void *)) { dispatch_sync_f(cee_bt_le.queue, context, f); }

static void cee_bt_le_scan_on(void *context) {
    (void)context;
    for (int k = 0; k < cee_bt_le.seen; k++) {
        cee_bt_le.peripherals[k].in_scan = 0;
    }
    cee_bt_le.scanning = 1;
}
static void cee_bt_le_scan_off(void *context) {
    (void)context;
    cee_bt_le.scanning = 0;
}
static void cee_bt_le_scan_list(void *context) {
    struct cee_bt_list *l = (struct cee_bt_list *)context;
    for (int k = 0; k < cee_bt_le.seen; k++) {
        if (cee_bt_le.peripherals[k].in_scan) {
            cee_bt_add(l, CEE_BT_LOW_ENERGY, cee_bt_le.peripherals[k].address,
                       strlen(cee_bt_le.peripherals[k].address), cee_bt_le.peripherals[k].name,
                       strlen(cee_bt_le.peripherals[k].name));
        }
    }
}

static int64_t cee_bt_le_scan(uint32_t milliseconds, uint8_t **out) {
    if (cee_bt_undeclared()) {
        return -CEE_BT_DENIED;
    }
    int started = cee_bt_le_start();
    if (started != 0) {
        return -started;
    }
    cee_bt_le_sync(NULL, cee_bt_le_scan_on);
    cee_bt_do_id_id(cee_bt_le.central, "scanForPeripheralsWithServices:options:", cee_bt_le_service_uuids(), NULL);
    usleep((useconds_t)milliseconds * 1000);
    cee_bt_do(cee_bt_le.central, "stopScan");
    cee_bt_le_sync(NULL, cee_bt_le_scan_off);
    struct cee_bt_list l;
    memset(&l, 0, sizeof l);
    cee_bt_le_sync(&l, cee_bt_le_scan_list);
    return cee_bt_finish(&l, out);
}

struct cee_bt_le_find {
    const char *address;
    cee_bt_id found;
};
static void cee_bt_le_find_seen(void *context) {
    struct cee_bt_le_find *f = (struct cee_bt_le_find *)context;
    for (int k = 0; k < cee_bt_le.seen; k++) {
        if (strcmp(cee_bt_le.peripherals[k].address, f->address) == 0) {
            f->found = cee_bt_le.peripherals[k].peripheral;
            cee_bt_do(f->found, "retain");
            return;
        }
    }
}

/* The peripheral named `address` (a UUID): one a scan saw, else one the system remembers. Retained. */
static cee_bt_id cee_bt_le_peripheral(const char *address) {
    struct cee_bt_le_find f = {address, NULL};
    cee_bt_le_sync(&f, cee_bt_le_find_seen);
    if (f.found != NULL) {
        return f.found;
    }
    cee_bt_id uuid =
        cee_bt_get_id(cee_bt_get(cee_bt_cls("NSUUID"), "alloc"), "initWithUUIDString:", cee_bt_string(address));
    if (uuid == NULL) {
        return NULL;
    }
    cee_bt_id known = cee_bt_get_id(cee_bt_le.central, "retrievePeripheralsWithIdentifiers:", cee_bt_array(&uuid, 1));
    cee_bt_do(uuid, "release");
    cee_bt_id peripheral = cee_bt_count(known) > 0 ? cee_bt_get_index(known, "objectAtIndex:", 0) : NULL;
    cee_bt_do(peripheral, "retain");
    return peripheral;
}

static void cee_bt_le_begin(void *context) {
    cee_bt_le.current = (cee_bt_id)context;
    cee_bt_le.failed = 0;
    cee_bt_le.disconnected = 0;
}

/* Connects, finds the printer services and their characteristics, and takes the first characteristic that takes
   acknowledged writes, else the first that takes writes without a response. */
static int cee_bt_le_open(struct cee_bt_link *link, const char *address) {
    if (cee_bt_undeclared()) {
        return CEE_BT_DENIED;
    }
    int started = cee_bt_le_start();
    if (started != 0) {
        return started;
    }
    cee_bt_id peripheral = cee_bt_le_peripheral(address);
    if (peripheral == NULL) {
        return CEE_BT_NOT_FOUND;
    }
    cee_bt_le_sync(peripheral, cee_bt_le_begin);
    cee_bt_le_drain();
    cee_bt_do_id(peripheral, "setDelegate:", cee_bt_le.delegate);
    cee_bt_do_id_id(cee_bt_le.central, "connectPeripheral:options:", peripheral, NULL);
    int error = 0;
    if (!cee_bt_le_wait(15.0) || cee_bt_le.failed || cee_bt_le.disconnected) {
        error = CEE_BT_REFUSED;
    }
    if (error == 0) {
        cee_bt_do_id(peripheral, "discoverServices:", cee_bt_le_service_uuids());
        if (!cee_bt_le_wait(10.0) || cee_bt_le.failed || cee_bt_le.disconnected) {
            error = CEE_BT_REFUSED;
        }
    }
    cee_bt_id chosen = NULL;
    int acknowledged = 0;
    cee_bt_id services = error == 0 ? cee_bt_get(peripheral, "services") : NULL;
    for (unsigned long s = 0; error == 0 && chosen == NULL && s < cee_bt_count(services); s++) {
        cee_bt_id service = cee_bt_get_index(services, "objectAtIndex:", s);
        cee_bt_do_id_id(peripheral, "discoverCharacteristics:forService:", NULL, service);
        if (!cee_bt_le_wait(10.0) || cee_bt_le.disconnected) {
            error = CEE_BT_REFUSED;
            break;
        }
        cee_bt_id characteristics = cee_bt_get(service, "characteristics");
        for (int pass = 0; pass < 2 && chosen == NULL; pass++) {
            for (unsigned long c = 0; c < cee_bt_count(characteristics); c++) {
                cee_bt_id characteristic = cee_bt_get_index(characteristics, "objectAtIndex:", c);
                unsigned long properties = (unsigned long)cee_bt_long(characteristic, "properties");
                /* CBCharacteristicPropertyWrite 0x08, then WriteWithoutResponse 0x04. */
                if ((pass == 0 && (properties & 0x08)) || (pass == 1 && (properties & 0x04))) {
                    chosen = characteristic;
                    acknowledged = pass == 0;
                    break;
                }
            }
        }
    }
    if (error == 0 && chosen == NULL) {
        error = CEE_BT_REFUSED; /* not a printer */
    }
    if (error != 0) {
        cee_bt_do_id(cee_bt_le.central, "cancelPeripheralConnection:", peripheral);
        cee_bt_do(peripheral, "release");
        return error;
    }
    cee_bt_do(chosen, "retain");
    unsigned long piece = ((unsigned long (*)(cee_bt_id, cee_bt_sel, long))cee_bt_objc.send)(
        peripheral, CEE_BT_SEL("maximumWriteValueLengthForType:"), acknowledged ? 0 : 1);
    link->device = peripheral;
    link->channel = chosen;
    link->acknowledged = acknowledged;
    link->piece = piece > 0 ? piece : 20;
    return 0;
}

static int32_t cee_bt_le_write(struct cee_bt_link *link, const uint8_t *bytes, size_t length) {
    cee_bt_id peripheral = (cee_bt_id)link->device;
    cee_bt_le_sync(peripheral, cee_bt_le_begin);
    while (length > 0) {
        size_t n = length < link->piece ? length : link->piece;
        if (!link->acknowledged) {
            /* The system's flow control: wait until it takes another write without a response, five seconds at
               most. */
            for (int k = 0; k < 100 && !cee_bt_bool(peripheral, "canSendWriteWithoutResponse"); k++) {
                (void)cee_bt_le_wait(0.05);
                if (cee_bt_le.disconnected) {
                    return -CEE_BT_LOST;
                }
            }
        }
        cee_bt_le_drain();
        cee_bt_le.failed = 0;
        void *pool = cee_bt_objc.pool_push();
        cee_bt_id data = ((cee_bt_id(*)(cee_bt_id, cee_bt_sel, const void *, unsigned long))cee_bt_objc.send)(
            cee_bt_cls("NSData"), CEE_BT_SEL("dataWithBytes:length:"), bytes, (unsigned long)n);
        ((void (*)(cee_bt_id, cee_bt_sel, cee_bt_id, cee_bt_id, long))cee_bt_objc.send)(
            peripheral, CEE_BT_SEL("writeValue:forCharacteristic:type:"), data, (cee_bt_id)link->channel,
            link->acknowledged ? 0 : 1);
        cee_bt_objc.pool_pop(pool);
        if (link->acknowledged && (!cee_bt_le_wait(10.0) || cee_bt_le.failed)) {
            return -CEE_BT_LOST;
        }
        if (cee_bt_le.disconnected || cee_bt_long(peripheral, "state") != 2) { /* CBPeripheralStateConnected */
            return -CEE_BT_LOST;
        }
        bytes += n;
        length -= n;
    }
    return 0;
}

static void cee_bt_le_close(struct cee_bt_link *link) {
    cee_bt_do_id(cee_bt_le.central, "cancelPeripheralConnection:", (cee_bt_id)link->device);
    cee_bt_do((cee_bt_id)link->channel, "release");
    cee_bt_do((cee_bt_id)link->device, "release");
}

/* ---- The system's calls ---- */

static int64_t cee_bt_native_list(uint32_t scan, uint32_t milliseconds, uint8_t **out) {
    if (!cee_bt_objc_load()) {
        return -CEE_BT_UNSUPPORTED;
    }
    pthread_mutex_lock(&cee_bt_lock);
    void *pool = cee_bt_objc.pool_push();
    int64_t result;
    if (scan) {
        result = cee_bt_le_scan(milliseconds, out);
    } else {
#if TARGET_OS_IPHONE
        result = -CEE_BT_UNSUPPORTED; /* no Classic serial port for a program on iOS (ADR 0177 §1) */
#else
        result = cee_bt_io_paired(out);
#endif
    }
    cee_bt_objc.pool_pop(pool);
    pthread_mutex_unlock(&cee_bt_lock);
    return result;
}

static int cee_bt_native_open(struct cee_bt_link *link, uint32_t transport, const char *address, size_t length) {
    (void)length;
    if (!cee_bt_objc_load()) {
        return CEE_BT_UNSUPPORTED;
    }
    pthread_mutex_lock(&cee_bt_lock);
    void *pool = cee_bt_objc.pool_push();
    int error;
    link->transport = transport;
    if (transport == CEE_BT_LOW_ENERGY) {
        error = cee_bt_le_open(link, address);
    } else {
#if TARGET_OS_IPHONE
        error = CEE_BT_UNSUPPORTED;
#else
        error = cee_bt_io_open(link, address);
#endif
    }
    cee_bt_objc.pool_pop(pool);
    pthread_mutex_unlock(&cee_bt_lock);
    return error;
}

static int32_t cee_bt_native_write(struct cee_bt_link *link, const uint8_t *bytes, size_t length) {
    pthread_mutex_lock(&cee_bt_lock);
    void *pool = cee_bt_objc.pool_push();
    int32_t result;
    if (link->transport == CEE_BT_LOW_ENERGY) {
        result = cee_bt_le_write(link, bytes, length);
    } else {
#if TARGET_OS_IPHONE
        result = -CEE_BT_LOST;
#else
        result = cee_bt_io_write(link, bytes, length);
#endif
    }
    cee_bt_objc.pool_pop(pool);
    pthread_mutex_unlock(&cee_bt_lock);
    return result;
}

static void cee_bt_native_close(struct cee_bt_link *link) {
    pthread_mutex_lock(&cee_bt_lock);
    void *pool = cee_bt_objc.pool_push();
    if (link->transport == CEE_BT_LOW_ENERGY) {
        cee_bt_le_close(link);
    } else {
#if !TARGET_OS_IPHONE
        cee_bt_io_close(link);
#endif
    }
    cee_bt_objc.pool_pop(pool);
    pthread_mutex_unlock(&cee_bt_lock);
}

#elif defined(__ANDROID__)
/* Android: org.ceesafe.window.Bluetooth (runtime/android), a class of the window runtime's embedded dex, called over
   JNI from the program's thread, attached to the VM for the call. The window runtime gives the VM, the activity and
   the class (ceesafe_android_vm, ceesafe_android_bluetooth); they are weak here, so a program that opens no window
   links without them and has no Bluetooth (Unsupported), as one before its activity is made. One call at a time. */
#include <jni.h>
#include <pthread.h>

JavaVM *ceesafe_android_vm(void) __attribute__((weak));
jclass ceesafe_android_bluetooth(JNIEnv *env, jobject *activity) __attribute__((weak));

static pthread_mutex_t cee_bt_lock = PTHREAD_MUTEX_INITIALIZER;

/* The calling thread's env, attached for the call when it was not (`*attached` set): NULL without a VM. */
static JNIEnv *cee_bt_env(JavaVM **vm, int *attached) {
    *attached = 0;
    *vm = ceesafe_android_vm != NULL ? ceesafe_android_vm() : NULL;
    if (*vm == NULL) {
        return NULL;
    }
    JNIEnv *env = NULL;
    if ((**vm)->GetEnv(*vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if ((**vm)->AttachCurrentThread(*vm, &env, NULL) != JNI_OK) {
            return NULL;
        }
        *attached = 1;
    }
    return env;
}

static void cee_bt_env_done(JavaVM *vm, int attached) {
    if (attached) {
        (void)(*vm)->DetachCurrentThread(vm);
    }
}

/* Whether a Java exception is pending, which is cleared. */
static int cee_bt_thrown(JNIEnv *env) {
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return 1;
    }
    return 0;
}

static int64_t cee_bt_native_list(uint32_t scan, uint32_t milliseconds, uint8_t **out) {
    pthread_mutex_lock(&cee_bt_lock);
    JavaVM *vm;
    int attached;
    JNIEnv *env = cee_bt_env(&vm, &attached);
    int64_t result = -CEE_BT_UNSUPPORTED;
    if (env != NULL && (*env)->PushLocalFrame(env, 16) == 0) {
        jobject activity = NULL;
        jclass bluetooth = ceesafe_android_bluetooth != NULL ? ceesafe_android_bluetooth(env, &activity) : NULL;
        jmethodID method =
            bluetooth == NULL ? NULL
            : scan ? (*env)->GetStaticMethodID(env, bluetooth, "scan", "(Landroid/app/Activity;I)[B")
                   : (*env)->GetStaticMethodID(env, bluetooth, "paired", "(Landroid/app/Activity;)[B");
        jbyteArray answer = NULL;
        if (method != NULL && !cee_bt_thrown(env)) {
            answer = scan ? (jbyteArray)(*env)->CallStaticObjectMethod(env, bluetooth, method, activity,
                                                                         (jint)milliseconds)
                          : (jbyteArray)(*env)->CallStaticObjectMethod(env, bluetooth, method, activity);
            if (cee_bt_thrown(env)) {
                answer = NULL;
                result = -CEE_BT_RESOURCES;
            }
        }
        jsize length = answer != NULL ? (*env)->GetArrayLength(env, answer) : 0;
        if (length > 0) {
            jbyte first = 0;
            (*env)->GetByteArrayRegion(env, answer, 0, 1, &first);
            if (first != 0) {
                result = -(int64_t)first;
            } else if (length == 1) {
                result = 0;
            } else {
                uint8_t *bytes = (uint8_t *)ceesafe_platform_alloc((size_t)length - 1);
                if (bytes == NULL) {
                    result = -CEE_BT_RESOURCES;
                } else {
                    (*env)->GetByteArrayRegion(env, answer, 1, length - 1, (jbyte *)bytes);
                    *out = bytes;
                    result = (int64_t)length - 1;
                }
            }
        }
        (void)cee_bt_thrown(env);
        (void)(*env)->PopLocalFrame(env, NULL);
    }
    if (env != NULL) {
        cee_bt_env_done(vm, attached);
    }
    pthread_mutex_unlock(&cee_bt_lock);
    return result;
}

static int cee_bt_native_open(struct cee_bt_link *link, uint32_t transport, const char *address, size_t length) {
    (void)length;
    pthread_mutex_lock(&cee_bt_lock);
    JavaVM *vm;
    int attached;
    JNIEnv *env = cee_bt_env(&vm, &attached);
    int error = CEE_BT_UNSUPPORTED;
    if (env != NULL && (*env)->PushLocalFrame(env, 16) == 0) {
        jobject activity = NULL;
        jclass bluetooth = ceesafe_android_bluetooth != NULL ? ceesafe_android_bluetooth(env, &activity) : NULL;
        jmethodID open = bluetooth != NULL ? (*env)->GetStaticMethodID(
                                                 env, bluetooth, "open",
                                                 "(Landroid/app/Activity;ILjava/lang/String;)"
                                                 "Lorg/ceesafe/window/Bluetooth$Link;")
                                           : NULL;
        jstring text = open != NULL ? (*env)->NewStringUTF(env, address) : NULL;
        jobject made = NULL;
        if (text != NULL && !cee_bt_thrown(env)) {
            made = (*env)->CallStaticObjectMethod(env, bluetooth, open, activity, (jint)transport, text);
            error = cee_bt_thrown(env) || made == NULL ? CEE_BT_RESOURCES : 0;
        }
        if (made != NULL) {
            jclass link_class = (*env)->GetObjectClass(env, made);
            jfieldID field = (*env)->GetFieldID(env, link_class, "error", "I");
            error = field != NULL ? (int)(*env)->GetIntField(env, made, field) : CEE_BT_RESOURCES;
            if (error == 0) {
                link->device = (void *)(*env)->NewGlobalRef(env, made);
                error = link->device != NULL ? 0 : CEE_BT_RESOURCES;
            }
        }
        (void)cee_bt_thrown(env);
        (void)(*env)->PopLocalFrame(env, NULL);
    }
    if (env != NULL) {
        cee_bt_env_done(vm, attached);
    }
    pthread_mutex_unlock(&cee_bt_lock);
    return error;
}

static int32_t cee_bt_native_write(struct cee_bt_link *link, const uint8_t *bytes, size_t length) {
    if (length > 0x7FFFFFFF) {
        return -CEE_BT_RESOURCES;
    }
    pthread_mutex_lock(&cee_bt_lock);
    JavaVM *vm;
    int attached;
    JNIEnv *env = cee_bt_env(&vm, &attached);
    int32_t result = -CEE_BT_LOST;
    if (env != NULL && (*env)->PushLocalFrame(env, 8) == 0) {
        jobject made = (jobject)link->device;
        jclass link_class = (*env)->GetObjectClass(env, made);
        jmethodID write = (*env)->GetMethodID(env, link_class, "write", "([B)I");
        jbyteArray array = (*env)->NewByteArray(env, (jsize)length);
        if (write != NULL && array != NULL && !cee_bt_thrown(env)) {
            (*env)->SetByteArrayRegion(env, array, 0, (jsize)length, (const jbyte *)bytes);
            jint error = (*env)->CallIntMethod(env, made, write, array);
            result = cee_bt_thrown(env) ? -CEE_BT_LOST : -(int32_t)error;
        } else {
            result = -CEE_BT_RESOURCES;
        }
        (void)cee_bt_thrown(env);
        (void)(*env)->PopLocalFrame(env, NULL);
    }
    if (env != NULL) {
        cee_bt_env_done(vm, attached);
    }
    pthread_mutex_unlock(&cee_bt_lock);
    return result;
}

static void cee_bt_native_close(struct cee_bt_link *link) {
    pthread_mutex_lock(&cee_bt_lock);
    JavaVM *vm;
    int attached;
    JNIEnv *env = cee_bt_env(&vm, &attached);
    if (env != NULL) {
        jobject made = (jobject)link->device;
        jclass link_class = (*env)->GetObjectClass(env, made);
        jmethodID close = link_class != NULL ? (*env)->GetMethodID(env, link_class, "close", "()V") : NULL;
        if (close != NULL) {
            (*env)->CallVoidMethod(env, made, close);
        }
        (void)cee_bt_thrown(env);
        (*env)->DeleteLocalRef(env, link_class);
        (*env)->DeleteGlobalRef(env, made);
        cee_bt_env_done(vm, attached);
    }
    pthread_mutex_unlock(&cee_bt_lock);
}

#elif defined(_WIN32)
/* Windows: Classic through Winsock's RFCOMM (AF_BTH) to the Serial Port Profile, the paired devices from
   BluetoothApis.dll (bthprops.cpl before Windows 8), loaded at the first call. Low Energy (WinRT's) is not yet here
   (ADR 0177 §5): a scan finds nothing and a Low Energy link is Unsupported. Winsock is the I/O runtime's library
   (ws2_32). One call at a time. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2bth.h>
#include <windows.h>
#include <bluetoothapis.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#endif

static SRWLOCK cee_bt_lock = SRWLOCK_INIT;

/* SerialPortServiceClass_UUID, 00001101-0000-1000-8000-00805F9B34FB. */
static const GUID cee_bt_spp = {0x00001101, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};

static struct {
    int state; /* 0 not tried, 1 loaded, -1 not there */
    HBLUETOOTH_RADIO_FIND(WINAPI *first_radio)(const BLUETOOTH_FIND_RADIO_PARAMS *, HANDLE *);
    BOOL(WINAPI *radio_close)(HBLUETOOTH_RADIO_FIND);
    HBLUETOOTH_DEVICE_FIND(WINAPI *first_device)(const BLUETOOTH_DEVICE_SEARCH_PARAMS *, BLUETOOTH_DEVICE_INFO *);
    BOOL(WINAPI *next_device)(HBLUETOOTH_DEVICE_FIND, BLUETOOTH_DEVICE_INFO *);
    BOOL(WINAPI *device_close)(HBLUETOOTH_DEVICE_FIND);
    DWORD(WINAPI *services)(HANDLE, const BLUETOOTH_DEVICE_INFO *, DWORD *, GUID *);
} cee_bt_win;

static FARPROC cee_bt_proc(HMODULE module, const char *name) { return GetProcAddress(module, name); }

static int cee_bt_win_load(void) {
    if (cee_bt_win.state == 0) {
        cee_bt_win.state = -1;
        HMODULE module = LoadLibraryA("BluetoothApis.dll");
        if (module == NULL) {
            module = LoadLibraryA("bthprops.cpl");
        }
        WSADATA data;
        if (module != NULL && WSAStartup(MAKEWORD(2, 2), &data) == 0) {
            /* GetProcAddress gives a FARPROC; each is the function of that name's type. */
            FARPROC f[6] = {cee_bt_proc(module, "BluetoothFindFirstRadio"), cee_bt_proc(module, "BluetoothFindRadioClose"),
                            cee_bt_proc(module, "BluetoothFindFirstDevice"), cee_bt_proc(module, "BluetoothFindNextDevice"),
                            cee_bt_proc(module, "BluetoothFindDeviceClose"),
                            cee_bt_proc(module, "BluetoothEnumerateInstalledServices")};
            int all = 1;
            for (int k = 0; k < 6; k++) {
                all = all && f[k] != NULL;
            }
            if (all) {
                memcpy(&cee_bt_win.first_radio, &f[0], sizeof f[0]);
                memcpy(&cee_bt_win.radio_close, &f[1], sizeof f[1]);
                memcpy(&cee_bt_win.first_device, &f[2], sizeof f[2]);
                memcpy(&cee_bt_win.next_device, &f[3], sizeof f[3]);
                memcpy(&cee_bt_win.device_close, &f[4], sizeof f[4]);
                memcpy(&cee_bt_win.services, &f[5], sizeof f[5]);
                cee_bt_win.state = 1;
            }
        }
    }
    return cee_bt_win.state == 1;
}

/* The first radio (its handle, to close), or NULL when there is none: the adapter is off or missing. */
static HANDLE cee_bt_win_radio(void) {
    BLUETOOTH_FIND_RADIO_PARAMS params;
    memset(&params, 0, sizeof params);
    params.dwSize = sizeof params;
    HANDLE radio = NULL;
    HBLUETOOTH_RADIO_FIND find = cee_bt_win.first_radio(&params, &radio);
    if (find == NULL) {
        return NULL;
    }
    cee_bt_win.radio_close(find);
    return radio;
}

/* "00:11:22:AA:BB:CC" from the address's six bytes, the most significant first. */
static void cee_bt_win_text(BTH_ADDR address, char *out) {
    static const char hex[] = "0123456789ABCDEF";
    for (int k = 0; k < 6; k++) {
        unsigned byte = (unsigned)((address >> (8 * (5 - k))) & 0xFF);
        out[k * 3] = hex[byte >> 4];
        out[k * 3 + 1] = hex[byte & 15];
        out[k * 3 + 2] = k < 5 ? ':' : '\0';
    }
}

/* The address from its text, or 0 when it is not one. */
static BTH_ADDR cee_bt_win_address(const char *text, size_t length) {
    if (length != 17) {
        return 0;
    }
    BTH_ADDR address = 0;
    for (size_t k = 0; k < 17; k++) {
        char c = text[k];
        if (k % 3 == 2) {
            if (c != ':' && c != '-') {
                return 0;
            }
            continue;
        }
        int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                     : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (digit < 0) {
            return 0;
        }
        address = (address << 4) | (BTH_ADDR)digit;
    }
    return address;
}

/* Whether a paired device may be a printer: it has the Serial Port Profile installed; or none is known and its class
   is imaging (a printer's), miscellaneous or unclassified. */
static int cee_bt_win_printer(HANDLE radio, const BLUETOOTH_DEVICE_INFO *info) {
    GUID services[32];
    DWORD count = 32;
    if (cee_bt_win.services(radio, info, &count, services) == ERROR_SUCCESS && count > 0) {
        for (DWORD k = 0; k < count && k < 32; k++) {
            if (memcmp(&services[k], &cee_bt_spp, sizeof cee_bt_spp) == 0) {
                return 1;
            }
        }
        return 0;
    }
    ULONG major = (info->ulClassofDevice >> 8) & 0x1F;
    return info->ulClassofDevice != 0 && (major == 6 || major == 0 || major == 0x1F);
}

static int64_t cee_bt_native_list(uint32_t scan, uint32_t milliseconds, uint8_t **out) {
    (void)milliseconds;
    AcquireSRWLockExclusive(&cee_bt_lock);
    int64_t result;
    HANDLE radio = NULL;
    if (!cee_bt_win_load()) {
        result = -CEE_BT_UNSUPPORTED;
    } else if ((radio = cee_bt_win_radio()) == NULL) {
        result = -CEE_BT_OFF;
    } else if (scan) {
        result = 0; /* Low Energy is not yet here (ADR 0177 §5) */
    } else {
        struct cee_bt_list l;
        memset(&l, 0, sizeof l);
        BLUETOOTH_DEVICE_SEARCH_PARAMS params;
        memset(&params, 0, sizeof params);
        params.dwSize = sizeof params;
        params.fReturnAuthenticated = TRUE;
        params.fReturnRemembered = TRUE;
        params.hRadio = radio;
        BLUETOOTH_DEVICE_INFO info;
        memset(&info, 0, sizeof info);
        info.dwSize = sizeof info;
        HBLUETOOTH_DEVICE_FIND find = cee_bt_win.first_device(&params, &info);
        while (find != NULL) {
            if (info.fAuthenticated && cee_bt_win_printer(radio, &info)) {
                char address[18];
                cee_bt_win_text(info.Address.ullLong, address);
                char name[248 * 3];
                int n = WideCharToMultiByte(CP_UTF8, 0, info.szName, -1, name, (int)sizeof name, NULL, NULL);
                cee_bt_add(&l, CEE_BT_CLASSIC, address, 17, name, n > 0 ? (size_t)n - 1 : 0);
            }
            memset(&info, 0, sizeof info);
            info.dwSize = sizeof info;
            if (!cee_bt_win.next_device(find, &info)) {
                cee_bt_win.device_close(find);
                find = NULL;
            }
        }
        result = cee_bt_finish(&l, out);
    }
    if (radio != NULL) {
        CloseHandle(radio);
    }
    ReleaseSRWLockExclusive(&cee_bt_lock);
    return result;
}

static int cee_bt_native_open(struct cee_bt_link *link, uint32_t transport, const char *address, size_t length) {
    if (transport != CEE_BT_CLASSIC) {
        return CEE_BT_UNSUPPORTED; /* Low Energy is not yet here (ADR 0177 §5) */
    }
    BTH_ADDR to = cee_bt_win_address(address, length);
    if (to == 0) {
        return CEE_BT_NOT_FOUND;
    }
    AcquireSRWLockExclusive(&cee_bt_lock);
    int error = 0;
    HANDLE radio = NULL;
    if (!cee_bt_win_load()) {
        error = CEE_BT_UNSUPPORTED;
    } else if ((radio = cee_bt_win_radio()) == NULL) {
        error = CEE_BT_OFF;
    } else {
        SOCKET s = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
        if (s == INVALID_SOCKET) {
            error = CEE_BT_RESOURCES;
        } else {
            SOCKADDR_BTH target;
            memset(&target, 0, sizeof target);
            target.addressFamily = AF_BTH;
            target.btAddr = to;
            target.serviceClassId = cee_bt_spp;
            target.port = 0; /* the channel, found by the service's record */
            if (connect(s, (struct sockaddr *)&target, (int)sizeof target) != 0) {
                int why = WSAGetLastError();
                closesocket(s);
                error = why == WSAEACCES ? CEE_BT_DENIED : why == WSAEADDRNOTAVAIL ? CEE_BT_NOT_FOUND : CEE_BT_REFUSED;
            } else {
                link->device = (void *)(uintptr_t)s;
                link->piece = 0x10000;
            }
        }
    }
    if (radio != NULL) {
        CloseHandle(radio);
    }
    ReleaseSRWLockExclusive(&cee_bt_lock);
    return error;
}

static int32_t cee_bt_native_write(struct cee_bt_link *link, const uint8_t *bytes, size_t length) {
    SOCKET s = (SOCKET)(uintptr_t)link->device;
    while (length > 0) {
        int n = (int)(length < link->piece ? length : link->piece);
        int sent = send(s, (const char *)bytes, n, 0);
        if (sent <= 0) {
            return -CEE_BT_LOST;
        }
        bytes += sent;
        length -= (size_t)sent;
    }
    return 0;
}

static void cee_bt_native_close(struct cee_bt_link *link) {
    SOCKET s = (SOCKET)(uintptr_t)link->device;
    shutdown(s, SD_SEND);
    closesocket(s);
}

#else
/* Linux, FreeBSD and CosmoOS: deferred (ADR 0177 §2.3). */
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

#endif

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
    ceesafe_platform_free((void *)(uintptr_t)list); /* the block ceesafe_bluetooth_list gave */
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
