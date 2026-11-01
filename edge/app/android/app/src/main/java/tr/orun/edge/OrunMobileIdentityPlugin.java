package tr.orun.edge;

import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;

import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.annotation.CapacitorPlugin;
import com.getcapacitor.PluginMethod;

import java.nio.charset.StandardCharsets;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.security.Signature;
import java.security.interfaces.ECPublicKey;
import java.security.spec.ECGenParameterSpec;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Owner-initiated P-256 phone identity. NOT sign-in or device authority.
 *
 * Private key remains in AndroidKeyStore. This uses documented Android
 * Keystore EC support, not the earlier Ed25519-only KeyMint assumption.
 * No software private-key fallback, no token or key in Android preferences.
 *
 * Server authenticates the exact SPKI DER + Ed25519/P256-distinct proof
 * from backend.edge_enrollment.signed_challenge(P256-SHA256).
 */
@CapacitorPlugin(name = "OrunMobileIdentity")
public final class OrunMobileIdentityPlugin extends Plugin {
    private static final String ALIAS = "orun.mobile.owner.p256.v1";
    private static final byte[] DOMAIN =
            "ORUN:STEP3C:EDGE-ENROLL:P256:v2\0"
                    .getBytes(StandardCharsets.US_ASCII);
    private final ExecutorService worker = Executors.newSingleThreadExecutor();

    private static KeyStore.PrivateKeyEntry entry(boolean create) throws Exception {
        KeyStore ks = KeyStore.getInstance("AndroidKeyStore");
        ks.load(null);
        if (!ks.containsAlias(ALIAS) && create) {
            KeyPairGenerator generator = KeyPairGenerator.getInstance(
                    KeyProperties.KEY_ALGORITHM_EC, "AndroidKeyStore");
            generator.initialize(new KeyGenParameterSpec.Builder(
                    ALIAS, KeyProperties.PURPOSE_SIGN)
                    .setAlgorithmParameterSpec(new ECGenParameterSpec("secp256r1"))
                    .setDigests(KeyProperties.DIGEST_SHA256)
                    .build());
            generator.generateKeyPair();
            ks.load(null);
        }
        if (!ks.containsAlias(ALIAS)) return null;
        KeyStore.Entry key = ks.getEntry(ALIAS, null);
        if (!(key instanceof KeyStore.PrivateKeyEntry)) {
            throw new IllegalStateException("KEY_TYPE");
        }
        return (KeyStore.PrivateKeyEntry) key;
    }

    private static byte[] publicKey(KeyStore.PrivateKeyEntry key) throws Exception {
        if (!(key.getCertificate().getPublicKey() instanceof ECPublicKey)) {
            throw new IllegalStateException("NOT_EC");
        }
        ECPublicKey pub = (ECPublicKey) key.getCertificate().getPublicKey();
        if (pub.getParams().getCurve().getField().getFieldSize() != 256) {
            throw new IllegalStateException("NOT_P256");
        }
        byte[] der = pub.getEncoded();
        if (der == null || der.length < 60 || der.length > 160) {
            throw new IllegalStateException("PUBLIC_KEY_ENCODING");
        }
        return der;
    }

    private static String hex(byte[] source) {
        char[] values = "0123456789abcdef".toCharArray();
        char[] out = new char[source.length * 2];
        for (int i = 0; i < source.length; i++) {
            int b = source[i] & 0xff;
            out[i * 2] = values[b >>> 4];
            out[i * 2 + 1] = values[b & 15];
        }
        return new String(out);
    }

    private static byte[] sha256(byte[] value) throws Exception {
        return MessageDigest.getInstance("SHA-256").digest(value);
    }

    private static String installation(byte[] publicDer) throws Exception {
        return "edge-" + hex(sha256(publicDer));
    }

    private static void proveUsable(KeyStore.PrivateKeyEntry key) throws Exception {
        Signature signer = Signature.getInstance("SHA256withECDSA");
        signer.initSign(key.getPrivateKey());
        signer.update(DOMAIN);
        byte[] proof = signer.sign();
        Signature verifier = Signature.getInstance("SHA256withECDSA");
        verifier.initVerify(key.getCertificate().getPublicKey());
        verifier.update(DOMAIN);
        if (!verifier.verify(proof)) throw new IllegalStateException("VERIFY");
    }

    /** No automatic private-key creation at startup. */
    @PluginMethod
    public void getIdentity(PluginCall call) {
        final boolean create = Boolean.TRUE.equals(call.getBoolean("create"));
        worker.execute(() -> {
            try {
                KeyStore.PrivateKeyEntry key = entry(create);
                JSObject answer = new JSObject();
                if (key == null) {
                    answer.put("present", false);
                } else {
                    byte[] publicDer = publicKey(key);
                    proveUsable(key);
                    answer.put("present", true);
                    answer.put("algorithm", "P256-SHA256");
                    answer.put("publicKeyBase64",
                            Base64.encodeToString(publicDer, Base64.NO_WRAP));
                    answer.put("installationId", installation(publicDer));
                    answer.put("fingerprint",
                            hex(sha256(publicDer)).substring(0, 20).toUpperCase(
                                    java.util.Locale.ROOT));
                }
                call.resolve(answer);
            } catch (Exception | LinkageError ignored) {
                call.reject("ORUN_MOBILE_IDENTITY_UNAVAILABLE",
                            "ORUN_MOBILE_IDENTITY_UNAVAILABLE");
            }
        });
    }

    /** Only signs a 16-byte server request ID + 32-byte challenge. */
    @PluginMethod
    public void signEnrollment(PluginCall call) {
        final String requestId = call.getString("requestId", "");
        final String challengeB64 = call.getString("challengeBase64", "");
        if (!requestId.matches("[0-9a-f]{32}") ||
                !challengeB64.matches("[A-Za-z0-9+/]{43}=")) {
            call.reject("ORUN_MOBILE_CHALLENGE_INVALID");
            return;
        }
        worker.execute(() -> {
            try {
                byte[] challenge = Base64.decode(challengeB64, Base64.NO_WRAP);
                if (challenge.length != 32) {
                    call.reject("ORUN_MOBILE_CHALLENGE_INVALID");
                    return;
                }
                KeyStore.PrivateKeyEntry key = entry(false);
                if (key == null) {
                    call.reject("ORUN_MOBILE_IDENTITY_MISSING");
                    return;
                }
                byte[] publicDer = publicKey(key);
                String installation = installation(publicDer);
                byte[] request = new byte[16];
                for (int i = 0; i < 16; i++) {
                    request[i] = (byte) Integer.parseInt(
                            requestId.substring(i * 2, i * 2 + 2), 16);
                }
                byte[] install = installation.getBytes(StandardCharsets.US_ASCII);
                byte[] data = new byte[DOMAIN.length + request.length +
                        challenge.length + publicDer.length + install.length];
                int offset = 0;
                for (byte[] part : new byte[][]{
                        DOMAIN, request, challenge, publicDer, install}) {
                    System.arraycopy(part, 0, data, offset, part.length);
                    offset += part.length;
                }
                Signature signature = Signature.getInstance("SHA256withECDSA");
                signature.initSign(key.getPrivateKey());
                signature.update(data);
                byte[] proof = signature.sign();
                if (proof.length < 8 || proof.length > 80) {
                    throw new IllegalStateException("INVALID_SIGNATURE");
                }
                JSObject result = new JSObject();
                result.put("signatureBase64",
                        Base64.encodeToString(proof, Base64.NO_WRAP));
                result.put("installationId", installation);
                call.resolve(result);
            } catch (Exception | LinkageError ignored) {
                call.reject("ORUN_MOBILE_SIGNATURE_UNAVAILABLE");
            }
        });
    }

    @Override
    protected void handleOnDestroy() {
        worker.shutdown();
        super.handleOnDestroy();
    }
}
