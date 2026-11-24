// The clipboard on Android (ADR 0154 §4, amended 2026-11-25): the eighth Java class of CeeSafe's runtime, compiled into
// the same embedded dex as InputView by tools/gen_android_dex.py. Edit it here, then run the generator.
//
// The runtime calls these from the program's thread (Window.setClipboardText, Window.clipboardText), attached to the
// VM for the call. The text is UTF-8 both ways. Android 10 and later give the clipboard only to the app with the focus
// (or the keyboard): a program reads it while its window is in front, as an edit menu's Paste does.
package org.ceesafe.window;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import java.nio.charset.StandardCharsets;

final class Clip {
    private Clip() {}

    /** Puts `utf8` on the clipboard as plain text: 0, or -1 when the system refused. */
    static int set(Activity activity, byte[] utf8) {
        try {
            ClipboardManager clipboard = (ClipboardManager) activity.getSystemService(Context.CLIPBOARD_SERVICE);
            clipboard.setPrimaryClip(ClipData.newPlainText("text", new String(utf8, StandardCharsets.UTF_8)));
            return 0;
        } catch (Exception e) {
            return -1;
        }
    }

    /** The clipboard's text as UTF-8, or null when it holds none (or the app may not read it now). */
    static byte[] get(Activity activity) {
        try {
            ClipboardManager clipboard = (ClipboardManager) activity.getSystemService(Context.CLIPBOARD_SERVICE);
            ClipData clip = clipboard.getPrimaryClip();
            if (clip == null || clip.getItemCount() == 0) {
                return null;
            }
            CharSequence text = clip.getItemAt(0).coerceToText(activity);
            return text != null ? text.toString().getBytes(StandardCharsets.UTF_8) : null;
        } catch (Exception e) {
            return null;
        }
    }
}
