package tr.orun.edge;

import android.content.Context;
import android.util.AtomicFile;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

/**
 * Account-scoped cache of UNVERIFIED encrypted history pages. This is NOT a
 * ledger or a delivery receipt. Only AccountObservationMirror may AEAD-verify
 * the frames and advance its durable SQLite cursor.
 *
 * Each native watch poll may append one bounded page AFTER alerts/latest.
 * Never cache credentials, plaintext, ACKs or live delivery claims.
 */
final class HerdHistoryStage {
    private static final String FILE = "herd-history-stage.v1.json";
    private static final int MAX_PAGES = 32;
    private static final int MAX_FILE_BYTES = 1_500_000;
    private static final int MAX_PAGE_BYTES = 150_000;

    private HerdHistoryStage() {}

    private static AtomicFile file(Context context) {
        return new AtomicFile(new File(context.getNoBackupFilesDir(), FILE));
    }

    private static boolean same(JSONObject saved, JSONObject account) {
        for (String key : new String[]{"origin", "site", "actor", "installation"}) {
            if (!saved.optString(key, "").equals(account.optString(key, ""))) return false;
        }
        return true;
    }

    private static JSONObject fresh(JSONObject account, long cursor) throws Exception {
        JSONObject out = new JSONObject();
        for (String key : new String[]{"origin", "site", "actor", "installation"})
            out.put(key, account.getString(key));
        out.put("base", cursor);
        out.put("pages", new JSONArray());
        return out;
    }

    private static JSONObject read(Context ctx) throws Exception {
        File path = file(ctx).getBaseFile();
        if (!path.exists()) return null;
        if (path.length() > MAX_FILE_BYTES) throw new IllegalStateException("HISTORY_STAGE_TOO_LARGE");
        return new JSONObject(new String(file(ctx).readFully(), StandardCharsets.UTF_8));
    }

    private static void write(Context ctx, JSONObject state) throws Exception {
        byte[] data = state.toString().getBytes(StandardCharsets.UTF_8);
        if (data.length > MAX_FILE_BYTES) throw new IllegalStateException("HISTORY_STAGE_FULL");
        AtomicFile atomic = file(ctx);
        FileOutputStream stream = atomic.startWrite();
        try {
            stream.write(data);
            atomic.finishWrite(stream);
        } catch (Exception ex) {
            atomic.failWrite(stream);
            throw ex;
        }
    }

    /** The latest cursor that was actually AEAD-verified and committed to SQLite. */
    static synchronized void seed(Context ctx, JSONObject account, long cursor) throws Exception {
        if (cursor < 0 || cursor > Long.MAX_VALUE / 2) throw new IllegalArgumentException("CURSOR");
        JSONObject state = read(ctx);
        if (state == null || !same(state, account)) state = fresh(account, cursor);
        long base = Math.max(state.getLong("base"), cursor);
        JSONArray old = state.getJSONArray("pages"), keep = new JSONArray();
        for (int i = 0; i < old.length(); i++) {
            JSONObject p = old.getJSONObject(i);
            if (p.getJSONObject("body").getLong("cursor") > base) keep.put(p);
        }
        state.put("base", base);
        state.put("pages", keep);
        write(ctx, state);
    }

    /** May be called only after the corresponding SQLite transaction has committed. */
    static synchronized void acknowledge(Context ctx, JSONObject account, long cursor) throws Exception {
        JSONObject state = read(ctx);
        if (state == null || !same(state, account)) return;
        seed(ctx, account, cursor);
    }

    /** The native service continues after its last cached page, not after an unverified server claim. */
    static synchronized long nextAfter(Context ctx, JSONObject account) throws Exception {
        JSONObject state = read(ctx);
        if (state == null || !same(state, account)) return -1;
        JSONArray pages = state.getJSONArray("pages");
        if (pages.length() >= MAX_PAGES) return -1;
        if (pages.length() == 0) return state.getLong("base");
        return pages.getJSONObject(pages.length() - 1).getJSONObject("body").getLong("cursor");
    }

    /** Returns only staged ciphertext, scoped to the same account and installation. */
    static synchronized String pending(Context ctx, JSONObject account) throws Exception {
        JSONObject state = read(ctx);
        if (state == null || !same(state, account)) return null;
        JSONObject result = new JSONObject();
        result.put("pages", state.getJSONArray("pages"));
        return result.toString();
    }

    /** Minimal shape/continuity guard. This does NOT authenticate any frame. */
    private static void checkPage(JSONObject page, long after) throws Exception {
        JSONArray records = page.getJSONArray("records");
        if (records.length() > 64 || records.length() == 0) throw new IllegalArgumentException("PAGE_EMPTY");
        long previous = after;
        for (int i = 0; i < records.length(); i++) {
            JSONObject row = records.getJSONObject(i);
            long cursor = row.getLong("cursor");
            if (cursor <= previous) throw new IllegalArgumentException("PAGE_ORDER");
            previous = cursor;
            if (row.getString("frame").length() > 200) throw new IllegalArgumentException("FRAME_SIZE");
        }
        if (page.getLong("cursor") != previous || !page.has("has_more"))
            throw new IllegalArgumentException("PAGE_CURSOR");
    }

    static synchronized void append(Context ctx, JSONObject account, long after, String body) throws Exception {
        if (body.length() > MAX_PAGE_BYTES) return;
        JSONObject page = new JSONObject(body);
        JSONArray rows = page.getJSONArray("records");
        if (rows.length() == 0) return; // already caught up; try again on the next poll
        checkPage(page, after);
        JSONObject state = read(ctx);
        if (state == null || !same(state, account)) return;
        JSONArray pages = state.getJSONArray("pages");
        if (pages.length() >= MAX_PAGES) return;
        long expected = pages.length() == 0
                ? state.getLong("base")
                : pages.getJSONObject(pages.length() - 1).getJSONObject("body").getLong("cursor");
        if (expected != after) return; // foreground verified newer records while GET was in flight
        JSONObject staged = new JSONObject();
        staged.put("after", after);
        staged.put("body", page);
        pages.put(staged);
        state.put("pages", pages);
        write(ctx, state);
    }

    static synchronized void clear(Context ctx) {
        file(ctx).delete();
    }
}
