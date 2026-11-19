// Bluetooth printers on Android (ADR 0177): the fourth Java class of CeeSafe's runtime, compiled into the same
// embedded dex as InputView by tools/gen_android_dex.py. Edit it here, then run the generator.
//
// The runtime calls these from the program's thread that makes the Bluetooth call (attached to the VM), never from the
// activity's: each blocks until it is done. Errors are BluetoothError's numbers (lib/std/bluetooth.cee).
//
// A list is the records of ceesafe_rt_bluetooth.c (a transport byte, 'C' or 'L', then the address and the name, each
// ended by a zero byte) after a first byte that is 0, or one byte that is the error.
package org.ceesafe.window;

import android.Manifest;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothClass;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothSocket;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanRecord;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Looper;
import android.os.ParcelUuid;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;

public final class Bluetooth {
    static final int UNSUPPORTED = 1;
    static final int OFF = 2;
    static final int DENIED = 3;
    static final int NOT_FOUND = 4;
    static final int REFUSED = 5;
    static final int LOST = 6;

    static final UUID SPP = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB");
    /** The printers' services (ADR 0177 §1). */
    static final UUID[] SERVICES = {
        UUID.fromString("000018F0-0000-1000-8000-00805F9B34FB"),
        UUID.fromString("0000FF00-0000-1000-8000-00805F9B34FB"),
        UUID.fromString("E7810A71-73AE-499D-8C15-FAA9AEF0C3F2"),
    };

    // ---- Permissions -------------------------------------------------------------------------------------------

    /** The permissions a call needs, by the system's release. */
    private static String[] needed(boolean scan) {
        if (Build.VERSION.SDK_INT >= 31) {
            return scan ? new String[] {Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.BLUETOOTH_SCAN}
                        : new String[] {Manifest.permission.BLUETOOTH_CONNECT};
        }
        return scan ? new String[] {Manifest.permission.ACCESS_FINE_LOCATION} : new String[0];
    }

    private static boolean granted(Activity activity, String[] permissions) {
        for (String p : permissions) {
            if (activity.checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) {
                return false;
            }
        }
        return true;
    }

    /**
     * 0 when the program has the permissions, else DENIED. When it lacks them, the system is asked to ask the user,
     * and this waits for the answer: the activity loses the focus while the system's dialog is up and has it again
     * when the user has answered (two seconds for the dialog to come, a minute for the answer). A permission the app
     * does not declare, or one the user refused for good, brings no dialog. Never on the activity's thread, which the
     * dialog needs.
     */
    static int permit(final Activity activity, boolean scan) {
        final String[] permissions = needed(scan);
        if (granted(activity, permissions)) {
            return 0;
        }
        if (Looper.myLooper() == Looper.getMainLooper()) {
            return DENIED;
        }
        activity.runOnUiThread(() -> activity.requestPermissions(permissions, 0x0177));
        long until = System.currentTimeMillis() + 2000;
        while (activity.hasWindowFocus() && System.currentTimeMillis() < until) {
            pause(50);
        }
        until = System.currentTimeMillis() + 60000;
        while (!activity.hasWindowFocus() && System.currentTimeMillis() < until) {
            pause(100);
        }
        return granted(activity, permissions) ? 0 : DENIED;
    }

    private static void pause(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    /** The adapter, or null with the error in error[0]. */
    private static BluetoothAdapter adapter(Activity activity, int[] error) {
        BluetoothManager manager = (BluetoothManager) activity.getSystemService(Context.BLUETOOTH_SERVICE);
        BluetoothAdapter adapter = manager != null ? manager.getAdapter() : null;
        if (adapter == null) {
            error[0] = UNSUPPORTED;
            return null;
        }
        if (!adapter.isEnabled()) {
            error[0] = OFF;
            return null;
        }
        return adapter;
    }

    // ---- Lists -------------------------------------------------------------------------------------------------

    private static void record(ByteArrayOutputStream out, char transport, String address, String name) {
        out.write(transport);
        byte[] a = address.getBytes(StandardCharsets.UTF_8);
        out.write(a, 0, a.length);
        out.write(0);
        byte[] n = (name != null ? name.replace("\0", "") : "").getBytes(StandardCharsets.UTF_8);
        out.write(n, 0, n.length);
        out.write(0);
    }

    private static byte[] failure(int error) {
        return new byte[] {(byte) error};
    }

    /**
     * Whether a paired device may be a printer: it lists the Serial Port Profile; or it lists nothing yet and its class
     * is imaging (a printer's), miscellaneous or unclassified, as receipt printers say.
     */
    private static boolean printer(BluetoothDevice device) {
        ParcelUuid[] uuids = device.getUuids();
        if (uuids != null && uuids.length > 0) {
            for (ParcelUuid u : uuids) {
                if (SPP.equals(u.getUuid())) {
                    return true;
                }
            }
            return false;
        }
        BluetoothClass c = device.getBluetoothClass();
        if (c == null) {
            return false;
        }
        int major = c.getMajorDeviceClass();
        return major == BluetoothClass.Device.Major.IMAGING || major == BluetoothClass.Device.Major.MISC ||
            major == BluetoothClass.Device.Major.UNCATEGORIZED;
    }

    /** The paired devices that may be printers (Classic). */
    static byte[] paired(Activity activity) {
        int permitted = permit(activity, false);
        if (permitted != 0) {
            return failure(permitted);
        }
        int[] error = {0};
        BluetoothAdapter adapter = adapter(activity, error);
        if (adapter == null) {
            return failure(error[0]);
        }
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        out.write(0);
        try {
            for (BluetoothDevice device : adapter.getBondedDevices()) {
                if (device.getType() != BluetoothDevice.DEVICE_TYPE_LE && printer(device)) {
                    record(out, 'C', device.getAddress(), device.getName());
                }
            }
        } catch (SecurityException e) {
            return failure(DENIED);
        }
        return out.toByteArray();
    }

    /** The Low Energy devices advertising a printer's service, found within `milliseconds`. */
    static byte[] scan(Activity activity, int milliseconds) {
        int permitted = permit(activity, true);
        if (permitted != 0) {
            return failure(permitted);
        }
        int[] error = {0};
        BluetoothAdapter adapter = adapter(activity, error);
        if (adapter == null) {
            return failure(error[0]);
        }
        BluetoothLeScanner scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) {
            return failure(OFF);
        }
        final Map<String, String> found = new LinkedHashMap<>();
        ScanCallback callback = new ScanCallback() {
            @Override
            public void onScanResult(int type, ScanResult result) {
                String name = null;
                ScanRecord r = result.getScanRecord();
                if (r != null) {
                    name = r.getDeviceName();
                }
                try {
                    if (name == null) {
                        name = result.getDevice().getName();
                    }
                } catch (SecurityException e) {
                    name = null;
                }
                synchronized (found) {
                    String address = result.getDevice().getAddress();
                    if (!found.containsKey(address) || (name != null && found.get(address) == null)) {
                        found.put(address, name);
                    }
                }
            }
        };
        List<ScanFilter> filters = new ArrayList<>();
        for (UUID u : SERVICES) {
            filters.add(new ScanFilter.Builder().setServiceUuid(new ParcelUuid(u)).build());
        }
        ScanSettings settings = new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        try {
            scanner.startScan(filters, settings, callback);
            pause(milliseconds);
            scanner.stopScan(callback);
        } catch (SecurityException e) {
            return failure(DENIED);
        }
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        out.write(0);
        synchronized (found) {
            for (Map.Entry<String, String> e : found.entrySet()) {
                record(out, 'L', e.getKey(), e.getValue());
            }
        }
        return out.toByteArray();
    }

    // ---- Links -------------------------------------------------------------------------------------------------

    /** A connection: Classic, a socket; Low Energy, a GATT connection and its characteristic. `error` after open. */
    public static final class Link extends BluetoothGattCallback {
        int error;
        private BluetoothSocket socket;
        private OutputStream stream;
        private BluetoothGatt gatt;
        private BluetoothGattCharacteristic characteristic;
        private boolean acknowledged;
        private int piece = 20;
        private final Semaphore signal = new Semaphore(0);
        private volatile boolean connected;
        private volatile boolean failed;

        int write(byte[] bytes) {
            if (stream != null) {
                try {
                    stream.write(bytes);
                    stream.flush();
                    return 0;
                } catch (IOException e) {
                    return LOST;
                }
            }
            return gatt != null ? writeGatt(bytes) : LOST;
        }

        void close() {
            try {
                if (socket != null) {
                    socket.close();
                }
            } catch (IOException e) {
                // closed already
            }
            try {
                if (gatt != null) {
                    gatt.disconnect();
                    gatt.close();
                }
            } catch (SecurityException e) {
                // the permission went; the system closes it with the process
            }
        }

        private boolean await(long ms) {
            try {
                return signal.tryAcquire(ms, TimeUnit.MILLISECONDS);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return false;
            }
        }

        @Override
        public void onConnectionStateChange(BluetoothGatt g, int status, int state) {
            connected = status == BluetoothGatt.GATT_SUCCESS && state == BluetoothProfile.STATE_CONNECTED;
            if (!connected) {
                failed = true;
            }
            signal.release();
        }

        @Override
        public void onMtuChanged(BluetoothGatt g, int mtu, int status) {
            if (status == BluetoothGatt.GATT_SUCCESS && mtu > 3) {
                piece = mtu - 3;
            }
            signal.release();
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt g, int status) {
            failed = failed || status != BluetoothGatt.GATT_SUCCESS;
            signal.release();
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
            failed = failed || status != BluetoothGatt.GATT_SUCCESS;
            signal.release();
        }

        /** In pieces the connection takes, each waited for (the system answers every write, acknowledged or not). */
        @SuppressWarnings("deprecation")
        private int writeGatt(byte[] bytes) {
            characteristic.setWriteType(acknowledged ? BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                                                     : BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE);
            for (int at = 0; at < bytes.length; at += piece) {
                int n = Math.min(piece, bytes.length - at);
                byte[] part = new byte[n];
                System.arraycopy(bytes, at, part, 0, n);
                signal.drainPermits();
                failed = false;
                boolean sent = false;
                try {
                    for (int tries = 0; tries < 50 && !sent; tries++) {
                        characteristic.setValue(part);
                        sent = gatt.writeCharacteristic(characteristic);
                        if (!sent) {
                            pause(20); // the previous write is still going
                        }
                    }
                } catch (SecurityException e) {
                    return DENIED;
                }
                if (!sent || !await(10000) || failed || !connected) {
                    return LOST;
                }
            }
            return 0;
        }
    }

    /** Opens a link to the device at `address` by `transport` (0 Classic, 1 Low Energy); `error` says how it went. */
    static Link open(Activity activity, int transport, String address) {
        Link link = new Link();
        link.error = permit(activity, false);
        if (link.error != 0) {
            return link;
        }
        int[] error = {0};
        BluetoothAdapter adapter = adapter(activity, error);
        if (adapter == null) {
            link.error = error[0];
            return link;
        }
        if (!BluetoothAdapter.checkBluetoothAddress(address)) {
            link.error = NOT_FOUND;
            return link;
        }
        try {
            BluetoothDevice device = adapter.getRemoteDevice(address);
            if (transport == 0) {
                openClassic(adapter, device, link);
            } else {
                openGatt(activity, device, link);
            }
        } catch (SecurityException e) {
            link.close();
            link.error = DENIED;
        }
        return link;
    }

    private static void openClassic(BluetoothAdapter adapter, BluetoothDevice device, Link link) {
        if (device.getBondState() != BluetoothDevice.BOND_BONDED) {
            link.error = NOT_FOUND;
            return;
        }
        adapter.cancelDiscovery();
        for (int k = 0; k < 2 && link.stream == null; k++) {
            try {
                link.socket = k == 0 ? device.createRfcommSocketToServiceRecord(SPP)
                                     : device.createInsecureRfcommSocketToServiceRecord(SPP);
                link.socket.connect();
                link.stream = link.socket.getOutputStream();
            } catch (IOException e) {
                try {
                    if (link.socket != null) {
                        link.socket.close();
                    }
                } catch (IOException ignored) {
                    // closed already
                }
                link.socket = null;
            }
        }
        if (link.stream == null) {
            link.error = REFUSED;
        }
    }

    private static boolean printerService(UUID u) {
        for (UUID s : SERVICES) {
            if (s.equals(u)) {
                return true;
            }
        }
        return false;
    }

    private static void openGatt(Activity activity, BluetoothDevice device, Link link) {
        link.gatt = device.connectGatt(activity, false, link, BluetoothDevice.TRANSPORT_LE);
        if (link.gatt == null || !link.await(15000) || !link.connected) {
            link.close();
            link.error = REFUSED;
            return;
        }
        // A larger MTU, where the printer takes one; its answer, or five seconds, then the services.
        link.signal.drainPermits();
        if (link.gatt.requestMtu(185)) {
            link.await(5000);
        }
        link.signal.drainPermits();
        link.failed = false;
        if (!link.gatt.discoverServices() || !link.await(10000) || link.failed) {
            link.close();
            link.error = REFUSED;
            return;
        }
        for (int pass = 0; pass < 2 && link.characteristic == null; pass++) {
            int wanted = pass == 0 ? BluetoothGattCharacteristic.PROPERTY_WRITE
                                   : BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE;
            for (BluetoothGattService service : link.gatt.getServices()) {
                if (!printerService(service.getUuid())) {
                    continue;
                }
                for (BluetoothGattCharacteristic c : service.getCharacteristics()) {
                    if ((c.getProperties() & wanted) != 0) {
                        link.characteristic = c;
                        link.acknowledged = pass == 0;
                        break;
                    }
                }
                if (link.characteristic != null) {
                    break;
                }
            }
        }
        if (link.characteristic == null) {
            link.close();
            link.error = REFUSED; // not a printer
        }
    }
}
