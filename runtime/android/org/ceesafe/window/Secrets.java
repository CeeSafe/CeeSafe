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

    // ---- Device keys (ADR 0275) ----
    // A P-256 key in the Android Keystore under "ceesafe.key.<app>.<name>", made in StrongBox where the device has
    // one (else the TEE), for signing with SHA-256; not bound to the user's authentication, so it signs in the
    // background; never exportable. Each method takes the io array: its input after a 4-byte big-endian length, and
    // the answer written over it from the start; it returns the answer's length, or -1 none, -6 Failed.

    private static String alias(String app, String name) {
        return "ceesafe.key." + app + "." + name;
    }

    private static byte[] input(byte[] io) {
        int n = ((io[0] & 0xff) << 24) | ((io[1] & 0xff) << 16) | ((io[2] & 0xff) << 8) | (io[3] & 0xff);
        if (n < 0 || n > io.length - 4) {
            return null;
        }
        byte[] in = new byte[n];
        System.arraycopy(io, 4, in, 0, n);
        return in;
    }

    private static int put32(byte[] io, int at, int v) {
        io[at] = (byte) (v >>> 24);
        io[at + 1] = (byte) (v >>> 16);
        io[at + 2] = (byte) (v >>> 8);
        io[at + 3] = (byte) v;
        return at + 4;
    }

    private static KeyStore keystore() throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        return store;
    }

    private static void generate(String alias, byte[] challenge, boolean strongBox) throws Exception {
        java.security.KeyPairGenerator generator =
            java.security.KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_EC, "AndroidKeyStore");
        KeyGenParameterSpec.Builder spec = new KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_SIGN)
            .setAlgorithmParameterSpec(new java.security.spec.ECGenParameterSpec("secp256r1"))
            .setDigests(KeyProperties.DIGEST_SHA256);
        if (challenge != null && challenge.length > 0) {
            spec.setAttestationChallenge(challenge);
        }
        if (strongBox && Build.VERSION.SDK_INT >= 28) {
            spec.setIsStrongBoxBacked(true);
        }
        generator.initialize(spec.build());
        generator.generateKeyPair();
    }

    /** The key, made when there is none (attested over the input as its challenge): created, hardware (1, 0, or 2
        not known), the attestation's kind (1: the Keystore's certificate chain), the SPKI, and the chain. */
    @SuppressWarnings("deprecation")
    static int keyObtain(Context activity, String app, String name, byte[] io) {
        synchronized (LOCK) {
            try {
                byte[] challenge = input(io);
                String alias = alias(app, name);
                KeyStore store = keystore();
                boolean created = false;
                if (!store.containsAlias(alias)) {
                    try {
                        generate(alias, challenge, true);
                    } catch (Exception strongBoxFailed) {
                        generate(alias, challenge, false); // no StrongBox here: the TEE
                    }
                    created = true;
                    store = keystore();
                }
                java.security.cert.Certificate[] chain = store.getCertificateChain(alias);
                if (chain == null || chain.length == 0) {
                    return FAILED;
                }
                byte[] spki = chain[0].getPublicKey().getEncoded();
                int hardware = 2;
                try {
                    java.security.PrivateKey key = (java.security.PrivateKey) store.getKey(alias, null);
                    android.security.keystore.KeyInfo info = (android.security.keystore.KeyInfo)
                        java.security.KeyFactory.getInstance(key.getAlgorithm(), "AndroidKeyStore")
                            .getKeySpec(key, android.security.keystore.KeyInfo.class);
                    hardware = info.isInsideSecureHardware() ? 1 : 0;
                } catch (Exception e) {
                    hardware = 2;
                }
                int need = 7 + spki.length + 4;
                for (java.security.cert.Certificate c : chain) {
                    need += 4 + c.getEncoded().length;
                }
                boolean attested = created && chain.length > 1;
                if (need > io.length) {
                    attested = false;
                    need = 7 + spki.length;
                    if (need > io.length) {
                        return FAILED;
                    }
                }
                io[0] = (byte) (created ? 1 : 0);
                io[1] = (byte) hardware;
                io[2] = (byte) (attested ? 1 : 0);
                int at = put32(io, 3, spki.length);
                System.arraycopy(spki, 0, io, at, spki.length);
                at += spki.length;
                if (attested) {
                    at = put32(io, at, chain.length);
                    for (java.security.cert.Certificate c : chain) {
                        byte[] der = c.getEncoded();
                        at = put32(io, at, der.length);
                        System.arraycopy(der, 0, io, at, der.length);
                        at += der.length;
                    }
                }
                return at;
            } catch (Exception e) {
                return FAILED;
            }
        }
    }

    /** The key's public half (SPKI), or -1 when there is no key. */
    static int keyPublic(Context activity, String app, String name, byte[] io) {
        synchronized (LOCK) {
            try {
                java.security.cert.Certificate c = keystore().getCertificate(alias(app, name));
                if (c == null) {
                    return NONE;
                }
                byte[] spki = c.getPublicKey().getEncoded();
                if (spki.length > io.length) {
                    return FAILED;
                }
                System.arraycopy(spki, 0, io, 0, spki.length);
                return spki.length;
            } catch (Exception e) {
                return FAILED;
            }
        }
    }

    /** The input signed (ECDSA with SHA-256, DER), or -1 when there is no key. */
    static int keySign(Context activity, String app, String name, byte[] io) {
        synchronized (LOCK) {
            try {
                byte[] message = input(io);
                if (message == null) {
                    return FAILED;
                }
                KeyStore.Entry entry = keystore().getEntry(alias(app, name), null);
                if (!(entry instanceof KeyStore.PrivateKeyEntry)) {
                    return NONE;
                }
                java.security.Signature signer = java.security.Signature.getInstance("SHA256withECDSA");
                signer.initSign(((KeyStore.PrivateKeyEntry) entry).getPrivateKey());
                signer.update(message);
                byte[] signature = signer.sign();
                if (signature.length > io.length) {
                    return FAILED;
                }
                System.arraycopy(signature, 0, io, 0, signature.length);
                return signature.length;
            } catch (Exception e) {
                return FAILED;
            }
        }
    }

    /** Removes the key: 0, or -1 when there was none. */
    static int keyRemove(Context activity, String app, String name, byte[] io) {
        synchronized (LOCK) {
            try {
                KeyStore store = keystore();
                String alias = alias(app, name);
                if (!store.containsAlias(alias)) {
                    return NONE;
                }
                store.deleteEntry(alias);
                return 0;
            } catch (Exception e) {
                return FAILED;
            }
        }
    }
}
