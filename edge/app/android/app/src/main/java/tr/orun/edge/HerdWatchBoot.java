package tr.orun.edge;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** Starts the herd watch again after a reboot or an app update, if this phone is signed in. */
public final class HerdWatchBoot extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        String action = intent == null ? null : intent.getAction();
        if (!Intent.ACTION_BOOT_COMPLETED.equals(action) &&
                !Intent.ACTION_MY_PACKAGE_REPLACED.equals(action)) return;
        try {
            if (OrunMobileSessionPlugin.accountForService(context) != null)
                HerdWatchService.start(context);
        } catch (RuntimeException ignored) {
            // Android refused the start; the app starts the watch when opened.
        }
    }
}
