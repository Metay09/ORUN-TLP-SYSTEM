package tr.orun.edge;

import java.util.Arrays;

/**
 * Where the native bridge keeps what it received, before it tells the
 * Gateway "aldım" (custody moves only after this returned STORED or
 * DUPLICATE). Same decision as the app's phone ledger (core/ledger.ts):
 * content-aware duplicates, conflicting content kept as evidence and never
 * acknowledged, read back after the write. Storage primitives are abstract:
 * SQLite on the phone (BridgeStore), memory in the PC check.
 */
abstract class BridgeSpool {
    static final int MAX_CONFLICTS = 4096;
    static final String STORED = "STORED", DUPLICATE = "DUPLICATE", CONFLICT = "CONFLICT",
            UNCERTAIN = "UNCERTAIN";

    static final class Saved {
        final int family;
        final byte[] plaintext;
        Saved(int family, byte[] plaintext) { this.family = family; this.plaintext = plaintext; }
    }

    /** One arrival: the opened record and how it reached this phone. */
    static final class Arrival {
        final ProductFrame.Opened item;
        final long gateway, nowSec;
        final Integer rssi, snr;
        Arrival(ProductFrame.Opened item, long gateway, Integer rssi, Integer snr, long nowSec) {
            this.item = item; this.gateway = gateway; this.rssi = rssi; this.snr = snr; this.nowSec = nowSec;
        }
    }

    // ---- storage primitives (inside one transaction) ----------------------
    protected abstract void begin() throws Exception;
    protected abstract void commit() throws Exception;
    protected abstract void rollback();
    protected abstract Saved find(byte[] device, byte[] incarnation, long sequence) throws Exception;
    protected abstract boolean anyConflict(byte[] device, byte[] incarnation, long sequence) throws Exception;
    protected abstract boolean sameConflict(byte[] device, byte[] incarnation, long sequence, int family,
                                            byte[] plaintext) throws Exception;
    protected abstract int conflictCount() throws Exception;
    protected abstract void insert(Arrival a) throws Exception;
    protected abstract void insertConflict(Arrival a) throws Exception;
    /** First sighting wins; a later signal only fills a missing one. */
    protected abstract void receipt(Arrival a) throws Exception;
    /** After COMMIT: the stored frame, read back, or null. */
    protected abstract byte[] storedFrame(Arrival a, boolean conflict) throws Exception;

    /** Ledger.ingest for an already authenticated record. */
    final String put(Arrival a, ProductFrame.Keys keys) {
        ProductFrame.Opened it = a.item;
        String state;
        boolean conflict = false;
        try {
            begin();
            try {
                Saved saved = find(it.device, it.incarnation, it.sequence);
                if (saved == null) {
                    insert(a);
                    state = STORED;
                } else if (saved.family == it.family && Arrays.equals(saved.plaintext, it.plaintext)) {
                    // A known divergence quarantines the whole identity.
                    state = anyConflict(it.device, it.incarnation, it.sequence) ? CONFLICT : DUPLICATE;
                } else {
                    if (!sameConflict(it.device, it.incarnation, it.sequence, it.family, it.plaintext)) {
                        if (conflictCount() >= MAX_CONFLICTS) throw new IllegalStateException("conflict capacity");
                        insertConflict(a);
                    }
                    state = CONFLICT;
                    conflict = true;
                }
                commit();
            } catch (Exception e) {
                rollback();
                throw e;
            }
            // Read back: a damaged store never yields a success.
            byte[] frame = storedFrame(a, conflict);
            ProductFrame.Opened back = frame == null ? null : ProductFrame.open(frame, keys);
            if (back == null || !Arrays.equals(back.device, it.device) ||
                    !Arrays.equals(back.incarnation, it.incarnation) || back.sequence != it.sequence ||
                    back.family != it.family || !Arrays.equals(back.plaintext, it.plaintext))
                return UNCERTAIN;
        } catch (Exception e) {
            return UNCERTAIN;
        }
        try {
            receipt(a);
        } catch (Exception ignored) {
            // Arrival time and signal are map facts; the record is committed.
        }
        return state;
    }
}
