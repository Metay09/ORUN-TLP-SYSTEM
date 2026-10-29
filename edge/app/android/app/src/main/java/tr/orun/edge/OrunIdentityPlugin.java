package tr.orun.edge;

import android.os.Build;
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
import java.security.PrivateKey;
import java.security.Signature;
import java.security.spec.ECGenParameterSpec;
import java.util.Arrays;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * ORUN Step3C Ed25519 AndroidKeyStore capability preflight.
 *
 * The native secret is NEVER exported, backed up, regenerated on read, or
 * stored in browser localStorage. Only an explicit user action may create it.
 *
 * Security boundary: this is a *pre-enrollment* identity only; no server token,
 * tracker read key, USB/BLE/LoRa authority or APPLIED state is issued here.
 */
@CapacitorPlugin(name = "OrunIdentity")
public class OrunIdentityPlugin extends Plugin {
    private static final String ALIAS = "orun.step3c.edge.ed25519.v1";
    private static final byte[] DOMAIN =
            "ORUN:STEP3C:EDGE-ENROLL:v1\0".getBytes(StandardCharsets.US_ASCII);
    // Strict X.509 SubjectPublicKeyInfo DER for Ed25519 (OID 1.3.101.112).
    private static final byte[] ED25519_SPKI_PREFIX = {
            0x30, 0x2a, 0x30, 0x05, 0x06, 0x03,
            0x2b, 0x65, 0x70, 0x03, 0x21, 0x00
    };

    private final ExecutorService worker = Executors.newSingleThreadExecutor();

    private static byte[] rawPublic(KeyStore.PrivateKeyEntry entry) throws Exception {
        byte[] enc = entry.getCertificate().getPublicKey().getEncoded();
        if (enc == null || enc.length != ED25519_SPKI_PREFIX.length + 32 ||
                !Arrays.equals(Arrays.copyOf(enc, ED25519_SPKI_PREFIX.length),
                        ED25519_SPKI_PREFIX)) {
            throw new IllegalStateException("NOT_ED25519");
        }
        return Arrays.copyOfRange(enc, ED25519_SPKI_PREFIX.length, enc.length);
    }

    private static KeyStore.PrivateKeyEntry entry(boolean create) throws Exception {
        KeyStore ks = KeyStore.getInstance("AndroidKeyStore");
        ks.load(null);
        if (!ks.containsAlias(ALIAS) && create) {
            if (Build.VERSION.SDK_INT < 33) {
                throw new IllegalStateException("UNSUPPORTED_ANDROID_API");
            }
            // Curve25519 KeyMint keys permit exactly one purpose: SIGN only.\n            // Fail closed if OEM KeyMint/TEE cannot generate Ed25519.
            // No software-only private-key fallback or exporting to preferences.
            KeyPairGenerator gen = KeyPairGenerator.getInstance(
                    "Ed25519", "AndroidKeyStore");
            gen.initialize(new KeyGenParameterSpec.Builder(ALIAS,
                    KeyProperties.PURPOSE_SIGN)
                    .setAlgorithmParameterSpec(new ECGenParameterSpec("Ed25519"))
                    .setDigests(KeyProperties.DIGEST_NONE)
                    .build());
            gen.generateKeyPair();
            ks.load(null);
        }
        if (!ks.containsAlias(ALIAS)) return null;
        KeyStore.Entry candidate = ks.getEntry(ALIAS, null);
        if (!(candidate instanceof KeyStore.PrivateKeyEntry)) {
            throw new IllegalStateException("UNEXPECTED_KEY_TYPE");
        }
        return (KeyStore.PrivateKeyEntry) candidate;
    }

    private static void proveKeyUsable(KeyStore.PrivateKeyEntry key) throws Exception {
        // Do not report a key as ready unless this AndroidKeyStore can really
        // sign and verify Ed25519 with the non-exportable private key.
        Signature sign = Signature.getInstance("Ed25519");
        sign.initSign(key.getPrivateKey());
        sign.update(DOMAIN);
        byte[] proof = sign.sign();
        if (proof.length != 64) throw new IllegalStateException("SIGNATURE_SIZE");
        Signature verify = Signature.getInstance("Ed25519");
        verify.initVerify(key.getCertificate().getPublicKey());
        verify.update(DOMAIN);
        if (!verify.verify(proof)) throw new IllegalStateException("SIGNATURE_VERIFY");
    }

    private static byte[] sha256(byte[] source) throws Exception {
        return MessageDigest.getInstance("SHA-256").digest(source);
    }

    private static String hex(byte[] source) {
        char[] table = "0123456789abcdef".toCharArray();
        char[] out = new char[source.length * 2];
        for (int i = 0; i < source.length; i++) {
            int n = source[i] & 0xff;
            out[2 * i] = table[n >>> 4];
            out[2 * i + 1] = table[n & 0x0f];
        }
        return new String(out);
    }

    private static String installationId(byte[] raw) throws Exception {
        return "edge-" + hex(sha256(raw));
    }

    @PluginMethod
    public void getIdentity(PluginCall call) {
        // The UI must pass create:true only after an explicit user tap.
        boolean create = Boolean.TRUE.equals(call.getBoolean("create"));
        worker.execute(() -> {
            try {
                KeyStore.PrivateKeyEntry key = entry(create);
                JSObject out = new JSObject();
                if (key == null) {
                    out.put("present", false);
                } else {
                    byte[] raw = rawPublic(key);
                    proveKeyUsable(key);
                    out.put("present", true);
                    out.put("publicKeyBase64", Base64.encodeToString(raw, Base64.NO_WRAP));
                    out.put("installationId", installationId(raw));
                    out.put("fingerprint", hex(sha256(raw)).substring(0, 20).toUpperCase());
                }
                call.resolve(out);
            } catch (Exception | LinkageError err) {
                // Do not include stack trace or provider-specific exception text.
                call.reject("ANDROID_KEYSTORE_ED25519_UNAVAILABLE");
            }
        });
    }

    /**
     * Sign only the server's exact one-time, domain-separated enrollment
     * challenge. No general-purpose signing or remote command signing API.
     * Owner approval + server nonce checks are additional required steps.
     */
    @PluginMethod
    public void signEnrollment(PluginCall call) {
        final String requestId = call.getString("requestId", "");
        final String challengeBase64 = call.getString("challengeBase64", "");
        if (!requestId.matches("[0-9a-f]{32}") || challengeBase64.length() > 64) {
            call.reject("ENROLLMENT_CHALLENGE_INVALID");
            return;
        }
        worker.execute(() -> {
            try {
                byte[] challenge = Base64.decode(challengeBase64, Base64.NO_WRAP);
                if (challenge.length != 32) {
                    call.reject("ENROLLMENT_CHALLENGE_INVALID");
                    return;
                }
                KeyStore.PrivateKeyEntry key = entry(false);
                if (key == null) {
                    call.reject("ENROLLMENT_KEY_NOT_CREATED");
                    return;
                }
                byte[] raw = rawPublic(key);
                byte[] identity = installationId(raw).getBytes(StandardCharsets.US_ASCII);
                byte[] req = new byte[16];
                for (int i = 0; i < 16; i++) {
                    req[i] = (byte) Integer.parseInt(requestId.substring(i * 2, i * 2 + 2), 16);
                }
                // Must exactly match backend.edge_enrollment.signed_challenge.
                byte[] data = new byte[DOMAIN.length + req.length +
                        challenge.length + raw.length + identity.length];
                int p = 0;
                for (byte[] part : new byte[][]{DOMAIN, req, challenge, raw, identity}) {
                    System.arraycopy(part, 0, data, p, part.length);
                    p += part.length;
                }
                PrivateKey privateKey = key.getPrivateKey();
                Signature signer = Signature.getInstance("Ed25519");
                signer.initSign(privateKey);
                signer.update(data);
                byte[] signed = signer.sign();
                if (signed.length != 64) throw new IllegalStateException("INVALID_PROOF");
                JSObject out = new JSObject();
                out.put("signatureBase64", Base64.encodeToString(signed, Base64.NO_WRAP));
                out.put("installationId", new String(identity, StandardCharsets.US_ASCII));
                call.resolve(out);
            } catch (Exception | LinkageError err) {
                call.reject("ENROLLMENT_SIGNATURE_UNAVAILABLE");
            }
        });
    }

    @Override
    protected void handleOnDestroy() {
        worker.shutdown();
        super.handleOnDestroy();
    }
}
