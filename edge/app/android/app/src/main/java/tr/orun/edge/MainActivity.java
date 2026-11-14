package tr.orun.edge;

import android.bluetooth.BluetoothGatt;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.webkit.WebView;
import com.getcapacitor.BridgeActivity;
import com.getcapacitor.PluginHandle;

import java.lang.reflect.Field;
import java.util.Map;

public class MainActivity extends BridgeActivity {
    @Override
    public void onCreate(Bundle savedInstanceState) {
        // Registration only: does not create a key, start enrollment, or
        // change the normal ORUN Edge observation/upload lifecycle.
        registerPlugin(OrunMobileIdentityPlugin.class);
        registerPlugin(OrunMobileSessionPlugin.class);
        registerPlugin(OrunHerdWatchPlugin.class);
        super.onCreate(savedInstanceState);
        // The whole app (sync, notifications, Gateway link) runs inside the
        // WebView. By default Android lowers the WebView's renderer process
        // to "cached" as soon as the app leaves the screen, and a cached
        // process is frozen or killed within minutes even while our
        // foreground service runs: the owner's phone showed old data and a
        // full reload after a few minutes in the background (2026-11-14).
        // Keep the renderer at the app's own priority when not visible.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && getBridge() != null) {
            getBridge().getWebView().setRendererPriorityPolicy(
                WebView.RENDERER_PRIORITY_IMPORTANT, false);
        }
    }

    /**
     * The app's code (and with it the Gateway link) lives in the WebView.
     * When the screen goes away for good (swiped from recents), the BLE
     * plugin does not close its connections, and our foreground services
     * keep the process alive: the Gateway stayed connected to nobody, did
     * not advertise, could not be found or reached, and no record moved
     * (owner's phone, 2026-11-14 17:08). Close them, and stop the "gateway'e
     * bağlı" service whose notification would now be wrong. The herd watch
     * (server alerts) keeps running.
     */
    @Override
    public void onDestroy() {
        if (!isChangingConfigurations()) {
            closeBleConnections();
            try {
                stopService(new Intent().setClassName(this,
                        "io.capawesome.capacitorjs.plugins.foregroundservice.AndroidForegroundService"));
            } catch (RuntimeException ignored) {
                // Not running.
            }
        }
        super.onDestroy();
    }

    // @capacitor-community/bluetooth-le 8.3.0 keeps BluetoothLe.deviceMap
    // (id -> Device) and Device.bluetoothGatt private. Version pinned in
    // package.json; on any change this only stops doing anything.
    private void closeBleConnections() {
        try {
            if (getBridge() == null) return;
            PluginHandle handle = getBridge().getPlugin("BluetoothLe");
            if (handle == null || handle.getInstance() == null) return;
            Object plugin = handle.getInstance();
            Field mapField = plugin.getClass().getDeclaredField("deviceMap");
            mapField.setAccessible(true);
            Object map = mapField.get(plugin);
            if (!(map instanceof Map)) return;
            for (Object device : ((Map<?, ?>) map).values()) {
                if (device == null) continue;
                Field gattField = device.getClass().getDeclaredField("bluetoothGatt");
                gattField.setAccessible(true);
                Object gatt = gattField.get(device);
                if (gatt instanceof BluetoothGatt) {
                    ((BluetoothGatt) gatt).disconnect();
                    ((BluetoothGatt) gatt).close();
                }
            }
        } catch (Exception | LinkageError ignored) {
            // Best effort: the process ending closes them too.
        }
    }
}
