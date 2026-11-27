// SPDX-License-Identifier: MIT
// Voice notes (ADR 0272): a MediaRecorder writing AAC in an MPEG-4 file (.m4a) from the microphone, and a MediaPlayer
// for a file. Called from the program's thread (attached to the VM); the objects are the runtime's, held by global
// references until stopped or closed. RECORD_AUDIO is asked first by Picker (dialog kind 13).

package org.ceesafe.window;

import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.media.MediaRecorder;

import java.io.File;

final class Audio {
    private Audio() {}

    /** A recorder writing to `path`, recording; null when it could not start. */
    @SuppressWarnings("deprecation")
    static Object recordStart(String path) {
        MediaRecorder recorder = new MediaRecorder();
        try {
            recorder.setAudioSource(MediaRecorder.AudioSource.MIC);
            recorder.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4);
            recorder.setAudioEncoder(MediaRecorder.AudioEncoder.AAC);
            recorder.setAudioChannels(1);
            recorder.setAudioSamplingRate(44100);
            recorder.setAudioEncodingBitRate(64000);
            recorder.setOutputFile(path);
            recorder.prepare();
            recorder.start();
            return recorder;
        } catch (Exception e) {
            recorder.release();
            return null;
        }
    }

    /** The loudest moment since the last call, 0 to 1000. */
    static int recordLevel(Object recorder) {
        try {
            return ((MediaRecorder) recorder).getMaxAmplitude() * 1000 / 32767;
        } catch (RuntimeException e) {
            return 0;
        }
    }

    /** Stops and releases the recorder; with `cancel` the file is deleted. Whether a recording was made. */
    static boolean recordStop(Object recorder, String path, boolean cancel) {
        MediaRecorder r = (MediaRecorder) recorder;
        boolean made = true;
        try {
            r.stop();
        } catch (RuntimeException e) {
            made = false; // stopped at once: nothing was recorded
        }
        r.release();
        if (cancel || !made) {
            new File(path).delete();
        }
        return made && !cancel;
    }

    /** A player of the file at `path`, prepared; null when it cannot be played. */
    static Object playOpen(String path) {
        MediaPlayer player = new MediaPlayer();
        try {
            player.setAudioAttributes(new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                .build());
            player.setDataSource(path);
            player.prepare();
            return player;
        } catch (Exception e) {
            player.release();
            return null;
        }
    }

    /** 0 play, 1 pause, 2 close (released). */
    static void playAction(Object player, int action) {
        MediaPlayer p = (MediaPlayer) player;
        try {
            if (action == 0) {
                p.start();
            } else if (action == 1) {
                if (p.isPlaying()) {
                    p.pause();
                }
            } else {
                p.release();
            }
        } catch (RuntimeException e) {
            // a player in an error state: nothing to do
        }
    }

    static void playSeek(Object player, int ms) {
        try {
            ((MediaPlayer) player).seekTo(ms);
        } catch (RuntimeException e) {
            // not prepared: nothing to do
        }
    }

    /** 0 its position, 1 its length (milliseconds), 2 whether it plays (1 or 0). */
    static int playTime(Object player, int which) {
        MediaPlayer p = (MediaPlayer) player;
        try {
            return which == 0 ? p.getCurrentPosition() : which == 1 ? p.getDuration() : p.isPlaying() ? 1 : 0;
        } catch (RuntimeException e) {
            return 0;
        }
    }
}
