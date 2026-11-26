// Local notifications on Android (ADR 0243): the seventh Java class of CeeSafe's runtime, compiled into the same
// embedded dex as InputView by tools/gen_android_dex.py. Edit it here, then run the generator.
//
// The runtime calls these from the program's thread that makes the call (attached to the VM), never from the
// activity's, whose looper the permission dialog needs. The Context is the activity's, or a notification
// extension's service's (ADR 0263), which shows and clears but cannot ask for the permission. One channel, "ceesafe", named after the app, high importance.
// A notification's tag is its id, so showing an id again replaces it. Answers: 0, or ceesafe_rt_notify.c's codes:
// -5 Denied, -6 Failed.
package org.ceesafe.window;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Looper;

final class Notify {
    private static final int DENIED = -5;
    private static final int FAILED = -6;
    private static final String CHANNEL = "ceesafe";
    private static final String PERMISSION = "android.permission.POST_NOTIFICATIONS";

    private Notify() {}

    private static NotificationManager manager(Context context) {
        return (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
    }

    /**
     * 0 when the app may notify, else DENIED. Android 13 and later ask the user (the app declares POST_NOTIFICATIONS):
     * the system's dialog, waited for as Bluetooth's are (two seconds for it to come, a minute for the answer). Never
     * on the activity's thread, which the dialog needs.
     */
    static int permission(Context context) {
        if (!(context instanceof Activity)) {
            return DENIED; // a notification extension (ADR 0263) has no activity to ask from
        }
        final Activity activity = (Activity) context;
        try {
            if (Build.VERSION.SDK_INT >= 33 && activity.checkSelfPermission(PERMISSION) != PackageManager.PERMISSION_GRANTED) {
                if (Looper.myLooper() == Looper.getMainLooper()) {
                    return DENIED;
                }
                activity.runOnUiThread(() -> activity.requestPermissions(new String[] {PERMISSION}, 0x0243));
                long until = System.currentTimeMillis() + 2000;
                while (activity.hasWindowFocus() && System.currentTimeMillis() < until) {
                    pause(50);
                }
                until = System.currentTimeMillis() + 60000;
                while (!activity.hasWindowFocus() && System.currentTimeMillis() < until) {
                    pause(100);
                }
            }
            return manager(activity).areNotificationsEnabled() ? 0 : DENIED;
        } catch (Exception e) {
            return FAILED;
        }
    }

    private static void pause(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    static int show(Context activity, String id, String title, String body) {
        try {
            NotificationManager manager = manager(activity);
            if (!manager.areNotificationsEnabled()) {
                return DENIED;
            }
            Notification.Builder builder;
            if (Build.VERSION.SDK_INT >= 26) {
                if (manager.getNotificationChannel(CHANNEL) == null) {
                    CharSequence name = activity.getApplicationInfo().loadLabel(activity.getPackageManager());
                    manager.createNotificationChannel(
                            new NotificationChannel(CHANNEL, name, NotificationManager.IMPORTANCE_HIGH));
                }
                builder = new Notification.Builder(activity, CHANNEL);
            } else {
                builder = new Notification.Builder(activity);
                builder.setPriority(Notification.PRIORITY_HIGH);
                builder.setDefaults(Notification.DEFAULT_ALL);
            }
            Intent launch = activity.getPackageManager().getLaunchIntentForPackage(activity.getPackageName());
            if (launch == null) {
                if (!(activity instanceof Activity)) {
                    return FAILED;
                }
                launch = new Intent(activity, activity.getClass());
            }
            // The launcher's own intent: the app's task comes forward as it was, or the app starts.
            launch.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
            PendingIntent open = PendingIntent.getActivity(activity, id.hashCode(), launch,
                    PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
            int icon = activity.getApplicationInfo().icon;
            builder.setSmallIcon(icon != 0 ? icon : android.R.drawable.sym_action_chat)
                    .setContentTitle(title)
                    .setContentText(body)
                    .setStyle(new Notification.BigTextStyle().bigText(body))
                    .setCategory(Notification.CATEGORY_MESSAGE)
                    .setShowWhen(true)
                    .setWhen(System.currentTimeMillis())
                    .setAutoCancel(true)
                    .setContentIntent(open);
            manager.notify(id, 1, builder.build());
            return 0;
        } catch (Exception e) {
            return FAILED;
        }
    }

    static int clear(Context activity, String id) {
        try {
            manager(activity).cancel(id, 1);
            return 0;
        } catch (Exception e) {
            return FAILED;
        }
    }
}
