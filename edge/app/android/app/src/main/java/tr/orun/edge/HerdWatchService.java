package tr.orun.edge;

import android.app.AlarmManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.AtomicFile;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Sürü izleme servisi (sahip kararı 2026-11-14).
 *
 * The app's own code lives in the WebView, and Android stops a WebView that
 * left the screen within minutes even while a foreground service runs (seen
 * on the owner's phone). So the part that must work with the app closed is
 * native and deliberately small:
 *
 *   every ~2 minutes: ask the account server for the herd's alerts
 *   (GET /api/v1/herd/alerts; the server decides), show each new one once,
 *   and keep the newest position of every tracker in a file the app reads
 *   on its next start (GET /v1/edge/records?latest=1; still AEAD-verified by
 *   the app before use).
 *
 * It decrypts nothing, writes no ledger and holds no rules. It survives the
 * app being swiped away (START_STICKY) and starts again after a reboot
 * (HerdWatchBoot). Without a stored login it stops itself.
 *
 * Wake-ups come from AlarmManager; a wake lock is held only for one poll.
 */
public final class HerdWatchService extends Service {
    static final String ACTION_POLL = "tr.orun.edge.herdwatch.POLL";
    static final String PREFS = "orun.herdwatch.v1";
    static final String LATEST_FILE = "herd-latest.v1.json";

    private static final String CH_WATCH = "orun-watch";
    private static final String CH_ALERTS = "orun-alerts";       // same id as the app's own alerts
    private static final int ID_ONGOING = 7101, ID_SERVER = 7102, ID_LOGIN = 7103;
    private static final int FGS_SPECIAL_USE = 1 << 30;           // ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
    private static final long EVERY_MS = 2 * 60_000L;
    private static final long UNREACHABLE_MS = 30 * 60_000L;
    private static final int MAX_BODY = 1024 * 1024;

    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final AtomicBoolean busy = new AtomicBoolean(false);

    static void start(Context context) {
        Intent i = new Intent(context, HerdWatchService.class).setAction(ACTION_POLL);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) context.startForegroundService(i);
        else context.startService(i);
    }

    static void stop(Context context) {
        AlarmManager alarms = (AlarmManager) context.getSystemService(Context.ALARM_SERVICE);
        if (alarms != null) alarms.cancel(pollIntent(context));
        context.stopService(new Intent(context, HerdWatchService.class));
        HerdHistoryStage.clear(context); // clear unverified account cache on sign-out
        NotificationManager nm = (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm != null) { nm.cancel(ID_SERVER); nm.cancel(ID_LOGIN); }
    }

    private static PendingIntent pollIntent(Context context) {
        Intent i = new Intent(context, HerdWatchService.class).setAction(ACTION_POLL);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE;
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? PendingIntent.getForegroundService(context, 1, i, flags)
                : PendingIntent.getService(context, 1, i, flags);
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        // Always first: a service started with startForegroundService must
        // call startForeground, also when it is about to stop itself.
        try {
            goForeground(prefs().getString("line", "Sunucuya soruluyor…"));
        } catch (RuntimeException refused) {
            // Android refused a foreground start (e.g. from the background
            // after a crash): try again at the next alarm instead of dying.
            schedule();
            return START_STICKY;
        }
        if (OrunMobileSessionPlugin.accountForService(this) == null) {
            stop(this);
            stopSelf();
            return START_NOT_STICKY;
        }
        schedule();
        poll();
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        worker.shutdownNow();
        super.onDestroy();
    }

    private SharedPreferences prefs() { return getSharedPreferences(PREFS, MODE_PRIVATE); }

    private void schedule() {
        AlarmManager alarms = (AlarmManager) getSystemService(ALARM_SERVICE);
        if (alarms == null) return;
        long at = SystemClock.elapsedRealtime() + EVERY_MS;
        alarms.setAndAllowWhileIdle(AlarmManager.ELAPSED_REALTIME_WAKEUP, at, pollIntent(this));
    }

    private void poll() {
        if (!busy.compareAndSet(false, true)) return;
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        final PowerManager.WakeLock lock = pm == null ? null
                : pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "orun:herdwatch");
        if (lock != null) lock.acquire(60_000L);
        worker.execute(() -> {
            try {
                once();
            } catch (Exception | LinkageError ignored) {
                // One failed round is not a reason to stop watching.
            } finally {
                busy.set(false);
                if (lock != null && lock.isHeld()) lock.release();
            }
        });
    }

    // ------------------------------------------------------------ one round ---
    private void once() throws Exception {
        JSONObject account = OrunMobileSessionPlugin.accountForService(this);
        if (account == null) { stop(this); stopSelf(); return; }
        String origin = account.getString("origin"), token = account.getString("accessToken");
        long now = System.currentTimeMillis();
        SharedPreferences p = prefs();

        Reply alerts;
        try {
            alerts = get(origin + "/api/v1/herd/alerts", token);
        } catch (Exception unreachable) {
            alerts = null;
        }
        BridgeRuntime bridge = BridgeRuntime.running();
        now = System.currentTimeMillis();
        boolean sameActor = account.optString("site").equals(p.getString("localSite", null))
                && account.optString("actor").equals(p.getString("localActor", null));
        long usbAge = now - p.getLong("usbAt", 0);
        boolean gateway = sameActor && usbAge >= 0 && usbAge < 90_000L;
        java.util.Map<String, Long> measured = new java.util.HashMap<>();
        if (sameActor) {
            JSONObject local = new JSONObject(p.getString("localMeasured", "{}"));
            for (java.util.Iterator<String> it = local.keys(); it.hasNext();) {
                String id = it.next();
                measured.put(id, local.optLong(id));
            }
        }
        if (bridge != null) {
            try {
                gateway |= bridge.gatewayReady();
                for (java.util.Map.Entry<String, Long> e : bridge.lastMeasured(now).entrySet()) {
                    Long previous = measured.get(e.getKey());
                    if (previous == null || previous < e.getValue()) measured.put(e.getKey(), e.getValue());
                }
            } catch (Exception ignored) { /* use the foreground ledger if the bridge is unavailable */ }
        }
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        JSONArray stored = new JSONArray(p.getString("keys", "[]"));
        List<String> known = new ArrayList<>();
        java.util.Set<Integer> obsolete = new java.util.HashSet<>();
        for (int i = 0; i < stored.length(); i++) {
            String key = stored.getString(i);
            known.add(key);
            if (HerdWatchLogic.superseded(key, measured, now)) obsolete.add(HerdWatchLogic.idOf(key));
        }
        if (nm != null && !obsolete.isEmpty()) {
            for (android.service.notification.StatusBarNotification active : nm.getActiveNotifications())
                if (obsolete.contains(active.getId())) nm.cancel(active.getId());
        }
        if (gateway) {
            if (nm != null) nm.cancel(ID_SERVER);
            p.edit().putBoolean("serverTold", false).apply();
        }
        if (alerts != null && (alerts.status == 401 || alerts.status == 403)) {
            if (!p.getBoolean("loginTold", false)) {
                show(ID_LOGIN, "ORUN: yeniden giriş yap",
                        "Sunucu oturumu sona erdi. Eşitlemek için yeniden giriş yap.");
                p.edit().putBoolean("loginTold", true).apply();
            }
            setLine(gateway ? "Gateway bağlı · sunucuya giriş gerekli" : "Sunucuya giriş gerekli");
            return;
        }
        if (alerts == null || alerts.status != 200) {
            long lastOk = p.getLong("lastOk", 0L), since = p.getLong("failSince", 0L);
            if (since == 0L) { since = now; p.edit().putLong("failSince", now).apply(); }
            long blind = now - (lastOk > 0L ? lastOk : since);
            if (!gateway && blind >= UNREACHABLE_MS && !p.getBoolean("serverTold", false)) {
                show(ID_SERVER, "Sunucuya ulaşılamıyor",
                        HerdWatchLogic.minutes(blind) + " sunucuyla eşitleme yapılamadı");
                p.edit().putBoolean("serverTold", true).apply();
            }
            setLine(gateway ? "Gateway bağlı · sunucuyla eşitleme bekliyor" : "Sunucuya ulaşılamıyor · " + (lastOk == 0L ? "henüz bağlanamadı" : "son kontrol " + clock(lastOk)));
            return;
        }

        // Server connectivity recovered.
        if (nm != null) { nm.cancel(ID_SERVER); nm.cancel(ID_LOGIN); }
        JSONArray list = new JSONObject(alerts.body).getJSONArray("alerts");
        List<HerdWatchLogic.Offer> offers = new ArrayList<>();
        for (int i = 0; i < list.length(); i++) {
            JSONObject a = list.getJSONObject(i);
            String key = a.getString("key");
            if (HerdWatchLogic.superseded(key, measured, now)) continue;
            offers.add(new HerdWatchLogic.Offer(key, a.getString("title"),
                    a.optString("body", ""), a.has("needs") ? a.optString("needs", null) : null));
        }
        HerdWatchLogic.Outcome outcome = HerdWatchLogic.decide(offers, p.getBoolean("ready", false), known);
        for (HerdWatchLogic.Offer o : outcome.show) show(HerdWatchLogic.idOf(o.key), o.title, o.body);
        p.edit().putBoolean("ready", true).putString("keys", new JSONArray(outcome.keys).toString())
                .putLong("lastOk", now).putLong("failSince", 0L)
                .putBoolean("serverTold", false).putBoolean("loginTold", false).apply();
        setLine("Son kontrol " + clock(now) + (offers.isEmpty() ? " · uyarı yok" : " · " + offers.size() + " uyarı"));

        // Newest position of every tracker, for the app's next start. The app
        // verifies each frame with its read key before using it.
        try {
            Reply latest = get(origin + "/v1/edge/records?latest=1", token);
            if (latest.status == 200) {
                JSONObject doc = new JSONObject();
                doc.put("at", now);
                doc.put("actor", account.optString("actor", ""));
                doc.put("body", new JSONObject(latest.body));
                writeLatest(doc.toString().getBytes(StandardCharsets.UTF_8));
            }
        } catch (Exception ignored) {
            // The alerts were handled; the snapshot is a convenience.
        }

        // Lower priority than alerts and live locations: stage a single page
        // of still-encrypted history for later app-side AEAD verification.
        // The native service NEVER advances the phone's verified SQLite cursor.
        // Diagnostic metadata only: no credentials, frames, coordinates or
        // response bodies. Keep errors isolated from critical alerts/latest.
        p.edit().putLong("historyAttemptAt", System.currentTimeMillis())
                .putString("historyState", "starting")
                .remove("historyHttp").remove("historyError").apply();
        try {
            long after = HerdHistoryStage.nextAfter(this, account);
            p.edit().putLong("historyAfter", after).apply();
            if (after < 0) {
                p.edit().putString("historyState", "not_seeded").apply();
            } else {
                Reply history = get(origin + "/v1/edge/records?after=" + after, token);
                p.edit().putInt("historyHttp", history.status).apply();
                if (history.status != 200) {
                    p.edit().putString("historyState", "http_error")
                            .putLong("historyDoneAt", System.currentTimeMillis()).apply();
                } else {
                    // Observe the response independently of whether append
                    // succeeds: 200/empty and 200/rejected are different cases.
                    int received = new JSONObject(history.body)
                            .getJSONArray("records").length();
                    HerdHistoryStage.append(this, account, after, history.body);
                    long tail = HerdHistoryStage.nextAfter(this, account);
                    p.edit().putInt("historyReceived", received)
                            .putLong("historyTail", tail)
                            .putString("historyState", received == 0 ? "empty"
                                    : tail > after ? "staged" : "not_staged")
                            .putLong("historyDoneAt", System.currentTimeMillis()).apply();
                }
            }
        } catch (Exception failure) {
            // Only an exception's class is recorded; messages can include
            // URLs or server internals and must never reach diagnostics.
            p.edit().putString("historyState", "exception")
                    .putString("historyError", failure.getClass().getSimpleName())
                    .putLong("historyDoneAt", System.currentTimeMillis()).apply();
            // Failure must never block critical alerts or the latest snapshot.
        }
    }

    private static final class Reply {
        final int status; final String body;
        Reply(int status, String body) { this.status = status; this.body = body; }
    }

    private static Reply get(String url, String token) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        try {
            c.setConnectTimeout(15_000);
            c.setReadTimeout(20_000);
            c.setInstanceFollowRedirects(false);
            c.setUseCaches(false);
            c.setRequestProperty("Authorization", "Bearer " + token);
            c.setRequestProperty("Accept", "application/json");
            int status = c.getResponseCode();
            if (status != 200) return new Reply(status, "");
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            try (InputStream in = c.getInputStream()) {
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                    if (out.size() > MAX_BODY) throw new IllegalStateException("BODY_TOO_BIG");
                }
            }
            return new Reply(200, new String(out.toByteArray(), StandardCharsets.UTF_8));
        } finally {
            c.disconnect();
        }
    }

    private void writeLatest(byte[] data) throws Exception {
        AtomicFile file = new AtomicFile(new File(getNoBackupFilesDir(), LATEST_FILE));
        FileOutputStream out = file.startWrite();
        try {
            out.write(data);
            file.finishWrite(out);
        } catch (Exception ex) {
            file.failWrite(out);
            throw ex;
        }
    }

    private static String clock(long ms) {
        return new SimpleDateFormat("HH:mm", Locale.ROOT).format(new Date(ms));
    }

    // -------------------------------------------------------- notifications ---
    private void channels() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return;
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm == null) return;
        NotificationChannel watch = new NotificationChannel(CH_WATCH, "Sürü izleme",
                NotificationManager.IMPORTANCE_LOW);
        watch.setDescription("ORUN sürüyü izlerken görünür");
        nm.createNotificationChannel(watch);
        NotificationChannel alerts = new NotificationChannel(CH_ALERTS, "Sürü uyarıları",
                NotificationManager.IMPORTANCE_HIGH);
        alerts.setDescription("Alandan çıkan, dönen, haber alınamayan hayvan ve biten pil");
        nm.createNotificationChannel(alerts);
    }

    private PendingIntent openApp() {
        Intent launch = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (launch == null) launch = new Intent(this, MainActivity.class);
        return PendingIntent.getActivity(this, 2, launch,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    }

    private Notification.Builder builder(String channel) {
        Notification.Builder b = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? new Notification.Builder(this, channel) : new Notification.Builder(this);
        return b.setSmallIcon(R.drawable.ic_stat_orun).setContentIntent(openApp());
    }

    private void goForeground(String line) {
        channels();
        Notification n = builder(CH_WATCH).setContentTitle("ORUN sürüyü izliyor").setContentText(line)
                .setOngoing(true).setOnlyAlertOnce(true).build();
        if (Build.VERSION.SDK_INT >= 34) startForeground(ID_ONGOING, n, FGS_SPECIAL_USE);
        else startForeground(ID_ONGOING, n);
    }

    private void setLine(String line) {
        prefs().edit().putString("line", line).apply();
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm == null) return;
        nm.notify(ID_ONGOING, builder(CH_WATCH).setContentTitle("ORUN sürüyü izliyor").setContentText(line)
                .setOngoing(true).setOnlyAlertOnce(true).build());
    }

    private void show(int id, String title, String body) {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm == null) return;
        nm.notify(id, builder(CH_ALERTS).setContentTitle(title).setContentText(body)
                .setStyle(new Notification.BigTextStyle().bigText(body))
                .setAutoCancel(true).setWhen(System.currentTimeMillis()).setShowWhen(true).build());
    }

}
