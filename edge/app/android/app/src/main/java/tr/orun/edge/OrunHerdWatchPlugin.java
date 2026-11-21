package tr.orun.edge;

import android.content.SharedPreferences;
import android.util.AtomicFile;

import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.CapacitorPlugin;

import java.io.File;
import java.nio.charset.StandardCharsets;

/** The app's handle on HerdWatchService: start/stop it and read what it kept. */
@CapacitorPlugin(name = "OrunHerdWatch")
public final class OrunHerdWatchPlugin extends Plugin {
    @PluginMethod
    public void start(PluginCall call) {
        try {
            HerdWatchService.start(getContext());
            call.resolve();
        } catch (RuntimeException e) {
            call.reject("ORUN_HERD_WATCH_START_FAILED");
        }
    }

    @PluginMethod
    public void stop(PluginCall call) {
        HerdWatchService.stop(getContext());
        // A new login starts from a clean memory: nothing of the old account is shown.
        getContext().getSharedPreferences(HerdWatchService.PREFS, 0).edit().clear().apply();
        new File(getContext().getNoBackupFilesDir(), HerdWatchService.LATEST_FILE).delete();
        call.resolve();
    }

    private org.json.JSONObject currentAccount(PluginCall call) {
        org.json.JSONObject account = OrunMobileSessionPlugin.accountForService(getContext());
        if (account == null || !account.optString("site").equals(call.getString("site")) ||
                !account.optString("actor").equals(call.getString("actor")) ||
                !account.optString("installation").equals(call.getString("installation")))
            return null;
        return account;
    }

    // Capacitor's JSON may decode a valid cursor as Integer or Long.
    private static long cursor(PluginCall call) {
        Object raw = call.getData().opt("cursor");
        if (!(raw instanceof Number)) throw new IllegalArgumentException("CURSOR_TYPE");
        double n = ((Number) raw).doubleValue();
        if (Double.isNaN(n) || Double.isInfinite(n) || n < 0 ||
                n > 9007199254740991L || n != Math.rint(n))
            throw new IllegalArgumentException("CURSOR_RANGE");
        return (long) n;
    }

    /** A cursor is seeded only after AEAD verification + SQLite commit in JS. */
    @PluginMethod
    public void historySeed(PluginCall call) {
        try {
            org.json.JSONObject account = currentAccount(call);
            if (account == null) throw new IllegalArgumentException("ACCOUNT");
            HerdHistoryStage.seed(getContext(), account, cursor(call));
            call.resolve();
        } catch (Exception e) {
            call.reject("ORUN_HISTORY_SEED_FAILED");
        }
    }

    /** Unverified encrypted frames; the JS mirror must verify before commit. */
    @PluginMethod
    public void historyPending(PluginCall call) {
        try {
            org.json.JSONObject account = OrunMobileSessionPlugin.accountForService(getContext());
            JSObject out = new JSObject();
            out.put("stage", account == null ? null : HerdHistoryStage.pending(getContext(), account));
            call.resolve(out);
        } catch (Exception e) {
            call.reject("ORUN_HISTORY_STAGE_READ_FAILED");
        }
    }

    /** Acknowledge ONLY after the app's own verified SQLite commit. Not a radio ACK. */
    @PluginMethod
    public void historyAck(PluginCall call) {
        try {
            org.json.JSONObject account = currentAccount(call);
            if (account == null) throw new IllegalArgumentException("ACCOUNT");
            HerdHistoryStage.acknowledge(getContext(), account, cursor(call));
            call.resolve();
        } catch (Exception e) {
            call.reject("ORUN_HISTORY_STAGE_ACK_FAILED");
        }
    }

    /** The foreground USB reader is owned by the WebView. Expire its heartbeat if it stops. */
    @PluginMethod
    public void localGateway(PluginCall call) {
        org.json.JSONObject account = OrunMobileSessionPlugin.accountForService(getContext());
        if (account != null && account.optString("site").equals(call.getString("site"))
                && account.optString("actor").equals(call.getString("actor"))) {
            SharedPreferences.Editor edit = getContext().getSharedPreferences(HerdWatchService.PREFS, 0).edit();
            edit.putString("localSite", account.optString("site"));
            edit.putString("localActor", account.optString("actor"));
            edit.putLong("usbAt", Boolean.TRUE.equals(call.getBoolean("usb")) ? System.currentTimeMillis() : 0);
            edit.putString("localMeasured", call.getString("measured", "{}"));
            edit.apply();
        }
        call.resolve();
    }

    /** When the service last reached the server, and the snapshot it kept (JSON text) if any. */
    @PluginMethod
    public void status(PluginCall call) {
        SharedPreferences p = getContext().getSharedPreferences(HerdWatchService.PREFS, 0);
        JSObject out = new JSObject();
        out.put("lastOk", p.getLong("lastOk", 0L));
        out.put("line", p.getString("line", ""));
        try {
            File f = new File(getContext().getNoBackupFilesDir(), HerdWatchService.LATEST_FILE);
            if (f.exists()) out.put("latest", new String(new AtomicFile(f).readFully(), StandardCharsets.UTF_8));
        } catch (Exception ignored) {
            // No snapshot: the app syncs as before.
        }
        call.resolve(out);
    }
}
