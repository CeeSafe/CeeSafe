/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * Push tokens (type-system.md §26.23, ADR 0244): the token a server sends this device's pushes to, from the system's
 * push service. Linked only into programs that ask for one (c-backend.md §6).
 *
 * ceesafe_push_token blocks, from any thread but the window's, and writes the token as text: "apns-sandbox:HEX" (an
 * iOS development build), "apns:HEX" (iOS), "fcm:TOKEN" (Android); it returns the length, or -2 Unsupported,
 * -5 Denied, -6 Failed (PushError). iOS: the window runtime's ceesafe_ios_push_token (weak here, so a program with no
 * window has none). Android (ADR 0249): the app's Firebase Messaging, through the runtime's Java class Push; an app
 * without Firebase is Unsupported. macOS and the rest: Unsupported for now (ADR 0244 §2.3).
 *
 * CEESAFE_PUSH_TOKEN=<text> is the token instead, on every system (for tests): its text as it is.
 *
 * A notification service extension (ADR 0246): ceesafe_push_extension_next gives the program the next push iOS handed
 * the extension (its title NUL body NUL payload JSON) and ceesafe_push_extension_deliver shows it rewritten; the window
 * runtime does the work (ceesafe_ios_push_extension_*, weak here). CEESAFE_PUSH_REQUEST=<file> is one push instead, on
 * every system (its title, body and payload JSON on three lines), and a delivery appends `deliver TITLE<TAB>BODY` to
 * CEESAFE_NOTIFICATIONS_DUMP. ceesafe_push_group_folder is an App Group's folder (Apple's systems), the app's files
 * folder on Android (ADR 0263), else -2. On Android the extension is the program the APK's Firebase service starts in
 * its own process (ceesafe_android_push_extension_*, the window runtime's, weak here).
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv */
#endif
#include "ceesafe/rt.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define CEE_PUSH_UNSUPPORTED (-2)
#define CEE_PUSH_FAILED (-6)

int64_t ceesafe_push_token(uint8_t *out, uint64_t capacity);
int64_t ceesafe_push_extension_next(uint32_t milliseconds, uint8_t *out, uint64_t capacity, uint64_t *id);
int32_t ceesafe_push_extension_deliver(uint64_t id, const uint8_t *title, uint64_t title_length, const uint8_t *body,
                                       uint64_t body_length);
int64_t ceesafe_push_group_folder(const uint8_t *group, uint64_t group_length, uint8_t *out, uint64_t capacity);
#if defined(__ANDROID__)
int64_t ceesafe_host_android_files(uint8_t *out, uint64_t capacity); /* ceesafe_rt_io.c */
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE
int64_t ceesafe_ios_push_token(char *out, size_t capacity) __attribute__((weak));
int64_t ceesafe_ios_push_extension_next(uint32_t milliseconds, uint8_t *out, uint64_t capacity, uint64_t *id)
    __attribute__((weak));
int32_t ceesafe_ios_push_extension_deliver(uint64_t id, const uint8_t *title, uint64_t title_length,
                                           const uint8_t *body, uint64_t body_length) __attribute__((weak));

static int64_t cee_push_system(char *out, size_t capacity) {
    return ceesafe_ios_push_token != NULL ? ceesafe_ios_push_token(out, capacity) : CEE_PUSH_UNSUPPORTED;
}

static int64_t cee_push_system_next(uint32_t milliseconds, uint8_t *out, uint64_t capacity, uint64_t *id) {
    return ceesafe_ios_push_extension_next != NULL ? ceesafe_ios_push_extension_next(milliseconds, out, capacity, id)
                                                   : CEE_PUSH_UNSUPPORTED;
}

static int32_t cee_push_system_deliver(uint64_t id, const uint8_t *title, uint64_t title_length, const uint8_t *body,
                                       uint64_t body_length) {
    return ceesafe_ios_push_extension_deliver != NULL
               ? ceesafe_ios_push_extension_deliver(id, title, title_length, body, body_length)
               : CEE_PUSH_UNSUPPORTED;
}
#else
#if defined(__ANDROID__)
/* Android (ADR 0249): org.ceesafe.window.Push (runtime/android), a class of the window runtime's embedded dex, asks
   the app's own Firebase Messaging, found by reflection, for its token; called over JNI from the program's thread,
   attached to the VM for the call. The window runtime gives the VM and the class (weak here). An app without Firebase
   has no token: Unsupported. */
#include <jni.h>

JavaVM *ceesafe_android_vm(void) __attribute__((weak));
jclass ceesafe_android_push(JNIEnv *env, jobject *activity) __attribute__((weak));

static int64_t cee_push_system(char *out, size_t capacity) {
    JavaVM *vm = ceesafe_android_vm != NULL ? ceesafe_android_vm() : NULL;
    if (vm == NULL || ceesafe_android_push == NULL) {
        return CEE_PUSH_UNSUPPORTED;
    }
    JNIEnv *env = NULL;
    int attached = 0;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) {
            return CEE_PUSH_UNSUPPORTED;
        }
        attached = 1;
    }
    int64_t result = CEE_PUSH_UNSUPPORTED;
    if ((*env)->PushLocalFrame(env, 16) == 0) {
        jobject activity = NULL;
        jclass push = ceesafe_android_push(env, &activity);
        jmethodID m = push != NULL ? (*env)->GetStaticMethodID(env, push, "token",
                                                               "(Landroid/content/Context;)Ljava/lang/String;")
                                   : NULL;
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            m = NULL;
        }
        if (m != NULL) {
            jstring token = (jstring)(*env)->CallStaticObjectMethod(env, push, m, activity);
            if ((*env)->ExceptionCheck(env)) {
                (*env)->ExceptionClear(env);
                result = CEE_PUSH_FAILED;
            } else if (token == NULL) {
                result = CEE_PUSH_UNSUPPORTED; /* no Firebase Messaging in the app */
            } else {
                const char *chars = (*env)->GetStringUTFChars(env, token, NULL);
                size_t length = chars != NULL ? strlen(chars) : 0;
                result = chars == NULL || length == 0 || length > capacity ? CEE_PUSH_FAILED : (int64_t)length;
                if (result > 0) {
                    memcpy(out, chars, length);
                }
                if (chars != NULL) {
                    (*env)->ReleaseStringUTFChars(env, token, chars);
                }
            }
        } else if (push != NULL) {
            result = CEE_PUSH_FAILED;
        }
        (void)(*env)->PopLocalFrame(env, NULL);
    }
    if (attached) {
        (void)(*vm)->DetachCurrentThread(vm);
    }
    return result;
}

/* A notification extension (ADR 0263): the program the APK's Firebase service started in its own process; the window
   runtime keeps the pushes the service hands over (weak here, as on iOS). */
int64_t ceesafe_android_push_extension_next(uint32_t milliseconds, uint8_t *out, uint64_t capacity, uint64_t *id)
    __attribute__((weak));
int32_t ceesafe_android_push_extension_deliver(uint64_t id, const uint8_t *title, uint64_t title_length,
                                               const uint8_t *body, uint64_t body_length) __attribute__((weak));

static int64_t cee_push_system_next(uint32_t milliseconds, uint8_t *out, uint64_t capacity, uint64_t *id) {
    return ceesafe_android_push_extension_next != NULL
               ? ceesafe_android_push_extension_next(milliseconds, out, capacity, id)
               : CEE_PUSH_UNSUPPORTED;
}

static int32_t cee_push_system_deliver(uint64_t id, const uint8_t *title, uint64_t title_length, const uint8_t *body,
                                       uint64_t body_length) {
    return ceesafe_android_push_extension_deliver != NULL
               ? ceesafe_android_push_extension_deliver(id, title, title_length, body, body_length)
               : CEE_PUSH_UNSUPPORTED;
}
#else
static int64_t cee_push_system(char *out, size_t capacity) {
    (void)out;
    (void)capacity;
    return CEE_PUSH_UNSUPPORTED;
}

static int64_t cee_push_system_next(uint32_t milliseconds, uint8_t *out, uint64_t capacity, uint64_t *id) {
    (void)milliseconds;
    (void)out;
    (void)capacity;
    (void)id;
    return CEE_PUSH_UNSUPPORTED;
}

static int32_t cee_push_system_deliver(uint64_t id, const uint8_t *title, uint64_t title_length, const uint8_t *body,
                                       uint64_t body_length) {
    (void)id;
    (void)title;
    (void)title_length;
    (void)body;
    (void)body_length;
    return CEE_PUSH_UNSUPPORTED;
}
#endif
#endif

/* ---- CEESAFE_PUSH_REQUEST: one push from a file, for tests ---- */
static int cee_push_test_taken;

static const char *cee_push_test_file(void) {
    const char *file = getenv("CEESAFE_PUSH_REQUEST");
    return file != NULL && file[0] != 0 ? file : NULL;
}

/* The file's three lines as a record (title NUL body NUL payload), once; then nothing. */
static int64_t cee_push_test_next(const char *file, uint8_t *out, uint64_t capacity, uint64_t *id) {
    if (cee_push_test_taken) {
        return 0;
    }
    FILE *f = fopen(file, "rb");
    if (f == NULL) {
        return -6;
    }
    size_t n = fread(out, 1, (size_t)capacity, f);
    fclose(f);
    int lines = 0;
    for (size_t i = 0; i < n && lines < 2; i++) {
        if (out[i] == '\n') {
            out[i] = 0;
            lines++;
        }
    }
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) {
        n--;
    }
    if (lines < 2) {
        return -6;
    }
    cee_push_test_taken = 1;
    *id = 1;
    return (int64_t)n;
}

static int32_t cee_push_test_deliver(const uint8_t *title, uint64_t title_length, const uint8_t *body,
                                     uint64_t body_length) {
    const char *dump = getenv("CEESAFE_NOTIFICATIONS_DUMP");
    if (dump == NULL || dump[0] == 0) {
        return 0;
    }
    FILE *f = fopen(dump, "ab");
    if (f == NULL) {
        return -6;
    }
    fputs("deliver ", f);
    fwrite(title, 1, (size_t)title_length, f);
    fputc('\t', f);
    fwrite(body, 1, (size_t)body_length, f);
    fputc('\n', f);
    return fclose(f) == 0 ? 0 : -6;
}

#if defined(__APPLE__)
#include <dlfcn.h>
/* The App Group's container: NSFileManager's containerURLForSecurityApplicationGroupIdentifier:, through the
   Objective-C runtime. Its path's length, or -2 when there is none for this program (not entitled, or no such group). */
static int64_t cee_push_apple_group(const char *group, uint8_t *out, uint64_t capacity) {
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL);
    (void)dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW | RTLD_GLOBAL);
    if (objc == NULL) {
        return CEE_PUSH_UNSUPPORTED;
    }
    void *(*cls)(const char *) = (void *(*)(const char *))dlsym(objc, "objc_getClass");
    void *(*sel)(const char *) = (void *(*)(const char *))dlsym(objc, "sel_registerName");
    void (*send)(void) = (void (*)(void))dlsym(objc, "objc_msgSend");
    void *(*push)(void) = (void *(*)(void))dlsym(objc, "objc_autoreleasePoolPush");
    void (*pop)(void *) = (void (*)(void *))dlsym(objc, "objc_autoreleasePoolPop");
    if (cls == NULL || sel == NULL || send == NULL || push == NULL || pop == NULL || cls("NSFileManager") == NULL) {
        return CEE_PUSH_UNSUPPORTED;
    }
    void *pool = push();
    void *string = ((void *(*)(void *, void *, const char *))send)(cls("NSString"), sel("stringWithUTF8String:"), group);
    void *manager = ((void *(*)(void *, void *))send)(cls("NSFileManager"), sel("defaultManager"));
    void *url = string != NULL && manager != NULL
                    ? ((void *(*)(void *, void *, void *))send)(
                          manager, sel("containerURLForSecurityApplicationGroupIdentifier:"), string)
                    : NULL;
    void *path = url != NULL ? ((void *(*)(void *, void *))send)(url, sel("path")) : NULL;
    const char *text = path != NULL ? ((const char *(*)(void *, void *))send)(path, sel("UTF8String")) : NULL;
    int64_t result = CEE_PUSH_UNSUPPORTED;
    if (text != NULL) {
        size_t n = strlen(text);
        if (n <= capacity) {
            memcpy(out, text, n);
            result = (int64_t)n;
        } else {
            result = -6;
        }
    }
    pop(pool);
    return result;
}
#endif

int64_t ceesafe_push_token(uint8_t *out, uint64_t capacity) {
    const char *test = getenv("CEESAFE_PUSH_TOKEN");
    if (test != NULL && test[0] != 0) {
        size_t length = strlen(test);
        if (length > capacity) {
            return CEE_PUSH_FAILED;
        }
        memcpy(out, test, length);
        return (int64_t)length;
    }
    return cee_push_system((char *)out, (size_t)capacity);
}

int64_t ceesafe_push_extension_next(uint32_t milliseconds, uint8_t *out, uint64_t capacity, uint64_t *id) {
    const char *test = cee_push_test_file();
    return test != NULL ? cee_push_test_next(test, out, capacity, id)
                        : cee_push_system_next(milliseconds, out, capacity, id);
}

int32_t ceesafe_push_extension_deliver(uint64_t id, const uint8_t *title, uint64_t title_length, const uint8_t *body,
                                       uint64_t body_length) {
    if (cee_push_test_file() != NULL) {
        return id == 1 ? cee_push_test_deliver(title, title_length, body, body_length) : -1;
    }
    return cee_push_system_deliver(id, title, title_length, body, body_length);
}

int64_t ceesafe_push_group_folder(const uint8_t *group, uint64_t group_length, uint8_t *out, uint64_t capacity) {
    char name[256];
    if (group_length == 0 || group_length >= sizeof name || memchr(group, 0, (size_t)group_length) != NULL) {
        return -3;
    }
    memcpy(name, group, (size_t)group_length);
    name[group_length] = 0;
#if defined(__APPLE__)
    return cee_push_apple_group(name, out, capacity);
#elif defined(__ANDROID__)
    /* Android has no groups: the app's processes (its program and a notification extension's, ADR 0263 §2.5) share
       its files folder, which stands for any group. */
    (void)name;
    return ceesafe_host_android_files(out, capacity);
#else
    (void)out;
    (void)capacity;
    return CEE_PUSH_UNSUPPORTED;
#endif
}
