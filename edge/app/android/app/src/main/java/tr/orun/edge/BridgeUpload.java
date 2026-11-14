package tr.orun.edge;

import java.util.List;

/**
 * Sends what the native bridge holds to the account server
 * (POST /v1/edge/batch with the account session), exactly like the app's
 * core/sync.ts Uploader: newest first, 100 per request, every record tried
 * once per pass, UNCERTAIN left for the next pass, the session list sent
 * again while it grows. No Android types; the HTTP call is passed in.
 */
final class BridgeUpload {
    static final class Reply {
        final int status;
        final List<String> results;
        final boolean sessionsOk;
        Reply(int status, List<String> results, boolean sessionsOk) {
            this.status = status; this.results = results; this.sessionsOk = sessionsOk;
        }
    }

    interface Post { Reply post(String body) throws Exception; }

    static final class Unauthorized extends Exception {
        Unauthorized() { super("ORUN_SESSION_REJECTED"); }
    }

    static final class Result { int sent, settled, uncertain; }

    private BridgeUpload() { }

    /** The request body; every value is hex, a number, null or a fixed word. */
    static String body(String edge, BridgeStore.Batch b) {
        if (!edge.matches("[A-Za-z0-9._-]{1,64}")) throw new IllegalArgumentException("edge");
        StringBuilder s = new StringBuilder(256 + b.rows.size() * 260);
        s.append("{\"edge\":\"").append(edge).append("\",\"records\":[");
        for (int i = 0; i < b.rows.size(); i++) {
            BridgeStore.Outgoing o = b.rows.get(i);
            if (!o.frame.matches("[0-9A-F]+") || (o.gateway != null && !o.gateway.matches("[0-9A-F]{16}")))
                throw new IllegalArgumentException("row");
            if (i > 0) s.append(',');
            s.append("{\"frame\":\"").append(o.frame).append("\",\"gateway\":")
                .append(o.gateway == null ? "null" : "\"" + o.gateway + "\"")
                .append(",\"first_seen_utc\":").append(o.firstSeen)
                .append(",\"rssi\":").append(o.rssi)
                .append(",\"snr\":").append(o.snr).append('}');
        }
        s.append("],\"sessions\":[");
        for (int i = 0; i < b.sessions.size(); i++) {
            BridgeStore.Session x = b.sessions.get(i);
            if (i > 0) s.append(',');
            s.append("{\"id\":").append(x.id).append(",\"started_utc\":").append(x.started)
                .append(",\"alive_utc\":").append(x.alive).append(",\"transport\":")
                .append("ble".equals(x.transport) || "usb".equals(x.transport) ? "\"" + x.transport + "\"" : "null")
                .append('}');
        }
        return s.append("]}").toString();
    }

    /** One batch; beforeRowid continues a pass. */
    private static Result batch(BridgeStore store, Post post, Long beforeRowid, long nowSec, Long[] next)
            throws Exception {
        Result r = new Result();
        BridgeStore.Batch b = store.nextBatch(beforeRowid);
        next[0] = b.nextRowid;
        if (b.rows.isEmpty() && !b.sessionsChanged) return r;
        Reply reply = post.post(body(store.edgeId(), b));
        if (reply.status == 401) throw new Unauthorized();
        if (reply.status != 200) throw new IllegalStateException("HTTP " + reply.status);
        if (reply.results == null || reply.results.size() != b.rows.size())
            throw new IllegalStateException("unexpected answer");
        r.sent = b.rows.size();
        r.settled = store.settle(b, reply.results, reply.sessionsOk, nowSec);
        r.uncertain = r.sent - r.settled;
        return r;
    }

    /** One newest-to-oldest pass: each pending record is tried at most once. */
    static Result drain(BridgeStore store, Post post, long nowSec) throws Exception {
        Result total = new Result();
        Long before = null;
        Long[] next = {null};
        for (int i = 0; i < 1000; i++) {
            Result r = batch(store, post, before, nowSec, next);
            total.sent += r.sent; total.settled += r.settled; total.uncertain += r.uncertain;
            if (r.sent < BridgeStore.BATCH || next[0] == null) break;
            before = next[0];
        }
        return total;
    }
}
