// SPDX-License-Identifier: Apache-2.0
package com.musegadgets.relay;

import android.Manifest;
import android.app.Activity;
import android.bluetooth.*;
import android.bluetooth.le.*;
import android.content.*;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.widget.*;
import java.util.HashSet;
import java.util.Set;

public final class MainActivity extends Activity {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Set<String> found = new HashSet<>();
    private BluetoothLeScanner scanner;
    private LinearLayout devices;
    private TextView state;
    private final Runnable stopScan = this::stopScanning;
    private final BroadcastReceiver updates = new BroadcastReceiver() {
        @Override
        public void onReceive(Context c, Intent i) {
            state.setText(i.getStringExtra("status"));
        }
    };
    private final ScanCallback scan = new ScanCallback() {
        @Override
        public void onScanResult(int type, ScanResult result) {
            BluetoothDevice d = result.getDevice();
            String name = d.getName();
            if (name == null || !name.toLowerCase(java.util.Locale.ROOT).contains("muse")
                || !found.add(d.getAddress()))
                return;
            Button button = new Button(MainActivity.this);
            button.setText(name + "\n" + d.getAddress());
            devices.addView(button);
            button.setOnClickListener(v -> {
                stopScanning();
                startForegroundService(
                    new Intent(MainActivity.this, RelayService.class).putExtra("address", d.getAddress()));
                state.setText("Connecting. If asked, enter the passkey shown on the gadget.");
            });
        }
        @Override
        public void onScanFailed(int error) {
            state.setText("Bluetooth scan failed (" + error + "). Try again.");
        }
    };
    @Override
    public void onCreate(Bundle saved) {
        super.onCreate(saved);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(32, 48, 32, 32);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(content);
        setContentView(scroll);
        TextView title = new TextView(this);
        title.setText("Muse Relay");
        title.setTextSize(28);
        content.addView(title);
        state = new TextView(this);
        state.setText(
            "Connect your gadget over Bluetooth. This phone supplies internet; speech stays on the gadget.");
        content.addView(state);
        Button find = new Button(this);
        find.setText("Find gadget");
        content.addView(find);
        find.setOnClickListener(v -> startScanning());
        Button stop = new Button(this);
        stop.setText("Disconnect");
        content.addView(stop);
        stop.setOnClickListener(v -> {
            stopService(new Intent(this, RelayService.class));
            state.setText("Disconnected");
        });
        devices = new LinearLayout(this);
        devices.setOrientation(LinearLayout.VERTICAL);
        content.addView(devices);
        registerReceiver(updates, new IntentFilter(RelayService.STATUS), Context.RECEIVER_NOT_EXPORTED);
    }
    private void startScanning() {
        if (checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED
            || checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT)
                != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(
                new String[] {Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT,
                    Manifest.permission.POST_NOTIFICATIONS},
                1);
            return;
        }
        stopScanning();
        BluetoothAdapter adapter = getSystemService(BluetoothManager.class).getAdapter();
        if (adapter == null || !adapter.isEnabled()) {
            state.setText("Turn on Bluetooth, then tap Find gadget.");
            return;
        }
        scanner = adapter.getBluetoothLeScanner();
        found.clear();
        devices.removeAllViews();
        state.setText("Looking for Muse gadgets…");
        scanner.startScan(
            null, new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), scan);
        handler.postDelayed(stopScan, 15000);
    }
    private void stopScanning() {
        handler.removeCallbacks(stopScan);
        if (scanner != null) {
            scanner.stopScan(scan);
            scanner = null;
        }
    }
    @Override
    protected void onDestroy() {
        stopScanning();
        unregisterReceiver(updates);
        super.onDestroy();
    }
}
