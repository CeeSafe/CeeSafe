// The Android window's file dialogs (ADR 0158): the second Java class of CeeSafe's runtime, compiled into the same
// embedded dex as InputView by tools/gen_android_dex.py. Edit it here, then run the generator.
//
// A fragment without a view, added to the activity, since only an activity or a fragment is given the result of
// another activity. It starts the system's document picker (the Storage Access Framework) and, at its answer, copies
// the picked documents into the app's cache folder (open) or writes the program's file to the created document
// (export) on a thread of its own, then tells the runtime, whose program is waiting.
//
// The photo library and the camera (ADR 0265) go the same way: the system's photo picker (or, without it, the content
// chooser for images and videos), each item copied to the cache with a HEIF photo made JPEG; and the camera's app,
// which writes to a new item of the gallery (MediaStore), published there and copied to the cache.
//
// android.app.Fragment is deprecated and present in every release; its replacement is a library an app would have to
// ship. If the system ends the app's process while the picker is in front, the activity that comes back cannot make
// this fragment again unless the app's APK holds this class (the runtime looks there first).
package org.ceesafe.window;

import android.Manifest;
import android.app.Activity;
import android.app.Fragment;
import android.app.FragmentManager;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.ImageDecoder;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.ext.SdkExtensions;
import android.provider.MediaStore;
import android.provider.OpenableColumns;
import android.webkit.MimeTypeMap;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.Locale;

public final class Picker extends Fragment {
    private static final String TAG = "org.ceesafe.window.Picker";
    private static final int FILE = 0;
    private static final int FILES = 1;
    private static final int EXPORT = 4;
    private static final int PRINT = 5;
    private static final int MEDIA = 6;
    private static final int CAPTURE = 7;
    private static final int PERMISSIONS = 100;
    /** No camera's app: the runtime's Unsupported (FAILED, -1, is its Resources). */
    private static final int UNSUPPORTED = -2;
    private static final int SAVED = 1;
    private static final int CANCELLED = 0;
    private static final int FAILED = -1;

    /** The file an export writes out, between its start and its result. */
    private String exporting;
    /** A library pick's limit, between its start and its result. */
    private int limit;
    /** A capture: the gallery's item the camera writes, whether it is a photo, and the permissions asked first. */
    private Uri capturing;
    private boolean capturingPhoto;

    /** The picker's answer: 1 with the paths of the copies, each followed by a zero byte (none for an export), 0 a
        cancel, -1 a failure. */
    private static native void nativePicked(int status, byte[] paths);

    /** A dialog's answer, from another class of the runtime's (Printing). */
    static void answer(int status, byte[] paths) {
        nativePicked(status, paths);
    }

    /** Starts the picker for the dialog of `kind` (0 a file, 1 files, 4 an export of `file` as `name`, 5 printing the
        PDF `file` as the job `name`: ADR 0178); `extensions` is the comma-separated list the open dialogs offer. On
        the activity's thread. */
    public static void start(Activity activity, int kind, String extensions, String name, String file) {
        try {
            if (kind == PRINT) {
                Printing.start(activity, name, file);
                return;
            }
            FragmentManager manager = activity.getFragmentManager();
            Picker picker = (Picker) manager.findFragmentByTag(TAG);
            if (picker == null) {
                picker = new Picker();
                manager.beginTransaction().add(picker, TAG).commitAllowingStateLoss();
                manager.executePendingTransactions();
            }
            if (kind == MEDIA) {
                picker.limit = Math.max(1, parse(name));
                picker.startActivityForResult(mediaIntent(extensions, picker.limit), kind + 1);
                return;
            }
            if (kind == CAPTURE) {
                picker.capturingPhoto = !extensions.equals("video");
                String[] needed = needed(activity);
                if (needed.length > 0) {
                    picker.requestPermissions(needed, PERMISSIONS); // then onRequestPermissionsResult
                } else {
                    picker.capture(activity);
                }
                return;
            }
            Intent intent;
            if (kind == EXPORT) {
                picker.exporting = file;
                intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                intent.setType(mime(name, "application/octet-stream"));
                intent.putExtra(Intent.EXTRA_TITLE, name);
            } else {
                intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.setType("*/*");
                String[] types = mimes(extensions);
                if (types != null) {
                    intent.putExtra(Intent.EXTRA_MIME_TYPES, types);
                }
                intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, kind == FILES);
            }
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            picker.startActivityForResult(intent, kind + 1);
        } catch (RuntimeException e) {
            nativePicked(FAILED, null); // no picker on this system, or the activity is ending
        }
    }

    @Override
    public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        Activity activity = getActivity();
        boolean granted = activity != null && results.length > 0;
        for (int result : results) {
            granted = granted && result == PackageManager.PERMISSION_GRANTED;
        }
        if (granted) {
            capture(activity);
        } else {
            nativePicked(FAILED, null); // refused: the program's Resources, "the system refused"
        }
    }

    @Override
    public void onActivityResult(int request, int result, Intent data) {
        final int kind = request - 1;
        final Activity activity = getActivity();
        if (kind == CAPTURE) {
            captured(activity, result == Activity.RESULT_OK, data);
            return;
        }
        if (result != Activity.RESULT_OK || data == null || activity == null) {
            nativePicked(CANCELLED, null);
            return;
        }
        final ArrayList<Uri> uris = new ArrayList<Uri>();
        ClipData clip = data.getClipData();
        if (clip != null) {
            for (int k = 0; k < clip.getItemCount(); k++) {
                uris.add(clip.getItemAt(k).getUri());
            }
        } else if (data.getData() != null) {
            uris.add(data.getData());
        }
        while (kind == MEDIA && uris.size() > limit) {
            uris.remove(uris.size() - 1); // the content chooser has no limit of its own
        }
        final String from = exporting;
        exporting = null;
        final ContentResolver resolver = activity.getContentResolver();
        final File cache = new File(activity.getCacheDir(), "ceesafe-picked");
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    if (uris.isEmpty()) {
                        nativePicked(CANCELLED, null);
                    } else if (kind == EXPORT) {
                        try (InputStream in = new FileInputStream(from);
                             OutputStream out = resolver.openOutputStream(uris.get(0), "wt")) {
                            copy(in, out);
                        }
                        nativePicked(SAVED, null);
                    } else {
                        ByteArrayOutputStream paths = new ByteArrayOutputStream();
                        for (int k = 0; k < uris.size(); k++) {
                            File folder = new File(cache, Long.toString(System.nanoTime()) + "-" + k);
                            folder.mkdirs();
                            File copy = kind == MEDIA ? media(resolver, uris.get(k), folder)
                                : new File(folder, name(resolver, uris.get(k)));
                            if (kind != MEDIA) {
                                try (InputStream in = resolver.openInputStream(uris.get(k));
                                     OutputStream out = new FileOutputStream(copy)) {
                                    copy(in, out);
                                }
                            }
                            paths.write(copy.getAbsolutePath().getBytes(StandardCharsets.UTF_8));
                            paths.write(0);
                        }
                        nativePicked(SAVED, paths.toByteArray());
                    }
                } catch (Exception e) {
                    nativePicked(FAILED, null);
                }
            }
        }, "ceesafe-picker").start();
    }

    // ---- The photo library and the camera (ADR 0265) ----

    /** The library's picker: the system's photo picker where it has one (no permission), else the content chooser. */
    private static Intent mediaIntent(String kinds, int limit) {
        boolean photos = kinds.contains("photos");
        boolean videos = kinds.contains("videos");
        String type = photos && !videos ? "image/*" : videos && !photos ? "video/*" : null;
        if (hasPhotoPicker()) {
            Intent intent = new Intent(MediaStore.ACTION_PICK_IMAGES);
            if (type != null) {
                intent.setType(type);
            }
            if (limit > 1) {
                intent.putExtra(MediaStore.EXTRA_PICK_IMAGES_MAX, Math.min(limit, MediaStore.getPickImagesMaxLimit()));
            }
            return intent;
        }
        Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
        intent.setType(type != null ? type : "*/*");
        if (type == null) {
            intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"image/*", "video/*"});
        }
        intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, limit > 1);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        return intent;
    }

    /** Android 13, or Android 11 and 12 with the photo picker from Google Play's system updates. */
    private static boolean hasPhotoPicker() {
        return Build.VERSION.SDK_INT >= 33
            || (Build.VERSION.SDK_INT >= 30 && SdkExtensions.getExtensionVersion(Build.VERSION_CODES.R) >= 2);
    }

    /** A picked item copied into `folder`: a HEIF photo made JPEG (ADR 0265 §2.1), anything else as it is, under a
        name with an extension (the program tells a photo from a video by it). */
    private static File media(ContentResolver resolver, Uri uri, File folder) throws java.io.IOException {
        String type = resolver.getType(uri);
        String name = name(resolver, uri);
        if (name.lastIndexOf('.') < 0 && type != null) {
            String extension = MimeTypeMap.getSingleton().getExtensionFromMimeType(type);
            name = name + "." + (extension != null ? extension : type.startsWith("video/") ? "mp4" : "jpg");
        }
        boolean heif = (type != null && (type.equals("image/heic") || type.equals("image/heif")))
            || name.toLowerCase(Locale.ROOT).endsWith(".heic") || name.toLowerCase(Locale.ROOT).endsWith(".heif");
        if (heif && Build.VERSION.SDK_INT >= 28) {
            File copy = new File(folder, name.substring(0, name.lastIndexOf('.')) + ".jpg");
            Bitmap bitmap = ImageDecoder.decodeBitmap(ImageDecoder.createSource(resolver, uri));
            try (OutputStream out = new FileOutputStream(copy)) {
                if (!bitmap.compress(Bitmap.CompressFormat.JPEG, 90, out)) {
                    throw new java.io.IOException("the photo could not be made JPEG");
                }
            } finally {
                bitmap.recycle();
            }
            return copy;
        }
        File copy = new File(folder, name);
        try (InputStream in = resolver.openInputStream(uri); OutputStream out = new FileOutputStream(copy)) {
            copy(in, out);
        }
        return copy;
    }

    /** The permissions a capture needs and the app does not hold: the camera's, when the app declares it (the
        camera's app is then refused it until granted), and the storage's below Android 10, for the gallery's item. */
    private static String[] needed(Activity activity) {
        ArrayList<String> needed = new ArrayList<String>();
        if (declares(activity, Manifest.permission.CAMERA)
                && activity.checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            needed.add(Manifest.permission.CAMERA);
        }
        if (Build.VERSION.SDK_INT < 29 && activity.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                != PackageManager.PERMISSION_GRANTED) {
            needed.add(Manifest.permission.WRITE_EXTERNAL_STORAGE);
        }
        return needed.toArray(new String[0]);
    }

    private static boolean declares(Context context, String permission) {
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(),
                PackageManager.GET_PERMISSIONS);
            if (info.requestedPermissions != null) {
                for (String p : info.requestedPermissions) {
                    if (permission.equals(p)) {
                        return true;
                    }
                }
            }
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        }
        return false;
    }

    /** Whether the device has a camera and an app that captures a photo (or a video); for MediaPicker.canCapture. */
    static boolean canCapture(Context context, boolean photo) {
        PackageManager manager = context.getPackageManager();
        return manager.hasSystemFeature(PackageManager.FEATURE_CAMERA_ANY)
            && new Intent(photo ? MediaStore.ACTION_IMAGE_CAPTURE : MediaStore.ACTION_VIDEO_CAPTURE)
                .resolveActivity(manager) != null;
    }

    /** The camera's app, writing to a new item of the gallery (Pictures or Movies, under the app's name). */
    private void capture(Activity activity) {
        try {
            boolean photo = capturingPhoto;
            String label = activity.getApplicationInfo().loadLabel(activity.getPackageManager()).toString()
                .replaceAll("[/\\\\:*?\"<>|]", "_");
            String stamp = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date());
            String file = (photo ? "IMG_" : "VID_") + stamp + (photo ? ".jpg" : ".mp4");
            String folder = photo ? Environment.DIRECTORY_PICTURES : Environment.DIRECTORY_MOVIES;
            ContentValues values = new ContentValues();
            values.put(MediaStore.MediaColumns.DISPLAY_NAME, file);
            values.put(MediaStore.MediaColumns.MIME_TYPE, photo ? "image/jpeg" : "video/mp4");
            if (Build.VERSION.SDK_INT >= 29) {
                // Not pending: only the item's owner may write a pending one, and the camera's app writes this one
                // (found on the Moto: the camera's app ended at its confirm). It is deleted again at a cancel.
                values.put(MediaStore.MediaColumns.RELATIVE_PATH, folder + "/" + label);
            } else {
                File where = new File(Environment.getExternalStoragePublicDirectory(folder), label);
                where.mkdirs();
                values.put(MediaStore.MediaColumns.DATA, new File(where, file).getAbsolutePath());
            }
            Uri collection = photo ? MediaStore.Images.Media.EXTERNAL_CONTENT_URI
                : MediaStore.Video.Media.EXTERNAL_CONTENT_URI;
            Intent intent = new Intent(photo ? MediaStore.ACTION_IMAGE_CAPTURE : MediaStore.ACTION_VIDEO_CAPTURE);
            if (intent.resolveActivity(activity.getPackageManager()) == null) {
                nativePicked(UNSUPPORTED, null); // no camera's app
                return;
            }
            capturing = activity.getContentResolver().insert(collection, values);
            if (capturing == null) {
                nativePicked(FAILED, null);
                return;
            }
            intent.putExtra(MediaStore.EXTRA_OUTPUT, capturing);
            intent.setClipData(ClipData.newRawUri("", capturing)); // the grant reaches older camera apps too
            intent.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivityForResult(intent, CAPTURE + 1);
        } catch (RuntimeException e) {
            nativePicked(FAILED, null);
        }
    }

    /** The camera's answer: the gallery's item, now holding the capture, copied to the cache; or deleted at a cancel. */
    private void captured(final Activity activity, final boolean taken, final Intent data) {
        final Uri item = capturing;
        capturing = null;
        if (activity == null || item == null) {
            nativePicked(CANCELLED, null);
            return;
        }
        final ContentResolver resolver = activity.getContentResolver();
        final File cache = new File(activity.getCacheDir(), "ceesafe-picked");
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    Uri from = item;
                    if (taken && size(resolver, item) == 0 && data != null && data.getData() != null) {
                        from = data.getData(); // a camera's app that kept the video where it chose
                    }
                    if (!taken || size(resolver, from) == 0) {
                        resolver.delete(item, null, null);
                        nativePicked(CANCELLED, null);
                        return;
                    }
                    if (!from.equals(item)) {
                        resolver.delete(item, null, null);
                    }
                    File folder = new File(cache, Long.toString(System.nanoTime()) + "-0");
                    folder.mkdirs();
                    File copy = media(resolver, from, folder);
                    ByteArrayOutputStream paths = new ByteArrayOutputStream();
                    paths.write(copy.getAbsolutePath().getBytes(StandardCharsets.UTF_8));
                    paths.write(0);
                    nativePicked(SAVED, paths.toByteArray());
                } catch (Exception e) {
                    nativePicked(FAILED, null);
                }
            }
        }, "ceesafe-camera").start();
    }

    /** The item's size in bytes (1 for one of an unknown size that has a byte), 0 when it is empty or unreadable. */
    private static long size(ContentResolver resolver, Uri uri) {
        try (android.content.res.AssetFileDescriptor file = resolver.openAssetFileDescriptor(uri, "r")) {
            long length = file != null ? file.getLength() : 0;
            if (length >= 0) {
                return length;
            }
        } catch (Exception e) {
            return 0;
        }
        try (InputStream in = resolver.openInputStream(uri)) {
            return in != null && in.read() >= 0 ? 1 : 0;
        } catch (Exception e) {
            return 0;
        }
    }

    private static int parse(String text) {
        try {
            return Integer.parseInt(text.trim());
        } catch (NumberFormatException e) {
            return 1;
        }
    }

    private static void copy(InputStream in, OutputStream out) throws java.io.IOException {
        if (in == null || out == null) {
            throw new java.io.IOException("the document cannot be opened");
        }
        byte[] buffer = new byte[65536];
        for (int n = in.read(buffer); n > 0; n = in.read(buffer)) {
            out.write(buffer, 0, n);
        }
    }

    /** The document's name as its provider shows it, without a folder; "document" when it gives none. */
    private static String name(ContentResolver resolver, Uri uri) {
        String found = null;
        try (Cursor cursor = resolver.query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                found = cursor.getString(0);
            }
        } catch (RuntimeException e) {
            found = null;
        }
        if (found == null) {
            found = uri.getLastPathSegment();
        }
        if (found == null) {
            return "document";
        }
        found = found.substring(found.lastIndexOf('/') + 1);
        return found.isEmpty() || found.equals(".") || found.equals("..") ? "document" : found;
    }

    /** The MIME type of a file name's extension, or `otherwise`. */
    private static String mime(String name, String otherwise) {
        int dot = name.lastIndexOf('.');
        String type = dot < 0 ? null
            : MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substring(dot + 1).toLowerCase());
        return type != null ? type : otherwise;
    }

    /** The MIME types of a list of extensions ("png, .jpg"); null, for every file, when the list is empty or names
        an extension the system does not know. */
    private static String[] mimes(String extensions) {
        ArrayList<String> types = new ArrayList<String>();
        for (String part : extensions.split("[, ]+")) {
            String extension = part.startsWith(".") ? part.substring(1) : part;
            if (extension.isEmpty()) {
                continue;
            }
            String type = MimeTypeMap.getSingleton().getMimeTypeFromExtension(extension.toLowerCase());
            if (type == null) {
                return null;
            }
            types.add(type);
        }
        return types.isEmpty() ? null : types.toArray(new String[0]);
    }
}
