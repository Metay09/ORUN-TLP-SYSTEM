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
