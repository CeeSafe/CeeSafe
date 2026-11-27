// SPDX-License-Identifier: MIT
// Haptics (ADR 0270): the window's view's performHapticFeedback, on the activity's thread. It follows the user's
// setting for touch feedback and needs no permission. Kinds: 0 light, 1 medium, 2 heavy, 3 selection, 4 success,
// 5 warning, 6 error; success and error use CONFIRM and REJECT from Android 11 (API 30), a key's and a long press's
// before it.

package org.ceesafe.window;

import android.app.Activity;
import android.content.Context;
import android.os.Build;
import android.os.Vibrator;
import android.view.HapticFeedbackConstants;
import android.view.View;

final class Haptic {
    private Haptic() {}

    /** The feedback constant for `kind`. */
    private static int constant(int kind) {
        switch (kind) {
            case 0: return HapticFeedbackConstants.CLOCK_TICK;
            case 1: return HapticFeedbackConstants.VIRTUAL_KEY;
            case 2: return HapticFeedbackConstants.LONG_PRESS;
            case 3: return HapticFeedbackConstants.CLOCK_TICK;
            case 4: return Build.VERSION.SDK_INT >= 30 ? HapticFeedbackConstants.CONFIRM
                                                       : HapticFeedbackConstants.VIRTUAL_KEY;
            case 6: return Build.VERSION.SDK_INT >= 30 ? HapticFeedbackConstants.REJECT
                                                       : HapticFeedbackConstants.LONG_PRESS;
            default: return HapticFeedbackConstants.LONG_PRESS;
        }
    }

    /** Plays `kind`'s haptic on the activity's view; whether it was posted. */
    static boolean play(final Activity activity, final int kind) {
        try {
            final int feedback = constant(kind);
            activity.runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    View view = activity.getWindow() != null ? activity.getWindow().getDecorView() : null;
                    if (view != null) {
                        view.performHapticFeedback(feedback);
                    }
                }
            });
            return true;
        } catch (RuntimeException e) {
            return false;
        }
    }

    /** Whether the device has a vibrator. */
    static boolean available(Activity activity) {
        try {
            Vibrator vibrator = (Vibrator) activity.getSystemService(Context.VIBRATOR_SERVICE);
            return vibrator != null && vibrator.hasVibrator();
        } catch (RuntimeException e) {
            return false;
        }
    }
}
