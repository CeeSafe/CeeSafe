/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * Push tokens (type-system.md §26.23, ADR 0244): the token a server sends this device's pushes to, from the system's
 * push service. Linked only into programs that ask for one (c-backend.md §6).
 *
 * ceesafe_push_token blocks, from any thread but the window's, and writes the token as text: "apns-sandbox:HEX" (an
 * iOS development build), "apns:HEX" (iOS), "fcm:TOKEN" (Android); it returns the length, or -2 Unsupported,
 * -5 Denied, -6 Failed (PushError). iOS: the window runtime's ceesafe_ios_push_token (weak here, so a program with no
 * window has none). Android, macOS and the rest: Unsupported for now (ADR 0244 §2.3).
 *
 * CEESAFE_PUSH_TOKEN=<text> is the token instead, on every system (for tests): its text as it is.
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv */
#endif
#include "ceesafe/rt.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CEE_PUSH_UNSUPPORTED (-2)
#define CEE_PUSH_FAILED (-6)

int64_t ceesafe_push_token(uint8_t *out, uint64_t capacity);

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE
int64_t ceesafe_ios_push_token(char *out, size_t capacity) __attribute__((weak));

static int64_t cee_push_system(char *out, size_t capacity) {
    return ceesafe_ios_push_token != NULL ? ceesafe_ios_push_token(out, capacity) : CEE_PUSH_UNSUPPORTED;
}
#else
static int64_t cee_push_system(char *out, size_t capacity) {
    (void)out;
    (void)capacity;
    return CEE_PUSH_UNSUPPORTED;
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
