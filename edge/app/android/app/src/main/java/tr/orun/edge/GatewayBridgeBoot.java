package tr.orun.edge;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** After a reboot or an app update: bridge the chosen Gateway again, if the owner left it on. */
public final class GatewayBridgeBoot extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        String action = intent == null ? null : intent.getAction();
        if (!Intent.ACTION_BOOT_COMPLETED.equals(action) &&
                !Intent.ACTION_MY_PACKAGE_REPLACED.equals(action)) return;
        try {
            if (GatewayBridgeService.enabled(context) && OrunMobileSessionPlugin.accountForService(context) != null)
                GatewayBridgeService.launch(context);
        } catch (RuntimeException ignored) {
            // Android refused the start; the app starts the bridge when opened.
        }
    }
}
