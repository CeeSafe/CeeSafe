/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * Making a photo smaller (type-system.md §26.24, ADR 0266): ceesafe_image_shrink reads the photo at `from`, turns it
 * upright (its EXIF orientation applied), scales it so its longest side is at most `longest` (never larger), and
 * writes it to `to` as a JPEG at `quality` thousandths, without its metadata; its size in `*w` and `*h`. 0, or -1
 * Unsupported (no codec on this system), -2 NotFound, -3 InvalidData (not a photo the system reads), -4 an error
 * writing. Linked only into programs that call it (c-backend.md §6).
 *
 * Apple's systems: ImageIO (CGImageSourceCreateThumbnailAtIndex, which decodes no more than it needs, with the
 * transform; CGImageDestination for the JPEG), loaded with dlopen. Android: the runtime's class
 * org.ceesafe.window.Photos (BitmapFactory, ExifInterface, Bitmap.compress), through the window runtime (weak here:
 * a program with no window has none). Elsewhere Unsupported, for now (ADR 0266 §6.2).
 */
#include "ceesafe/rt.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CEE_IMAGE_UNSUPPORTED (-1)
#define CEE_IMAGE_NOT_FOUND (-2)
#define CEE_IMAGE_INVALID (-3)
#define CEE_IMAGE_FAILED (-4)

int32_t ceesafe_image_shrink(const uint8_t *from, uint64_t from_length, const uint8_t *to, uint64_t to_length,
                             uint32_t longest, uint32_t quality, uint32_t *w, uint32_t *h);

/* A NUL-terminated copy of a path, or NULL (no memory, a NUL inside, too long). */
static char *cee_image_path(const uint8_t *text, uint64_t length) {
    if (length == 0 || length > 4096u || memchr(text, 0, (size_t)length) != NULL) {
        return NULL;
    }
    char *copy = (char *)malloc((size_t)length + 1u);
    if (copy != NULL) {
        memcpy(copy, text, (size_t)length);
        copy[length] = 0;
    }
    return copy;
}

#if defined(__APPLE__)
#include <dlfcn.h>
#include <sys/stat.h>

/* CoreFoundation's, CoreGraphics' and ImageIO's types, as opaque pointers (no headers needed to build). */
typedef const void *cee_cf;

static struct {
    int loaded;
    void (*release)(cee_cf);
    cee_cf (*url)(cee_cf, const uint8_t *, long, unsigned char);
    cee_cf (*dictionary)(cee_cf, const void **, const void **, long, const void *, const void *);
    cee_cf (*number)(cee_cf, long, const void *);
    cee_cf (*string)(cee_cf, const char *, uint32_t);
    cee_cf (*source)(cee_cf, cee_cf);
    long (*source_count)(cee_cf);
    cee_cf (*thumbnail)(cee_cf, long, cee_cf);
    cee_cf (*destination)(cee_cf, cee_cf, long, cee_cf);
    void (*add)(cee_cf, cee_cf, cee_cf);
    unsigned char (*finalize)(cee_cf);
    size_t (*width)(cee_cf);
    size_t (*height)(cee_cf);
    const void *key_callbacks, *value_callbacks;
    cee_cf *always, *max_size, *transform, *quality_key, *yes;
} cee_io;

static int cee_image_load(void) {
    if (cee_io.loaded != 0) {
        return cee_io.loaded == 1;
    }
    cee_io.loaded = -1;
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_NOW);
    void *io = dlopen("/System/Library/Frameworks/ImageIO.framework/ImageIO", RTLD_NOW);
    if (cf == NULL || cg == NULL || io == NULL) {
        return 0;
    }
    cee_io.release = (void (*)(cee_cf))dlsym(cf, "CFRelease");
    cee_io.url = (cee_cf(*)(cee_cf, const uint8_t *, long, unsigned char))dlsym(
        cf, "CFURLCreateFromFileSystemRepresentation");
    cee_io.dictionary = (cee_cf(*)(cee_cf, const void **, const void **, long, const void *, const void *))dlsym(
        cf, "CFDictionaryCreate");
    cee_io.number = (cee_cf(*)(cee_cf, long, const void *))dlsym(cf, "CFNumberCreate");
    cee_io.string = (cee_cf(*)(cee_cf, const char *, uint32_t))dlsym(cf, "CFStringCreateWithCString");
    cee_io.key_callbacks = dlsym(cf, "kCFTypeDictionaryKeyCallBacks");
    cee_io.value_callbacks = dlsym(cf, "kCFTypeDictionaryValueCallBacks");
    cee_io.yes = (cee_cf *)dlsym(cf, "kCFBooleanTrue");
    cee_io.width = (size_t(*)(cee_cf))dlsym(cg, "CGImageGetWidth");
    cee_io.height = (size_t(*)(cee_cf))dlsym(cg, "CGImageGetHeight");
    cee_io.source = (cee_cf(*)(cee_cf, cee_cf))dlsym(io, "CGImageSourceCreateWithURL");
    cee_io.source_count = (long (*)(cee_cf))dlsym(io, "CGImageSourceGetCount");
    cee_io.thumbnail = (cee_cf(*)(cee_cf, long, cee_cf))dlsym(io, "CGImageSourceCreateThumbnailAtIndex");
    cee_io.destination = (cee_cf(*)(cee_cf, cee_cf, long, cee_cf))dlsym(io, "CGImageDestinationCreateWithURL");
    cee_io.add = (void (*)(cee_cf, cee_cf, cee_cf))dlsym(io, "CGImageDestinationAddImage");
    cee_io.finalize = (unsigned char (*)(cee_cf))dlsym(io, "CGImageDestinationFinalize");
    cee_io.always = (cee_cf *)dlsym(io, "kCGImageSourceCreateThumbnailFromImageAlways");
    cee_io.max_size = (cee_cf *)dlsym(io, "kCGImageSourceThumbnailMaxPixelSize");
    cee_io.transform = (cee_cf *)dlsym(io, "kCGImageSourceCreateThumbnailWithTransform");
    cee_io.quality_key = (cee_cf *)dlsym(io, "kCGImageDestinationLossyCompressionQuality");
    if (cee_io.release == NULL || cee_io.url == NULL || cee_io.dictionary == NULL || cee_io.number == NULL ||
        cee_io.string == NULL || cee_io.key_callbacks == NULL || cee_io.value_callbacks == NULL ||
        cee_io.yes == NULL || cee_io.width == NULL || cee_io.height == NULL || cee_io.source == NULL ||
        cee_io.source_count == NULL || cee_io.thumbnail == NULL || cee_io.destination == NULL ||
        cee_io.add == NULL || cee_io.finalize == NULL || cee_io.always == NULL || cee_io.max_size == NULL ||
        cee_io.transform == NULL || cee_io.quality_key == NULL) {
        return 0;
    }
    cee_io.loaded = 1;
    return 1;
}

#define CEE_CF_NUMBER_SINT32 3  /* kCFNumberSInt32Type */
#define CEE_CF_NUMBER_DOUBLE 13 /* kCFNumberDoubleType */
#define CEE_CF_UTF8 0x08000100u /* kCFStringEncodingUTF8 */

static int32_t cee_image_system(const char *from, const char *to, uint32_t longest, uint32_t quality, uint32_t *w,
                                uint32_t *h) {
    if (!cee_image_load()) {
        return CEE_IMAGE_UNSUPPORTED;
    }
    struct stat st;
    if (stat(from, &st) != 0) {
        return CEE_IMAGE_NOT_FOUND;
    }
    int32_t result = CEE_IMAGE_INVALID;
    cee_cf in_url = cee_io.url(NULL, (const uint8_t *)from, (long)strlen(from), 0);
    cee_cf out_url = cee_io.url(NULL, (const uint8_t *)to, (long)strlen(to), 0);
    cee_cf source = in_url != NULL ? cee_io.source(in_url, NULL) : NULL;
    int32_t size = (int32_t)(longest > 0x7fffffffu ? 0x7fffffffu : longest);
    cee_cf max = cee_io.number(NULL, CEE_CF_NUMBER_SINT32, &size);
    const void *keys[3] = {*cee_io.always, *cee_io.max_size, *cee_io.transform};
    const void *values[3] = {*cee_io.yes, max, *cee_io.yes};
    cee_cf options = max != NULL ? cee_io.dictionary(NULL, keys, values, 3, cee_io.key_callbacks,
                                                     cee_io.value_callbacks)
                                 : NULL;
    cee_cf image = source != NULL && options != NULL && cee_io.source_count(source) > 0
                       ? cee_io.thumbnail(source, 0, options)
                       : NULL;
    if (image != NULL) {
        result = CEE_IMAGE_FAILED;
        double q = (double)quality / 1000.0;
        cee_cf qn = cee_io.number(NULL, CEE_CF_NUMBER_DOUBLE, &q);
        const void *qkeys[1] = {*cee_io.quality_key};
        const void *qvalues[1] = {qn};
        cee_cf props = qn != NULL ? cee_io.dictionary(NULL, qkeys, qvalues, 1, cee_io.key_callbacks,
                                                     cee_io.value_callbacks)
                                  : NULL;
        cee_cf jpeg = cee_io.string(NULL, "public.jpeg", CEE_CF_UTF8);
        cee_cf dest = out_url != NULL && jpeg != NULL ? cee_io.destination(out_url, jpeg, 1, NULL) : NULL;
        if (dest != NULL && props != NULL) {
            /* No metadata is given: the photo's EXIF (its place, its camera) is not written (ADR 0266 §6.3). */
            cee_io.add(dest, image, props);
            if (cee_io.finalize(dest)) {
                *w = (uint32_t)cee_io.width(image);
                *h = (uint32_t)cee_io.height(image);
                result = 0;
            }
        }
        if (dest != NULL) cee_io.release(dest);
        if (jpeg != NULL) cee_io.release(jpeg);
        if (props != NULL) cee_io.release(props);
        if (qn != NULL) cee_io.release(qn);
        cee_io.release(image);
    }
    if (options != NULL) cee_io.release(options);
    if (max != NULL) cee_io.release(max);
    if (source != NULL) cee_io.release(source);
    if (in_url != NULL) cee_io.release(in_url);
    if (out_url != NULL) cee_io.release(out_url);
    return result;
}

#elif defined(__ANDROID__)
#include <jni.h>
#include <sys/stat.h>

JavaVM *ceesafe_android_vm(void) __attribute__((weak));
jclass ceesafe_android_photos(JNIEnv *env, jobject *context) __attribute__((weak));
jstring ceesafe_android_string(JNIEnv *env, const char *utf8, size_t length) __attribute__((weak));

/* Photos.shrink(from, to, longest, quality) -> int[] {w, h}, or null for a file that is no photo. */
static int32_t cee_image_system(const char *from, const char *to, uint32_t longest, uint32_t quality, uint32_t *w,
                                uint32_t *h) {
    struct stat st;
    if (stat(from, &st) != 0) {
        return CEE_IMAGE_NOT_FOUND;
    }
    JavaVM *vm = ceesafe_android_vm != NULL ? ceesafe_android_vm() : NULL;
    if (vm == NULL || ceesafe_android_photos == NULL) {
        return CEE_IMAGE_UNSUPPORTED;
    }
    JNIEnv *env = NULL;
    int attached = 0;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) {
            return CEE_IMAGE_UNSUPPORTED;
        }
        attached = 1;
    }
    int32_t result = CEE_IMAGE_UNSUPPORTED;
    if ((*env)->PushLocalFrame(env, 16) == 0) {
        jobject context = NULL;
        jclass photos = ceesafe_android_photos(env, &context);
        jmethodID shrink = photos != NULL ? (*env)->GetStaticMethodID(env, photos, "shrink",
                                                                      "(Ljava/lang/String;Ljava/lang/String;II)[I")
                                          : NULL;
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            shrink = NULL;
        }
        /* Standard UTF-8 where the window runtime converts it (ADR 0263); a path of the app's own folders else. */
        jstring a = ceesafe_android_string != NULL ? ceesafe_android_string(env, from, strlen(from))
                                                   : (*env)->NewStringUTF(env, from);
        jstring b = ceesafe_android_string != NULL ? ceesafe_android_string(env, to, strlen(to))
                                                   : (*env)->NewStringUTF(env, to);
        if (shrink != NULL && a != NULL && b != NULL) {
            jintArray made = (jintArray)(*env)->CallStaticObjectMethod(env, photos, shrink, a, b,
                                                                       (jint)(longest > 0x7fffffffu ? 0x7fffffff
                                                                                                     : longest),
                                                                       (jint)quality);
            if ((*env)->ExceptionCheck(env)) {
                (*env)->ExceptionClear(env);
                result = CEE_IMAGE_FAILED;
            } else if (made == NULL || (*env)->GetArrayLength(env, made) < 2) {
                result = CEE_IMAGE_INVALID;
            } else {
                jint size[2];
                (*env)->GetIntArrayRegion(env, made, 0, 2, size);
                if (size[0] < 0) {
                    result = CEE_IMAGE_FAILED; /* read, but not written */
                } else {
                    *w = (uint32_t)size[0];
                    *h = (uint32_t)size[1];
                    result = 0;
                }
            }
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

#else
static int32_t cee_image_system(const char *from, const char *to, uint32_t longest, uint32_t quality, uint32_t *w,
                                uint32_t *h) {
    (void)from;
    (void)to;
    (void)longest;
    (void)quality;
    (void)w;
    (void)h;
    return CEE_IMAGE_UNSUPPORTED; /* no codec of the system's here, for now (ADR 0266 §6.2) */
}
#endif

int32_t ceesafe_image_shrink(const uint8_t *from, uint64_t from_length, const uint8_t *to, uint64_t to_length,
                             uint32_t longest, uint32_t quality, uint32_t *w, uint32_t *h) {
    *w = 0;
    *h = 0;
    if (longest == 0 || quality > 1000u) {
        return CEE_IMAGE_INVALID;
    }
    char *a = cee_image_path(from, from_length);
    char *b = cee_image_path(to, to_length);
    int32_t result = a == NULL || b == NULL ? CEE_IMAGE_NOT_FOUND : cee_image_system(a, b, longest, quality, w, h);
    free(a);
    free(b);
    return result;
}
