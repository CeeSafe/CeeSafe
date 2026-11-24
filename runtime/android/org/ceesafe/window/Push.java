// Push tokens on Android (ADR 0249): the ninth Java class of CeeSafe's runtime, compiled into the same embedded dex.
// The token is Firebase Cloud Messaging's, from the app's own copy of Firebase Messaging (CeeUI's packer puts it in an
// APK built with --firebase). The runtime does not link Firebase: its classes are found by reflection through the
// app's class loader, so an app without them has no token (Unsupported) and nothing else changes. Called on the
// program's thread, never the activity's: the token may take a round trip to Google.
package org.ceesafe.window;

import android.app.Activity;
import android.util.Log;

import java.lang.reflect.Method;
import java.util.concurrent.TimeUnit;

final class Push {
    private Push() {}

    /** The token as "fcm:TOKEN"; null when the app has no Firebase Messaging; "" when it has but no token came. */
    static String token(Activity activity) {
        ClassLoader loader = activity.getClassLoader();
        Class<?> messaging;
        Class<?> tasks;
        Class<?> task;
        try {
            messaging = Class.forName("com.google.firebase.messaging.FirebaseMessaging", true, loader);
            tasks = Class.forName("com.google.android.gms.tasks.Tasks", true, loader);
            task = Class.forName("com.google.android.gms.tasks.Task", true, loader);
        } catch (ClassNotFoundException e) {
            return null;
        }
        try {
            Object instance = messaging.getMethod("getInstance").invoke(null);
            Object pending = messaging.getMethod("getToken").invoke(instance);
            Method await = tasks.getMethod("await", task, long.class, TimeUnit.class);
            Object token = await.invoke(null, pending, 30L, TimeUnit.SECONDS);
            return token instanceof String && !((String) token).isEmpty() ? "fcm:" + token : "";
        } catch (Throwable e) {
            // No configuration (google-services.json missing), no Google Play services, no network in 30 s: why goes
            // to the log, the program has Failed.
            Throwable cause = e.getCause() != null ? e.getCause() : e;
            if (cause.getCause() != null) {
                cause = cause.getCause();
            }
            Log.w("ceesafe", "push token: " + cause);
            return "";
        }
    }
}
