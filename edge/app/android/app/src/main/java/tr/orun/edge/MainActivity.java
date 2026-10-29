package tr.orun.edge;

import android.os.Bundle;
import com.getcapacitor.BridgeActivity;

public class MainActivity extends BridgeActivity {
    @Override
    public void onCreate(Bundle savedInstanceState) {
        // Registration only: does not create a key, start enrollment, or
        // change the normal ORUN Edge observation/upload lifecycle.
        registerPlugin(OrunMobileIdentityPlugin.class);
        super.onCreate(savedInstanceState);
    }
}
