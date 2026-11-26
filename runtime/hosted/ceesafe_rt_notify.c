/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * Local notifications (type-system.md §26.22, ADR 0243): the system's notifications, asked for, shown, replaced by
 * id and cleared. Linked only into programs that use them (c-backend.md §6).
 *
 * Every function blocks and may be called from any thread but the window's. lib/std has checked the id and the
 * texts (Notifications.checkId); they are checked again here, cheaply. Results: 0 done, or -2 Unsupported,
 * -3 InvalidInput, -5 Denied, -6 Failed (NotificationError).
 *
 * iOS, and macOS in a bundled app: UNUserNotificationCenter, UserNotifications.framework loaded with dlopen and driven
 * through the Objective-C runtime; its completion handlers are blocks made here as constants (no captures), one call
 * at a time. Android: org.ceesafe.window.Notify (runtime/android), called over JNI. Elsewhere Unsupported.
 *
 * CEESAFE_NOTIFICATIONS_DUMP=<file> appends the calls to the file instead, on every system: `permission`,
 * `show ID<TAB>TITLE<TAB>BODY`, `clear ID`, one a line, a tab, a newline and a backslash in a text written `\t`, `\n`
 * and `\\`; every call is then 0.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* fopen, getenv */
#endif
#include "ceesafe/rt.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CEE_NOTE_UNSUPPORTED (-2)
#define CEE_NOTE_INVALID (-3)
#define CEE_NOTE_DENIED (-5)
#define CEE_NOTE_FAILED (-6)

#define CEE_NOTE_ID_LIMIT 64u
#define CEE_NOTE_TITLE_LIMIT 256u
#define CEE_NOTE_BODY_LIMIT 4096u

int32_t ceesafe_notify_permission(void);
int32_t ceesafe_notify_show(const uint8_t *id, uint64_t id_length, const uint8_t *title, uint64_t title_length,
                            const uint8_t *body, uint64_t body_length);
int32_t ceesafe_notify_clear(const uint8_t *id, uint64_t id_length);

/* The id as a C string, checked as lib/std's Notifications.checkId does; 0 when it is not one. */
static int cee_note_id(const uint8_t *id, uint64_t length, char out[CEE_NOTE_ID_LIMIT + 1]) {
    if (length == 0 || length > CEE_NOTE_ID_LIMIT) {
        return 0;
    }
    for (uint64_t i = 0; i < length; i++) {
        uint8_t b = id[i];
        int fits = (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '.' ||
                   b == '-' || b == '_' || b == ':';
        if (!fits) {
            return 0;
        }
    }
    memcpy(out, id, (size_t)length);
    out[length] = 0;
    return 1;
}

/* A text as a C string in a block of its own (the caller frees it), or NULL when it is too long or holds a zero
   byte. */
static char *cee_note_text(const uint8_t *text, uint64_t length, uint64_t limit) {
    if (length > limit || (length > 0 && memchr(text, 0, (size_t)length) != NULL)) {
        return NULL;
    }
    char *out = (char *)malloc((size_t)length + 1);
    if (out != NULL) {
        if (length > 0) {
            memcpy(out, text, (size_t)length);
        }
        out[length] = 0;
    }
    return out;
}

/* ---- CEESAFE_NOTIFICATIONS_DUMP ----------------------------------------------------------------------------------- */

static const char *cee_note_dump_file(void) {
    const char *file = getenv("CEESAFE_NOTIFICATIONS_DUMP");
    return file != NULL && file[0] != 0 ? file : NULL;
}

static void cee_note_escaped(FILE *f, const char *text) {
    for (const char *p = text; *p != 0; p++) {
        if (*p == '\t') {
            fputs("\\t", f);
        } else if (*p == '\n') {
            fputs("\\n", f);
        } else if (*p == '\\') {
            fputs("\\\\", f);
        } else {
            fputc(*p, f);
        }
    }
}

/* Appends one line: `what`, then the id and the texts that are not NULL, a tab before each but the id. */
static int32_t cee_note_dump(const char *file, const char *what, const char *id, const char *title, const char *body) {
    FILE *f = fopen(file, "ab");
    if (f == NULL) {
        return CEE_NOTE_FAILED;
    }
    fputs(what, f);
    if (id != NULL) {
        fputc(' ', f);
        fputs(id, f);
    }
    if (title != NULL) {
        fputc('\t', f);
        cee_note_escaped(f, title);
    }
    if (body != NULL) {
        fputc('\t', f);
        cee_note_escaped(f, body);
    }
    fputc('\n', f);
    return fclose(f) == 0 ? 0 : CEE_NOTE_FAILED;
}

/* ---- the system's notifications --------------------------------------------------------------------------------- */

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

typedef void *cee_note_obj;
typedef void *cee_note_sel;

static struct {
    int loaded; /* 1 loaded, -1 failed */
    cee_note_obj (*cls)(const char *);
    cee_note_sel (*sel)(const char *);
    void (*send)(void); /* objc_msgSend, cast to each call's signature */
    void *(*pool_push)(void);
    void (*pool_pop)(void *);
    void *global_block; /* _NSConcreteGlobalBlock */
} cee_note;

/* One call at a time: the blocks below are constants, and their answer is in `cee_note_answer`. */
static pthread_mutex_t cee_note_call = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cee_note_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cee_note_done = PTHREAD_COND_INITIALIZER;
static struct {
    int answered;
    long value;   /* granted (permission), the authorization status (settings) */
    int failed;   /* an NSError came */
} cee_note_answer;

struct cee_note_block_descriptor {
    unsigned long reserved, size;
};
struct cee_note_block {
    void *isa;
    int flags, reserved;
    void *invoke; /* the function, its bytes copied in: each block's has another signature */
    const struct cee_note_block_descriptor *descriptor;
};
static const struct cee_note_block_descriptor cee_note_descriptor = {0ul, sizeof(struct cee_note_block)};

static void cee_note_answered(long value, int failed) {
    pthread_mutex_lock(&cee_note_lock);
    cee_note_answer.value = value;
    cee_note_answer.failed = failed;
    cee_note_answer.answered = 1;
    pthread_cond_broadcast(&cee_note_done);
    pthread_mutex_unlock(&cee_note_lock);
}

/* requestAuthorizationWithOptions:completionHandler: — (BOOL granted, NSError *error) */
static void cee_note_granted(void *block, signed char granted, void *error) {
    (void)block;
    cee_note_answered(granted ? 1 : 0, error != NULL);
}
/* getNotificationSettingsWithCompletionHandler: — (UNNotificationSettings *settings) */
static void cee_note_settings(void *block, cee_note_obj settings);
/* addNotificationRequest:withCompletionHandler: — (NSError *error) */
static void cee_note_added(void *block, void *error) {
    (void)block;
    cee_note_answered(0, error != NULL);
}

static struct cee_note_block cee_note_granted_block;
static struct cee_note_block cee_note_settings_block;
static struct cee_note_block cee_note_added_block;

static void cee_note_block_init(struct cee_note_block *b) {
    b->isa = cee_note.global_block;
    b->flags = 1 << 28; /* BLOCK_IS_GLOBAL */
    b->reserved = 0;
    b->descriptor = &cee_note_descriptor;
}

static int cee_note_load(void) {
    pthread_mutex_lock(&cee_note_lock);
    if (cee_note.loaded == 0) {
        cee_note.loaded = -1;
        void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL);
        void *foundation = dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW | RTLD_GLOBAL);
        void *framework = dlopen("/System/Library/Frameworks/UserNotifications.framework/UserNotifications",
                                 RTLD_NOW | RTLD_GLOBAL);
        if (objc != NULL && foundation != NULL && framework != NULL) {
            cee_note.cls = (cee_note_obj(*)(const char *))dlsym(objc, "objc_getClass");
            cee_note.sel = (cee_note_sel(*)(const char *))dlsym(objc, "sel_registerName");
            cee_note.send = (void (*)(void))dlsym(objc, "objc_msgSend");
            cee_note.pool_push = (void *(*)(void))dlsym(objc, "objc_autoreleasePoolPush");
            cee_note.pool_pop = (void (*)(void *))dlsym(objc, "objc_autoreleasePoolPop");
            cee_note.global_block = dlsym(RTLD_DEFAULT, "_NSConcreteGlobalBlock");
            if (cee_note.cls != NULL && cee_note.sel != NULL && cee_note.send != NULL && cee_note.pool_push != NULL &&
                cee_note.pool_pop != NULL && cee_note.global_block != NULL &&
                cee_note.cls("UNUserNotificationCenter") != NULL) {
                /* ISO C has no function-to-object pointer conversion: the pointers' bytes are copied, as the window
                   runtime copies a JNINativeMethod's. */
                void (*granted)(void *, signed char, void *) = cee_note_granted;
                void (*settings)(void *, cee_note_obj) = cee_note_settings;
                void (*added)(void *, void *) = cee_note_added;
                cee_note_block_init(&cee_note_granted_block);
                cee_note_block_init(&cee_note_settings_block);
                cee_note_block_init(&cee_note_added_block);
                memcpy(&cee_note_granted_block.invoke, &granted, sizeof granted);
                memcpy(&cee_note_settings_block.invoke, &settings, sizeof settings);
                memcpy(&cee_note_added_block.invoke, &added, sizeof added);
                cee_note.loaded = 1;
            }
        }
    }
    int loaded = cee_note.loaded == 1;
    pthread_mutex_unlock(&cee_note_lock);
    return loaded;
}

static cee_note_obj cee_note_msg(cee_note_obj target, const char *selector) {
    return ((cee_note_obj(*)(cee_note_obj, cee_note_sel))cee_note.send)(target, cee_note.sel(selector));
}

static cee_note_obj cee_note_msg1(cee_note_obj target, const char *selector, cee_note_obj argument) {
    return ((cee_note_obj(*)(cee_note_obj, cee_note_sel, cee_note_obj))cee_note.send)(target, cee_note.sel(selector),
                                                                                      argument);
}

/* An NSString of `text` (UTF-8), autoreleased; NULL when it cannot be made. */
static cee_note_obj cee_note_string(const char *text) {
    cee_note_obj made = cee_note_msg(cee_note.cls("NSString"), "alloc");
    made = ((cee_note_obj(*)(cee_note_obj, cee_note_sel, const void *, unsigned long, unsigned long))cee_note.send)(
        made, cee_note.sel("initWithBytes:length:encoding:"), text, (unsigned long)strlen(text), 4ul /* UTF-8 */);
    return made != NULL ? cee_note_msg(made, "autorelease") : NULL;
}

/* Whether this process has a bundle identifier: UNUserNotificationCenter refuses (raises) a process without one. */
static int cee_note_bundled(void) {
    cee_note_obj bundle = cee_note_msg(cee_note.cls("NSBundle"), "mainBundle");
    return bundle != NULL && cee_note_msg(bundle, "bundleIdentifier") != NULL;
}

static cee_note_obj cee_note_center(void) {
    return cee_note_msg(cee_note.cls("UNUserNotificationCenter"), "currentNotificationCenter");
}

static void cee_note_settings(void *block, cee_note_obj settings) {
    (void)block;
    long status = settings != NULL
                      ? ((long (*)(cee_note_obj, cee_note_sel))cee_note.send)(settings, cee_note.sel("authorizationStatus"))
                      : 0;
    cee_note_answered(status, settings == NULL);
}

/* Starts a call whose block answers, and waits for the answer (`seconds` at most): 1 answered. Called with
   cee_note_call held. */
static int cee_note_wait(int seconds) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += seconds;
    pthread_mutex_lock(&cee_note_lock);
    while (!cee_note_answer.answered) {
        if (pthread_cond_timedwait(&cee_note_done, &cee_note_lock, &until) == ETIMEDOUT) {
            break;
        }
    }
    int answered = cee_note_answer.answered;
    pthread_mutex_unlock(&cee_note_lock);
    return answered;
}

static void cee_note_ask(void) {
    pthread_mutex_lock(&cee_note_lock);
    cee_note_answer.answered = 0;
    cee_note_answer.value = 0;
    cee_note_answer.failed = 0;
    pthread_mutex_unlock(&cee_note_lock);
}

/* The authorization status (UNAuthorizationStatus: 0 not determined, 1 denied, 2 authorized, 3 provisional,
   4 ephemeral), or -1 when it did not come. Called with cee_note_call held. */
static long cee_note_status(cee_note_obj center) {
    cee_note_ask();
    ((void (*)(cee_note_obj, cee_note_sel, void *))cee_note.send)(
        center, cee_note.sel("getNotificationSettingsWithCompletionHandler:"), &cee_note_settings_block);
    return cee_note_wait(10) && !cee_note_answer.failed ? cee_note_answer.value : -1;
}

/* Runs `call` (one of the three below) with the framework loaded, a bundle, the call lock and a pool. */
static int32_t cee_note_apple(int32_t (*call)(cee_note_obj, const char *, const char *, const char *),
                              const char *id, const char *title, const char *body) {
    if (!cee_note_load()) {
        return CEE_NOTE_UNSUPPORTED;
    }
    pthread_mutex_lock(&cee_note_call);
    void *pool = cee_note.pool_push();
    int32_t result = CEE_NOTE_UNSUPPORTED;
    if (cee_note_bundled()) {
        cee_note_obj center = cee_note_center();
        result = center != NULL ? call(center, id, title, body) : CEE_NOTE_FAILED;
    }
    cee_note.pool_pop(pool);
    pthread_mutex_unlock(&cee_note_call);
    return result;
}

static int32_t cee_note_apple_permission(cee_note_obj center, const char *id, const char *title, const char *body) {
    (void)id;
    (void)title;
    (void)body;
    long status = cee_note_status(center);
    if (status == 2 || status == 3 || status == 4) {
        return 0;
    }
    if (status == 1) {
        return CEE_NOTE_DENIED;
    }
    cee_note_ask();
    ((void (*)(cee_note_obj, cee_note_sel, unsigned long, void *))cee_note.send)(
        center, cee_note.sel("requestAuthorizationWithOptions:completionHandler:"), 7ul /* badge, sound, alert */,
        &cee_note_granted_block);
    if (!cee_note_wait(120)) { /* the user is still deciding */
        return CEE_NOTE_DENIED;
    }
    return cee_note_answer.value ? 0 : cee_note_answer.failed ? CEE_NOTE_FAILED : CEE_NOTE_DENIED;
}

static int32_t cee_note_apple_show(cee_note_obj center, const char *id, const char *title, const char *body) {
    long status = cee_note_status(center);
    if (status == 0 || status == 1) {
        return CEE_NOTE_DENIED;
    }
    cee_note_obj content = cee_note_msg(cee_note_msg(cee_note.cls("UNMutableNotificationContent"), "alloc"), "init");
    cee_note_obj ident = cee_note_string(id);
    cee_note_obj heading = cee_note_string(title);
    cee_note_obj text = cee_note_string(body);
    if (content == NULL || ident == NULL || heading == NULL || text == NULL) {
        return CEE_NOTE_FAILED;
    }
    (void)cee_note_msg(content, "autorelease");
    (void)cee_note_msg1(content, "setTitle:", heading);
    (void)cee_note_msg1(content, "setBody:", text);
    (void)cee_note_msg1(content, "setSound:", cee_note_msg(cee_note.cls("UNNotificationSound"), "defaultSound"));
    cee_note_obj request = ((cee_note_obj(*)(cee_note_obj, cee_note_sel, cee_note_obj, cee_note_obj, cee_note_obj))
                                cee_note.send)(cee_note.cls("UNNotificationRequest"),
                                               cee_note.sel("requestWithIdentifier:content:trigger:"), ident, content,
                                               NULL);
    if (request == NULL) {
        return CEE_NOTE_FAILED;
    }
    cee_note_ask();
    ((void (*)(cee_note_obj, cee_note_sel, cee_note_obj, void *))cee_note.send)(
        center, cee_note.sel("addNotificationRequest:withCompletionHandler:"), request, &cee_note_added_block);
    return cee_note_wait(10) && !cee_note_answer.failed ? 0 : CEE_NOTE_FAILED;
}

static int32_t cee_note_apple_clear(cee_note_obj center, const char *id, const char *title, const char *body) {
    (void)title;
    (void)body;
    cee_note_obj ident = cee_note_string(id);
    cee_note_obj list = ident != NULL ? cee_note_msg1(cee_note.cls("NSArray"), "arrayWithObject:", ident) : NULL;
    if (list == NULL) {
        return CEE_NOTE_FAILED;
    }
    (void)cee_note_msg1(center, "removeDeliveredNotificationsWithIdentifiers:", list);
    (void)cee_note_msg1(center, "removePendingNotificationRequestsWithIdentifiers:", list);
    return 0;
}

static int32_t cee_note_system_permission(void) {
    return cee_note_apple(cee_note_apple_permission, NULL, NULL, NULL);
}

static int32_t cee_note_system_show(const char *id, const char *title, const char *body) {
    return cee_note_apple(cee_note_apple_show, id, title, body);
}

static int32_t cee_note_system_clear(const char *id) {
    return cee_note_apple(cee_note_apple_clear, id, NULL, NULL);
}

#elif defined(__ANDROID__)
/* Android: org.ceesafe.window.Notify (runtime/android), a class of the window runtime's embedded dex, called over JNI
   from the program's thread, attached to the VM for the call. The window runtime gives the VM, the activity and the
   class (ceesafe_android_vm, ceesafe_android_notify); they are weak here, so a program that opens no window links
   without them and has no notifications (Unsupported). */
#include <jni.h>

JavaVM *ceesafe_android_vm(void) __attribute__((weak));
jclass ceesafe_android_notify(JNIEnv *env, jobject *activity) __attribute__((weak));
jstring ceesafe_android_string(JNIEnv *env, const char *utf8, size_t length) __attribute__((weak));

/* One call of Notify's `method` (`signature`: the activity, then `strings` of its Strings): its int answer, or a
   code. */
static int32_t cee_note_call(const char *method, const char *signature, int strings, const char *a, const char *b,
                             const char *c) {
    JavaVM *vm = ceesafe_android_vm != NULL ? ceesafe_android_vm() : NULL;
    if (vm == NULL) {
        return CEE_NOTE_UNSUPPORTED;
    }
    JNIEnv *env = NULL;
    int attached = 0;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) {
            return CEE_NOTE_UNSUPPORTED;
        }
        attached = 1;
    }
    int32_t result = CEE_NOTE_UNSUPPORTED;
    if ((*env)->PushLocalFrame(env, 16) == 0) {
        jobject activity = NULL;
        jclass notify = ceesafe_android_notify != NULL ? ceesafe_android_notify(env, &activity) : NULL;
        jmethodID m = notify != NULL ? (*env)->GetStaticMethodID(env, notify, method, signature) : NULL;
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            m = NULL;
        }
        jstring s[3] = {NULL, NULL, NULL};
        const char *texts[3] = {a, b, c};
        int made = 1;
        for (int k = 0; k < strings; k++) {
            /* Standard UTF-8, which NewStringUTF (modified UTF-8) refuses for a character beyond the BMP (an emoji). */
            s[k] = ceesafe_android_string != NULL ? ceesafe_android_string(env, texts[k], strlen(texts[k]))
                                                  : (*env)->NewStringUTF(env, texts[k]);
            made = made && s[k] != NULL;
        }
        if (m != NULL && made && !(*env)->ExceptionCheck(env)) {
            jint answer = strings == 0   ? (*env)->CallStaticIntMethod(env, notify, m, activity)
                          : strings == 1 ? (*env)->CallStaticIntMethod(env, notify, m, activity, s[0])
                                         : (*env)->CallStaticIntMethod(env, notify, m, activity, s[0], s[1], s[2]);
            if ((*env)->ExceptionCheck(env)) {
                (*env)->ExceptionClear(env);
                result = CEE_NOTE_FAILED;
            } else {
                result = (int32_t)answer;
            }
        } else if (notify != NULL) {
            result = CEE_NOTE_FAILED;
        }
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
        (void)(*env)->PopLocalFrame(env, NULL);
    }
    if (attached) {
        (void)(*vm)->DetachCurrentThread(vm);
    }
    return result;
}

static int32_t cee_note_system_permission(void) {
    return cee_note_call("permission", "(Landroid/content/Context;)I", 0, NULL, NULL, NULL);
}

static int32_t cee_note_system_show(const char *id, const char *title, const char *body) {
    return cee_note_call("show", "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I", 3,
                         id, title, body);
}

static int32_t cee_note_system_clear(const char *id) {
    return cee_note_call("clear", "(Landroid/content/Context;Ljava/lang/String;)I", 1, id, NULL, NULL);
}

#else
/* Windows, Linux, FreeBSD and the rest: none yet (ADR 0243 §6.1). */
static int32_t cee_note_system_permission(void) {
    return CEE_NOTE_UNSUPPORTED;
}

static int32_t cee_note_system_show(const char *id, const char *title, const char *body) {
    (void)id;
    (void)title;
    (void)body;
    return CEE_NOTE_UNSUPPORTED;
}

static int32_t cee_note_system_clear(const char *id) {
    (void)id;
    return CEE_NOTE_UNSUPPORTED;
}
#endif

/* ---- the entry points -------------------------------------------------------------------------------------------- */

int32_t ceesafe_notify_permission(void) {
    const char *dump = cee_note_dump_file();
    return dump != NULL ? cee_note_dump(dump, "permission", NULL, NULL, NULL) : cee_note_system_permission();
}

int32_t ceesafe_notify_show(const uint8_t *id, uint64_t id_length, const uint8_t *title, uint64_t title_length,
                            const uint8_t *body, uint64_t body_length) {
    char ident[CEE_NOTE_ID_LIMIT + 1];
    if (!cee_note_id(id, id_length, ident)) {
        return CEE_NOTE_INVALID;
    }
    char *heading = cee_note_text(title, title_length, CEE_NOTE_TITLE_LIMIT);
    char *text = cee_note_text(body, body_length, CEE_NOTE_BODY_LIMIT);
    int32_t result = CEE_NOTE_INVALID;
    if (heading != NULL && text != NULL) {
        const char *dump = cee_note_dump_file();
        result = dump != NULL ? cee_note_dump(dump, "show", ident, heading, text)
                              : cee_note_system_show(ident, heading, text);
    }
    free(heading);
    free(text);
    return result;
}

int32_t ceesafe_notify_clear(const uint8_t *id, uint64_t id_length) {
    char ident[CEE_NOTE_ID_LIMIT + 1];
    if (!cee_note_id(id, id_length, ident)) {
        return CEE_NOTE_INVALID;
    }
    const char *dump = cee_note_dump_file();
    return dump != NULL ? cee_note_dump(dump, "clear", ident, NULL, NULL) : cee_note_system_clear(ident);
}
