// The system's secret store on Android (ADR 0242): the sixth Java class of CeeSafe's runtime, compiled into the same
// embedded dex as InputView by tools/gen_android_dex.py. Edit it here, then run the generator.
//
// The runtime calls these from the program's thread that makes the call (attached to the VM), never from the
// activity's; the Context is the activity's, or a notification extension's service's (ADR 0263). Each value is sealed with an AES-256-GCM key kept in the Android Keystore under the alias
// "ceesafe.secrets.<app>" (made at the first write, in StrongBox where the device has one; never exportable), with
// a nonce of the Keystore's choosing and "<app>/<name>" as associated data, and kept in the app's no-backup folder as
// ceesafe-secrets/<app>/<name>: the nonce's length (one byte), the nonce, then the sealed bytes. Answers are a read's
// length, 0 for a write or a remove, or ceesafe_rt_secrets.c's codes: -1 none, -5 Denied, -6 Failed.
package org.ceesafe.window;

import android.content.Context;
import android.os.Build;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

final class Secrets {
    private static final int NONE = -1;
    private static final int FAILED = -6;
    private static final int LIMIT = 65536;
    private static final Object LOCK = new Object();

    private Secrets() {}

    private static File folder(Context activity, String app) {
        return new File(new File(activity.getNoBackupFilesDir(), "ceesafe-secrets"), app);
    }

    private static byte[] associated(String app, String name) {
        return (app + "/" + name).getBytes(StandardCharsets.UTF_8);
    }

    /** The app's key, made if `make` and missing; null when there is none. */
    private static SecretKey key(String app, boolean make) throws Exception {
        String alias = "ceesafe.secrets." + app;
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        KeyStore.Entry entry = store.getEntry(alias, null);
        if (entry instanceof KeyStore.SecretKeyEntry) {
            return ((KeyStore.SecretKeyEntry) entry).getSecretKey();
        }
        if (!make) {
            return null;
        }
        Exception last = null;
        for (int attempt = 0; attempt < 2; attempt++) {
            boolean strong = attempt == 0 && Build.VERSION.SDK_INT >= 28;
            if (attempt == 0 && !strong) {
                continue;
            }
            try {
                KeyGenParameterSpec.Builder spec = new KeyGenParameterSpec.Builder(alias,
                        KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                        .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                        .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                        .setKeySize(256);
                if (strong) {
                    spec.setIsStrongBoxBacked(true);
                }
                KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
                generator.init(spec.build());
                return generator.generateKey();
            } catch (Exception e) {
                last = e; // no StrongBox here: the TEE's, or the system's, Keystore
            }
        }
        throw last != null ? last : new IllegalStateException("no key");
    }

    static int read(Context activity, String app, String name, byte[] out) {
        synchronized (LOCK) {
            try {
                File file = new File(folder(activity, app), name);
                if (!file.isFile()) {
                    return NONE;
                }
                long size = file.length();
                if (size < 2 || size > LIMIT + 1024) {
                    return FAILED;
                }
                byte[] sealed = new byte[(int) size];
                try (FileInputStream in = new FileInputStream(file)) {
                    int done = 0;
                    while (done < sealed.length) {
                        int n = in.read(sealed, done, sealed.length - done);
                        if (n < 0) {
                            return FAILED;
                        }
                        done += n;
                    }
                }
                int nonce = sealed[0] & 0xff;
                if (nonce < 12 || 1 + nonce > sealed.length) {
                    return FAILED;
                }
                SecretKey key = key(app, false);
                if (key == null) {
                    return FAILED; // the Keystore lost its key: the value can never be opened
                }
                Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
                byte[] iv = new byte[nonce];
                System.arraycopy(sealed, 1, iv, 0, nonce);
                cipher.init(Cipher.DECRYPT_MODE, key, new GCMParameterSpec(128, iv));
                cipher.updateAAD(associated(app, name));
                byte[] plain = cipher.doFinal(sealed, 1 + nonce, sealed.length - 1 - nonce);
                if (plain.length > out.length) {
                    return FAILED;
                }
                System.arraycopy(plain, 0, out, 0, plain.length);
                java.util.Arrays.fill(plain, (byte) 0);
                return plain.length;
            } catch (Exception e) {
                return FAILED;
            }
        }
    }

    static int write(Context activity, String app, String name, byte[] value) {
        synchronized (LOCK) {
            try {
                File dir = folder(activity, app);
                if (!dir.isDirectory() && !dir.mkdirs()) {
                    return FAILED;
                }
                Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
                cipher.init(Cipher.ENCRYPT_MODE, key(app, true));
                cipher.updateAAD(associated(app, name));
                byte[] sealed = cipher.doFinal(value);
                byte[] iv = cipher.getIV();
                File part = new File(dir, name + ".part");
                try (FileOutputStream out = new FileOutputStream(part)) {
                    out.write(iv.length);
                    out.write(iv);
                    out.write(sealed);
                    out.getFD().sync();
                }
                if (!part.renameTo(new File(dir, name))) {
                    part.delete();
                    return FAILED;
                }
                return 0;
            } catch (Exception e) {
                return FAILED;
            }
        }
    }

    static int remove(Context activity, String app, String name, byte[] unused) {
        synchronized (LOCK) {
            File file = new File(folder(activity, app), name);
            return !file.exists() || file.delete() ? 0 : FAILED;
        }
    }
}
