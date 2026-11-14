package tr.orun.edge;

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
import android.os.SystemClock;

import org.json.JSONObject;

/**
 * Keeps the phone a Gateway bridge with the app closed or swiped away
 * (v1 step 7): a foreground service of type connectedDevice holds the
 * process; the work itself is BridgeRuntime. Started by the app
 * (OrunGatewayBridgePlugin.start), again after a reboot or an app update
 * (GatewayBridgeBoot), and by Android after it had to stop the process
 * (START_STICKY). Only the owner's "Bağlantıyı kes" or signing out stops it.
 */
public final class GatewayBridgeService extends Service {
    static final String ACTION_START = "tr.orun.edge.bridge.START";
    private static final String CHANNEL = "orun-bridge";
    private static final int ID = 7201;
    private static final int FGS_CONNECTED_DEVICE = 0x10;     // ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
    private static final long NOTIFY_MS = 3_000;

    private BridgeRuntime runtime;
    private final BridgeRuntime.Listener listener = this::refresh;
    private String shown;
    private long shownAt;
    private boolean refreshQueued;

    /** The owner chose this Gateway (Bluetooth address) as the one to bridge. */
    static void start(Context context, String address) {
        BridgeRuntime.prefs(context).edit().putBoolean("enabled", true).putString("address", address).apply();
        launch(context);
    }

    static void launch(Context context) {
        Intent i = new Intent(context, GatewayBridgeService.class).setAction(ACTION_START);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) context.startForegroundService(i);
        else context.startService(i);
    }

    /** The owner disconnected (or signed out): stop until chosen again. */
    static void stop(Context context) {
        BridgeRuntime.prefs(context).edit().putBoolean("enabled", false).apply();
        BridgeRuntime.get(context).stop();
        context.stopService(new Intent(context, GatewayBridgeService.class));
    }

    static boolean enabled(Context context) {
        SharedPreferences p = BridgeRuntime.prefs(context);
        return p.getBoolean("enabled", false) && p.getString("address", null) != null;
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }

    @Override
    public void onCreate() {
        super.onCreate();
        runtime = BridgeRuntime.get(this);
        runtime.addListener(listener);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        // Always first: a service started with startForegroundService must
        // call startForeground, also when it is about to stop itself.
        try {
            foreground(shown != null ? shown : "Gateway'e bağlanıyor…");
        } catch (RuntimeException refused) {
            // Android refused a foreground start from the background (e.g. a
            // restart after the process was stopped). The link keeps running
            // while the process lives; the app starts the service when opened.
        }
        if (!enabled(this)) {
            stopForegroundCompat();
            stopSelf();
            return START_NOT_STICKY;
        }
        runtime.start(BridgeRuntime.prefs(this).getString("address", null));
        refresh();
        return START_STICKY;
    }

    @Override
    public void onTaskRemoved(Intent rootIntent) {
        // The app was swiped away: the bridge stays (that is the point of it).
        super.onTaskRemoved(rootIntent);
    }

    @Override
    public void onDestroy() {
        runtime.removeListener(listener);
        if (!enabled(this)) runtime.stop();
        super.onDestroy();
    }

    @SuppressWarnings("deprecation")
    private void stopForegroundCompat() {
        if (Build.VERSION.SDK_INT >= 24) stopForeground(STOP_FOREGROUND_REMOVE);
        else stopForeground(true);
    }

    // ---- notification ----------------------------------------------------------
    /** Any thread; the work runs on the bridge's worker (where the listener is called too). */
    private void refresh() {
        runtime.handler.post(() -> {
            if (refreshQueued) return;
            refreshQueued = true;
            long wait = Math.max(0, NOTIFY_MS - (SystemClock.elapsedRealtime() - shownAt));
            runtime.handler.postDelayed(() -> {
                refreshQueued = false;
                String line;
                try { line = text(runtime.status()); } catch (Exception e) { return; }
                if (!line.equals(shown)) show(line);
            }, wait);
        });
    }

    static String text(JSONObject s) {
        String state = s.optString("state", "off");
        long pending = s.optLong("pending", 0);
        String tail = pending > 0 ? " · " + pending + " kayıt sunucuya gidecek" : "";
        switch (state) {
            case BridgeLink.READY:
                long held = s.isNull("held") ? -1 : s.optLong("held", -1);
                return "Gateway'e bağlı" + (held > 0 ? " · gateway'de " + held + " kayıt" : "") + tail;
            case BridgeLink.ADAPTER_OFF:
                return "Bluetooth kapalı; açılınca gateway'e bağlanacak" + tail;
            case BridgeLink.CONNECTING:
                return "Gateway'e bağlanıyor…" + tail;
            case BridgeLink.WAITING:
                return "Gateway'e ulaşılamıyor; menzile girince bağlanacak" + tail;
            default:
                return "Köprü kapalı" + tail;
        }
    }

    private void show(String line) {
        shown = line;
        shownAt = SystemClock.elapsedRealtime();
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm != null) nm.notify(ID, notification(line));
    }

    private void channel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return;
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm == null) return;
        NotificationChannel c = new NotificationChannel(CHANNEL, "Gateway köprüsü", NotificationManager.IMPORTANCE_LOW);
        c.setDescription("Telefon gateway'in kayıtlarını alıp sunucuya iletirken görünür");
        c.setShowBadge(false);
        nm.createNotificationChannel(c);
    }

    @SuppressWarnings("deprecation")
    private Notification notification(String line) {
        Notification.Builder b = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
            ? new Notification.Builder(this, CHANNEL) : new Notification.Builder(this);
        Intent launch = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (launch == null) launch = new Intent(this, MainActivity.class);
        PendingIntent open = PendingIntent.getActivity(this, 3, launch,
            PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        return b.setSmallIcon(R.drawable.ic_stat_orun).setContentIntent(open)
            .setContentTitle("ORUN köprü").setContentText(line)
            .setOngoing(true).setOnlyAlertOnce(true).setShowWhen(false).build();
    }

    private void foreground(String line) {
        channel();
        Notification n = notification(line);
        shown = line;
        shownAt = SystemClock.elapsedRealtime();
        if (Build.VERSION.SDK_INT >= 29) startForeground(ID, n, FGS_CONNECTED_DEVICE);
        else startForeground(ID, n);
    }
}
