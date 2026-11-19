// The Android window's file dialogs (ADR 0158): the second Java class of CeeSafe's runtime, compiled into the same
// embedded dex as InputView by tools/gen_android_dex.py. Edit it here, then run the generator.
//
// A fragment without a view, added to the activity, since only an activity or a fragment is given the result of
// another activity. It starts the system's document picker (the Storage Access Framework) and, at its answer, copies
// the picked documents into the app's cache folder (open) or writes the program's file to the created document
// (export) on a thread of its own, then tells the runtime, whose program is waiting.
//
// android.app.Fragment is deprecated and present in every release; its replacement is a library an app would have to
// ship. If the system ends the app's process while the picker is in front, the activity that comes back cannot make
// this fragment again unless the app's APK holds this class (the runtime looks there first).
package org.ceesafe.window;

import android.app.Activity;
import android.app.Fragment;
import android.app.FragmentManager;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.webkit.MimeTypeMap;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;

public final class Picker extends Fragment {
    private static final String TAG = "org.ceesafe.window.Picker";
    private static final int FILE = 0;
    private static final int FILES = 1;
    private static final int EXPORT = 4;
    private static final int PRINT = 5;
    private static final int SAVED = 1;
    private static final int CANCELLED = 0;
    private static final int FAILED = -1;

    /** The file an export writes out, between its start and its result. */
    private String exporting;

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
    public void onActivityResult(int request, int result, Intent data) {
        final int kind = request - 1;
        final Activity activity = getActivity();
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
                            File copy = new File(folder, name(resolver, uris.get(k)));
                            try (InputStream in = resolver.openInputStream(uris.get(k));
                                 OutputStream out = new FileOutputStream(copy)) {
                                copy(in, out);
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
