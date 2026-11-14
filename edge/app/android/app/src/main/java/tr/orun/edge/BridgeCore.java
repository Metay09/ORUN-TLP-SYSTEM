package tr.orun.edge;

import java.util.ArrayDeque;
import java.util.Deque;
import java.util.HashSet;
import java.util.Iterator;
import java.util.Set;

/**
 * The phone's custody hand-off with one Bluetooth-attached Gateway, for the
 * native bridge (v1 step 7). Line-for-line the BLE half of the app's
 * core/edge.ts EdgeCore: HELLO first; every HELD is authenticated and stored
 * before its signed ACCEPT is queued; one ACCEPT in flight, sent again on
 * BUSY or after 3 s without an answer; OK/UNKNOWN settle it, REJECTED/FAIL
 * leave the record on the Gateway. No Android types.
 *
 * Checked on the PC: tools/android_mantik/bridge_core_vectors.txt holds
 * scenarios played through the real EdgeCore, step by step; this class must
 * produce the same ACCEPT bytes and the same counters at every step.
 */
final class BridgeCore {
    static final long ACCEPT_TIMEOUT_MS = 3000;

    interface EdgeKeys { byte[] edgeKey(String gatewayHex); }
    interface Clock { long seconds(); }

    private static final class Pending {
        final String key;
        final int page, slot;
        final byte[] message;
        long sent;
        Pending(String key, int page, int slot, byte[] message) {
            this.key = key; this.page = page; this.slot = slot; this.message = message;
        }
    }

    // counts: STORED, DUPLICATE, REJECTED, CONFLICT, UNCERTAIN
    final int[] counts = new int[5];
    int handedOff, acceptRejected;
    boolean acceptKeyMissing;
    Long gatewayId;
    private int helloHeld = -1, helloCapacity;
    private final Set<String> heldKeys = new HashSet<>(), dumpSeen = new HashSet<>();
    private boolean passDone;
    /** The last DUMP END matched the records announced (false: a message was lost). */
    boolean lastDumpComplete = true;
    private int incompleteFloor, okSinceHello;
    private final Deque<Pending> queue = new ArrayDeque<>();
    private final Set<String> queued = new HashSet<>();
    private Pending inFlight;

    private final BridgeSpool spool;
    private final ProductFrame.Keys keys;
    private final EdgeKeys edgeKeys;
    private final Clock clock;

    BridgeCore(BridgeSpool spool, ProductFrame.Keys keys, EdgeKeys edgeKeys, Clock clock) {
        this.spool = spool; this.keys = keys; this.edgeKeys = edgeKeys; this.clock = clock;
    }

    static String gatewayHex(long id) { return String.format("%016X", id); }

    int capacity() { return helloCapacity; }

    // ---- link ----------------------------------------------------------------
    /** The link dropped: the next attach starts from HELLO, nothing in flight. */
    synchronized void sessionEnd() {
        Integer held = heldNow();
        inFlight = null;
        queue.clear();
        queued.clear();
        gatewayId = null;
        if (helloHeld >= 0 && held != null) helloHeld = held;
        heldKeys.clear();
        dumpSeen.clear();
        passDone = false;
        incompleteFloor = 0;
        okSinceHello = 0;
    }

    /** One complete Gateway message. Returns a short log line or null. */
    synchronized String handleMessage(byte[] raw) {
        EdgeWire.Message m = EdgeWire.decode(raw);
        if (m == null) return null;
        switch (m.type) {
            case EdgeWire.HELLO:
                gatewayId = m.gateway;
                helloHeld = m.held;
                helloCapacity = m.capacity;
                heldKeys.clear();
                dumpSeen.clear();
                passDone = false;
                incompleteFloor = 0;
                okSinceHello = 0;
                return "HELLO " + gatewayHex(m.gateway) + " held=" + m.held + "/" + m.capacity;
            case EdgeWire.HELD:
            case EdgeWire.HELD_PREVIEW:
                if (gatewayId == null) return null;          // HELLO comes first; never guess
                return ingestHeld(gatewayId, m, m.type == EdgeWire.HELD_PREVIEW);
            case EdgeWire.DUMP_END:
                return "DUMP END count=" + m.count + (finishDump(m.count) ? "" : " incomplete");
            case EdgeWire.ACCEPT_RESULT:
                return acceptReply(word(m.code), m.page, m.slot);
            case EdgeWire.RECLAIMED:
                if (m.ok) forgetPage(m.page);
                return "RECLAIMED page=" + m.page + (m.ok ? " OK" : " FAIL");
            default:
                return null;
        }
    }

    /** The phone asked the Gateway for every held record again (DUMP). */
    synchronized void beginDump() {
        dumpSeen.clear();
        passDone = false;
        incompleteFloor = 0;
    }

    static String word(int code) {
        switch (code) {
            case 0: case 1: return "OK";
            case 2: return "UNKNOWN";
            case 3: return "BUSY";
            case 7: return "FAIL";
            default: return "REJECTED";
        }
    }

    // ---- accept queue --------------------------------------------------------
    /** The next ACCEPT to write, if any (one in flight at a time). nowMs: any monotonic clock. */
    synchronized byte[] nextAcceptMessage(long nowMs) {
        if (gatewayId == null || edgeKeys.edgeKey(gatewayHex(gatewayId)) == null) {
            queue.clear(); queued.clear(); inFlight = null;
            return null;
        }
        if (inFlight != null) {
            if (nowMs - inFlight.sent < ACCEPT_TIMEOUT_MS) return null;
            queue.addFirst(inFlight);                    // no reply: send again
            inFlight = null;
        }
        Pending p = queue.pollFirst();
        if (p == null) return null;
        p.sent = nowMs;
        inFlight = p;
        return p.message;
    }

    synchronized int pendingAccepts() { return queue.size() + (inFlight != null ? 1 : 0); }

    /** Exact after a complete dump; otherwise a conservative known count. */
    synchronized Integer heldNow() {
        if (passDone) return heldKeys.size();
        if (helloHeld < 0 && incompleteFloor == 0) return null;
        return Math.max(Math.max(heldKeys.size(), incompleteFloor),
                helloHeld >= 0 ? Math.max(0, helloHeld - okSinceHello) : 0);
    }

    private void queueAccept(long gateway, int page, int slot, long gen, byte[] raw) {
        String key = page + ":" + slot + ":" + Long.toUnsignedString(gen);
        if (queued.contains(key)) return;
        byte[] secret = edgeKeys.edgeKey(gatewayHex(gateway));
        acceptKeyMissing = secret == null;
        if (secret == null) return;
        byte[] message;
        try {
            message = EdgeWire.acceptMessage(gateway, page, slot, gen, raw, secret);
        } catch (Exception e) {
            acceptKeyMissing = true;
            return;
        }
        queued.add(key);
        queue.addLast(new Pending(key, page, slot, message));
    }

    private boolean finishDump(int count) {
        boolean complete = count >= 0 && dumpSeen.size() == count;
        passDone = complete;
        lastDumpComplete = complete;
        incompleteFloor = complete ? 0 : Math.max(0, count - okSinceHello);
        dumpSeen.clear();
        return complete;
    }

    private String acceptReply(String word, int page, int slot) {
        Pending f = inFlight;
        if (f == null) return null;
        if ("BUSY".equals(word)) {                       // store busy: retry
            queue.addFirst(f);
            inFlight = null;
            return null;
        }
        if (page != f.page || slot != f.slot) return null;
        inFlight = null;
        queued.remove(f.key);
        if ("OK".equals(word) || "UNKNOWN".equals(word)) {
            if ("OK".equals(word)) { handedOff++; okSinceHello++; }
            heldKeys.remove(f.page + ":" + f.slot);
            return null;
        }
        acceptRejected++;
        return "ACCEPT " + word + " page=" + f.page + " slot=" + f.slot + "; record stays on the Gateway";
    }

    private void forgetPage(int page) {
        String prefix = page + ":";
        for (Iterator<String> it = heldKeys.iterator(); it.hasNext(); )
            if (it.next().startsWith(prefix)) it.remove();
    }

    private String ingestHeld(long gateway, EdgeWire.Message m, boolean preview) {
        String place = m.page + ":" + m.slot;
        heldKeys.add(place);
        if (!preview) dumpSeen.add(place);
        ProductFrame.Opened item = ProductFrame.open(m.object, keys);
        if (item == null) {
            counts[2]++;
            return "REJECTED page=" + m.page + " slot=" + m.slot;
        }
        String state = spool.put(new BridgeSpool.Arrival(item, gateway, m.hasLink ? m.rssi : null,
                m.hasLink ? m.snr : null, clock.seconds()), keys);
        switch (state) {
            case BridgeSpool.STORED: counts[0]++; break;
            case BridgeSpool.DUPLICATE: counts[1]++; break;
            case BridgeSpool.CONFLICT: counts[3]++; break;
            default: counts[4]++; return "UNCERTAIN page=" + m.page + " slot=" + m.slot;
        }
        if (BridgeSpool.STORED.equals(state) || BridgeSpool.DUPLICATE.equals(state))
            queueAccept(gateway, m.page, m.slot, m.generation, m.object);
        return state + " " + BridgeCore.hexOf(item.device) + " seq=" + item.sequence;
    }

    static String hexOf(byte[] b) {
        StringBuilder sb = new StringBuilder();
        for (byte v : b) sb.append(String.format("%02X", v & 0xff));
        return sb.toString();
    }
}
