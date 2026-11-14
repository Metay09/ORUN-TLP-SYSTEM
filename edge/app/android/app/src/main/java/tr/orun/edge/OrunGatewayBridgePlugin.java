package tr.orun.edge;

import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.CapacitorPlugin;

import org.json.JSONArray;
import org.json.JSONObject;

/**
 * The app's handle on the native Gateway bridge (GatewayBridgeService /
 * BridgeRuntime): choose or drop the Gateway, read its state, take over the
 * records it kept, carry LoRa command lines. Event "change": something in
 * the bridge changed, ask status() again.
 */
@CapacitorPlugin(name = "OrunGatewayBridge")
public final class OrunGatewayBridgePlugin extends Plugin {
    private BridgeRuntime runtime;
    private final BridgeRuntime.Listener listener = () -> notifyListeners("change", new JSObject());

    @Override
    public void load() {
        runtime = BridgeRuntime.get(getContext());
        runtime.addListener(listener);
    }

    @Override
    protected void handleOnDestroy() {
        if (runtime != null) runtime.removeListener(listener);
    }

    private static JSObject js(JSONObject o) throws Exception { return new JSObject(o.toString()); }

    @PluginMethod
    public void start(PluginCall call) {
        String address = call.getString("address", "");
        if (address == null || !address.matches("[0-9A-F]{2}(:[0-9A-F]{2}){5}")) {
            call.reject("ORUN_BRIDGE_ADDRESS");
            return;
        }
        try {
            GatewayBridgeService.start(getContext(), address);
            call.resolve();
        } catch (RuntimeException e) {
            call.reject("ORUN_BRIDGE_START_FAILED");
        }
    }

    @PluginMethod
    public void stop(PluginCall call) {
        GatewayBridgeService.stop(getContext());
        call.resolve();
    }

    /** Signed out: stop and forget this account's keys. The ledger file stays (the app holds its copy). */
    @PluginMethod
    public void forget(PluginCall call) {
        GatewayBridgeService.stop(getContext());
        runtime.forget();
        call.resolve();
    }

    /** The app opened while the bridge was meant to run but its service is not: start it again. */
    @PluginMethod
    public void resume(PluginCall call) {
        try {
            if (GatewayBridgeService.enabled(getContext())) GatewayBridgeService.launch(getContext());
            call.resolve();
        } catch (RuntimeException e) {
            call.reject("ORUN_BRIDGE_START_FAILED");
        }
    }

    @PluginMethod
    public void status(PluginCall call) {
        runtime.request(runtime::status, (s, e) -> {
            if (e != null) { call.reject("ORUN_BRIDGE_STATUS"); return; }
            try { call.resolve(js(s)); } catch (Exception x) { call.reject("ORUN_BRIDGE_STATUS"); }
        });
    }

    @PluginMethod
    public void takeRecords(PluginCall call) {
        final String db = call.getString("db", "");
        // Capacitor's getLong() only takes Long; a JS number arrives as Integer.
        final long after = call.getData().optLong("after", 0L);
        final int limit = call.getData().optInt("limit", 200);
        runtime.request(() -> runtime.changes(db, after, limit), (s, e) -> {
            if (e != null) { call.reject("ORUN_BRIDGE_RECORDS"); return; }
            try { call.resolve(js(s)); } catch (Exception x) { call.reject("ORUN_BRIDGE_RECORDS"); }
        });
    }

    @PluginMethod
    public void taken(PluginCall call) {
        final String db = call.getString("db", "");
        final long rev = call.getData().optLong("rev", 0L);
        runtime.request(() -> { runtime.taken(db, rev); return true; }, (s, e) -> {
            if (e != null) call.reject("ORUN_BRIDGE_TAKEN");
            else call.resolve();
        });
    }

    @PluginMethod
    public void takeLora(PluginCall call) {
        final long after = call.getData().optLong("after", 0L);
        runtime.request(() -> runtime.takeLora(after), (s, e) -> {
            if (e != null) { call.reject("ORUN_BRIDGE_LORA"); return; }
            try { call.resolve(js(s)); } catch (Exception x) { call.reject("ORUN_BRIDGE_LORA"); }
        });
    }

    @PluginMethod
    public void sendLine(PluginCall call) {
        final String line = call.getString("line", "");
        if (line == null || line.isEmpty() || line.length() > 159 || !line.matches("[\\x20-\\x7E]+")) {
            call.reject("ORUN_BRIDGE_LINE");
            return;
        }
        runtime.handler.post(() -> runtime.sendLine(line, ok -> {
            if (ok) call.resolve();
            else call.reject("ORUN_BRIDGE_NOT_SENT");
        }));
    }

    @PluginMethod
    public void redump(PluginCall call) {
        runtime.handler.post(runtime::redump);
        call.resolve();
    }

    @PluginMethod
    public void uploadNow(PluginCall call) {
        runtime.handler.post(runtime::upload);
        call.resolve();
    }

    @PluginMethod
    public void setReadKeys(PluginCall call) {
        final JSONArray keys = call.getArray("keys", null);
        if (keys == null || keys.length() > 512) { call.reject("ORUN_BRIDGE_KEYS"); return; }
        runtime.request(() -> { runtime.setReadKeys(keys); return true; }, (s, e) -> {
            if (e != null) call.reject("ORUN_BRIDGE_KEYS");
            else call.resolve();
        });
    }
}
