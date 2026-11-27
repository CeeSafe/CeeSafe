/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * Making a photo smaller (type-system.md §26.24, ADR 0266): ceesafe_image_shrink reads the photo at `from`, turns it
 * upright (its EXIF orientation applied), scales it so its longest side is at most `longest` (never larger), and
 * writes it to `to` as a JPEG at `quality` thousandths, without its metadata; its size in `*w` and `*h`. 0, or -1
 * Unsupported (no codec on this system), -2 NotFound, -3 InvalidData (not a photo the system reads), -4 an error
 * writing. Linked only into programs that call it (c-backend.md §6).
 *
 * ceesafe_image_video_frame (ADR 0273) writes a video's first frame the same way: upright (the video's rotation
 * applied), its longest side at most `longest`, a JPEG at `quality`. Apple's systems: AVFoundation's
 * AVAssetImageGenerator, through the Objective-C runtime's functions; Android: Photos.videoFrame
 * (MediaMetadataRetriever). Elsewhere Unsupported.
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
int32_t ceesafe_image_video_frame(const uint8_t *from, uint64_t from_length, const uint8_t *to, uint64_t to_length,
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

/* `image` written to `out_url` as a JPEG at `quality` thousandths, without metadata; its size in *w and *h. */
static int32_t cee_image_write(cee_cf image, cee_cf out_url, uint32_t quality, uint32_t *w, uint32_t *h) {
    int32_t result = CEE_IMAGE_FAILED;
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
    return result;
}

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
        result = cee_image_write(image, out_url, quality, w, h);
        cee_io.release(image);
    }
    if (options != NULL) cee_io.release(options);
    if (max != NULL) cee_io.release(max);
    if (source != NULL) cee_io.release(source);
    if (in_url != NULL) cee_io.release(in_url);
    if (out_url != NULL) cee_io.release(out_url);
    return result;
}

/* The Objective-C runtime's functions, for AVFoundation (ADR 0273). */
static struct {
    int loaded;
    void (*send)(void);
    void *(*cls)(const char *);
    void *(*sel)(const char *);
    void *(*pool_push)(void);
    void (*pool_pop)(void *);
} cee_av_image;

typedef struct {
    int64_t value;
    int32_t timescale;
    uint32_t flags;
    int64_t epoch;
} cee_cm_time;
typedef struct {
    double width, height;
} cee_cg_size;

static int cee_av_image_load(void) {
    if (cee_av_image.loaded == 0) {
        cee_av_image.loaded = -1;
        (void)dlopen("/System/Library/Frameworks/AVFoundation.framework/AVFoundation", RTLD_NOW | RTLD_GLOBAL);
        cee_av_image.send = (void (*)(void))dlsym(RTLD_DEFAULT, "objc_msgSend");
        cee_av_image.cls = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
        cee_av_image.sel = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
        cee_av_image.pool_push = (void *(*)(void))dlsym(RTLD_DEFAULT, "objc_autoreleasePoolPush");
        cee_av_image.pool_pop = (void (*)(void *))dlsym(RTLD_DEFAULT, "objc_autoreleasePoolPop");
        if (cee_av_image.send != NULL && cee_av_image.cls != NULL && cee_av_image.sel != NULL &&
            cee_av_image.pool_push != NULL && cee_av_image.pool_pop != NULL &&
            cee_av_image.cls("AVAssetImageGenerator") != NULL && cee_av_image.cls("AVURLAsset") != NULL) {
            cee_av_image.loaded = 1;
        }
    }
    return cee_av_image.loaded == 1;
}

static int32_t cee_image_video(const char *from, const char *to, uint32_t longest, uint32_t quality, uint32_t *w,
                               uint32_t *h) {
    if (!cee_image_load() || !cee_av_image_load()) {
        return CEE_IMAGE_UNSUPPORTED;
    }
    struct stat st;
    if (stat(from, &st) != 0) {
        return CEE_IMAGE_NOT_FOUND;
    }
    int32_t result = CEE_IMAGE_INVALID;
    void *pool = cee_av_image.pool_push();
    /* A CFURL is an NSURL (toll-free bridged). */
    cee_cf in_url = cee_io.url(NULL, (const uint8_t *)from, (long)strlen(from), 0);
    cee_cf out_url = cee_io.url(NULL, (const uint8_t *)to, (long)strlen(to), 0);
    void *asset = in_url != NULL ? ((void *(*)(void *, void *, cee_cf, void *))cee_av_image.send)(
                                       cee_av_image.cls("AVURLAsset"), cee_av_image.sel("URLAssetWithURL:options:"),
                                       in_url, NULL)
                                 : NULL;
    void *generator = asset != NULL ? ((void *(*)(void *, void *, void *))cee_av_image.send)(
                                          cee_av_image.cls("AVAssetImageGenerator"),
                                          cee_av_image.sel("assetImageGeneratorWithAsset:"), asset)
                                    : NULL;
    if (generator != NULL && out_url != NULL) {
        ((void (*)(void *, void *, signed char))cee_av_image.send)(
            generator, cee_av_image.sel("setAppliesPreferredTrackTransform:"), 1);
        cee_cg_size most = {(double)longest, (double)longest};
        ((void (*)(void *, void *, cee_cg_size))cee_av_image.send)(generator, cee_av_image.sel("setMaximumSize:"),
                                                                   most);
        cee_cm_time start = {0, 600, 1u, 0}; /* the start, a valid time */
        cee_cf image = ((cee_cf(*)(void *, void *, cee_cm_time, cee_cm_time *, void **))cee_av_image.send)(
            generator, cee_av_image.sel("copyCGImageAtTime:actualTime:error:"), start, NULL, NULL);
        if (image != NULL) {
            result = cee_image_write(image, out_url, quality, w, h);
            cee_io.release(image);
        }
    }
    if (in_url != NULL) cee_io.release(in_url);
    if (out_url != NULL) cee_io.release(out_url);
    cee_av_image.pool_pop(pool);
    return result;
}

#elif defined(__ANDROID__)
#include <jni.h>
#include <sys/stat.h>

JavaVM *ceesafe_android_vm(void) __attribute__((weak));
jclass ceesafe_android_photos(JNIEnv *env, jobject *context) __attribute__((weak));
jstring ceesafe_android_string(JNIEnv *env, const char *utf8, size_t length) __attribute__((weak));

/* Photos.`method`(from, to, longest, quality) -> int[] {w, h}, or null for a file that is no photo (shrink) or no
   video (videoFrame). */
static int32_t cee_image_android(const char *method, const char *from, const char *to, uint32_t longest,
                                 uint32_t quality, uint32_t *w, uint32_t *h) {
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
        jmethodID shrink = photos != NULL ? (*env)->GetStaticMethodID(env, photos, method,
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

static int32_t cee_image_system(const char *from, const char *to, uint32_t longest, uint32_t quality, uint32_t *w,
                                uint32_t *h) {
    return cee_image_android("shrink", from, to, longest, quality, w, h);
}

static int32_t cee_image_video(const char *from, const char *to, uint32_t longest, uint32_t quality, uint32_t *w,
                               uint32_t *h) {
    return cee_image_android("videoFrame", from, to, longest, quality, w, h);
}

#elif defined(_WIN32)
/* Windows: the Windows Imaging Component, through COM. ole32.dll is loaded with LoadLibraryW, as the window runtime's
   file dialogs do, and the GUIDs are written here, so the link names no import library. The frame is scaled with
   WIC's scaler (Fant), turned upright with its flip-rotator (the JPEG's EXIF orientation, read through the frame's
   metadata query reader), converted to 24-bit BGR, and written by WIC's JPEG encoder at the quality, with no
   metadata. */
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <windows.h>
#include <wincodec.h>

static const GUID cee_clsid_wic_factory = {0xcacaf262, 0x9370, 0x4615,
                                           {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
static const GUID cee_iid_wic_factory = {0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
static const GUID cee_container_jpeg = {0x19e4a5aa, 0x5662, 0x4fc5, {0xa0, 0xc0, 0x17, 0x58, 0x02, 0x8e, 0x10, 0x57}};
static const GUID cee_format_24bgr = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c}};

static struct {
    int state; /* 0 not tried, 1 loaded, -1 failed */
    HRESULT(WINAPI *initialize)(void *, DWORD);
    HRESULT(WINAPI *create)(REFCLSID, void *, DWORD, REFIID, void **);
    HRESULT(WINAPI *clear)(PROPVARIANT *);
} cee_wic;

static int cee_wic_load(void) {
    if (cee_wic.state == 0) {
        cee_wic.state = -1;
        HMODULE ole = LoadLibraryW(L"ole32.dll");
        if (ole != NULL) {
            cee_wic.initialize = (HRESULT(WINAPI *)(void *, DWORD))(void (*)(void))GetProcAddress(ole, "CoInitializeEx");
            cee_wic.create = (HRESULT(WINAPI *)(REFCLSID, void *, DWORD, REFIID, void **))(void (*)(void))GetProcAddress(
                ole, "CoCreateInstance");
            cee_wic.clear = (HRESULT(WINAPI *)(PROPVARIANT *))(void (*)(void))GetProcAddress(ole, "PropVariantClear");
            if (cee_wic.initialize != NULL && cee_wic.create != NULL && cee_wic.clear != NULL) {
                cee_wic.state = 1;
            }
        }
    }
    return cee_wic.state == 1;
}

/* A NUL-terminated UTF-16 copy of a UTF-8 path, or NULL. */
static wchar_t *cee_wic_path(const char *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    wchar_t *wide = n > 0 ? (wchar_t *)malloc((size_t)n * sizeof(wchar_t)) : NULL;
    if (wide != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, n) != n) {
        free(wide);
        wide = NULL;
    }
    return wide;
}

/* The EXIF orientation of a JPEG's frame (1 to 8), or 1 when it has none or is no JPEG. */
static unsigned cee_wic_orientation(IWICBitmapFrameDecode *frame) {
    IWICMetadataQueryReader *reader = NULL;
    unsigned orientation = 1;
    if (SUCCEEDED(IWICBitmapFrameDecode_GetMetadataQueryReader(frame, &reader)) && reader != NULL) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(reader, L"/app1/ifd/{ushort=274}", &value)) &&
            value.vt == VT_UI2 && value.uiVal >= 1 && value.uiVal <= 8) {
            orientation = value.uiVal;
        }
        (void)cee_wic.clear(&value);
        IWICMetadataQueryReader_Release(reader);
    }
    return orientation;
}

static int32_t cee_image_system(const char *from, const char *to, uint32_t longest, uint32_t quality, uint32_t *w,
                                uint32_t *h) {
    if (GetFileAttributesA(from) == INVALID_FILE_ATTRIBUTES) {
        wchar_t *probe = cee_wic_path(from);
        DWORD found = probe != NULL ? GetFileAttributesW(probe) : INVALID_FILE_ATTRIBUTES;
        free(probe);
        if (found == INVALID_FILE_ATTRIBUTES) {
            return CEE_IMAGE_NOT_FOUND;
        }
    }
    if (!cee_wic_load()) {
        return CEE_IMAGE_UNSUPPORTED;
    }
    HRESULT started = cee_wic.initialize(NULL, COINIT_MULTITHREADED);
    int uninitialize = started == S_OK || started == S_FALSE; /* RPC_E_CHANGED_MODE: the thread's own apartment */
    wchar_t *in = cee_wic_path(from);
    wchar_t *out = cee_wic_path(to);
    int32_t result = CEE_IMAGE_INVALID;
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICBitmapFlipRotator *rotator = NULL;
    IWICFormatConverter *converter = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *target = NULL;
    IPropertyBag2 *options = NULL;
    UINT fw = 0, fh = 0;
    if (in == NULL || out == NULL ||
        FAILED(cee_wic.create(&cee_clsid_wic_factory, NULL, CLSCTX_INPROC_SERVER, &cee_iid_wic_factory,
                              (void **)&factory))) {
        result = in == NULL || out == NULL ? CEE_IMAGE_NOT_FOUND : CEE_IMAGE_UNSUPPORTED;
    } else if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(factory, in, NULL, GENERIC_READ,
                                                                        WICDecodeMetadataCacheOnDemand, &decoder)) &&
               SUCCEEDED(IWICBitmapDecoder_GetFrame(decoder, 0, &frame)) &&
               SUCCEEDED(IWICBitmapFrameDecode_GetSize(frame, &fw, &fh)) && fw > 0 && fh > 0) {
        /* Scaled first (never enlarged), then turned. */
        UINT big = fw > fh ? fw : fh;
        UINT sw = fw, sh = fh;
        if (big > longest) {
            sw = (UINT)(((uint64_t)fw * longest + big / 2u) / big);
            sh = (UINT)(((uint64_t)fh * longest + big / 2u) / big);
            sw = sw > 0 ? sw : 1u;
            sh = sh > 0 ? sh : 1u;
        }
        IWICBitmapSource *source = (IWICBitmapSource *)frame;
        result = CEE_IMAGE_FAILED;
        if ((sw == fw && sh == fh) ||
            (SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(factory, &scaler)) &&
             SUCCEEDED(IWICBitmapScaler_Initialize(scaler, source, sw, sh, WICBitmapInterpolationModeFant)))) {
            if (scaler != NULL) {
                source = (IWICBitmapSource *)scaler;
            }
            unsigned orientation = cee_wic_orientation(frame);
            /* EXIF 2.32's orientations as WIC's transforms (a rotation clockwise, then a flip). */
            WICBitmapTransformOptions turn = orientation == 2   ? WICBitmapTransformFlipHorizontal
                                             : orientation == 3 ? WICBitmapTransformRotate180
                                             : orientation == 4 ? WICBitmapTransformFlipVertical
                                             : orientation == 5
                                                 ? (WICBitmapTransformOptions)(WICBitmapTransformRotate90 |
                                                                               WICBitmapTransformFlipHorizontal)
                                             : orientation == 6 ? WICBitmapTransformRotate90
                                             : orientation == 7
                                                 ? (WICBitmapTransformOptions)(WICBitmapTransformRotate270 |
                                                                               WICBitmapTransformFlipHorizontal)
                                             : orientation == 8 ? WICBitmapTransformRotate270
                                                                : WICBitmapTransformRotate0;
            int turned = turn != WICBitmapTransformRotate0;
            if (!turned || (SUCCEEDED(IWICImagingFactory_CreateBitmapFlipRotator(factory, &rotator)) &&
                            SUCCEEDED(IWICBitmapFlipRotator_Initialize(rotator, source, turn)))) {
                if (rotator != NULL) {
                    source = (IWICBitmapSource *)rotator;
                }
                UINT ow = 0, oh = 0;
                WICPixelFormatGUID format = cee_format_24bgr;
                PROPBAG2 name;
                memset(&name, 0, sizeof name);
                name.pstrName = (LPOLESTR)L"ImageQuality";
                VARIANT q; /* set by hand: VariantInit is oleaut32's, not loaded here */
                memset(&q, 0, sizeof q);
                q.vt = VT_R4;
                q.fltVal = (float)quality / 1000.0f;
                if (SUCCEEDED(IWICImagingFactory_CreateFormatConverter(factory, &converter)) &&
                    SUCCEEDED(IWICFormatConverter_Initialize(converter, source, &cee_format_24bgr,
                                                             WICBitmapDitherTypeNone, NULL, 0.0,
                                                             WICBitmapPaletteTypeCustom)) &&
                    SUCCEEDED(IWICFormatConverter_GetSize(converter, &ow, &oh)) &&
                    SUCCEEDED(IWICImagingFactory_CreateStream(factory, &stream)) &&
                    SUCCEEDED(IWICStream_InitializeFromFilename(stream, out, GENERIC_WRITE)) &&
                    SUCCEEDED(IWICImagingFactory_CreateEncoder(factory, &cee_container_jpeg, NULL, &encoder)) &&
                    SUCCEEDED(IWICBitmapEncoder_Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache)) &&
                    SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(encoder, &target, &options)) &&
                    SUCCEEDED(IPropertyBag2_Write(options, 1, &name, &q)) &&
                    SUCCEEDED(IWICBitmapFrameEncode_Initialize(target, options)) &&
                    SUCCEEDED(IWICBitmapFrameEncode_SetSize(target, ow, oh)) &&
                    SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(target, &format)) &&
                    IsEqualGUID(&format, &cee_format_24bgr) &&
                    SUCCEEDED(IWICBitmapFrameEncode_WriteSource(target, (IWICBitmapSource *)converter, NULL)) &&
                    SUCCEEDED(IWICBitmapFrameEncode_Commit(target)) && SUCCEEDED(IWICBitmapEncoder_Commit(encoder))) {
                    *w = (uint32_t)ow;
                    *h = (uint32_t)oh;
                    result = 0;
                }
            }
        }
    }
    if (options != NULL) IPropertyBag2_Release(options);
    if (target != NULL) IWICBitmapFrameEncode_Release(target);
    if (encoder != NULL) IWICBitmapEncoder_Release(encoder);
    if (stream != NULL) IWICStream_Release(stream);
    if (converter != NULL) IWICFormatConverter_Release(converter);
    if (rotator != NULL) IWICBitmapFlipRotator_Release(rotator);
    if (scaler != NULL) IWICBitmapScaler_Release(scaler);
    if (frame != NULL) IWICBitmapFrameDecode_Release(frame);
    if (decoder != NULL) IWICBitmapDecoder_Release(decoder);
    if (factory != NULL) IWICImagingFactory_Release(factory);
    if (uninitialize) {
        HMODULE ole = GetModuleHandleW(L"ole32.dll");
        void(WINAPI * done)(void) = ole != NULL ? (void(WINAPI *)(void))(void (*)(void))GetProcAddress(ole, "CoUninitialize")
                                                 : NULL;
        if (done != NULL) {
            done();
        }
    }
    free(in);
    free(out);
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

#if !defined(__APPLE__) && !defined(__ANDROID__)
static int32_t cee_image_video(const char *from, const char *to, uint32_t longest, uint32_t quality, uint32_t *w,
                               uint32_t *h) {
    (void)from;
    (void)to;
    (void)longest;
    (void)quality;
    (void)w;
    (void)h;
    return CEE_IMAGE_UNSUPPORTED; /* the phones and macOS (ADR 0273 §2) */
}
#endif

int32_t ceesafe_image_video_frame(const uint8_t *from, uint64_t from_length, const uint8_t *to, uint64_t to_length,
                                  uint32_t longest, uint32_t quality, uint32_t *w, uint32_t *h) {
    *w = 0;
    *h = 0;
    if (longest == 0 || quality > 1000u) {
        return CEE_IMAGE_INVALID;
    }
    char *a = cee_image_path(from, from_length);
    char *b = cee_image_path(to, to_length);
    int32_t result = a == NULL || b == NULL ? CEE_IMAGE_NOT_FOUND : cee_image_video(a, b, longest, quality, w, h);
    free(a);
    free(b);
    return result;
}

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
