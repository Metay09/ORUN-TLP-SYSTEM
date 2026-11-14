package tr.orun.edge;

import android.Manifest;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothStatusCodes;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Handler;

import java.lang.reflect.Method;
import java.util.UUID;

/**
 * BridgeLink.Radio on Android's BluetoothGatt. Every call comes on the
 * bridge's worker thread; every GATT callback (a binder thread) is posted to
 * that thread and dropped if it belongs to a GATT object already closed.
 * Each Bluetooth event keeps the CPU awake for a few seconds (wake), long
 * enough to store the record and answer the Gateway with the screen off.
 */
final class AndroidRadio extends BluetoothGattCallback implements BridgeLink.Radio {
    private static final UUID SERVICE = UUID.fromString(EdgeWire.SERVICE);
    private static final UUID TO_GATEWAY = UUID.fromString(EdgeWire.TO_GATEWAY);
    private static final UUID TO_EDGE = UUID.fromString(EdgeWire.TO_EDGE);
    private static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    private final Context context;
    private final Handler handler;
    private final Runnable wake;
    private BridgeLink link;
    private String address;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic toGateway, toEdge;

    AndroidRadio(Context context, Handler handler, Runnable wake) {
        this.context = context.getApplicationContext();
        this.handler = handler;
        this.wake = wake;
    }

    void attach(BridgeLink link) { this.link = link; }

    void setAddress(String address) { this.address = address; }

    String address() { return address; }

    private BluetoothAdapter adapter() {
        BluetoothManager m = (BluetoothManager) context.getSystemService(Context.BLUETOOTH_SERVICE);
        return m == null ? null : m.getAdapter();
    }

    boolean permitted() {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED;
    }

    @Override
    public boolean adapterOn() {
        try {
            BluetoothAdapter a = adapter();
            return a != null && a.isEnabled();
        } catch (RuntimeException e) {
            return false;
        }
    }

    @Override
    public boolean connect(boolean auto) {
        BluetoothAdapter a = adapter();
        if (a == null || address == null || !permitted() || !BluetoothAdapter.checkBluetoothAddress(address)) return false;
        try {
            BluetoothDevice device = a.getRemoteDevice(address);
            gatt = device.connectGatt(context, auto, this, BluetoothDevice.TRANSPORT_LE);
            return gatt != null;
        } catch (RuntimeException e) {          // SecurityException: permission taken away
            gatt = null;
            return false;
        }
    }

    @Override
    public void discover() {
        final BluetoothGatt g = gatt;
        boolean started;
        try { started = g != null && g.discoverServices(); } catch (RuntimeException e) { started = false; }
        if (!started) handler.post(() -> { if (g == gatt && link != null) link.onDisconnected(-3); });
    }

    @Override
    public void refreshAndDiscover() {
        final BluetoothGatt g = gatt;
        if (g == null) return;
        try {
            // Hidden but long-standing: clears Android's cached service table.
            Method refresh = g.getClass().getMethod("refresh");
            refresh.invoke(g);
        } catch (Exception ignored) {
            // Not available: discover again anyway.
        }
        handler.postDelayed(() -> { if (g == gatt) discover(); }, 600);
    }

    @Override
    @SuppressWarnings("deprecation")
    public boolean enableIndications() {
        BluetoothGatt g = gatt;
        if (g == null || toEdge == null) return false;
        try {
            if (!g.setCharacteristicNotification(toEdge, true)) return false;
            BluetoothGattDescriptor d = toEdge.getDescriptor(CCCD);
            if (d == null) return false;
            byte[] value = (toEdge.getProperties() & BluetoothGattCharacteristic.PROPERTY_INDICATE) != 0
                ? BluetoothGattDescriptor.ENABLE_INDICATION_VALUE : BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE;
            if (Build.VERSION.SDK_INT >= 33) return g.writeDescriptor(d, value) == BluetoothStatusCodes.SUCCESS;
            d.setValue(value);
            return g.writeDescriptor(d);
        } catch (RuntimeException e) {
            return false;
        }
    }

    @Override
    @SuppressWarnings("deprecation")
    public boolean write(byte[] frame) {
        BluetoothGatt g = gatt;
        if (g == null || toGateway == null) return false;
        try {
            if (Build.VERSION.SDK_INT >= 33)
                return g.writeCharacteristic(toGateway, frame, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
                    == BluetoothStatusCodes.SUCCESS;
            toGateway.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            toGateway.setValue(frame);
            return g.writeCharacteristic(toGateway);
        } catch (RuntimeException e) {
            return false;
        }
    }

    @Override
    public void priority(boolean high) {
        BluetoothGatt g = gatt;
        if (g == null) return;
        try {
            g.requestConnectionPriority(high ? BluetoothGatt.CONNECTION_PRIORITY_HIGH
                : BluetoothGatt.CONNECTION_PRIORITY_BALANCED);
        } catch (RuntimeException ignored) {
            // Optional.
        }
    }

    @Override
    public void close() {
        BluetoothGatt g = gatt;
        gatt = null;
        toGateway = null;
        toEdge = null;
        if (g == null) return;
        try { g.disconnect(); } catch (RuntimeException ignored) { }
        try { g.close(); } catch (RuntimeException ignored) { }
    }

    // ---- callbacks (binder thread) -> worker thread ---------------------------
    private void post(BluetoothGatt g, Runnable r) {
        wake.run();
        handler.post(() -> { if (g == gatt && link != null) r.run(); });
    }

    @Override
    public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
        post(g, () -> {
            if (newState == BluetoothGatt.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) link.onConnected();
            else if (newState == BluetoothGatt.STATE_DISCONNECTED || status != BluetoothGatt.GATT_SUCCESS)
                link.onDisconnected(status);
        });
    }

    @Override
    public void onServicesDiscovered(BluetoothGatt g, int status) {
        post(g, () -> {
            BluetoothGattService s = status == BluetoothGatt.GATT_SUCCESS ? g.getService(SERVICE) : null;
            toGateway = s == null ? null : s.getCharacteristic(TO_GATEWAY);
            toEdge = s == null ? null : s.getCharacteristic(TO_EDGE);
            link.onServices(toGateway != null && toEdge != null);
        });
    }

    @Override
    public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int status) {
        if (!CCCD.equals(d.getUuid())) return;
        post(g, () -> link.onEnabled(status == BluetoothGatt.GATT_SUCCESS));
    }

    @Override
    public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
        if (!TO_GATEWAY.equals(c.getUuid())) return;
        post(g, () -> link.onWritten(status == BluetoothGatt.GATT_SUCCESS));
    }

    /** Android 13+: the value comes with the call. */
    @Override
    public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] value) {
        if (!TO_EDGE.equals(c.getUuid()) || value == null) return;
        final byte[] copy = value.clone();
        post(g, () -> link.onFrame(copy));
    }

    /** Before Android 13 only this one is called (from 13 on only the one above). */
    @Override
    @SuppressWarnings("deprecation")
    public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
        if (Build.VERSION.SDK_INT >= 33) return;
        byte[] value = c.getValue();
        if (!TO_EDGE.equals(c.getUuid()) || value == null) return;
        final byte[] copy = value.clone();
        post(g, () -> link.onFrame(copy));
    }
}
