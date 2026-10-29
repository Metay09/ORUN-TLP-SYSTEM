package tr.orun.edge;

import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.AtomicFile;

import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.annotation.CapacitorPlugin;
import com.getcapacitor.PluginMethod;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.regex.Pattern;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/**
 * ORUN S5B: Android-only encrypted session vault.
 *
 * AES-256-GCM key never leaves AndroidKeyStore. Ciphertext lives in the
 * app-private noBackupFilesDir and is atomically replaced. Never use
 * localStorage / SharedPreferences / Android Auto Backup for bearer secrets.
 *
 * This is NOT an account login flow, proof of phone enrollment, or an HTTP
 * client. It stores an already validated, owner-approved session only after
 * the explicit enrollment flow calls putSession.
 */
@CapacitorPlugin(name = "OrunMobileSession")
public final class OrunMobileSessionPlugin extends Plugin {
    private static final String AES_ALIAS = "orun.mobile.session.aesgcm.v1";
    private static final String PHONE_ALIAS = "orun.mobile.owner.p256.v1";
    private static final String FILE_NAME = "orun-mobile-session.v1.bin";
    private static final byte[] DOMAIN =
            "ORUN:MOBILE:SESSION:AESGCM:v1".getBytes(StandardCharsets.US_ASCII);
    private static final int IV_BYTES = 12;
    private static final int MAX_BLOB = 4096;
    private static final long MAX_LIFETIME_SECONDS = 30L * 24L * 3600L + 600;
    private static final Pattern TOKEN = Pattern.compile("[A-Za-z0-9_-]{48,128}");
    private static final Pattern ID = Pattern.compile("edge-[0-9a-f]{64}");
    private static final Pattern SITE = Pattern.compile("[A-Za-z0-9._-]{1,128}");

    private final ExecutorService worker = Executors.newSingleThreadExecutor();

    private AtomicFile store() {
        return new AtomicFile(new File(getContext().getNoBackupFilesDir(), FILE_NAME));
    }

    private static String hex(byte[] data) {
        char[] alphabet = "0123456789abcdef".toCharArray();
        char[] out = new char[data.length * 2];
        for (int i = 0; i < data.length; i++) {
            int b = data[i] & 0xff;
            out[i * 2] = alphabet[b >>> 4];
            out[i * 2 + 1] = alphabet[b & 15];
        }
        return new String(out);
    }

    private static String installedPhoneId() throws Exception {
        KeyStore ks = KeyStore.getInstance("AndroidKeyStore");
        ks.load(null);
        if (!ks.containsAlias(PHONE_ALIAS) || ks.getCertificate(PHONE_ALIAS) == null)
            throw new IllegalStateException("PHONE_KEY_MISSING");
        byte[] spki = ks.getCertificate(PHONE_ALIAS).getPublicKey().getEncoded();
        if (spki == null || spki.length < 60 || spki.length > 160)
            throw new IllegalStateException("PHONE_KEY_INVALID");
        return "edge-" + hex(MessageDigest.getInstance("SHA-256").digest(spki));
    }

    private static SecretKey key(boolean create) throws Exception {
        KeyStore ks = KeyStore.getInstance("AndroidKeyStore");
        ks.load(null);
        if (ks.containsAlias(AES_ALIAS)) {
            return (SecretKey) ks.getKey(AES_ALIAS, null);
        }
        if (!create) return null;
        KeyGenerator generator = KeyGenerator.getInstance(
                KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        generator.init(new KeyGenParameterSpec.Builder(
                AES_ALIAS, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setKeySize(256)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setRandomizedEncryptionRequired(true)
                .build());
        return generator.generateKey();
    }

    private static void validate(String origin, String installation,
                                 String site, String token, long expiry)
            throws Exception {
        java.net.URI uri = new java.net.URI(origin);
        if (!"https".equals(uri.getScheme()) || uri.getHost() == null ||
                uri.getRawUserInfo() != null || uri.getRawQuery() != null ||
                uri.getRawFragment() != null ||
                (uri.getRawPath() != null && !uri.getRawPath().isEmpty()) ||
                (uri.getPort() != -1 && (uri.getPort() < 1 || uri.getPort() > 65535)) ||
                !uri.toString().equals(origin))
            throw new IllegalArgumentException("HTTPS_ORIGIN_REQUIRED");
        if (!ID.matcher(installation).matches() ||
                !installation.equals(installedPhoneId()) ||
                !SITE.matcher(site).matches() ||
                !TOKEN.matcher(token).matches())
            throw new IllegalArgumentException("SESSION_IDENTITY_INVALID");
        long now = System.currentTimeMillis() / 1000;
        if (expiry <= now || expiry > now + MAX_LIFETIME_SECONDS)
            throw new IllegalArgumentException("SESSION_EXPIRY_INVALID");
    }

    private void write(JSONObject session) throws Exception {
        byte[] plain = session.toString().getBytes(StandardCharsets.UTF_8);
        if (plain.length > 2048) throw new IllegalArgumentException("SESSION_TOO_BIG");
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, key(true));
        cipher.updateAAD(DOMAIN);
        byte[] iv = cipher.getIV();
        if (iv == null || iv.length != IV_BYTES)
            throw new IllegalStateException("INVALID_GCM_IV");
        byte[] encrypted = cipher.doFinal(plain);
        byte[] packed = new byte[1 + iv.length + encrypted.length];
        packed[0] = 1;
        System.arraycopy(iv, 0, packed, 1, iv.length);
        System.arraycopy(encrypted, 0, packed, 1 + iv.length, encrypted.length);
        AtomicFile file = store();
        FileOutputStream out = file.startWrite();
        try {
            out.write(packed);
            file.finishWrite(out);
        } catch (Exception ex) {
            file.failWrite(out);
            throw ex;
        } finally {
            Arrays.fill(plain, (byte) 0);
        }
    }

    private JSONObject read() throws Exception {
        AtomicFile file = store();
        if (!file.getBaseFile().exists()) return null;
        byte[] packed = file.readFully();
        if (packed.length < 1 + IV_BYTES + 16 || packed.length > MAX_BLOB ||
                packed[0] != 1)
            throw new IllegalStateException("SESSION_BLOB_INVALID");
        SecretKey secret = key(false);
        if (secret == null) throw new IllegalStateException("SESSION_KEY_LOST");
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, secret,
                new GCMParameterSpec(128, packed, 1, IV_BYTES));
        cipher.updateAAD(DOMAIN);
        byte[] plain = cipher.doFinal(
                packed, 1 + IV_BYTES, packed.length - 1 - IV_BYTES);
        try {
            JSONObject json = new JSONObject(new String(plain, StandardCharsets.UTF_8));
            if (json.getInt("v") != 1) throw new IllegalStateException("SESSION_VERSION");
            String origin = json.getString("origin");
            String installation = json.getString("installation");
            String site = json.getString("site");
            String token = json.getString("accessToken");
            long expiry = json.getLong("expiresAt");
            validate(origin, installation, site, token, expiry);
            return json;
        } finally {
            Arrays.fill(plain, (byte) 0);
        }
    }

    @PluginMethod
    public void putSession(PluginCall call) {
        final String origin = call.getString("origin", "");
        final String installation = call.getString("installation", "");
        final String site = call.getString("site", "");
        final String token = call.getString("accessToken", "");
        final Long expiry = call.getLong("expiresAt");
        worker.execute(() -> {
            try {
                if (expiry == null) throw new IllegalArgumentException("EXPIRY_MISSING");
                validate(origin, installation, site, token, expiry);
                JSONObject session = new JSONObject();
                session.put("v", 1);
                session.put("origin", origin);
                session.put("installation", installation);
                session.put("site", site);
                session.put("accessToken", token);
                session.put("expiresAt", expiry);
                write(session);
                JSObject result = new JSObject();
                result.put("stored", true);
                call.resolve(result);
            } catch (Exception | LinkageError ignored) {
                call.reject("ORUN_MOBILE_SESSION_STORE_FAILED");
            }
        });
    }

    @PluginMethod
    public void getSession(PluginCall call) {
        final boolean includeToken = Boolean.TRUE.equals(call.getBoolean("includeToken"));
        worker.execute(() -> {
            try {
                JSONObject session = read();
                JSObject out = new JSObject();
                out.put("present", session != null);
                if (session != null) {
                    out.put("origin", session.getString("origin"));
                    out.put("installation", session.getString("installation"));
                    out.put("site", session.getString("site"));
                    out.put("expiresAt", session.getLong("expiresAt"));
                    if (includeToken)
                        out.put("accessToken", session.getString("accessToken"));
                }
                call.resolve(out);
            } catch (Exception | LinkageError ignored) {
                // Fail closed, never return a corrupt/expired/foreign session.
                // Explicit clearSession is still available when re-enrolling.
                call.reject("ORUN_MOBILE_SESSION_UNAVAILABLE");
            }
        });
    }

    @PluginMethod
    public void clearSession(PluginCall call) {
        worker.execute(() -> {
            try {
                AtomicFile file = store();
                if (file.getBaseFile().exists())
                    file.delete();
                JSObject out = new JSObject();
                out.put("cleared", true);
                call.resolve(out);
            } catch (Exception | LinkageError ignored) {
                call.reject("ORUN_MOBILE_SESSION_CLEAR_FAILED");
            }
        });
    }

    @Override
    protected void handleOnDestroy() {
        worker.shutdown();
        super.handleOnDestroy();
    }
}
