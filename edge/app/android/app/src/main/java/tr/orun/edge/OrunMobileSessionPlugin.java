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
    // 1b: account (username/e-mail + password) session, separate file and AAD.
    private static final String ACCOUNT_FILE = "orun-account-session.v1.bin";
    private static final byte[] ACCOUNT_DOMAIN =
            "ORUN:ACCOUNT:SESSION:AESGCM:v1".getBytes(StandardCharsets.US_ASCII);
    private static final long ACCOUNT_MAX_SECONDS = 200L * 24L * 3600L;
    private static final Pattern ACCOUNT_ID = Pattern.compile("account-[0-9a-f]{64}");
    private static final Pattern ACTOR = Pattern.compile("[A-Za-z0-9._-]{1,128}");
    private static final Pattern USERNAME = Pattern.compile("[a-z][a-z0-9._-]{2,63}");

    private static final String EDGE_FILE = "orun-edge-keys.v1.bin";
    private static final byte[] EDGE_DOMAIN =
            "ORUN:EDGE:KEYS:AESGCM:v1".getBytes(StandardCharsets.US_ASCII);

    private static final String WRITE_FILE = "orun-tracker-write-keys.v1.bin";
    private static final byte[] WRITE_DOMAIN =
            "ORUN:TRACKER:WRITEKEYS:AESGCM:v1".getBytes(StandardCharsets.US_ASCII);
    private static final int MAX_WRITE_KEYS = 1024;
    private static final int MAX_WRITE_BLOB = 196608;

    private final ExecutorService worker = Executors.newSingleThreadExecutor();

    private AtomicFile store() {
        return store(FILE_NAME);
    }

    private AtomicFile store(String name) {
        return new AtomicFile(new File(getContext().getNoBackupFilesDir(), name));
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
        write(store(), DOMAIN, session);
    }

    private void write(AtomicFile file, byte[] domain, JSONObject session) throws Exception {
        write(file, domain, session, 2048);
    }

    private void write(AtomicFile file, byte[] domain, JSONObject session, int maxPlain) throws Exception {
        byte[] plain = session.toString().getBytes(StandardCharsets.UTF_8);
        if (plain.length > maxPlain) throw new IllegalArgumentException("SESSION_TOO_BIG");
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, key(true));
        cipher.updateAAD(domain);
        byte[] iv = cipher.getIV();
        if (iv == null || iv.length != IV_BYTES)
            throw new IllegalStateException("INVALID_GCM_IV");
        byte[] encrypted = cipher.doFinal(plain);
        byte[] packed = new byte[1 + iv.length + encrypted.length];
        packed[0] = 1;
        System.arraycopy(iv, 0, packed, 1, iv.length);
        System.arraycopy(encrypted, 0, packed, 1 + iv.length, encrypted.length);
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

    /** Decrypted bytes of an encrypted session file, or null if absent. */
    private byte[] decrypt(AtomicFile file, byte[] domain) throws Exception {
        return decrypt(file, domain, MAX_BLOB);
    }

    private byte[] decrypt(AtomicFile file, byte[] domain, int maxBlob) throws Exception {
        if (!file.getBaseFile().exists()) return null;
        byte[] packed = file.readFully();
        if (packed.length < 1 + IV_BYTES + 16 || packed.length > maxBlob ||
                packed[0] != 1)
            throw new IllegalStateException("SESSION_BLOB_INVALID");
        SecretKey secret = key(false);
        if (secret == null) throw new IllegalStateException("SESSION_KEY_LOST");
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, secret,
                new GCMParameterSpec(128, packed, 1, IV_BYTES));
        cipher.updateAAD(domain);
        return cipher.doFinal(packed, 1 + IV_BYTES, packed.length - 1 - IV_BYTES);
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
        // Capacitor JSON commonly parses Unix seconds as Integer (until 2038).
        // PluginCall.getLong() accepts only Long and would reject valid sessions.
        final Object expiryValue = call.getData().opt("expiresAt");
        worker.execute(() -> {
            try {
                if (!(expiryValue instanceof Number) ||
                        !Double.isFinite(((Number) expiryValue).doubleValue()) ||
                        Math.floor(((Number) expiryValue).doubleValue()) !=
                                ((Number) expiryValue).doubleValue())
                    throw new IllegalArgumentException("EXPIRY_MISSING");
                long expiry = ((Number) expiryValue).longValue();
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

    private static void validateAccount(String origin, String site, String actor,
                                        String installation, String username,
                                        String token, long expiry) throws Exception {
        java.net.URI uri = new java.net.URI(origin);
        if (!"https".equals(uri.getScheme()) || uri.getHost() == null ||
                uri.getRawUserInfo() != null || uri.getRawQuery() != null ||
                uri.getRawFragment() != null ||
                (uri.getRawPath() != null && !uri.getRawPath().isEmpty()) ||
                !uri.toString().equals(origin))
            throw new IllegalArgumentException("HTTPS_ORIGIN_REQUIRED");
        boolean passwordInstallation = ACCOUNT_ID.matcher(installation).matches();
        boolean phoneInstallation = ID.matcher(installation).matches();
        if (!SITE.matcher(site).matches() || !ACTOR.matcher(actor).matches() ||
                !(passwordInstallation || phoneInstallation) ||
                (phoneInstallation && !installation.equals(installedPhoneId())) ||
                !USERNAME.matcher(username).matches() ||
                !TOKEN.matcher(token).matches())
            throw new IllegalArgumentException("ACCOUNT_SESSION_INVALID");
        long now = System.currentTimeMillis() / 1000;
        if (expiry > now + ACCOUNT_MAX_SECONDS)
            throw new IllegalArgumentException("ACCOUNT_EXPIRY_INVALID");
    }

    /** 1b: keep the account login across app restarts (AndroidKeyStore AES-GCM). */
    @PluginMethod
    public void putAccount(PluginCall call) {
        final String origin = call.getString("origin", "");
        final String site = call.getString("site", "");
        final String actor = call.getString("actor", "");
        final String installation = call.getString("installation", "");
        final String username = call.getString("username", "");
        final String token = call.getString("accessToken", "");
        final Object expiryValue = call.getData().opt("expiresAt");
        worker.execute(() -> {
            try {
                if (!(expiryValue instanceof Number))
                    throw new IllegalArgumentException("EXPIRY_MISSING");
                long expiry = ((Number) expiryValue).longValue();
                validateAccount(origin, site, actor, installation, username, token, expiry);
                if (expiry <= System.currentTimeMillis() / 1000)
                    throw new IllegalArgumentException("ACCOUNT_EXPIRED");
                JSONObject session = new JSONObject();
                session.put("v", 1);
                session.put("origin", origin);
                session.put("site", site);
                session.put("actor", actor);
                session.put("installation", installation);
                session.put("username", username);
                session.put("accessToken", token);
                session.put("expiresAt", expiry);
                write(store(ACCOUNT_FILE), ACCOUNT_DOMAIN, session);
                JSObject result = new JSObject();
                result.put("stored", true);
                call.resolve(result);
            } catch (Exception | LinkageError ignored) {
                call.reject("ORUN_ACCOUNT_SESSION_STORE_FAILED");
            }
        });
    }

    @PluginMethod
    public void getAccount(PluginCall call) {
        worker.execute(() -> {
            AtomicFile file = store(ACCOUNT_FILE);
            byte[] plain = null;
            try {
                plain = decrypt(file, ACCOUNT_DOMAIN);
                JSObject out = new JSObject();
                if (plain == null) {
                    out.put("present", false);
                    call.resolve(out);
                    return;
                }
                JSONObject json = new JSONObject(new String(plain, StandardCharsets.UTF_8));
                if (json.getInt("v") != 1) throw new IllegalStateException("ACCOUNT_VERSION");
                long expiry = json.getLong("expiresAt");
                validateAccount(json.getString("origin"), json.getString("site"),
                        json.getString("actor"), json.getString("installation"),
                        json.getString("username"), json.getString("accessToken"), expiry);
                if (expiry <= System.currentTimeMillis() / 1000) {
                    file.delete();
                    store(WRITE_FILE).delete();
                    out.put("present", false);
                    call.resolve(out);
                    return;
                }
                out.put("present", true);
                for (String k : new String[]{"origin", "site", "actor", "installation",
                        "username", "accessToken"})
                    out.put(k, json.getString(k));
                out.put("expiresAt", expiry);
                call.resolve(out);
            } catch (Exception | LinkageError ignored) {
                // Corrupt or foreign: forget it; the user signs in again.
                try { file.delete(); store(WRITE_FILE).delete(); }
                catch (Exception | LinkageError alsoIgnored) { }
                JSObject out = new JSObject();
                out.put("present", false);
                call.resolve(out);
            } finally {
                if (plain != null) Arrays.fill(plain, (byte) 0);
            }
        });
    }

    @PluginMethod
    public void clearAccount(PluginCall call) {
        worker.execute(() -> {
            try {
                store(EDGE_FILE).delete();
                store(WRITE_FILE).delete();
                AtomicFile file = store(ACCOUNT_FILE);
                if (file.getBaseFile().exists())
                    file.delete();
                JSObject out = new JSObject();
                out.put("cleared", true);
                call.resolve(out);
            } catch (Exception | LinkageError ignored) {
                call.reject("ORUN_ACCOUNT_SESSION_CLEAR_FAILED");
            }
        });
    }

    private JSONObject edgeAccount() throws Exception {
        byte[] plain = decrypt(store(ACCOUNT_FILE), ACCOUNT_DOMAIN);
        if (plain == null) throw new IllegalStateException("NO_ACCOUNT");
        try {
            JSONObject a = new JSONObject(new String(plain, StandardCharsets.UTF_8));
            long expiry = a.getLong("expiresAt");
            validateAccount(a.getString("origin"), a.getString("site"), a.getString("actor"),
                    a.getString("installation"), a.getString("username"), a.getString("accessToken"), expiry);
            if (expiry <= System.currentTimeMillis() / 1000) throw new IllegalStateException("EXPIRED");
            return a;
        } finally { Arrays.fill(plain, (byte) 0); }
    }

    private static void validateEdgeKeys(org.json.JSONArray keys) throws Exception {
        if (keys == null || keys.length() > 16) throw new IllegalArgumentException("EDGE_KEYS");
        java.util.HashSet<String> seen = new java.util.HashSet<>();
        for (int i = 0; i < keys.length(); i++) {
            JSONObject k = keys.getJSONObject(i);
            String gateway = k.getString("gateway"), key = k.getString("key");
            if (!gateway.matches("[0-9A-F]{16}") || !key.matches("[0-9A-F]{64}") ||
                    key.matches("0{64}") || key.matches("F{64}") || !seen.add(gateway))
                throw new IllegalArgumentException("EDGE_KEY");
        }
    }

    @PluginMethod
    public void putEdgeKeys(PluginCall call) {
        final org.json.JSONArray keys = call.getData().optJSONArray("keys");
        worker.execute(() -> {
            try {
                validateEdgeKeys(keys);
                JSONObject account = edgeAccount(), data = new JSONObject();
                for (String k : new String[]{"origin", "site", "actor", "installation"})
                    data.put(k, account.getString(k));
                data.put("keys", keys);
                write(store(EDGE_FILE), EDGE_DOMAIN, data);
                call.resolve();
            } catch (Exception | LinkageError ignored) {
                call.reject("ORUN_EDGE_KEYS_STORE_FAILED");
            }
        });
    }

    @PluginMethod
    public void getEdgeKeys(PluginCall call) {
        worker.execute(() -> {
            byte[] plain = null;
            try {
                JSONObject account = edgeAccount();
                plain = decrypt(store(EDGE_FILE), EDGE_DOMAIN);
                org.json.JSONArray keys = new org.json.JSONArray();
                if (plain != null) {
                    JSONObject data = new JSONObject(new String(plain, StandardCharsets.UTF_8));
                    for (String k : new String[]{"origin", "site", "actor", "installation"})
                        if (!account.getString(k).equals(data.getString(k)))
                            throw new IllegalStateException("FOREIGN_ACCOUNT");
                    keys = data.getJSONArray("keys");
                    validateEdgeKeys(keys);
                }
                JSObject result = new JSObject(); result.put("keys", keys); call.resolve(result);
            } catch (Exception | LinkageError ignored) {
                JSObject result = new JSObject(); result.put("keys", new org.json.JSONArray()); call.resolve(result);
            } finally { if (plain != null) Arrays.fill(plain, (byte) 0); }
        });
    }

    @PluginMethod
    public void clearEdgeKeys(PluginCall call) {
        worker.execute(() -> {
            try { store(EDGE_FILE).delete(); call.resolve(); }
            catch (Exception | LinkageError ignored) { call.reject("ORUN_EDGE_KEYS_CLEAR_FAILED"); }
        });
    }


    /** KQ-02B: validate and account-bind Tracker BLE/LoRa signing authority. */
    private static void validateWriteKeys(org.json.JSONArray keys) throws Exception {
        if (keys == null || keys.length() > MAX_WRITE_KEYS)
            throw new IllegalArgumentException("WRITE_KEY_COUNT");
        java.util.HashSet<String> seen = new java.util.HashSet<>();
        for (int i = 0; i < keys.length(); i++) {
            JSONObject k = keys.getJSONObject(i);
            String device = k.getString("device");
            String secret = k.getString("writeKey");
            Object rawEpoch = k.opt("epoch");
            if (!(rawEpoch instanceof Number))
                throw new IllegalArgumentException("WRITE_KEY_EPOCH");
            double n = ((Number) rawEpoch).doubleValue();
            if (!Double.isFinite(n) || Math.floor(n) != n ||
                    n <= 0 || n >= 0xffffffffL)
                throw new IllegalArgumentException("WRITE_KEY_EPOCH");
            long epoch = ((Number) rawEpoch).longValue();
            if (!device.matches("[0-9A-F]{16}") ||
                    !secret.matches("[0-9A-F]{64}") ||
                    secret.matches("0{64}") || secret.matches("F{64}") ||
                    !seen.add(device + ":" + epoch))
                throw new IllegalArgumentException("WRITE_KEY_INVALID");
        }
    }

    @PluginMethod
    public void putWriteKeys(PluginCall call) {
        final org.json.JSONArray keys = call.getData().optJSONArray("keys");
        worker.execute(() -> {
            try {
                validateWriteKeys(keys);
                JSONObject account = edgeAccount();
                JSONObject data = new JSONObject();
                for (String k : new String[]{"origin", "site", "actor", "installation"})
                    data.put(k, account.getString(k));
                data.put("keys", keys);
                write(store(WRITE_FILE), WRITE_DOMAIN, data, MAX_WRITE_BLOB - 29);
                call.resolve();
            } catch (Exception | LinkageError ignored) {
                call.reject("ORUN_WRITE_KEYS_STORE_FAILED");
            }
        });
    }

    @PluginMethod
    public void getWriteKeys(PluginCall call) {
        worker.execute(() -> {
            byte[] plain = null;
            try {
                JSONObject account = edgeAccount();
                plain = decrypt(store(WRITE_FILE), WRITE_DOMAIN, MAX_WRITE_BLOB);
                org.json.JSONArray keys = new org.json.JSONArray();
                if (plain != null) {
                    JSONObject data = new JSONObject(new String(plain, StandardCharsets.UTF_8));
                    for (String k : new String[]{"origin", "site", "actor", "installation"})
                        if (!account.getString(k).equals(data.getString(k)))
                            throw new IllegalStateException("FOREIGN_ACCOUNT");
                    keys = data.getJSONArray("keys");
                    validateWriteKeys(keys);
                }
                JSObject out = new JSObject(); out.put("keys", keys); call.resolve(out);
            } catch (Exception | LinkageError ignored) {
                call.reject("ORUN_WRITE_KEYS_UNAVAILABLE");
            } finally {
                if (plain != null) Arrays.fill(plain, (byte) 0);
            }
        });
    }

    @PluginMethod
    public void clearWriteKeys(PluginCall call) {
        worker.execute(() -> {
            try { store(WRITE_FILE).delete(); call.resolve(); }
            catch (Exception | LinkageError ignored) {
                call.reject("ORUN_WRITE_KEYS_CLEAR_FAILED");
            }
        });
    }

    @Override
    protected void handleOnDestroy() {
        worker.shutdown();
        super.handleOnDestroy();
    }
}
