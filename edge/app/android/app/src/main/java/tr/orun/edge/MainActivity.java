package tr.orun.edge;

import android.os.Bundle;

import com.getcapacitor.BridgeActivity;

public class MainActivity extends BridgeActivity {
    @Override
    public void onCreate(Bundle savedInstanceState) {
        registerPlugin(OrunIdentityPlugin.class);
        super.onCreate(savedInstanceState);
    }
}
