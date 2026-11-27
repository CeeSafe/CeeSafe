// SPDX-License-Identifier: MIT
// A video (ADR 0273): a full-screen dialog over the activity, black, with a VideoView (MediaPlayer underneath) and
// the system's MediaController; it plays at once, a tap shows or hides the controls, and Back closes it. Its close is
// the dialog's answer (Picker.answer: 1 closed; -1 when the file is not there or cannot be played).

package org.ceesafe.window;

import android.app.Activity;
import android.app.Dialog;
import android.content.DialogInterface;
import android.graphics.Color;
import android.media.MediaPlayer;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.MediaController;
import android.widget.VideoView;

import java.io.File;

final class Movie {
    private static final int SHOWN = 1;
    private static final int FAILED = -1;

    private Movie() {}

    /** Shows the video at `path`, on the activity's thread; the answer comes when the dialog closes. */
    static void show(Activity activity, String path) {
        if (path == null || !new File(path).isFile()) {
            Picker.answer(FAILED, null);
            return;
        }
        final Dialog dialog = new Dialog(activity, android.R.style.Theme_Black_NoTitleBar_Fullscreen);
        final boolean[] failed = {false};
        FrameLayout frame = new FrameLayout(activity);
        frame.setBackgroundColor(Color.BLACK);
        final VideoView video = new VideoView(activity);
        // Wrapped and centred, the view keeps the video's proportions.
        frame.addView(video, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER));
        final MediaController controls = new MediaController(activity);
        controls.setAnchorView(frame);
        video.setMediaController(controls);
        video.setOnPreparedListener(new MediaPlayer.OnPreparedListener() {
            @Override
            public void onPrepared(MediaPlayer player) {
                video.start();
                controls.show();
            }
        });
        video.setOnErrorListener(new MediaPlayer.OnErrorListener() {
            @Override
            public boolean onError(MediaPlayer player, int what, int extra) {
                failed[0] = true;
                dialog.dismiss();
                return true;
            }
        });
        dialog.setContentView(frame);
        dialog.setOnDismissListener(new DialogInterface.OnDismissListener() {
            @Override
            public void onDismiss(DialogInterface shown) {
                video.stopPlayback();
                Picker.answer(failed[0] ? FAILED : SHOWN, null);
            }
        });
        video.setVideoPath(path);
        dialog.show();
    }
}
