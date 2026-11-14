package tr.orun.edge;

import android.os.Build;
import android.os.Bundle;
import android.webkit.WebView;
import com.getcapacitor.BridgeActivity;

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
}
