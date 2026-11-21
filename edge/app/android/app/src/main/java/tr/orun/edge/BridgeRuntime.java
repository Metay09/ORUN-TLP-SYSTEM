package tr.orun.edge;

import android.bluetooth.BluetoothAdapter;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Callable;
import java.util.concurrent.CopyOnWriteArraySet;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;

/**
 * The phone as a Gateway bridge without the WebView (v1 step 7, owner
 * decision 2026-11-14: "bağlıysa paketleri versin, uygulamadan çıkınca da").
 * One per process. Everything with state runs on one worker thread:
 * Bluetooth (BridgeLink + AndroidRadio), custody (BridgeCore), the phone's
 * own ledger file (BridgeStore). HTTP runs on a second thread and touches
 * the store only through the worker (onDb).
 *
 * - Records are kept and handed off with the app closed or swiped away
 *   (GatewayBridgeService keeps the process in the foreground).
 * - They go to the account server when there is Internet (BridgeUpload);
 *   until then they wait on the phone.
 * - The app takes them over into its own ledger when it is opened
 *   (changes()), so the map works without Internet too.
 * - Read keys and Edge keys come from the account vault the app filled, and
 *   from the server while online.
 */
final class BridgeRuntime {
    static final String PREFS = "orun.bridge.v1";
    static final String DB_FILE = "orun-bridge.v1.db";
    private static final String TAG = "OrunBridge";
    private static final long UPLOAD_AFTER_RECORD_MS = 5_000, UPLOAD_RETRY_MS = 120_000,
            KEYS_EVERY_MS = 10 * 60_000L, REPORT_EVERY_MS = 120_000, PRUNE_EVERY_MS = 6 * 3_600_000L,
            WAKE_MS = 5_000, CHANGED_MS = 300;
    private static final int MAX_BODY = 512 * 1024, LORA_KEEP = 64, LOG_KEEP = 60;

    interface Listener { void changed(); }
    interface Reply<T> { void done(T value, Exception error); }

    private static BridgeRuntime instance;

    static synchronized BridgeRuntime get(Context context) {
        if (instance == null) instance = new BridgeRuntime(context.getApplicationContext());
        return instance;
    }

    static synchronized BridgeRuntime running() { return instance; }

    boolean gatewayReady() throws Exception {
        return onDb(() -> link != null && link.ready());
    }

    Map<String, Long> lastMeasured(long now) throws Exception {
        return onDb(() -> store == null ? Collections.emptyMap() : store.lastMeasured(now));
    }

    private final Context ctx;
    final Handler handler;
    private final ExecutorService net = Executors.newSingleThreadExecutor();
    private final PowerManager.WakeLock wake, uploadWake;
    private final CopyOnWriteArraySet<Listener> listeners = new CopyOnWriteArraySet<>();

    private AndroidSql sql;
    private BridgeStore store;
    private BridgeCore core;
    private AndroidRadio radio;
    private BridgeLink link;
    private String openError;

    private volatile Map<String, byte[]> readKeys = Collections.emptyMap();
    private volatile Map<String, byte[]> edgeKeys = Collections.emptyMap();
    private final Map<String, byte[]> edgeKeysOnline = new HashMap<>();
    private String lastGateway;
    private Integer lastHeld, lastCapacity;
    private long connectedAtWall = -1;

    private boolean uploading, uploadAgain, uploadScheduled;
    private long lastUploadOk, lastKeysAt = -KEYS_EVERY_MS, lastReportAt = -REPORT_EVERY_MS, lastPruneAt = -PRUNE_EVERY_MS;
    private String uploadError;
    private boolean changedQueued;

    private final ArrayDeque<String> lora = new ArrayDeque<>();
    private long loraSeq;
    private final ArrayDeque<String> log = new ArrayDeque<>();

    private BridgeRuntime(Context ctx) {
        this.ctx = ctx;
        HandlerThread thread = new HandlerThread("orun-bridge");
        thread.start();
        handler = new Handler(thread.getLooper());
        PowerManager pm = (PowerManager) ctx.getSystemService(Context.POWER_SERVICE);
        wake = pm == null ? null : pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "orun:bridge");
        uploadWake = pm == null ? null : pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "orun:bridge-upload");
        if (wake != null) wake.setReferenceCounted(false);
        if (uploadWake != null) uploadWake.setReferenceCounted(false);
        handler.post(this::open);
    }

    // ---- setup ---------------------------------------------------------------
    private void open() {
        try {
            sql = new AndroidSql(new File(ctx.getNoBackupFilesDir(), DB_FILE));
            store = new BridgeStore(sql);
        } catch (Exception e) {
            openError = "Köprü defteri açılamadı: " + e.getClass().getSimpleName();
            Log.e(TAG, "open", e);
            return;
        }
        core = new BridgeCore(store, (device, epoch) -> readKeys.get(BridgeCore.hexOf(device) + ":" + epoch),
            this::edgeKey, () -> System.currentTimeMillis() / 1000);
        radio = new AndroidRadio(ctx, handler, this::awake);
        BridgeLink.Timers timers = new BridgeLink.Timers() {
            public long nowMs() { return SystemClock.elapsedRealtime(); }
            public void after(long ms, Runnable r) { handler.postDelayed(r, ms); }
        };
        link = new BridgeLink(radio, timers, core, new BridgeLink.Sessions() {
            public void alive() throws Exception { store.sessionAlive(System.currentTimeMillis() / 1000, "ble"); }
            public void end() throws Exception { store.sessionEnd(System.currentTimeMillis() / 1000); }
        }, new LinkEvents());
        radio.attach(link);
        loadKeys();
        IntentFilter f = new IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED);
        BroadcastReceiver adapter = new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent i) {
                int s = i.getIntExtra(BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR);
                if (s == BluetoothAdapter.STATE_ON) handler.post(() -> link.onAdapter(true));
                else if (s == BluetoothAdapter.STATE_TURNING_OFF || s == BluetoothAdapter.STATE_OFF)
                    handler.post(() -> link.onAdapter(false));
            }
        };
        // A protected system broadcast: delivered with RECEIVER_NOT_EXPORTED too.
        if (Build.VERSION.SDK_INT >= 33) ctx.registerReceiver(adapter, f, Context.RECEIVER_NOT_EXPORTED);
        else ctx.registerReceiver(adapter, f);
        ConnectivityManager cm = (ConnectivityManager) ctx.getSystemService(Context.CONNECTIVITY_SERVICE);
        if (cm != null) {
            try {
                cm.registerDefaultNetworkCallback(new ConnectivityManager.NetworkCallback() {
                    @Override public void onAvailable(Network n) { handler.post(() -> online()); }
                    @Override public void onCapabilitiesChanged(Network n, NetworkCapabilities c) {
                        if (c.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)) handler.post(() -> online());
                    }
                });
            } catch (RuntimeException ignored) {
                // Uploads still run after new records and on the retry timer.
            }
        }
        handler.postDelayed(this::periodic, 60_000);
        changed();
    }

    private void awake() { awake(WAKE_MS); }

    /** Keeps the CPU on for ms (extends, never shortens a running hold). */
    private void awake(long ms) {
        try { if (wake != null) wake.acquire(ms); } catch (RuntimeException ignored) { }
    }

    private byte[] edgeKey(String gatewayHex) {
        byte[] k = edgeKeys.get(gatewayHex);
        return k != null ? k : edgeKeysOnline.get(gatewayHex);
    }

    /** Keys of the account on this phone (vault the app filled). */
    private void loadKeys() {
        Map<String, byte[]> r = OrunMobileSessionPlugin.readKeysForService(ctx);
        Map<String, byte[]> e = OrunMobileSessionPlugin.edgeKeysForService(ctx);
        boolean newRead = !sameKeys(r, readKeys);
        String gw = core != null && core.gatewayId != null ? BridgeCore.gatewayHex(core.gatewayId) : null;
        boolean edgeWasMissing = gw != null && core.acceptKeyMissing && edgeKey(gw) == null;
        readKeys = r;
        edgeKeys = e;
        boolean edgeAppeared = edgeWasMissing && edgeKey(gw) != null;
        if ((newRead || edgeAppeared) && link != null && link.ready()) {
            // Records the bridge could not open or hand off are still on the Gateway.
            note(newRead ? "[köprü] yeni okuma anahtarları; gateway'den kayıtlar yeniden isteniyor"
                : "[köprü] teslim anahtarı geldi; gateway'den kayıtlar yeniden isteniyor");
            link.redump();
        }
    }

    private static boolean sameKeys(Map<String, byte[]> a, Map<String, byte[]> b) {
        if (a.size() != b.size()) return false;
        for (Map.Entry<String, byte[]> e : a.entrySet())
            if (!java.util.Arrays.equals(e.getValue(), b.get(e.getKey()))) return false;
        return true;
    }

    private final class LinkEvents implements BridgeLink.Events {
        public void state(String state, String detail) {
            note("[BLE] " + state + (detail == null ? "" : ": " + detail));
            if (BridgeLink.READY.equals(state)) {
                connectedAtWall = System.currentTimeMillis();
                loadKeys();
            } else {
                connectedAtWall = -1;
            }
            changed();
        }
        public void changed() { BridgeRuntime.this.changed(); }
        public void stored(int n) {
            // Screen off: stay awake until the upload has started (it holds its own lock).
            awake(UPLOAD_AFTER_RECORD_MS + WAKE_MS);
            uploadSoon(UPLOAD_AFTER_RECORD_MS);
        }
        public void hello(String gatewayHex) {
            lastGateway = gatewayHex;
            if (edgeKey(gatewayHex) == null) {
                loadKeys();
                if (edgeKey(gatewayHex) == null) fetchKeys(true);
            }
            lastReportAt = -REPORT_EVERY_MS;
            report();
        }
        public void loraLine(String line) {
            lora.addLast(line);
            loraSeq++;
            while (lora.size() > LORA_KEEP) lora.removeFirst();
            BridgeRuntime.this.changed();
        }
        public void log(String line) { note(line); }
    }

    private void note(String line) {
        Log.i(TAG, line);
        // One line per record would push the connection history out of the short log.
        if (line.startsWith("STORED ") || line.startsWith("DUPLICATE ")) return;
        log.addLast(new java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.ROOT)
            .format(new java.util.Date()) + " " + line);
        while (log.size() > LOG_KEEP) log.removeFirst();
    }

    void addListener(Listener l) { listeners.add(l); }

    void removeListener(Listener l) { listeners.remove(l); }

    private void changed() {
        if (changedQueued) return;
        changedQueued = true;
        handler.postDelayed(() -> {
            changedQueued = false;
            Integer held = core == null ? null : core.heldNow();
            if (core != null && core.gatewayId != null) {
                lastGateway = BridgeCore.gatewayHex(core.gatewayId);
                lastHeld = held;
                lastCapacity = core.capacity();
            }
            for (Listener l : listeners) {
                try { l.changed(); } catch (RuntimeException ignored) { }
            }
        }, CHANGED_MS);
    }

    // ---- owner ---------------------------------------------------------------
    static SharedPreferences prefs(Context c) { return c.getSharedPreferences(PREFS, Context.MODE_PRIVATE); }

    void start(String address) {
        handler.post(() -> {
            if (link == null) return;
            if (address != null && !address.equals(radio.address())) {
                link.stop();
                radio.setAddress(address);
                lastGateway = null; lastHeld = null; lastCapacity = null;
            }
            link.start();
            uploadSoon(1_000);
        });
    }

    void stop() {
        handler.post(() -> { if (link != null) link.stop(); });
    }

    // ---- uploads, keys, gateway report ---------------------------------------------
    private void online() {
        uploadSoon(2_000);
        if (SystemClock.elapsedRealtime() - lastKeysAt >= KEYS_EVERY_MS) fetchKeys(false);
    }

    private void periodic() {
        handler.postDelayed(this::periodic, 60_000);
        if (store == null) return;
        long now = SystemClock.elapsedRealtime();
        try {
            if (store.counts().pending > 0 && now - lastUploadOk >= UPLOAD_RETRY_MS) uploadSoon(0);
            if (now - lastPruneAt >= PRUNE_EVERY_MS) {
                lastPruneAt = now;
                int n = store.prune(System.currentTimeMillis() / 1000);
                if (n > 0) note("[köprü] " + n + " eski kayıt köprü defterinden silindi (sunucuda ve uygulamada var)");
            }
        } catch (Exception ignored) { }
        if (link != null && link.ready()) report();
        if (now - lastKeysAt >= KEYS_EVERY_MS) fetchKeys(false);
    }

    void uploadSoon(long delayMs) {
        if (uploadScheduled) return;
        uploadScheduled = true;
        handler.postDelayed(() -> { uploadScheduled = false; upload(); }, delayMs);
    }

    void upload() {
        if (store == null) return;
        if (uploading) { uploadAgain = true; return; }
        final JSONObject account = OrunMobileSessionPlugin.accountForService(ctx);
        if (account == null) { uploadError = "Hesap oturumu yok"; changed(); return; }
        uploading = true;
        try { if (uploadWake != null) uploadWake.acquire(120_000); } catch (RuntimeException ignored) { }
        net.execute(() -> {
            String error = null;
            int settled = 0;
            try {
                String origin = account.getString("origin"), token = account.getString("accessToken");
                BridgeUpload.Result r = BridgeUpload.drain(store, body -> {
                    Http h = http("POST", origin + "/v1/edge/batch", token, body);
                    if (h.status != 200) return new BridgeUpload.Reply(h.status, null, false);
                    JSONObject o = new JSONObject(h.body);
                    JSONArray a = o.optJSONArray("results");
                    List<String> words = new ArrayList<>();
                    for (int i = 0; a != null && i < a.length(); i++) words.add(a.getString(i));
                    return new BridgeUpload.Reply(200, a == null ? null : words, o.optBoolean("sessions_ok", false));
                }, this::onDb, System.currentTimeMillis() / 1000);
                settled = r.settled;
            } catch (BridgeUpload.Unauthorized e) {
                error = "Sunucu oturumu kabul etmedi; uygulamada yeniden giriş gerekebilir";
            } catch (Exception e) {
                error = "Sunucuya ulaşılamadı; kayıtlar telefonda bekliyor";
            }
            final String err = error;
            final int n = settled;
            handler.post(() -> {
                uploading = false;
                try { if (uploadWake != null && uploadWake.isHeld()) uploadWake.release(); } catch (RuntimeException ignored) { }
                uploadError = err;
                if (err == null) {
                    lastUploadOk = SystemClock.elapsedRealtime();
                    if (n > 0) note("[sunucu] " + n + " kayıt gönderildi");
                    if (SystemClock.elapsedRealtime() - lastKeysAt >= KEYS_EVERY_MS) fetchKeys(false);
                }
                changed();
                if (uploadAgain) { uploadAgain = false; uploadSoon(1_000); }
            });
        });
    }

    /** Store work from the HTTP thread: runs on the worker, waits for it. */
    private <T> T onDb(Callable<T> work) throws Exception {
        FutureTask<T> task = new FutureTask<>(work);
        handler.post(task);
        return task.get(60, TimeUnit.SECONDS);
    }

    /** Read keys (and missing Edge keys) from the account server. */
    private void fetchKeys(boolean edgeOnly) {
        final JSONObject account = OrunMobileSessionPlugin.accountForService(ctx);
        if (account == null) return;
        if (!edgeOnly) lastKeysAt = SystemClock.elapsedRealtime();
        net.execute(() -> {
            try {
                String origin = account.getString("origin"), token = account.getString("accessToken");
                if (!edgeOnly) {
                    Http h = http("GET", origin + "/v1/edge/keys", token, null);
                    if (h.status == 200) {
                        JSONArray in = new JSONObject(h.body).getJSONArray("keys"), keep = new JSONArray();
                        for (int i = 0; i < in.length(); i++) {
                            JSONObject k = in.getJSONObject(i);
                            String device = k.optString("device", ""), key = k.optString("traffic_key", "");
                            if (!device.matches("[0-9A-Fa-f]{16}") || !key.matches("[0-9A-Fa-f]{32}")) continue;
                            keep.put(new JSONObject().put("device", device.toUpperCase(java.util.Locale.ROOT))
                                .put("epoch", k.getLong("epoch")).put("key", key.toUpperCase(java.util.Locale.ROOT)));
                        }
                        OrunMobileSessionPlugin.storeReadKeysForService(ctx, keep);
                        handler.post(this::loadKeys);
                    }
                }
                Http e = http("GET", origin + "/api/v1/devices/edge-keys", token, null);
                if (e.status == 200) {
                    JSONArray keys = new JSONObject(e.body).getJSONArray("keys");
                    Map<String, byte[]> got = new HashMap<>();
                    for (int i = 0; i < keys.length() && i < 16; i++) {
                        JSONObject k = keys.getJSONObject(i);
                        String gw = k.optString("gateway", ""), key = k.optString("key", "");
                        if (gw.matches("[0-9A-F]{16}") && key.matches("[0-9A-F]{64}") && !key.matches("0{64}|F{64}"))
                            got.put(gw, BridgeStore.unhex(key));
                    }
                    handler.post(() -> {
                        boolean wasMissing = core != null && core.acceptKeyMissing;
                        edgeKeysOnline.clear();
                        edgeKeysOnline.putAll(got);
                        if (wasMissing && core.gatewayId != null && edgeKey(BridgeCore.gatewayHex(core.gatewayId)) != null
                                && link.ready()) {
                            note("[köprü] teslim anahtarı sunucudan alındı; kayıtlar yeniden isteniyor");
                            link.redump();
                        }
                    });
                }
            } catch (Exception ignored) {
                // Offline: the vault keys stay in use.
            }
        });
    }

    /** 2c: which Gateway this phone bridges, and how full it is (at most every 2 min). */
    private void report() {
        if (core == null || core.gatewayId == null) return;
        long now = SystemClock.elapsedRealtime();
        if (now - lastReportAt < REPORT_EVERY_MS) return;
        final JSONObject account = OrunMobileSessionPlugin.accountForService(ctx);
        if (account == null) return;
        lastReportAt = now;
        final String gw = BridgeCore.gatewayHex(core.gatewayId);
        final Integer held = core.heldNow();
        final int capacity = core.capacity();
        net.execute(() -> {
            try {
                JSONObject body = new JSONObject().put("gateway", gw).put("transport", "ble")
                    .put("held", held == null ? JSONObject.NULL : held)
                    .put("capacity", capacity > 0 ? capacity : JSONObject.NULL);
                http("POST", account.getString("origin") + "/api/v1/devices/gateway",
                    account.getString("accessToken"), body.toString());
            } catch (Exception ignored) {
                handler.post(() -> lastReportAt = -REPORT_EVERY_MS);
            }
        });
    }

    private static final class Http {
        final int status;
        final String body;
        Http(int status, String body) { this.status = status; this.body = body; }
    }

    private static Http http(String method, String url, String token, String body) throws Exception {
        if (!url.startsWith("https://")) throw new IllegalStateException("HTTPS_REQUIRED");
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        try {
            c.setConnectTimeout(15_000);
            c.setReadTimeout(30_000);
            c.setInstanceFollowRedirects(false);
            c.setUseCaches(false);
            c.setRequestMethod(method);
            c.setRequestProperty("Authorization", "Bearer " + token);
            c.setRequestProperty("Accept", "application/json");
            if (body != null) {
                byte[] data = body.getBytes(StandardCharsets.UTF_8);
                c.setDoOutput(true);
                c.setRequestProperty("Content-Type", "application/json");
                c.setFixedLengthStreamingMode(data.length);
                try (OutputStream out = c.getOutputStream()) { out.write(data); }
            }
            int status = c.getResponseCode();
            if (status != 200) return new Http(status, "");
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            try (InputStream in = c.getInputStream()) {
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                    if (out.size() > MAX_BODY) throw new IllegalStateException("BODY_TOO_BIG");
                }
            }
            return new Http(200, new String(out.toByteArray(), StandardCharsets.UTF_8));
        } finally {
            c.disconnect();
        }
    }

    // ---- for the app (OrunGatewayBridgePlugin) ------------------------------------------
    /** Runs on the worker; the reply comes on the worker too. */
    <T> void request(Callable<T> work, Reply<T> reply) {
        handler.post(() -> {
            try { reply.done(work.call(), null); }
            catch (Exception e) { reply.done(null, e); }
        });
    }

    JSONObject status() throws Exception {
        SharedPreferences p = prefs(ctx);
        JSONObject s = new JSONObject();
        s.put("enabled", p.getBoolean("enabled", false));
        s.put("address", p.getString("address", null) == null ? JSONObject.NULL : p.getString("address", null));
        if (openError != null || link == null) {
            s.put("state", "off").put("detail", openError == null ? "başlatılıyor" : openError);
            return s;
        }
        s.put("state", link.state()).put("detail", link.detail() == null ? JSONObject.NULL : link.detail());
        s.put("permitted", radio.permitted());
        Long gw = core.gatewayId;
        String gwHex = gw != null ? BridgeCore.gatewayHex(gw) : lastGateway;
        s.put("gateway", gwHex == null ? JSONObject.NULL : gwHex);
        Integer held = gw != null ? core.heldNow() : lastHeld;
        int cap = gw != null ? core.capacity() : (lastCapacity == null ? 0 : lastCapacity);
        s.put("held", held == null ? JSONObject.NULL : held).put("capacity", cap > 0 ? cap : JSONObject.NULL);
        s.put("counts", new JSONObject().put("STORED", core.counts[0]).put("DUPLICATE", core.counts[1])
            .put("REJECTED", core.counts[2]).put("CONFLICT", core.counts[3]).put("UNCERTAIN", core.counts[4]));
        s.put("handedOff", core.handedOff).put("acceptRejected", core.acceptRejected)
            .put("pendingAccepts", core.pendingAccepts()).put("keyMissing", core.acceptKeyMissing);
        BridgeStore.Counts c = store.counts();
        s.put("records", c.records).put("uploaded", c.uploaded).put("pending", c.pending)
            .put("conflict", c.conflict).put("rejected", c.rejected);
        s.put("lastUploadAgoMs", lastUploadOk == 0 ? JSONObject.NULL : SystemClock.elapsedRealtime() - lastUploadOk);
        s.put("uploadError", uploadError == null ? JSONObject.NULL : uploadError);
        s.put("connectedAt", connectedAtWall > 0 ? connectedAtWall : JSONObject.NULL);
        s.put("db", store.dbId()).put("storage", "SQLite journal=" + sql.journal + " synchronous=" + sql.synchronous);
        s.put("loraSeq", loraSeq);
        s.put("log", new JSONArray(new ArrayList<>(log)));
        return s;
    }

    /** What changed in the bridge's ledger after `after` (the app imports it). */
    JSONObject changes(String db, long after, int limit) throws Exception {
        if (store == null) throw new IllegalStateException(openError == null ? "not open" : openError);
        String id = store.dbId();
        long from = id.equals(db) ? after : 0;           // another file: start over
        JSONArray rows = new JSONArray();
        long last = from;
        for (BridgeStore.Change x : store.changes(from, limit)) {
            JSONObject r = new JSONObject().put("rev", x.rev).put("conflict", x.conflict).put("device", x.device)
                .put("incarnation", x.incarnation).put("sequence", x.sequence).put("family", x.family)
                .put("plaintext", x.plaintext).put("frame", x.frame)
                .put("firstSeen", x.firstSeen == null ? JSONObject.NULL : x.firstSeen)
                .put("rssi", x.rssi == null ? JSONObject.NULL : x.rssi)
                .put("snr", x.snr == null ? JSONObject.NULL : x.snr)
                .put("gateway", x.gateway == null ? JSONObject.NULL : x.gateway)
                .put("upload", x.upload == null ? JSONObject.NULL : x.upload);
            rows.put(r);
            last = x.rev;
        }
        return new JSONObject().put("db", id).put("rows", rows).put("last", last);
    }

    void taken(String db, long rev) throws Exception {
        if (store != null && store.dbId().equals(db)) store.takenUpTo(rev);
    }

    JSONObject takeLora(long after) throws Exception {
        JSONArray lines = new JSONArray();
        long first = loraSeq - lora.size() + 1, seq = first;
        for (String l : lora) { if (seq > after) lines.put(l); seq++; }
        return new JSONObject().put("lines", lines).put("last", loraSeq);
    }

    void redump() { if (link != null) link.redump(); }

    void sendLine(String line, BridgeLink.Done done) {
        if (link == null) { done.done(false); return; }
        link.sendLine(line, done);
    }

    /** The app's read keys (it may have them while this phone is offline). */
    void setReadKeys(JSONArray keys) throws Exception {
        JSONArray keep = new JSONArray();
        for (int i = 0; i < keys.length(); i++) {
            JSONObject k = keys.getJSONObject(i);
            String device = k.optString("device", ""), key = k.optString("trafficKey", "");
            if (!device.matches("[0-9A-Fa-f]{16}") || !key.matches("[0-9A-Fa-f]{32}")) continue;
            keep.put(new JSONObject().put("device", device.toUpperCase(java.util.Locale.ROOT))
                .put("epoch", k.getLong("epoch")).put("key", key.toUpperCase(java.util.Locale.ROOT)));
        }
        Map<String, byte[]> merged = new HashMap<>(readKeys);
        for (int i = 0; i < keep.length(); i++) {
            JSONObject k = keep.getJSONObject(i);
            merged.put(k.getString("device") + ":" + k.getLong("epoch"), BridgeStore.unhex(k.getString("key")));
        }
        if (!sameKeys(merged, readKeys)) {
            JSONArray all = new JSONArray();
            for (Map.Entry<String, byte[]> e : merged.entrySet()) {
                String[] dk = e.getKey().split(":");
                all.put(new JSONObject().put("device", dk[0]).put("epoch", Long.parseLong(dk[1]))
                    .put("key", BridgeCore.hexOf(e.getValue())));
            }
            OrunMobileSessionPlugin.storeReadKeysForService(ctx, all);
        }
        loadKeys();          // also the Edge-key vault the app may just have filled
    }

    /** Signed out: no keys, no Gateway, nothing of the old account kept. */
    void forget() {
        handler.post(() -> {
            if (link != null) link.stop();
            OrunMobileSessionPlugin.clearReadKeysForService(ctx);
            readKeys = Collections.emptyMap();
            edgeKeys = Collections.emptyMap();
            edgeKeysOnline.clear();
            changed();
        });
    }
}
