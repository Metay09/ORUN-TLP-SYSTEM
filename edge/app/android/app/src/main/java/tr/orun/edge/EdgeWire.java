package tr.orun.edge;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/**
 * Gateway <-> phone link over Bluetooth, for the native bridge (v1 step 7).
 * Java copy of edge/app/src/core/edgeLink.ts (itself firmware edge_link.h):
 * 20-byte framing, Gateway messages, the signed ACCEPT. No Android types:
 * checked on the PC against vectors made by the TypeScript original
 * (tools/android_mantik/bridge_vectors.txt, bash tools/orun_android_mantik_test.sh).
 */
final class EdgeWire {
    static final String SERVICE = "6f9a1c20-5b3e-4d8a-9e61-2c4b7a0d3e51";
    static final String TO_GATEWAY = "6f9a1c21-5b3e-4d8a-9e61-2c4b7a0d3e51";
    static final String TO_EDGE = "6f9a1c22-5b3e-4d8a-9e61-2c4b7a0d3e51";
    static final int FRAME_SIZE = 20, FRAME_DATA = 16, MAX_MESSAGE = 160;
    static final long REASSEMBLY_TIMEOUT_MS = 3000;
    static final byte LORA_LINE_TO_EDGE = 0x07;
    static final byte[] DUMP = {(byte) 0x81};

    private EdgeWire() { }

    /** One logical message as 20-byte frames. */
    static List<byte[]> frames(byte[] message, int messageId) {
        if (message.length == 0 || message.length > MAX_MESSAGE)
            throw new IllegalArgumentException("message size");
        List<byte[]> out = new ArrayList<>();
        for (int offset = 0, index = 0; offset < message.length; index++) {
            int n = Math.min(FRAME_DATA, message.length - offset);
            byte[] f = new byte[4 + n];
            f[0] = 1;
            f[1] = (byte) ((index == 0 ? 1 : 0) | (offset + n == message.length ? 2 : 0));
            f[2] = (byte) messageId;
            f[3] = (byte) index;
            System.arraycopy(message, offset, f, 4, n);
            out.add(f);
            offset += n;
        }
        return out;
    }

    /** Rebuilds one message; any gap, reorder, version or size problem drops it. */
    static final class Reassembler {
        private boolean active;
        private final ByteArrayOutputStream buf = new ByteArrayOutputStream();
        private int id, next;
        private long started;

        void reset() { active = false; buf.reset(); }

        byte[] push(byte[] frame, long nowMs) {
            if (frame.length <= 4 || frame.length > FRAME_SIZE || frame[0] != 1 || (frame[1] & ~3) != 0) {
                reset();
                return null;
            }
            boolean start = (frame[1] & 1) != 0, end = (frame[1] & 2) != 0;
            int fid = frame[2] & 0xff, index = frame[3] & 0xff;
            if (start) {
                if (index != 0) { reset(); return null; }
                active = true; buf.reset(); id = fid; next = 0; started = nowMs;
            } else if (!active || fid != id || index != next) {
                reset();
                return null;
            }
            if (buf.size() + frame.length - 4 > MAX_MESSAGE) { reset(); return null; }
            buf.write(frame, 4, frame.length - 4);
            next++;
            if (!end) return null;
            active = false;
            return buf.toByteArray();
        }

        void poll(long nowMs) {
            if (active && nowMs - started >= REASSEMBLY_TIMEOUT_MS) reset();
        }
    }

    // ---- Gateway messages --------------------------------------------------
    static final int HELLO = 1, HELD = 2, DUMP_END = 3, ACCEPT_RESULT = 4, RECLAIMED = 5, HELD_PREVIEW = 6;

    static final class Message {
        int type;
        long gateway, generation;
        int held, capacity, page, slot, count, code;
        boolean ok, hasLink;
        int rssi, snr;
        byte[] object;
    }

    static int u16(byte[] b, int o) { return ((b[o] & 0xff) << 8) | (b[o + 1] & 0xff); }

    static long u64(byte[] b, int o) {
        long v = 0;
        for (int i = 0; i < 8; i++) v = (v << 8) | (b[o + i] & 0xff);
        return v;
    }

    static Message decode(byte[] m) {
        if (m == null || m.length == 0) return null;
        Message d = new Message();
        switch (m[0]) {
            case 0x01:
                if (m.length != 13) return null;
                d.type = HELLO; d.gateway = u64(m, 1); d.held = u16(m, 9); d.capacity = u16(m, 11);
                return d;
            case 0x02:
            case 0x06:
                if (m.length < 18 || m.length != 18 + (m[17] & 0xff)) return null;
                d.type = m[0] == 0x06 ? HELD_PREVIEW : HELD;
                d.page = u16(m, 1); d.slot = u16(m, 3); d.generation = u64(m, 5);
                d.hasLink = (m[13] & 1) != 0;
                if (d.hasLink) { d.rssi = (short) u16(m, 14); d.snr = m[16]; }
                d.object = java.util.Arrays.copyOfRange(m, 18, m.length);
                return d;
            case 0x03:
                if (m.length != 3) return null;
                d.type = DUMP_END; d.count = u16(m, 1);
                return d;
            case 0x04:
                if (m.length != 6) return null;
                d.type = ACCEPT_RESULT; d.page = u16(m, 1); d.slot = u16(m, 3); d.code = m[5] & 0xff;
                return d;
            case 0x05:
                if (m.length != 4) return null;
                d.type = RECLAIMED; d.page = u16(m, 1); d.ok = m[3] != 0;
                return d;
            default:
                return null;
        }
    }

    // ---- Signed hand-off (SF5L5) -------------------------------------------
    /** HMAC-SHA256(K_edge, "ORUN-EDGE-ACCEPT-v1" | gw | page | slot | gen | SHA-256(object), zero padded to 102). */
    static byte[] acceptMac(long gateway, int page, int slot, long generation, byte[] object, byte[] key)
            throws Exception {
        boolean nonzero = false;
        for (byte b : key) nonzero |= b != 0;
        if (key.length != 32 || !nonzero) throw new IllegalStateException("EDGE_KEY_MISSING");
        byte[] msg = new byte[102];
        byte[] label = "ORUN-EDGE-ACCEPT-v1".getBytes(StandardCharsets.US_ASCII);
        int o = 0;
        System.arraycopy(label, 0, msg, o, label.length); o += label.length;
        o = be(msg, o, gateway, 8); o = be(msg, o, page, 2); o = be(msg, o, slot, 2); o = be(msg, o, generation, 8);
        byte[] digest = MessageDigest.getInstance("SHA-256").digest(object);
        System.arraycopy(digest, 0, msg, o, digest.length);
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(key, "HmacSHA256"));
        return mac.doFinal(msg);
    }

    /** BLE ACCEPT: 0x82 | page(2) | slot(2) | gen(8) | mac(32). */
    static byte[] acceptMessage(long gateway, int page, int slot, long generation, byte[] object, byte[] key)
            throws Exception {
        byte[] out = new byte[45];
        out[0] = (byte) 0x82;
        int o = be(out, 1, page, 2); o = be(out, o, slot, 2); o = be(out, o, generation, 8);
        System.arraycopy(acceptMac(gateway, page, slot, generation, object, key), 0, out, o, 32);
        return out;
    }

    private static int be(byte[] dst, int o, long v, int n) {
        for (int i = n - 1; i >= 0; i--) { dst[o + i] = (byte) v; v >>>= 8; }
        return o + n;
    }
}
