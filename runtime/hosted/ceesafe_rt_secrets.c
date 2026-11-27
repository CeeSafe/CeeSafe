/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * The system's secret store (type-system.md §26.21, ADR 0242): an app's secrets, by the app's name and the secret's,
 * where the system keeps them encrypted. Linked only into programs that use it (c-backend.md §6).
 *
 * Every function blocks and may be called from any thread. lib/std has checked the names and the value's length
 * (Secrets.check); they are checked again here, cheaply. Results: a read's length (>= 0), or one of the codes below,
 * which lib/std maps to SecretError: -1 none (a read only), -2 Unsupported, -3 InvalidInput, -4 Locked, -5 Denied,
 * -6 Failed. Write and remove return 0 or a code.
 *
 * macOS and iOS: a keychain item (generic password, service = app, account = name), this device only, after the first
 * unlock; Security.framework and CoreFoundation loaded with dlopen. Android: org.ceesafe.window.Secrets (runtime/android),
 * an AES-256-GCM key in the Android Keystore sealing a file per value, called over JNI. Windows: DPAPI for the current
 * user, a file per value under %LOCALAPPDATA%\<app>\secrets, crypt32 loaded with LoadLibrary. Elsewhere Unsupported.
 *
 * CEESAFE_SECRETS_DIR=<folder> keeps the values as plain files <folder>/<app>/<name> instead, on every system (for
 * tests: they are not protected).
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv */
#endif
#include "ceesafe/rt.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CEE_SEC_NONE (-1)
#define CEE_SEC_UNSUPPORTED (-2)
#define CEE_SEC_INVALID (-3)
#define CEE_SEC_LOCKED (-4)
#define CEE_SEC_DENIED (-5)
#define CEE_SEC_FAILED (-6)

#define CEE_SEC_NAME_LIMIT 128u
#define CEE_SEC_VALUE_LIMIT 65536u

int64_t ceesafe_secrets_read(const uint8_t *app, uint64_t app_length, const uint8_t *name, uint64_t name_length,
                             uint8_t *out, uint64_t capacity);
int32_t ceesafe_secrets_write(const uint8_t *app, uint64_t app_length, const uint8_t *name, uint64_t name_length,
                              const uint8_t *value, uint64_t length);
int32_t ceesafe_secrets_remove(const uint8_t *app, uint64_t app_length, const uint8_t *name, uint64_t name_length);
/* The same in a group shared with the app's extensions (ADR 0246): on Apple's systems the keychain access group
   `group` (an App Group, or a team-prefixed keychain group); elsewhere the group changes nothing. */
int64_t ceesafe_secrets_group_read(const uint8_t *group, uint64_t group_length, const uint8_t *app, uint64_t app_length,
                                   const uint8_t *name, uint64_t name_length, uint8_t *out, uint64_t capacity);
int32_t ceesafe_secrets_group_write(const uint8_t *group, uint64_t group_length, const uint8_t *app,
                                    uint64_t app_length, const uint8_t *name, uint64_t name_length,
                                    const uint8_t *value, uint64_t length);
int32_t ceesafe_secrets_group_remove(const uint8_t *group, uint64_t group_length, const uint8_t *app,
                                     uint64_t app_length, const uint8_t *name, uint64_t name_length);

/* The names as C strings, checked as lib/std's Secrets.check does; 0 when they are not names the store takes. */
static int cee_sec_names(const uint8_t *app, uint64_t app_length, const uint8_t *name, uint64_t name_length,
                         char app_text[CEE_SEC_NAME_LIMIT + 1], char name_text[CEE_SEC_NAME_LIMIT + 1]) {
    if (app_length == 0 || app_length > CEE_SEC_NAME_LIMIT || name_length == 0 || name_length > CEE_SEC_NAME_LIMIT) {
        return 0;
    }
    for (uint64_t i = 0; i < app_length; i++) {
        if (app[i] == '/' || app[i] == '\\' || app[i] == 0) {
            return 0;
        }
    }
    for (uint64_t i = 0; i < name_length; i++) {
        uint8_t b = name[i];
        int fits = (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '.' ||
                   b == '-' || b == '_';
        if (!fits) {
            return 0;
        }
    }
    memcpy(app_text, app, (size_t)app_length);
    app_text[app_length] = 0;
    memcpy(name_text, name, (size_t)name_length);
    name_text[name_length] = 0;
    if (strcmp(app_text, ".") == 0 || strcmp(app_text, "..") == 0 || strcmp(name_text, ".") == 0 ||
        strcmp(name_text, "..") == 0) {
        return 0;
    }
    return 1;
}

/* ---- files: CEESAFE_SECRETS_DIR's plain files, and the sealed files of Windows ---------------------------------- */

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* `text` (UTF-8) as UTF-16 in `out` of `room` wide characters; 0 when it does not fit. */
static int cee_sec_wide(const char *text, wchar_t *out, int room) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out, room);
    return n > 0;
}

static int cee_sec_make_dir(const char *path) {
    wchar_t wide[1024];
    if (!cee_sec_wide(path, wide, 1024)) {
        return 0;
    }
    return CreateDirectoryW(wide, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

/* The file's bytes into `out` (at most `capacity`): its length, CEE_SEC_NONE when missing, else CEE_SEC_FAILED. */
static int64_t cee_sec_file_read(const char *path, uint8_t *out, uint64_t capacity) {
    wchar_t wide[1024];
    if (!cee_sec_wide(path, wide, 1024)) {
        return CEE_SEC_FAILED;
    }
    HANDLE h = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? CEE_SEC_NONE : CEE_SEC_FAILED;
    }
    uint64_t done = 0;
    int64_t result = 0;
    for (;;) {
        DWORD got = 0;
        DWORD want = (DWORD)(capacity - done > 65536u ? 65536u : capacity - done);
        if (want == 0) {
            uint8_t extra;
            if (ReadFile(h, &extra, 1, &got, NULL) && got != 0) {
                result = CEE_SEC_FAILED; /* longer than any value */
            }
            break;
        }
        if (!ReadFile(h, out + done, want, &got, NULL)) {
            result = CEE_SEC_FAILED;
            break;
        }
        if (got == 0) {
            break;
        }
        done += got;
    }
    CloseHandle(h);
    return result != 0 ? result : (int64_t)done;
}

/* Writes `path` whole: a temporary file beside it, then moved over it. */
static int32_t cee_sec_file_write(const char *path, const uint8_t *bytes, uint64_t length) {
    char temporary[1100];
    wchar_t wide[1100];
    wchar_t wide_temporary[1100];
    if (snprintf(temporary, sizeof temporary, "%s.part", path) >= (int)sizeof temporary ||
        !cee_sec_wide(path, wide, 1100) || !cee_sec_wide(temporary, wide_temporary, 1100)) {
        return CEE_SEC_FAILED;
    }
    HANDLE h = CreateFileW(wide_temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return CEE_SEC_FAILED;
    }
    DWORD wrote = 0;
    int ok = length == 0 || (WriteFile(h, bytes, (DWORD)length, &wrote, NULL) && wrote == (DWORD)length);
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (!ok || !MoveFileExW(wide_temporary, wide, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        (void)DeleteFileW(wide_temporary);
        return CEE_SEC_FAILED;
    }
    return 0;
}

static int32_t cee_sec_file_remove(const char *path) {
    wchar_t wide[1024];
    if (!cee_sec_wide(path, wide, 1024)) {
        return CEE_SEC_FAILED;
    }
    if (DeleteFileW(wide)) {
        return 0;
    }
    DWORD e = GetLastError();
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? 0 : CEE_SEC_FAILED;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int cee_sec_make_dir(const char *path) {
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

static int64_t cee_sec_file_read(const char *path, uint8_t *out, uint64_t capacity) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return errno == ENOENT ? CEE_SEC_NONE : CEE_SEC_FAILED;
    }
    uint64_t done = 0;
    int64_t result = 0;
    for (;;) {
        if (done == capacity) {
            uint8_t extra;
            ssize_t n = read(fd, &extra, 1);
            if (n != 0) {
                result = CEE_SEC_FAILED; /* longer than any value, or unreadable */
            }
            break;
        }
        ssize_t n = read(fd, out + done, (size_t)(capacity - done));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            result = CEE_SEC_FAILED;
            break;
        }
        if (n == 0) {
            break;
        }
        done += (uint64_t)n;
    }
    (void)close(fd);
    return result != 0 ? result : (int64_t)done;
}

static int32_t cee_sec_file_write(const char *path, const uint8_t *bytes, uint64_t length) {
    char temporary[1100];
    if (snprintf(temporary, sizeof temporary, "%s.part", path) >= (int)sizeof temporary) {
        return CEE_SEC_FAILED;
    }
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return CEE_SEC_FAILED;
    }
    uint64_t done = 0;
    while (done < length) {
        ssize_t n = write(fd, bytes + done, (size_t)(length - done));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        done += (uint64_t)n;
    }
    int synced = fsync(fd) == 0;
    int closed = close(fd) == 0;
    if (done != length || !synced || !closed || rename(temporary, path) != 0) {
        (void)unlink(temporary);
        return CEE_SEC_FAILED;
    }
    return 0;
}

static int32_t cee_sec_file_remove(const char *path) {
    return unlink(path) == 0 || errno == ENOENT ? 0 : CEE_SEC_FAILED;
}
#endif

/* `<dir>/<app>` made if missing, and the value's path `<dir>/<app>/<name>` in `path`; 0 when it cannot be. */
static int cee_sec_path(const char *dir, const char *app, const char *name, int make, char *path, size_t room) {
    char folder[1024];
    if (snprintf(folder, sizeof folder, "%s/%s", dir, app) >= (int)sizeof folder ||
        snprintf(path, room, "%s/%s", folder, name) >= (int)room) {
        return 0;
    }
    return !make || cee_sec_make_dir(folder);
}

/* CEESAFE_SECRETS_DIR, or NULL. */
static const char *cee_sec_test_dir(void) {
    const char *dir = getenv("CEESAFE_SECRETS_DIR");
    return dir != NULL && dir[0] != 0 ? dir : NULL;
}

static int64_t cee_sec_dir_read(const char *dir, const char *app, const char *name, uint8_t *out, uint64_t capacity) {
    char path[1100];
    return cee_sec_path(dir, app, name, 0, path, sizeof path) ? cee_sec_file_read(path, out, capacity) : CEE_SEC_FAILED;
}

static int32_t cee_sec_dir_write(const char *dir, const char *app, const char *name, const uint8_t *v, uint64_t n) {
    char path[1100];
    return cee_sec_path(dir, app, name, 1, path, sizeof path) ? cee_sec_file_write(path, v, n) : CEE_SEC_FAILED;
}

static int32_t cee_sec_dir_remove(const char *dir, const char *app, const char *name) {
    char path[1100];
    return cee_sec_path(dir, app, name, 0, path, sizeof path) ? cee_sec_file_remove(path) : CEE_SEC_FAILED;
}

/* ---- the system's store ------------------------------------------------------------------------------------------ */

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>

typedef const void *cee_cf;
typedef long cee_cf_index;
typedef int32_t cee_os_status;

static struct {
    int loaded; /* 1 loaded, -1 failed */
    int login;  /* macOS: the data protection keychain is not this program's; the login keychain is used */
    cee_cf (*dictionary)(cee_cf, cee_cf_index, const void *, const void *);
    void (*set)(cee_cf, cee_cf, cee_cf);
    cee_cf (*string)(cee_cf, const uint8_t *, cee_cf_index, uint32_t, unsigned char);
    cee_cf (*data)(cee_cf, const uint8_t *, cee_cf_index);
    cee_cf_index (*data_length)(cee_cf);
    const uint8_t *(*data_bytes)(cee_cf);
    void (*release)(cee_cf);
    const void *key_callbacks;
    const void *value_callbacks;
    cee_cf yes;
    cee_os_status (*copy_matching)(cee_cf, cee_cf *);
    cee_os_status (*add)(cee_cf, cee_cf *);
    cee_os_status (*update)(cee_cf, cee_cf);
    cee_os_status (*remove)(cee_cf);
    cee_cf cls, generic, service, account, value, return_data, match_limit, limit_one, accessible, after_unlock,
        data_protection, access_group, return_attributes;
    cee_cf (*get)(cee_cf, cee_cf);
    unsigned char (*c_string)(cee_cf, char *, cee_cf_index, uint32_t);
    int own_known;     /* 1: own_group found (empty when there is none to name) */
    char own_group[256]; /* the app's default access group: what a call without a group names (ADR 0246) */
} cee_sec;

static pthread_mutex_t cee_sec_lock = PTHREAD_MUTEX_INITIALIZER;

/* A constant of a framework's: the CFTypeRef its symbol holds. */
static cee_cf cee_sec_constant(void *library, const char *symbol) {
    const cee_cf *at = (const cee_cf *)dlsym(library, symbol);
    return at != NULL ? *at : NULL;
}

static int cee_sec_load(void) {
    pthread_mutex_lock(&cee_sec_lock);
    if (cee_sec.loaded == 0) {
        cee_sec.loaded = -1;
        void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW | RTLD_GLOBAL);
        void *sec = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_NOW | RTLD_GLOBAL);
        if (cf != NULL && sec != NULL) {
            cee_sec.dictionary = (cee_cf(*)(cee_cf, cee_cf_index, const void *, const void *))dlsym(
                cf, "CFDictionaryCreateMutable");
            cee_sec.set = (void (*)(cee_cf, cee_cf, cee_cf))dlsym(cf, "CFDictionarySetValue");
            cee_sec.string = (cee_cf(*)(cee_cf, const uint8_t *, cee_cf_index, uint32_t, unsigned char))dlsym(
                cf, "CFStringCreateWithBytes");
            cee_sec.data = (cee_cf(*)(cee_cf, const uint8_t *, cee_cf_index))dlsym(cf, "CFDataCreate");
            cee_sec.data_length = (cee_cf_index(*)(cee_cf))dlsym(cf, "CFDataGetLength");
            cee_sec.data_bytes = (const uint8_t *(*)(cee_cf))dlsym(cf, "CFDataGetBytePtr");
            cee_sec.release = (void (*)(cee_cf))dlsym(cf, "CFRelease");
            cee_sec.key_callbacks = dlsym(cf, "kCFTypeDictionaryKeyCallBacks");
            cee_sec.value_callbacks = dlsym(cf, "kCFTypeDictionaryValueCallBacks");
            cee_sec.yes = cee_sec_constant(cf, "kCFBooleanTrue");
            cee_sec.copy_matching = (cee_os_status(*)(cee_cf, cee_cf *))dlsym(sec, "SecItemCopyMatching");
            cee_sec.add = (cee_os_status(*)(cee_cf, cee_cf *))dlsym(sec, "SecItemAdd");
            cee_sec.update = (cee_os_status(*)(cee_cf, cee_cf))dlsym(sec, "SecItemUpdate");
            cee_sec.remove = (cee_os_status(*)(cee_cf))dlsym(sec, "SecItemDelete");
            cee_sec.cls = cee_sec_constant(sec, "kSecClass");
            cee_sec.generic = cee_sec_constant(sec, "kSecClassGenericPassword");
            cee_sec.service = cee_sec_constant(sec, "kSecAttrService");
            cee_sec.account = cee_sec_constant(sec, "kSecAttrAccount");
            cee_sec.value = cee_sec_constant(sec, "kSecValueData");
            cee_sec.return_data = cee_sec_constant(sec, "kSecReturnData");
            cee_sec.match_limit = cee_sec_constant(sec, "kSecMatchLimit");
            cee_sec.limit_one = cee_sec_constant(sec, "kSecMatchLimitOne");
            cee_sec.accessible = cee_sec_constant(sec, "kSecAttrAccessible");
            cee_sec.after_unlock = cee_sec_constant(sec, "kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly");
            cee_sec.data_protection = cee_sec_constant(sec, "kSecUseDataProtectionKeychain");
            cee_sec.access_group = cee_sec_constant(sec, "kSecAttrAccessGroup");
            cee_sec.return_attributes = cee_sec_constant(sec, "kSecReturnAttributes");
            cee_sec.get = (cee_cf(*)(cee_cf, cee_cf))dlsym(cf, "CFDictionaryGetValue");
            cee_sec.c_string = (unsigned char (*)(cee_cf, char *, cee_cf_index, uint32_t))dlsym(cf, "CFStringGetCString");
            if (cee_sec.dictionary != NULL && cee_sec.set != NULL && cee_sec.string != NULL && cee_sec.data != NULL &&
                cee_sec.data_length != NULL && cee_sec.data_bytes != NULL && cee_sec.release != NULL &&
                cee_sec.key_callbacks != NULL && cee_sec.value_callbacks != NULL && cee_sec.yes != NULL &&
                cee_sec.copy_matching != NULL && cee_sec.add != NULL && cee_sec.update != NULL &&
                cee_sec.remove != NULL && cee_sec.cls != NULL && cee_sec.generic != NULL && cee_sec.service != NULL &&
                cee_sec.account != NULL && cee_sec.value != NULL && cee_sec.return_data != NULL &&
                cee_sec.match_limit != NULL && cee_sec.limit_one != NULL && cee_sec.accessible != NULL &&
                cee_sec.after_unlock != NULL) {
                cee_sec.loaded = 1;
            }
        }
    }
    int loaded = cee_sec.loaded == 1;
    pthread_mutex_unlock(&cee_sec_lock);
    return loaded;
}

#define CEE_SEC_UTF8 0x08000100u /* kCFStringEncodingUTF8 */
#define CEE_SEC_NOT_FOUND (-25300)
#define CEE_SEC_DUPLICATE (-25299)
#define CEE_SEC_INTERACTION_NOT_ALLOWED (-25308)
#define CEE_SEC_AUTH_FAILED (-25293)
#define CEE_SEC_USER_CANCELED (-128)
#define CEE_SEC_MISSING_ENTITLEMENT (-34018)

/* The query naming the item (app, name), in the keychain `login` says (macOS), or NULL; the caller releases it. */
static const char *cee_sec_own(int login);

static cee_cf cee_sec_query(const char *app, const char *name, int login, const char *group) {
    if (group == NULL) {
        group = cee_sec_own(login);
    }
    cee_cf q = cee_sec.dictionary(NULL, 0, cee_sec.key_callbacks, cee_sec.value_callbacks);
    cee_cf service = cee_sec.string(NULL, (const uint8_t *)app, (cee_cf_index)strlen(app), CEE_SEC_UTF8, 0);
    cee_cf account = cee_sec.string(NULL, (const uint8_t *)name, (cee_cf_index)strlen(name), CEE_SEC_UTF8, 0);
    if (q == NULL || service == NULL || account == NULL) {
        if (q != NULL) cee_sec.release(q);
        if (service != NULL) cee_sec.release(service);
        if (account != NULL) cee_sec.release(account);
        return NULL;
    }
    cee_sec.set(q, cee_sec.cls, cee_sec.generic);
    cee_sec.set(q, cee_sec.service, service);
    cee_sec.set(q, cee_sec.account, account);
    cee_sec.release(service);
    cee_sec.release(account);
    if (group != NULL && cee_sec.access_group != NULL) {
        cee_cf shared = cee_sec.string(NULL, (const uint8_t *)group, (cee_cf_index)strlen(group), CEE_SEC_UTF8, 0);
        if (shared != NULL) {
            cee_sec.set(q, cee_sec.access_group, shared);
            cee_sec.release(shared);
        }
    }
#if !TARGET_OS_IPHONE
    if (!login && cee_sec.data_protection != NULL) {
        cee_sec.set(q, cee_sec.data_protection, cee_sec.yes);
    }
#else
    (void)login;
#endif
    return q;
}

/* The app's own access group (iOS, and macOS's data protection keychain), which a call without a group names, so that
   it reads, writes and removes the app's own item only: without a group, SecItemDelete takes an item of the same
   service and account in every group the app reaches, a shared group's too (ADR 0246). It is the group the system puts
   an item in when none is named: a probe item is added, its group read back, and the probe removed. NULL in the login
   keychain (it has no access groups) or when it cannot be found (calls then name none, as before). */
static const char *cee_sec_own(int login) {
    if (login || cee_sec.access_group == NULL || cee_sec.return_attributes == NULL || cee_sec.get == NULL ||
        cee_sec.c_string == NULL) {
        return NULL;
    }
    pthread_mutex_lock(&cee_sec_lock);
    int known = cee_sec.own_known;
    pthread_mutex_unlock(&cee_sec_lock);
    if (!known) {
        char found[256] = {0};
        static const char probe_service[] = "ceesafe.access-group-probe";
        cee_cf q = cee_sec.dictionary(NULL, 0, cee_sec.key_callbacks, cee_sec.value_callbacks);
        cee_cf service = cee_sec.string(NULL, (const uint8_t *)probe_service, (cee_cf_index)(sizeof probe_service - 1),
                                        CEE_SEC_UTF8, 0);
        cee_cf data = cee_sec.data(NULL, (const uint8_t *)"0", 1);
        if (q != NULL && service != NULL && data != NULL) {
            cee_sec.set(q, cee_sec.cls, cee_sec.generic);
            cee_sec.set(q, cee_sec.service, service);
            cee_sec.set(q, cee_sec.account, service);
#if !TARGET_OS_IPHONE
            if (cee_sec.data_protection != NULL) {
                cee_sec.set(q, cee_sec.data_protection, cee_sec.yes);
            }
#endif
            cee_cf add = cee_sec.dictionary(NULL, 0, cee_sec.key_callbacks, cee_sec.value_callbacks);
            if (add != NULL) {
                /* A copy of the query with the value and the wish for its attributes. */
                cee_sec.set(add, cee_sec.cls, cee_sec.generic);
                cee_sec.set(add, cee_sec.service, service);
                cee_sec.set(add, cee_sec.account, service);
#if !TARGET_OS_IPHONE
                if (cee_sec.data_protection != NULL) {
                    cee_sec.set(add, cee_sec.data_protection, cee_sec.yes);
                }
#endif
                cee_sec.set(add, cee_sec.value, data);
                cee_sec.set(add, cee_sec.accessible, cee_sec.after_unlock);
                cee_sec.set(add, cee_sec.return_attributes, cee_sec.yes);
                cee_cf result = NULL;
                cee_os_status status = cee_sec.add(add, &result);
                if (status == CEE_SEC_DUPLICATE) {
                    cee_sec.set(q, cee_sec.return_attributes, cee_sec.yes);
                    status = cee_sec.copy_matching(q, &result);
                }
                cee_cf group = status == 0 && result != NULL ? cee_sec.get(result, cee_sec.access_group) : NULL;
                if (group != NULL && !cee_sec.c_string(group, found, (cee_cf_index)sizeof found, CEE_SEC_UTF8)) {
                    found[0] = 0;
                }
                if (result != NULL) {
                    cee_sec.release(result);
                }
                cee_sec.release(add);
                cee_cf gone = cee_sec.dictionary(NULL, 0, cee_sec.key_callbacks, cee_sec.value_callbacks);
                if (gone != NULL) {
                    cee_sec.set(gone, cee_sec.cls, cee_sec.generic);
                    cee_sec.set(gone, cee_sec.service, service);
                    cee_sec.set(gone, cee_sec.account, service);
#if !TARGET_OS_IPHONE
                    if (cee_sec.data_protection != NULL) {
                        cee_sec.set(gone, cee_sec.data_protection, cee_sec.yes);
                    }
#endif
                    (void)cee_sec.remove(gone);
                    cee_sec.release(gone);
                }
            }
        }
        if (q != NULL) cee_sec.release(q);
        if (service != NULL) cee_sec.release(service);
        if (data != NULL) cee_sec.release(data);
        pthread_mutex_lock(&cee_sec_lock);
        memcpy(cee_sec.own_group, found, sizeof found);
        cee_sec.own_known = 1;
        pthread_mutex_unlock(&cee_sec_lock);
    }
    return cee_sec.own_group[0] != 0 ? cee_sec.own_group : NULL;
}

static int64_t cee_sec_code(cee_os_status status) {
    switch (status) {
    case CEE_SEC_NOT_FOUND:
        return CEE_SEC_NONE;
    case CEE_SEC_INTERACTION_NOT_ALLOWED:
        return CEE_SEC_LOCKED;
    case CEE_SEC_AUTH_FAILED:
    case CEE_SEC_USER_CANCELED:
    case CEE_SEC_MISSING_ENTITLEMENT:
        return CEE_SEC_DENIED;
    default:
        return CEE_SEC_FAILED;
    }
}

/* macOS: whether the keychain to use is the login keychain, found by the first call (the data protection keychain
   answers errSecMissingEntitlement to a program without a keychain group). iOS has only the one. */
static int cee_sec_login(void) {
#if TARGET_OS_IPHONE
    return 0;
#else
    pthread_mutex_lock(&cee_sec_lock);
    int login = cee_sec.login;
    pthread_mutex_unlock(&cee_sec_lock);
    return login;
#endif
}

static int cee_sec_fall_back(cee_os_status status) {
#if TARGET_OS_IPHONE
    (void)status;
    return 0;
#else
    if (status != CEE_SEC_MISSING_ENTITLEMENT || cee_sec.data_protection == NULL) {
        return 0;
    }
    pthread_mutex_lock(&cee_sec_lock);
    int was = cee_sec.login;
    cee_sec.login = 1;
    pthread_mutex_unlock(&cee_sec_lock);
    return !was;
#endif
}

static int64_t cee_sec_system_read(const char *group, const char *app, const char *name, uint8_t *out,
                                   uint64_t capacity) {
    if (!cee_sec_load()) {
        return CEE_SEC_UNSUPPORTED;
    }
    for (;;) {
        cee_cf q = cee_sec_query(app, name, cee_sec_login(), group);
        if (q == NULL) {
            return CEE_SEC_FAILED;
        }
        cee_sec.set(q, cee_sec.return_data, cee_sec.yes);
        cee_sec.set(q, cee_sec.match_limit, cee_sec.limit_one);
        cee_cf found = NULL;
        cee_os_status status = cee_sec.copy_matching(q, &found);
        cee_sec.release(q);
        if (status != 0) {
            if (cee_sec_fall_back(status)) {
                continue;
            }
            return cee_sec_code(status);
        }
        if (found == NULL) {
            return CEE_SEC_FAILED;
        }
        cee_cf_index length = cee_sec.data_length(found);
        int64_t result = CEE_SEC_FAILED;
        if (length >= 0 && (uint64_t)length <= capacity) {
            if (length > 0) {
                memcpy(out, cee_sec.data_bytes(found), (size_t)length);
            }
            result = (int64_t)length;
        }
        cee_sec.release(found);
        return result;
    }
}

static int32_t cee_sec_system_write(const char *group, const char *app, const char *name, const uint8_t *value,
                                    uint64_t length) {
    if (!cee_sec_load()) {
        return CEE_SEC_UNSUPPORTED;
    }
    for (;;) {
        int login = cee_sec_login();
        cee_cf q = cee_sec_query(app, name, login, group);
        cee_cf data = cee_sec.data(NULL, value, (cee_cf_index)length);
        cee_cf change = cee_sec.dictionary(NULL, 0, cee_sec.key_callbacks, cee_sec.value_callbacks);
        if (q == NULL || data == NULL || change == NULL) {
            if (q != NULL) cee_sec.release(q);
            if (data != NULL) cee_sec.release(data);
            if (change != NULL) cee_sec.release(change);
            return CEE_SEC_FAILED;
        }
        cee_sec.set(change, cee_sec.value, data);
        cee_os_status status = cee_sec.update(q, change);
        if (status == CEE_SEC_NOT_FOUND) {
            cee_sec.set(q, cee_sec.value, data);
            if (!login) {
                cee_sec.set(q, cee_sec.accessible, cee_sec.after_unlock);
            }
            status = cee_sec.add(q, NULL);
            if (status == CEE_SEC_DUPLICATE) { /* another thread made it between the two calls */
                cee_sec.release(q);
                q = cee_sec_query(app, name, login, group);
                status = q != NULL ? cee_sec.update(q, change) : -1;
            }
        }
        if (q != NULL) cee_sec.release(q);
        cee_sec.release(data);
        cee_sec.release(change);
        if (status != 0 && cee_sec_fall_back(status)) {
            continue;
        }
        return status == 0 ? 0 : (int32_t)(cee_sec_code(status) == CEE_SEC_NONE ? CEE_SEC_FAILED : cee_sec_code(status));
    }
}

static int32_t cee_sec_system_remove(const char *group, const char *app, const char *name) {
    if (!cee_sec_load()) {
        return CEE_SEC_UNSUPPORTED;
    }
    for (;;) {
        cee_cf q = cee_sec_query(app, name, cee_sec_login(), group);
        if (q == NULL) {
            return CEE_SEC_FAILED;
        }
        cee_os_status status = cee_sec.remove(q);
        cee_sec.release(q);
        if (status == 0 || status == CEE_SEC_NOT_FOUND) {
            return 0;
        }
        if (cee_sec_fall_back(status)) {
            continue;
        }
        return (int32_t)cee_sec_code(status);
    }
}

/* ---- Device keys (ADR 0275) on Apple's systems ----
   A P-256 key made in the Secure Enclave (kSecAttrTokenIDSecureEnclave; usable after the first unlock, on this device
   only, without the user's presence), never exportable; where the Enclave cannot hold it (an unsigned macOS program
   in the login keychain, a simulator), a key in the keychain instead, `hardware` 0. Its tag is "<app>.<name>". The
   public half is the SPKI of its X9.63 point; a signature is ECDSA with SHA-256, DER. A new key is attested with
   App Attest where the system has it: a new App Attest key, attesting SHA-256(challenge || SPKI). */
static struct {
    int loaded; /* 1 loaded, -1 failed */
    cee_cf (*random_key)(cee_cf, cee_cf *);
    cee_cf (*copy_public)(cee_cf);
    cee_cf (*external)(cee_cf, cee_cf *);
    cee_cf (*sign)(cee_cf, cee_cf, cee_cf, cee_cf *);
    cee_cf (*access)(cee_cf, cee_cf, unsigned long, cee_cf *);
    cee_cf (*number)(cee_cf, cee_cf_index, const void *);
    cee_cf_index (*error_code)(cee_cf);
    cee_cf key_class, tag, key_type, ec, key_kind, private_kind, return_ref, size, token, enclave, private_attrs,
        permanent, access_control, algorithm;
} cee_key;

static int cee_key_load(void) {
    if (!cee_sec_load()) {
        return 0;
    }
    pthread_mutex_lock(&cee_sec_lock);
    if (cee_key.loaded == 0) {
        cee_key.loaded = -1;
        void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW | RTLD_GLOBAL);
        void *sec = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_NOW | RTLD_GLOBAL);
        if (cf != NULL && sec != NULL) {
            cee_key.random_key = (cee_cf(*)(cee_cf, cee_cf *))dlsym(sec, "SecKeyCreateRandomKey");
            cee_key.copy_public = (cee_cf(*)(cee_cf))dlsym(sec, "SecKeyCopyPublicKey");
            cee_key.external = (cee_cf(*)(cee_cf, cee_cf *))dlsym(sec, "SecKeyCopyExternalRepresentation");
            cee_key.sign = (cee_cf(*)(cee_cf, cee_cf, cee_cf, cee_cf *))dlsym(sec, "SecKeyCreateSignature");
            cee_key.access = (cee_cf(*)(cee_cf, cee_cf, unsigned long, cee_cf *))dlsym(
                sec, "SecAccessControlCreateWithFlags");
            cee_key.number = (cee_cf(*)(cee_cf, cee_cf_index, const void *))dlsym(cf, "CFNumberCreate");
            cee_key.error_code = (cee_cf_index(*)(cee_cf))dlsym(cf, "CFErrorGetCode");
            cee_key.key_class = cee_sec_constant(sec, "kSecClassKey");
            cee_key.tag = cee_sec_constant(sec, "kSecAttrApplicationTag");
            cee_key.key_type = cee_sec_constant(sec, "kSecAttrKeyType");
            cee_key.ec = cee_sec_constant(sec, "kSecAttrKeyTypeECSECPrimeRandom");
            cee_key.key_kind = cee_sec_constant(sec, "kSecAttrKeyClass");
            cee_key.private_kind = cee_sec_constant(sec, "kSecAttrKeyClassPrivate");
            cee_key.return_ref = cee_sec_constant(sec, "kSecReturnRef");
            cee_key.size = cee_sec_constant(sec, "kSecAttrKeySizeInBits");
            cee_key.token = cee_sec_constant(sec, "kSecAttrTokenID");
            cee_key.enclave = cee_sec_constant(sec, "kSecAttrTokenIDSecureEnclave");
            cee_key.private_attrs = cee_sec_constant(sec, "kSecPrivateKeyAttrs");
            cee_key.permanent = cee_sec_constant(sec, "kSecAttrIsPermanent");
            cee_key.access_control = cee_sec_constant(sec, "kSecAttrAccessControl");
            cee_key.algorithm = cee_sec_constant(sec, "kSecKeyAlgorithmECDSASignatureMessageX962SHA256");
            if (cee_key.random_key != NULL && cee_key.copy_public != NULL && cee_key.external != NULL &&
                cee_key.sign != NULL && cee_key.access != NULL && cee_key.number != NULL && cee_key.error_code != NULL &&
                cee_key.key_class != NULL && cee_key.tag != NULL && cee_key.key_type != NULL && cee_key.ec != NULL &&
                cee_key.key_kind != NULL && cee_key.private_kind != NULL && cee_key.return_ref != NULL &&
                cee_key.size != NULL && cee_key.token != NULL && cee_key.enclave != NULL &&
                cee_key.private_attrs != NULL && cee_key.permanent != NULL && cee_key.access_control != NULL &&
                cee_key.algorithm != NULL) {
                cee_key.loaded = 1;
            }
        }
    }
    int loaded = cee_key.loaded == 1;
    pthread_mutex_unlock(&cee_sec_lock);
    return loaded;
}

static cee_cf cee_key_dictionary(void) {
    return cee_sec.dictionary(NULL, 0, cee_sec.key_callbacks, cee_sec.value_callbacks);
}

/* The tag "<app>.<name>" as CFData; the caller releases it. */
static cee_cf cee_key_tag(const char *app, const char *name) {
    char tag[CEE_SEC_NAME_LIMIT * 2 + 2];
    int n = snprintf(tag, sizeof tag, "%s.%s", app, name);
    return n > 0 && (size_t)n < sizeof tag ? cee_sec.data(NULL, (const uint8_t *)tag, (cee_cf_index)n) : NULL;
}

/* The private key of the tag, or NULL (*status why); the caller releases it. */
static cee_cf cee_key_find(cee_cf tag, int login, cee_os_status *status) {
    cee_cf q = cee_key_dictionary();
    if (q == NULL) {
        *status = -1;
        return NULL;
    }
    cee_sec.set(q, cee_sec.cls, cee_key.key_class);
    cee_sec.set(q, cee_key.tag, tag);
    cee_sec.set(q, cee_key.key_type, cee_key.ec);
    cee_sec.set(q, cee_key.key_kind, cee_key.private_kind);
    cee_sec.set(q, cee_key.return_ref, cee_sec.yes);
#if !TARGET_OS_IPHONE
    if (!login && cee_sec.data_protection != NULL) {
        cee_sec.set(q, cee_sec.data_protection, cee_sec.yes);
    }
#else
    (void)login;
#endif
    cee_cf key = NULL;
    *status = cee_sec.copy_matching(q, &key);
    cee_sec.release(q);
    return *status == 0 ? key : NULL;
}

/* A new private key of the tag: in the Secure Enclave when `enclave`, else in the keychain. NULL when it cannot be
   made (*why the system's code); the caller releases it. */
static cee_cf cee_key_make(cee_cf tag, int login, int enclave, cee_os_status *why) {
    cee_cf attrs = cee_key_dictionary();
    cee_cf private_attrs = cee_key_dictionary();
    int bits = 256;
    cee_cf size = cee_key.number(NULL, 3 /* kCFNumberSInt32Type */, &bits);
    /* After the first unlock, this device only; the Enclave's key may sign (kSecAccessControlPrivateKeyUsage), with no
       presence asked: the key signs in the background (ADR 0275 §6.3). */
    cee_cf access = cee_key.access(NULL, cee_sec.after_unlock, enclave ? (1ul << 30) : 0ul, NULL);
    cee_cf key = NULL;
    if (attrs != NULL && private_attrs != NULL && size != NULL && access != NULL) {
        cee_sec.set(attrs, cee_key.key_type, cee_key.ec);
        cee_sec.set(attrs, cee_key.size, size);
        if (enclave) {
            cee_sec.set(attrs, cee_key.token, cee_key.enclave);
        }
#if !TARGET_OS_IPHONE
        if (!login && cee_sec.data_protection != NULL) {
            cee_sec.set(attrs, cee_sec.data_protection, cee_sec.yes);
        }
#else
        (void)login;
#endif
        cee_sec.set(private_attrs, cee_key.permanent, cee_sec.yes);
        cee_sec.set(private_attrs, cee_key.tag, tag);
        if (!login) {
            cee_sec.set(private_attrs, cee_key.access_control, access);
        }
        cee_sec.set(attrs, cee_key.private_attrs, private_attrs);
        cee_cf error = NULL;
        key = cee_key.random_key(attrs, &error);
        if (error != NULL) {
            *why = (cee_os_status)cee_key.error_code(error);
            cee_sec.release(error);
        }
    }
    if (attrs != NULL) cee_sec.release(attrs);
    if (private_attrs != NULL) cee_sec.release(private_attrs);
    if (size != NULL) cee_sec.release(size);
    if (access != NULL) cee_sec.release(access);
    return key;
}

/* The SubjectPublicKeyInfo of a P-256 key: the algorithm's header, then the 65-byte uncompressed point. */
static const uint8_t cee_key_spki_header[26] = {0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2A, 0x86, 0x48,
                                                0xCE, 0x3D, 0x02, 0x01, 0x06, 0x08, 0x2A, 0x86, 0x48,
                                                0xCE, 0x3D, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00};

/* The key's public half as SPKI into `out` (91 bytes): its length, or a code. */
static int64_t cee_key_spki(cee_cf key, uint8_t *out, uint64_t capacity) {
    cee_cf pub = cee_key.copy_public(key);
    cee_cf point = pub != NULL ? cee_key.external(pub, NULL) : NULL;
    int64_t result = CEE_SEC_FAILED;
    if (point != NULL && cee_sec.data_length(point) == 65 && capacity >= 91) {
        memcpy(out, cee_key_spki_header, 26);
        memcpy(out + 26, cee_sec.data_bytes(point), 65);
        result = 91;
    }
    if (point != NULL) cee_sec.release(point);
    if (pub != NULL) cee_sec.release(pub);
    return result;
}

/* -- App Attest (DeviceCheck), through the Objective-C runtime: one at a time, its answers from blocks without
   captures, waited for. */
typedef void *cee_objc_id;
struct cee_key_block_descriptor {
    unsigned long reserved, size;
};
struct cee_key_block {
    void *isa;
    int flags, reserved;
    void (*invoke)(void *, cee_objc_id, cee_objc_id);
    const struct cee_key_block_descriptor *descriptor;
};
static struct {
    void (*send)(void);
    cee_objc_id (*cls)(const char *);
    void *(*sel)(const char *);
    volatile int state; /* 0 waiting, 1 answered, 2 failed */
    cee_objc_id key_id; /* the App Attest key's id, retained */
    uint8_t *attestation;
    size_t attestation_length;
} cee_attest;

static cee_objc_id cee_attest_msg(cee_objc_id o, const char *s) {
    return ((cee_objc_id(*)(cee_objc_id, void *))cee_attest.send)(o, cee_attest.sel(s));
}

static void cee_attest_generated(void *block, cee_objc_id key_id, cee_objc_id error) {
    (void)block;
    if (key_id != NULL && error == NULL) {
        cee_attest.key_id = cee_attest_msg(key_id, "retain");
        cee_attest.state = 1;
    } else {
        cee_attest.state = 2;
    }
}

static void cee_attest_attested(void *block, cee_objc_id data, cee_objc_id error) {
    (void)block;
    if (data != NULL && error == NULL) {
        size_t n = ((size_t(*)(cee_objc_id, void *))cee_attest.send)(data, cee_attest.sel("length"));
        const void *bytes = ((const void *(*)(cee_objc_id, void *))cee_attest.send)(data, cee_attest.sel("bytes"));
        cee_attest.attestation = n > 0 && bytes != NULL ? (uint8_t *)malloc(n) : NULL;
        if (cee_attest.attestation != NULL) {
            memcpy(cee_attest.attestation, bytes, n);
            cee_attest.attestation_length = n;
        }
        cee_attest.state = cee_attest.attestation != NULL ? 1 : 2;
    } else {
        cee_attest.state = 2;
    }
}

static const struct cee_key_block_descriptor cee_attest_descriptor = {0ul, sizeof(struct cee_key_block)};
static struct cee_key_block cee_attest_generate_block = {NULL, 1 << 28, 0, cee_attest_generated,
                                                         &cee_attest_descriptor};
static struct cee_key_block cee_attest_attest_block = {NULL, 1 << 28, 0, cee_attest_attested, &cee_attest_descriptor};

/* Waits for a block's answer, a minute at most: 1 when it came. */
static int cee_attest_wait(void) {
    for (int k = 0; k < 6000 && cee_attest.state == 0; k++) {
        struct timespec pause = {0, 10000000L};
        nanosleep(&pause, NULL);
    }
    return cee_attest.state == 1;
}

/* App Attest's attestation of SHA-256(challenge || spki), malloc'd into *out, or NULL where the system has none (an
   app without the App Attest entitlement, macOS, a simulator) or it failed: a key without one is still a key. */
static uint8_t *cee_key_attest(const uint8_t *challenge, size_t challenge_length, const uint8_t *spki,
                               size_t spki_length, size_t *length) {
    static pthread_mutex_t one = PTHREAD_MUTEX_INITIALIZER;
    *length = 0;
    (void)dlopen("/System/Library/Frameworks/DeviceCheck.framework/DeviceCheck", RTLD_NOW | RTLD_GLOBAL);
    unsigned char *(*sha256)(const void *, uint32_t, unsigned char *) =
        (unsigned char *(*)(const void *, uint32_t, unsigned char *))dlsym(RTLD_DEFAULT, "CC_SHA256");
    cee_attest.send = (void (*)(void))dlsym(RTLD_DEFAULT, "objc_msgSend");
    cee_attest.cls = (cee_objc_id(*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
    cee_attest.sel = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
    void *global = dlsym(RTLD_DEFAULT, "_NSConcreteGlobalBlock");
    if (sha256 == NULL || cee_attest.send == NULL || cee_attest.cls == NULL || cee_attest.sel == NULL ||
        global == NULL || cee_attest.cls("DCAppAttestService") == NULL || challenge_length + spki_length > 4096) {
        return NULL;
    }
    pthread_mutex_lock(&one);
    uint8_t *result = NULL;
    void *(*pool_push)(void) = (void *(*)(void))dlsym(RTLD_DEFAULT, "objc_autoreleasePoolPush");
    void (*pool_pop)(void *) = (void (*)(void *))dlsym(RTLD_DEFAULT, "objc_autoreleasePoolPop");
    void *pool = pool_push != NULL ? pool_push() : NULL;
    cee_objc_id service = cee_attest_msg(cee_attest.cls("DCAppAttestService"), "sharedService");
    int supported = service != NULL &&
                    ((signed char (*)(cee_objc_id, void *))cee_attest.send)(service, cee_attest.sel("isSupported"));
    if (supported) {
        cee_attest_generate_block.isa = global;
        cee_attest_attest_block.isa = global;
        cee_attest.state = 0;
        cee_attest.key_id = NULL;
        ((void (*)(cee_objc_id, void *, void *))cee_attest.send)(
            service, cee_attest.sel("generateKeyWithCompletionHandler:"), &cee_attest_generate_block);
        if (cee_attest_wait()) {
            uint8_t both[4096];
            unsigned char hash[32];
            memcpy(both, challenge, challenge_length);
            memcpy(both + challenge_length, spki, spki_length);
            (void)sha256(both, (uint32_t)(challenge_length + spki_length), hash);
            cee_objc_id data = ((cee_objc_id(*)(cee_objc_id, void *, const void *, unsigned long))cee_attest.send)(
                cee_attest.cls("NSData"), cee_attest.sel("dataWithBytes:length:"), hash, 32ul);
            cee_attest.state = 0;
            cee_attest.attestation = NULL;
            cee_attest.attestation_length = 0;
            ((void (*)(cee_objc_id, void *, cee_objc_id, cee_objc_id, void *))cee_attest.send)(
                service, cee_attest.sel("attestKey:clientDataHash:completionHandler:"), cee_attest.key_id, data,
                &cee_attest_attest_block);
            if (cee_attest_wait()) {
                result = cee_attest.attestation;
                *length = cee_attest.attestation_length;
            }
            (void)cee_attest_msg(cee_attest.key_id, "release");
        }
    }
    if (pool != NULL && pool_pop != NULL) {
        pool_pop(pool);
    }
    pthread_mutex_unlock(&one);
    return result;
}

static void cee_key_put32(uint8_t *at, uint32_t v) {
    at[0] = (uint8_t)(v >> 24);
    at[1] = (uint8_t)(v >> 16);
    at[2] = (uint8_t)(v >> 8);
    at[3] = (uint8_t)v;
}

/* ADR 0275's operations (CEE_KEY_*): see ceesafe_secrets_key. */
static int64_t cee_sec_system_key(int op, const char *app, const char *name, const uint8_t *in, uint64_t in_length,
                                  uint8_t *out, uint64_t capacity) {
    if (!cee_key_load()) {
        return CEE_SEC_UNSUPPORTED;
    }
    int login = cee_sec_login();
    cee_cf tag = cee_key_tag(app, name);
    if (tag == NULL) {
        return CEE_SEC_INVALID;
    }
    int64_t result = CEE_SEC_FAILED;
    cee_os_status status = 0;
    cee_cf key = cee_key_find(tag, login, &status);
    if (op == 3) {
        /* Remove: every key of the tag. */
        if (key != NULL) {
            cee_sec.release(key);
        }
        cee_cf q = cee_key_dictionary();
        if (q != NULL) {
            cee_sec.set(q, cee_sec.cls, cee_key.key_class);
            cee_sec.set(q, cee_key.tag, tag);
#if !TARGET_OS_IPHONE
            if (!login && cee_sec.data_protection != NULL) {
                cee_sec.set(q, cee_sec.data_protection, cee_sec.yes);
            }
#endif
            cee_os_status gone = cee_sec.remove(q);
            result = gone == 0 ? 0 : gone == CEE_SEC_NOT_FOUND ? CEE_SEC_NONE : cee_sec_code(gone);
            cee_sec.release(q);
        }
        cee_sec.release(tag);
        return result;
    }
    if (key == NULL && status != CEE_SEC_NOT_FOUND) {
        cee_sec.release(tag);
        return cee_sec_code(status);
    }
    if (op == 1 || op == 2) {
        if (key == NULL) {
            result = CEE_SEC_NONE;
        } else if (op == 1) {
            result = cee_key_spki(key, out, capacity);
        } else {
            cee_cf message = cee_sec.data(NULL, in, (cee_cf_index)in_length);
            cee_cf error = NULL;
            cee_cf signature = message != NULL ? cee_key.sign(key, cee_key.algorithm, message, &error) : NULL;
            if (signature != NULL && (uint64_t)cee_sec.data_length(signature) <= capacity) {
                result = (int64_t)cee_sec.data_length(signature);
                memcpy(out, cee_sec.data_bytes(signature), (size_t)result);
            }
            if (signature != NULL) cee_sec.release(signature);
            if (error != NULL) cee_sec.release(error);
            if (message != NULL) cee_sec.release(message);
        }
        if (key != NULL) cee_sec.release(key);
        cee_sec.release(tag);
        return result;
    }
    /* Obtain: the key, made when there is none (the Enclave first), and a new one attested. */
    int created = 0;
    int hardware = 0;
    if (key == NULL) {
        cee_os_status why = 0;
        key = login ? NULL : cee_key_make(tag, login, 1, &why);
        hardware = key != NULL;
        if (key == NULL) {
            key = cee_key_make(tag, login, 0, &why);
        }
        created = key != NULL;
        if (key == NULL && !login && cee_sec_fall_back(why)) {
            /* A macOS program without a keychain group (errSecMissingEntitlement): the login keychain, as Secrets
               does, where the key may be already, or is made in software. */
            login = 1;
            cee_os_status there = 0;
            key = cee_key_find(tag, login, &there);
            created = 0;
            hardware = 2;
            if (key == NULL) {
                key = cee_key_make(tag, login, 0, &why);
                created = key != NULL;
                hardware = 0;
            }
        }
    } else {
        /* An existing key: whether it is the Enclave's is not asked of the system; the program kept its first answer. */
        hardware = 2;
    }
    cee_sec.release(tag);
    if (key == NULL) {
        return CEE_SEC_FAILED;
    }
    uint8_t spki[91];
    if (cee_key_spki(key, spki, sizeof spki) != 91 || capacity < 3 + 4 + 91 + 4) {
        cee_sec.release(key);
        return CEE_SEC_FAILED;
    }
    cee_sec.release(key);
    size_t attest_length = 0;
    uint8_t *attestation = created && hardware == 1 ? cee_key_attest(in, (size_t)in_length, spki, 91, &attest_length)
                                                    : NULL;
    out[0] = (uint8_t)created;
    out[1] = (uint8_t)hardware;
    out[2] = attestation != NULL ? 2 : 0; /* 2: App Attest */
    cee_key_put32(out + 3, 91);
    memcpy(out + 7, spki, 91);
    result = 7 + 91;
    if (attestation != NULL) {
        if ((uint64_t)result + 4 + attest_length <= capacity) {
            cee_key_put32(out + result, (uint32_t)attest_length);
            memcpy(out + result + 4, attestation, attest_length);
            result += 4 + (int64_t)attest_length;
        } else {
            out[2] = 0;
        }
        free(attestation);
    }
    return result;
}

#elif defined(__ANDROID__)
/* Android: org.ceesafe.window.Secrets (runtime/android), a class of the window runtime's embedded dex, called over JNI
   from the program's thread, attached to the VM for the call. The window runtime gives the VM, the activity and the
   class (ceesafe_android_vm, ceesafe_android_secrets); they are weak here, so a program that opens no window links
   without them and has no store (Unsupported). */
#include <jni.h>

JavaVM *ceesafe_android_vm(void) __attribute__((weak));
jclass ceesafe_android_secrets(JNIEnv *env, jobject *activity) __attribute__((weak));

/* The calling thread's env, attached for the call when it was not (`*attached` set): NULL without a VM. */
static JNIEnv *cee_sec_env(JavaVM **vm, int *attached) {
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

/* One call of Secrets' `method` (signature `signature`: the activity, the app, the name, then a byte array): its int
   answer, or a code. `value` is the array's bytes for a write (copied in); for a read, the array of `capacity` bytes
   the answer's length of is copied out to `out`. */
static int64_t cee_sec_call(const char *method, const char *signature, const char *app, const char *name,
                            const uint8_t *value, uint64_t length, uint8_t *out, uint64_t capacity) {
    JavaVM *vm;
    int attached;
    JNIEnv *env = cee_sec_env(&vm, &attached);
    if (env == NULL) {
        return CEE_SEC_UNSUPPORTED;
    }
    int64_t result = CEE_SEC_UNSUPPORTED;
    if ((*env)->PushLocalFrame(env, 16) == 0) {
        jobject activity = NULL;
        jclass secrets = ceesafe_android_secrets != NULL ? ceesafe_android_secrets(env, &activity) : NULL;
        jmethodID m = secrets != NULL ? (*env)->GetStaticMethodID(env, secrets, method, signature) : NULL;
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            m = NULL;
        }
        jstring app_text = (*env)->NewStringUTF(env, app);
        jstring name_text = (*env)->NewStringUTF(env, name);
        jbyteArray bytes = out != NULL ? (*env)->NewByteArray(env, (jsize)capacity)
                           : value != NULL ? (*env)->NewByteArray(env, (jsize)length)
                                           : NULL;
        if (value != NULL && bytes != NULL && length > 0) {
            (*env)->SetByteArrayRegion(env, bytes, 0, (jsize)length, (const jbyte *)value);
        }
        if (m != NULL && app_text != NULL && name_text != NULL && (bytes != NULL || (out == NULL && value == NULL)) &&
            !(*env)->ExceptionCheck(env)) {
            jint answer = (*env)->CallStaticIntMethod(env, secrets, m, activity, app_text, name_text, bytes);
            if ((*env)->ExceptionCheck(env)) {
                (*env)->ExceptionClear(env);
                result = CEE_SEC_FAILED;
            } else {
                result = (int64_t)answer;
                if (out != NULL && answer > 0 && (uint64_t)answer <= capacity) {
                    (*env)->GetByteArrayRegion(env, bytes, 0, answer, (jbyte *)out);
                }
            }
        } else if (secrets != NULL) {
            result = CEE_SEC_FAILED;
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

static int64_t cee_sec_system_read(const char *group, const char *app, const char *name, uint8_t *out,
                                   uint64_t capacity) {
    (void)group;
    return cee_sec_call("read", "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;[B)I", app, name, NULL, 0,
                        out, capacity);
}

static int32_t cee_sec_system_write(const char *group, const char *app, const char *name, const uint8_t *value,
                                    uint64_t length) {
    (void)group;
    static const uint8_t nothing = 0;
    return (int32_t)cee_sec_call("write", "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;[B)I", app,
                                 name, value != NULL ? value : &nothing, length, NULL, 0);
}

static int32_t cee_sec_system_remove(const char *group, const char *app, const char *name) {
    (void)group;
    return (int32_t)cee_sec_call("remove", "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;[B)I", app,
                                 name, NULL, 0, NULL, 0);
}


/* Device keys (ADR 0275): Secrets' keyObtain, keyPublic, keySign and keyRemove, each with one array: the input after
   its 4-byte length, the answer over it. */
static int64_t cee_sec_system_key(int op, const char *app, const char *name, const uint8_t *in, uint64_t in_length,
                                  uint8_t *out, uint64_t capacity) {
    static const char *const methods[4] = {"keyObtain", "keyPublic", "keySign", "keyRemove"};
    if (op < 0 || op > 3 || in_length > 0x7fff0000u || capacity > 0x7fff0000u) {
        return CEE_SEC_INVALID;
    }
    JavaVM *vm;
    int attached;
    JNIEnv *env = cee_sec_env(&vm, &attached);
    if (env == NULL) {
        return CEE_SEC_UNSUPPORTED;
    }
    int64_t result = CEE_SEC_UNSUPPORTED;
    uint64_t room = capacity > in_length + 4 ? capacity : in_length + 4;
    if ((*env)->PushLocalFrame(env, 16) == 0) {
        jobject activity = NULL;
        jclass secrets = ceesafe_android_secrets != NULL ? ceesafe_android_secrets(env, &activity) : NULL;
        jmethodID m = secrets != NULL ? (*env)->GetStaticMethodID(env, secrets, methods[op],
                                                                  "(Landroid/content/Context;Ljava/lang/String;"
                                                                  "Ljava/lang/String;[B)I")
                                      : NULL;
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            m = NULL;
        }
        jstring app_text = (*env)->NewStringUTF(env, app);
        jstring name_text = (*env)->NewStringUTF(env, name);
        jbyteArray io = (*env)->NewByteArray(env, (jsize)room);
        if (m != NULL && app_text != NULL && name_text != NULL && io != NULL && !(*env)->ExceptionCheck(env)) {
            jbyte length[4] = {(jbyte)(in_length >> 24), (jbyte)(in_length >> 16), (jbyte)(in_length >> 8),
                               (jbyte)in_length};
            (*env)->SetByteArrayRegion(env, io, 0, 4, length);
            if (in_length > 0) {
                (*env)->SetByteArrayRegion(env, io, 4, (jsize)in_length, (const jbyte *)in);
            }
            jint answer = (*env)->CallStaticIntMethod(env, secrets, m, activity, app_text, name_text, io);
            if ((*env)->ExceptionCheck(env)) {
                (*env)->ExceptionClear(env);
                result = CEE_SEC_FAILED;
            } else {
                result = (int64_t)answer;
                if (answer > 0 && (uint64_t)answer <= capacity) {
                    (*env)->GetByteArrayRegion(env, io, 0, answer, (jbyte *)out);
                } else if (answer > 0) {
                    result = CEE_SEC_FAILED;
                }
            }
        } else if (secrets != NULL) {
            result = CEE_SEC_FAILED;
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

#elif defined(_WIN32)
/* Windows: DPAPI for the current user (no prompt; the app and the name as its entropy), a sealed file per value in
   %LOCALAPPDATA%\<app>\secrets. crypt32 is loaded at the first call. */
typedef struct {
    DWORD cbData;
    BYTE *pbData;
} cee_sec_blob;

static struct {
    volatile LONG state; /* 0 not tried, 1 loaded, 2 failed */
    BOOL(WINAPI *protect)(cee_sec_blob *, LPCWSTR, cee_sec_blob *, void *, void *, DWORD, cee_sec_blob *);
    BOOL(WINAPI *unprotect)(cee_sec_blob *, LPWSTR *, cee_sec_blob *, void *, void *, DWORD, cee_sec_blob *);
} cee_sec_dpapi;

#define CEE_SEC_UI_FORBIDDEN 0x1u /* CRYPTPROTECT_UI_FORBIDDEN */

static int cee_sec_dpapi_load(void) {
    if (cee_sec_dpapi.state == 0) {
        HMODULE crypt = LoadLibraryW(L"crypt32.dll");
        if (crypt != NULL) {
            *(FARPROC *)&cee_sec_dpapi.protect = GetProcAddress(crypt, "CryptProtectData");
            *(FARPROC *)&cee_sec_dpapi.unprotect = GetProcAddress(crypt, "CryptUnprotectData");
        }
        InterlockedExchange(&cee_sec_dpapi.state,
                            cee_sec_dpapi.protect != NULL && cee_sec_dpapi.unprotect != NULL ? 1 : 2);
    }
    return cee_sec_dpapi.state == 1;
}

/* %LOCALAPPDATA%\<app>\secrets (UTF-8, '/' between the parts), made if `make`; 0 when it cannot be. */
static int cee_sec_dpapi_dir(const char *app, int make, char *dir, size_t room) {
    wchar_t base[600];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, 600);
    if (n == 0 || n >= 600) {
        return 0;
    }
    char base_text[1024];
    if (WideCharToMultiByte(CP_UTF8, 0, base, -1, base_text, sizeof base_text, NULL, NULL) <= 0) {
        return 0;
    }
    char folder[1024];
    if (snprintf(folder, sizeof folder, "%s/%s", base_text, app) >= (int)sizeof folder ||
        snprintf(dir, room, "%s/secrets", folder) >= (int)room) {
        return 0;
    }
    return !make || (cee_sec_make_dir(folder) && cee_sec_make_dir(dir));
}

static cee_sec_blob cee_sec_entropy(const char *app, const char *name, char *buffer, size_t room) {
    int n = snprintf(buffer, room, "ceesafe.secrets/%s/%s", app, name);
    cee_sec_blob b = {(DWORD)(n > 0 ? n : 0), (BYTE *)buffer};
    return b;
}

static int64_t cee_sec_system_read(const char *group, const char *app, const char *name, uint8_t *out,
                                   uint64_t capacity) {
    (void)group;
    if (!cee_sec_dpapi_load()) {
        return CEE_SEC_UNSUPPORTED;
    }
    char dir[1100];
    char path[1200];
    if (!cee_sec_dpapi_dir(app, 0, dir, sizeof dir) || snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) {
        return CEE_SEC_FAILED;
    }
    /* A sealed value is longer than the value: DPAPI's header and MAC (a few hundred bytes). */
    uint64_t room = capacity + 4096u;
    uint8_t *sealed = (uint8_t *)malloc((size_t)room);
    if (sealed == NULL) {
        return CEE_SEC_FAILED;
    }
    int64_t got = cee_sec_file_read(path, sealed, room);
    int64_t result = got;
    if (got >= 0) {
        char e[300];
        cee_sec_blob in = {(DWORD)got, sealed};
        cee_sec_blob entropy = cee_sec_entropy(app, name, e, sizeof e);
        cee_sec_blob plain = {0, NULL};
        if (!cee_sec_dpapi.unprotect(&in, NULL, &entropy, NULL, NULL, CEE_SEC_UI_FORBIDDEN, &plain)) {
            result = CEE_SEC_FAILED;
        } else {
            result = plain.cbData <= capacity ? (int64_t)plain.cbData : CEE_SEC_FAILED;
            if (result > 0) {
                memcpy(out, plain.pbData, plain.cbData);
            }
            SecureZeroMemory(plain.pbData, plain.cbData);
            LocalFree(plain.pbData);
        }
    }
    free(sealed);
    return result;
}

static int32_t cee_sec_system_write(const char *group, const char *app, const char *name, const uint8_t *value,
                                    uint64_t length) {
    (void)group;
    if (!cee_sec_dpapi_load()) {
        return CEE_SEC_UNSUPPORTED;
    }
    char dir[1100];
    if (!cee_sec_dpapi_dir(app, 1, dir, sizeof dir)) {
        return CEE_SEC_FAILED;
    }
    char e[300];
    static BYTE nothing;
    /* DPAPI's input blob is not const, though CryptProtectData only reads it: the pointer is copied, not cast, so
       -Wcast-qual has nothing to say. */
    BYTE *bytes = &nothing;
    if (length > 0) {
        memcpy(&bytes, &value, sizeof bytes);
    }
    cee_sec_blob in = {(DWORD)length, bytes};
    cee_sec_blob entropy = cee_sec_entropy(app, name, e, sizeof e);
    cee_sec_blob sealed = {0, NULL};
    if (!cee_sec_dpapi.protect(&in, L"ceesafe", &entropy, NULL, NULL, CEE_SEC_UI_FORBIDDEN, &sealed)) {
        return CEE_SEC_FAILED;
    }
    char path[1200];
    int32_t result = snprintf(path, sizeof path, "%s/%s", dir, name) < (int)sizeof path
                         ? cee_sec_file_write(path, sealed.pbData, sealed.cbData)
                         : CEE_SEC_FAILED;
    LocalFree(sealed.pbData);
    return result;
}

static int32_t cee_sec_system_remove(const char *group, const char *app, const char *name) {
    (void)group;
    char dir[1100];
    char path[1200];
    if (!cee_sec_dpapi_dir(app, 0, dir, sizeof dir) || snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) {
        return CEE_SEC_FAILED;
    }
    return cee_sec_file_remove(path);
}


/* Device keys (ADR 0275): none here. */
static int64_t cee_sec_system_key(int op, const char *app, const char *name, const uint8_t *in, uint64_t in_length,
                                  uint8_t *out, uint64_t capacity) {
    (void)op;
    (void)app;
    (void)name;
    (void)in;
    (void)in_length;
    (void)out;
    (void)capacity;
    return CEE_SEC_UNSUPPORTED;
}
#else
/* Linux, FreeBSD and the rest: no store yet (ADR 0242 §6.1). */
static int64_t cee_sec_system_read(const char *group, const char *app, const char *name, uint8_t *out,
                                   uint64_t capacity) {
    (void)group;
    (void)app;
    (void)name;
    (void)out;
    (void)capacity;
    return CEE_SEC_UNSUPPORTED;
}

static int32_t cee_sec_system_write(const char *group, const char *app, const char *name, const uint8_t *value,
                                    uint64_t length) {
    (void)group;
    (void)app;
    (void)name;
    (void)value;
    (void)length;
    return CEE_SEC_UNSUPPORTED;
}

static int32_t cee_sec_system_remove(const char *group, const char *app, const char *name) {
    (void)group;
    (void)app;
    (void)name;
    return CEE_SEC_UNSUPPORTED;
}

/* Device keys (ADR 0275): none here. */
static int64_t cee_sec_system_key(int op, const char *app, const char *name, const uint8_t *in, uint64_t in_length,
                                  uint8_t *out, uint64_t capacity) {
    (void)op;
    (void)app;
    (void)name;
    (void)in;
    (void)in_length;
    (void)out;
    (void)capacity;
    return CEE_SEC_UNSUPPORTED;
}
#endif

/* ---- the entry points -------------------------------------------------------------------------------------------- */

/* The group as a C string in `out` (NULL for none, length 0), checked as an app's name is; 0 when it is not one. */
static int cee_sec_group(const uint8_t *group, uint64_t group_length, char out[CEE_SEC_NAME_LIMIT * 2 + 1],
                         const char **result) {
    *result = NULL;
    if (group == NULL || group_length == 0) {
        return 1;
    }
    if (group_length > CEE_SEC_NAME_LIMIT * 2 || memchr(group, 0, (size_t)group_length) != NULL) {
        return 0;
    }
    memcpy(out, group, (size_t)group_length);
    out[group_length] = 0;
    *result = out;
    return 1;
}

int64_t ceesafe_secrets_group_read(const uint8_t *group, uint64_t group_length, const uint8_t *app, uint64_t app_length,
                                   const uint8_t *name, uint64_t name_length, uint8_t *out, uint64_t capacity) {
    char a[CEE_SEC_NAME_LIMIT + 1];
    char n[CEE_SEC_NAME_LIMIT + 1];
    char g[CEE_SEC_NAME_LIMIT * 2 + 1];
    const char *shared;
    if (!cee_sec_names(app, app_length, name, name_length, a, n) || !cee_sec_group(group, group_length, g, &shared)) {
        return CEE_SEC_INVALID;
    }
    if (capacity > CEE_SEC_VALUE_LIMIT) {
        capacity = CEE_SEC_VALUE_LIMIT;
    }
    const char *dir = cee_sec_test_dir();
    return dir != NULL ? cee_sec_dir_read(dir, a, n, out, capacity) : cee_sec_system_read(shared, a, n, out, capacity);
}

int32_t ceesafe_secrets_group_write(const uint8_t *group, uint64_t group_length, const uint8_t *app,
                                    uint64_t app_length, const uint8_t *name, uint64_t name_length,
                                    const uint8_t *value, uint64_t length) {
    char a[CEE_SEC_NAME_LIMIT + 1];
    char n[CEE_SEC_NAME_LIMIT + 1];
    char g[CEE_SEC_NAME_LIMIT * 2 + 1];
    const char *shared;
    if (!cee_sec_names(app, app_length, name, name_length, a, n) || !cee_sec_group(group, group_length, g, &shared) ||
        length > CEE_SEC_VALUE_LIMIT) {
        return CEE_SEC_INVALID;
    }
    const char *dir = cee_sec_test_dir();
    return dir != NULL ? cee_sec_dir_write(dir, a, n, value, length) : cee_sec_system_write(shared, a, n, value, length);
}

int32_t ceesafe_secrets_group_remove(const uint8_t *group, uint64_t group_length, const uint8_t *app,
                                     uint64_t app_length, const uint8_t *name, uint64_t name_length) {
    char a[CEE_SEC_NAME_LIMIT + 1];
    char n[CEE_SEC_NAME_LIMIT + 1];
    char g[CEE_SEC_NAME_LIMIT * 2 + 1];
    const char *shared;
    if (!cee_sec_names(app, app_length, name, name_length, a, n) || !cee_sec_group(group, group_length, g, &shared)) {
        return CEE_SEC_INVALID;
    }
    const char *dir = cee_sec_test_dir();
    return dir != NULL ? cee_sec_dir_remove(dir, a, n) : cee_sec_system_remove(shared, a, n);
}

int64_t ceesafe_secrets_read(const uint8_t *app, uint64_t app_length, const uint8_t *name, uint64_t name_length,
                             uint8_t *out, uint64_t capacity) {
    return ceesafe_secrets_group_read(NULL, 0, app, app_length, name, name_length, out, capacity);
}

int32_t ceesafe_secrets_write(const uint8_t *app, uint64_t app_length, const uint8_t *name, uint64_t name_length,
                              const uint8_t *value, uint64_t length) {
    return ceesafe_secrets_group_write(NULL, 0, app, app_length, name, name_length, value, length);
}

int32_t ceesafe_secrets_remove(const uint8_t *app, uint64_t app_length, const uint8_t *name, uint64_t name_length) {
    return ceesafe_secrets_group_remove(NULL, 0, app, app_length, name, name_length);
}

/* Device keys (ADR 0275): `op` 0 obtains the key `name` of `app` (made when there is none, attested over `input`, the
   challenge: out is created, hardware, the attestation's kind, then 4-byte lengths before the SPKI and the
   attestation: 1 a certificate chain, its count then each certificate, 2 App Attest's object), 1 its public half
   (SPKI), 2 `input` signed (ECDSA with SHA-256, DER), 3 removes it. The answer's length (0 for a removal), or a code:
   -1 no key, -2 Unsupported (a system without a key store, and the test folder), -3 a name not taken, -4 Locked, -5
   Denied, -6 Failed. */
int64_t ceesafe_secrets_key(int32_t op, const uint8_t *app, uint64_t app_length, const uint8_t *name,
                            uint64_t name_length, const uint8_t *input, uint64_t input_length, uint8_t *out,
                            uint64_t capacity);

int64_t ceesafe_secrets_key(int32_t op, const uint8_t *app, uint64_t app_length, const uint8_t *name,
                            uint64_t name_length, const uint8_t *input, uint64_t input_length, uint8_t *out,
                            uint64_t capacity) {
    char a[CEE_SEC_NAME_LIMIT + 1];
    char n[CEE_SEC_NAME_LIMIT + 1];
    if (op < 0 || op > 3 || !cee_sec_names(app, app_length, name, name_length, a, n) ||
        input_length > CEE_SEC_VALUE_LIMIT) {
        return CEE_SEC_INVALID;
    }
    if (cee_sec_test_dir() != NULL) {
        return CEE_SEC_UNSUPPORTED; /* plain files hold no key */
    }
    return cee_sec_system_key((int)op, a, n, input, input_length, out, capacity);
}
