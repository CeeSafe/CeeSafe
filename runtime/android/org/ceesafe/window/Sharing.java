// SPDX-License-Identifier: MIT
// Sharing a file and opening a file or a link (ADR 0268), started by Picker.start on the activity's thread. A file
// goes to the other app as a content: URI of the app's provider (org.ceesafe.Files, which the packer declares with the
// authority <package>.ceesafe.files), with a grant to read that one URI; nothing is copied. The answer goes to the
// runtime as a file dialog's does (Picker.answer): 1 handed over, -2 no app for it or no provider, -1 failed (a file
// that is not there, or the system refused).

package org.ceesafe.window;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.Intent;
import android.net.Uri;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.util.ArrayList;

final class Sharing {
    private static final int SHARE = 8;
    private static final int OPEN_FILE = 9;
    private static final int OPEN_LINK = 10;
    private static final int DONE = 1;
    private static final int FAILED = -1;
    private static final int UNSUPPORTED = -2;

    private Sharing() {}

    /** Starts what `kind` asks: the share sheet for `paths` (one a line) and `text`; the viewer for the file `target`;
        the app for the link `target`. */
    static void start(Activity activity, int kind, String paths, String target, String text) {
        try {
            if (kind == OPEN_LINK) {
                activity.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(target)));
                Picker.answer(DONE, null);
                return;
            }
            if (activity.getPackageManager().resolveContentProvider(authority(activity), 0) == null) {
                Picker.answer(UNSUPPORTED, null); // the APK declares no provider (ADR 0268 §2)
                return;
            }
            if (kind == OPEN_FILE) {
                Uri uri = uriOf(activity, target);
                if (uri == null) {
                    Picker.answer(FAILED, null);
                    return;
                }
                Intent view = new Intent(Intent.ACTION_VIEW);
                view.setDataAndType(uri, mime(target));
                view.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                activity.startActivity(view);
                Picker.answer(DONE, null);
                return;
            }
            ArrayList<Uri> uris = new ArrayList<Uri>();
            String type = null;
            for (String path : paths.split("\n")) {
                if (path.isEmpty()) {
                    continue;
                }
                Uri uri = uriOf(activity, path);
                if (uri == null) {
                    Picker.answer(FAILED, null);
                    return;
                }
                uris.add(uri);
                String one = mime(path);
                type = type == null ? one : sameKind(type, one);
            }
            Intent send = new Intent(uris.size() > 1 ? Intent.ACTION_SEND_MULTIPLE : Intent.ACTION_SEND);
            send.setType(uris.isEmpty() ? "text/plain" : type);
            if (uris.size() == 1) {
                send.putExtra(Intent.EXTRA_STREAM, uris.get(0));
            } else if (uris.size() > 1) {
                send.putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris);
            }
            if (!text.isEmpty()) {
                send.putExtra(Intent.EXTRA_TEXT, text);
            }
            if (!uris.isEmpty()) {
                // The grant goes with the chooser to the app chosen through the intent's clip.
                ClipData clip = ClipData.newRawUri("", uris.get(0));
                for (int k = 1; k < uris.size(); k++) {
                    clip.addItem(new ClipData.Item(uris.get(k)));
                }
                send.setClipData(clip);
                send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            }
            Intent chooser = Intent.createChooser(send, null);
            chooser.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            activity.startActivity(chooser);
            Picker.answer(DONE, null);
        } catch (ActivityNotFoundException e) {
            Picker.answer(UNSUPPORTED, null); // no app opens it
        } catch (RuntimeException e) {
            Picker.answer(FAILED, null);
        }
    }

    /** The provider's authority: the app's package and ".ceesafe.files". */
    static String authority(Activity activity) {
        return activity.getPackageName() + ".ceesafe.files";
    }

    /** The content: URI of the file at `path`, or null when there is no file there. The provider answers only for the
        app's own folders. */
    private static Uri uriOf(Activity activity, String path) {
        File file = new File(path);
        if (!file.isFile()) {
            return null;
        }
        return new Uri.Builder().scheme("content").authority(authority(activity)).path(file.getAbsolutePath()).build();
    }

    /** The MIME type of `path` by its extension, application/octet-stream when the system does not know it. */
    static String mime(String path) {
        int dot = path.lastIndexOf('.');
        int slash = path.lastIndexOf('/');
        String type = null;
        if (dot > slash && dot + 1 < path.length()) {
            type = MimeTypeMap.getSingleton().getMimeTypeFromExtension(path.substring(dot + 1).toLowerCase());
        }
        return type != null ? type : "application/octet-stream";
    }

    /** One type for files of types `a` and `b`: the same, or their kind's (image/*), or any. */
    private static String sameKind(String a, String b) {
        if (a.equals(b)) {
            return a;
        }
        int ka = a.indexOf('/');
        int kb = b.indexOf('/');
        if (ka > 0 && kb > 0 && a.substring(0, ka).equals(b.substring(0, kb))) {
            return a.substring(0, ka) + "/*";
        }
        return "*/*";
    }
}
