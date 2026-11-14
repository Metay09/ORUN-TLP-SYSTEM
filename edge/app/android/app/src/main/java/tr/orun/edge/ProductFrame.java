package tr.orun.edge;

import java.security.MessageDigest;
import java.util.Arrays;

import javax.crypto.Cipher;
import javax.crypto.spec.SecretKeySpec;

/**
 * Opens a collar's PRODUCT_SECURE frame for the native bridge (v1 step 7):
 * authenticate (AES-CCM, RFC 3610) before trusting any field, then check the
 * content is canonical. Java copy of edge/app/src/core/product.ts
 * openProductFrame / periodicValid / eventValid and crypto.ts aesCcmDecrypt.
 * Checked on the PC against vectors made by the TypeScript original
 * (tools/android_mantik/bridge_vectors.txt): same input, same verdict.
 */
final class ProductFrame {
    /** Per-epoch traffic key of a collar, or null if unknown. */
    interface Keys { byte[] trafficKey(byte[] device, long epoch); }

    static final class Opened {
        final byte[] device, incarnation, plaintext, frame;
        final long sequence;
        final int family;
        Opened(byte[] device, byte[] incarnation, long sequence, int family, byte[] plaintext, byte[] frame) {
            this.device = device; this.incarnation = incarnation; this.sequence = sequence;
            this.family = family; this.plaintext = plaintext; this.frame = frame;
        }
    }

    private ProductFrame() { }

    /** The opened record, or null for anything not authentic or not canonical. */
    static Opened open(byte[] raw, Keys keys) {
        try {
            if (raw.length != 84 && raw.length != 100) return null;
            byte[] h = Arrays.copyOfRange(raw, 0, 28);
            int family = h[3] & 0xff, plainLen = h[6] & 0xff;
            boolean shape = (family == 1 && plainLen == 64 && raw.length == 100) ||
                    (family == 2 && plainLen == 48 && raw.length == 84);
            if (h[0] != 2 || h[1] != 4 || h[2] != 2 || h[7] != 0 || h[5] != 1 ||
                    (h[4] != 2 && h[4] != 3) || !shape)
                return null;
            byte[] device = Arrays.copyOfRange(h, 8, 16);
            long epoch = u32(h, 16);
            if (epoch == 0xffffffffL || u64(h, 20) == 0) return null;
            byte[] key = keys.trafficKey(device, epoch);
            if (key == null || key.length != 16) return null;
            byte[] nonce = new byte[13];
            System.arraycopy(h, 16, nonce, 0, 4);
            nonce[4] = 1;
            System.arraycopy(h, 20, nonce, 5, 8);
            byte[] plain = ccmDecrypt(key, nonce, Arrays.copyOfRange(raw, 28, raw.length), h, 8);
            if (plain == null) return null;
            if (!(family == 1 ? periodicValid(plain) : eventValid(plain))) return null;
            int inc = family == 1 ? 4 : 8, seq = family == 1 ? 12 : 16;
            return new Opened(device, Arrays.copyOfRange(plain, inc, inc + 8), u32(plain, seq), family,
                    plain, raw.clone());
        } catch (Exception e) {
            return null;
        }
    }

    // ---- canonical content (product.ts) -----------------------------------
    private static boolean okLocation(int source, long lat, long lon) {
        return source >= 1 && source <= 4 && lat >= -900000000L && lat <= 900000000L &&
                lon >= -1800000000L && lon <= 1800000000L;
    }

    static boolean periodicValid(byte[] p) {
        if (p.length != 64 || p[0] != 1 || (p[2] & 0xff) > 4 || (p[3] & 0xff) > 3 || (p[41] & 0xff) > 2)
            return false;
        int b42 = p[42] & 0xff;
        if ((b42 & 0x03) > 2 || ((b42 >> 2) & 0x07) > 4 || (b42 & 0xc0) != 0) return false;
        int flags = p[1] & 0xff, source = p[2] & 0xff, batteryState = p[3] & 0xff;
        if (u64(p, 4) == 0 || u32(p, 12) == 0) return false;
        long end = u32(p, 16), duration = u32(p, 20);
        if (duration == 0 || ((flags & 1) != 0) != (end != 0)) return false;
        long lat = i32(p, 24), lon = i32(p, 28), alt = i32(p, 32);
        int hdop = u16(p, 36), age = u16(p, 38), sats = p[40] & 0xff;
        boolean loc = (flags & 2) != 0, altitude = (flags & 4) != 0, ageValid = (flags & 8) != 0;
        if (!loc) {
            if (source != 0 || altitude || ageValid || lat != 0 || lon != 0 || alt != 0 || hdop != 0 ||
                    sats != 0 || age != 65535)
                return false;
        } else {
            if (!okLocation(source, lat, lon) || (!altitude && alt != 0)) return false;
            if ((ageValid && age > 65534) || (!ageValid && age != 65535)) return false;
            if (source != 1 && (hdop != 0 || sats != 0)) return false;
        }
        long covered = u32(p, 44), active = u32(p, 48);
        if ((flags & 16) == 0) {
            if (covered != 0 || active != 0 || u16(p, 52) != 0 || u16(p, 54) != 0 || p[41] != 0) return false;
        } else {
            if (covered == 0 || active > covered || covered > duration) return false;
            if ((p[41] & 0xff) != (covered == duration ? 2 : 1)) return false;
        }
        if ((flags & 128) != 0) {
            if ((flags & 16) == 0 || (p[43] & 0xff) > 200 || (active == 0 && p[43] != 0)) return false;
        } else if (p[43] != 0) {
            return false;
        }
        if ((flags & 32) == 0 && (u16(p, 56) != 0 || batteryState != 0 || p[42] != 0)) return false;
        if ((u16(p, 58) & ~0x07ff) != 0) return false;
        if ((flags & 64) == 0 && (u16(p, 58) != 0 || p[60] != 0 || p[61] != 0 || p[62] != 0 || p[63] != 0))
            return false;
        return true;
    }

    static boolean eventValid(byte[] p) {
        if (p.length != 48 || p[0] != 1) return false;
        int family = p[1] & 0xff, lifecycle = p[2] & 0xff, severity = p[3] & 0xff, flags = p[4] & 0xff;
        int reason = p[5] & 0xff, source = p[6] & 0xff, context = p[7] & 0xff;
        if (!(family >= 1 && family <= 4 && lifecycle >= 1 && lifecycle <= 2 && severity >= 1 && severity <= 3 &&
                flags <= 15 && source <= 4 && context <= 2))
            return false;
        if ((family == 3 && reason != 1 && reason != 2) || (family != 3 && reason != 0)) return false;
        if (u64(p, 8) == 0 || u32(p, 16) == 0 || u64(p, 24) == 0) return false;
        if (((flags & 1) != 0) != (u32(p, 20) != 0)) return false;
        long lat = i32(p, 32), lon = i32(p, 36);
        int age = u16(p, 40);
        if ((flags & 2) == 0) {
            if (source != 0 || (flags & 4) != 0 || lat != 0 || lon != 0 || age != 65535) return false;
        } else {
            if (!okLocation(source, lat, lon)) return false;
            if (((flags & 4) != 0 && age > 65534) || ((flags & 4) == 0 && age != 65535)) return false;
        }
        if (u16(p, 42) != 0) return false;
        if ((flags & 8) != 0) {
            if (context == 0) return false;
        } else if (context != 0 || u32(p, 44) != 0) {
            return false;
        }
        return true;
    }

    // ---- AES-CCM (crypto.ts), from AES block encryption --------------------
    static byte[] ccmDecrypt(byte[] key, byte[] nonce, byte[] sealed, byte[] aad, int tagLength) throws Exception {
        int L = 15 - nonce.length;
        if (sealed.length < tagLength || nonce.length < 7 || nonce.length > 13 || tagLength < 4 ||
                tagLength > 16 || tagLength % 2 != 0 || aad.length >= 0xff00)
            return null;
        int ctLen = sealed.length - tagLength;
        if (L < 4 && ctLen >= (1L << (8 * L))) return null;
        Cipher aes = Cipher.getInstance("AES/ECB/NoPadding");
        aes.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(key, "AES"));
        byte[] plain = new byte[ctLen];
        for (int i = 0, block = 1; i < ctLen; i += 16, block++) {
            byte[] s = aes.doFinal(counterBlock(nonce, block));
            for (int j = 0; j < 16 && i + j < ctLen; j++) plain[i + j] = (byte) (sealed[i + j] ^ s[j]);
        }
        // CBC-MAC over B_0 | (len(aad) | aad, padded) | plaintext (padded).
        byte[] b0 = new byte[16];
        b0[0] = (byte) ((aad.length > 0 ? 0x40 : 0) | (((tagLength - 2) / 2) << 3) | (L - 1));
        System.arraycopy(nonce, 0, b0, 1, nonce.length);
        long n = ctLen;
        for (int i = 15; i > 15 - L; i--) { b0[i] = (byte) n; n >>>= 8; }
        byte[] x = aes.doFinal(b0);
        if (aad.length > 0) {
            byte[] a = new byte[pad16(2 + aad.length)];
            a[0] = (byte) (aad.length >> 8); a[1] = (byte) aad.length;
            System.arraycopy(aad, 0, a, 2, aad.length);
            x = cbc(aes, x, a);
        }
        if (ctLen > 0) x = cbc(aes, x, Arrays.copyOf(plain, pad16(ctLen)));
        byte[] s0 = aes.doFinal(counterBlock(nonce, 0));
        byte[] expected = new byte[tagLength];
        for (int i = 0; i < tagLength; i++) expected[i] = (byte) (x[i] ^ s0[i]);
        return MessageDigest.isEqual(expected, Arrays.copyOfRange(sealed, ctLen, sealed.length)) ? plain : null;
    }

    private static int pad16(int n) { return (n + 15) / 16 * 16; }

    private static byte[] cbc(Cipher aes, byte[] x, byte[] data) throws Exception {
        for (int i = 0; i < data.length; i += 16) {
            byte[] b = new byte[16];
            for (int j = 0; j < 16; j++) b[j] = (byte) (x[j] ^ data[i + j]);
            x = aes.doFinal(b);
        }
        return x;
    }

    private static byte[] counterBlock(byte[] nonce, int i) {
        byte[] a = new byte[16];
        a[0] = (byte) (15 - nonce.length - 1);
        System.arraycopy(nonce, 0, a, 1, nonce.length);
        a[14] = (byte) (i >> 8);
        a[15] = (byte) i;
        return a;
    }

    static int u16(byte[] b, int o) { return ((b[o] & 0xff) << 8) | (b[o + 1] & 0xff); }
    static long u32(byte[] b, int o) {
        return ((long) (b[o] & 0xff) << 24) | ((b[o + 1] & 0xff) << 16) | ((b[o + 2] & 0xff) << 8) | (b[o + 3] & 0xff);
    }
    static long i32(byte[] b, int o) { return (int) u32(b, o); }
    static long u64(byte[] b, int o) {
        long v = 0;
        for (int i = 0; i < 8; i++) v = (v << 8) | (b[o + i] & 0xff);
        return v;
    }
}
