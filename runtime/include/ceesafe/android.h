/* ceesafe/android.h: Android's activity for a host program (ADR 0200). On Android a CeeSafe library is the
 * NativeActivity's: the runtime calls the library's `main` on a thread of its own once the activity is created, and
 * the process ends when `main` returns. These give that thread, and the threads a host attaches to the VM, what a JNI
 * host needs: the VM, and the activity, whose class loader (getClassLoader) finds the app's own classes, which a
 * native thread's FindClass does not. Both are NULL before the activity is created; the activity is a global
 * reference the runtime keeps. Empty on other systems. */
#ifndef CEESAFE_ANDROID_H
#define CEESAFE_ANDROID_H

#if defined(__ANDROID__)
#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

JavaVM *ceesafe_android_vm(void);
jobject ceesafe_android_activity(void);

#ifdef __cplusplus
}
#endif
#endif

#endif
