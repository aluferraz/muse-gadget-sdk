// SPDX-License-Identifier: Apache-2.0
package com.musegadgets.relay;

import android.app.*;
import android.bluetooth.*;
import android.content.*;
import android.net.ConnectivityManager;
import android.net.NetworkCapabilities;
import android.os.*;
import java.security.SecureRandom;
import java.util.*;
import java.util.concurrent.*;

/**
 * GATT state belongs to the handler; each socket has one reader and one writer.
 * One DATA packet per direction/stream is outstanding. ACK follows consumption.
 * No credentials or decrypted application messages enter this process.
 */
public final class RelayService extends Service {
    static final String STATUS = "com.musegadgets.relay.STATUS";
    private static final byte[] EMPTY = new byte[0];
    private HandlerThread thread;
    private Handler handler;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic rx;
    private final ArrayDeque<byte[]> writes = new ArrayDeque<>();
    private final Map<Integer, Pipe> pipes = new HashMap<>();
    private final Set<Integer> usedIds = new HashSet<>();
    private boolean writing, admitted, stopped;
    private int epoch, payload = 12;
    private long operation, lifecycle;
    private ThreadPoolExecutor workers;
    private String address;
    private int retries;
    private final Runnable writeTimeout = () -> {
        if (writing)
            fail("Bluetooth write timed out.", true);
    };
    private final BroadcastReceiver bonds = new BroadcastReceiver() {
        @Override
        public void onReceive(Context c, Intent i) {
            BluetoothDevice d = i.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice.class);
            if (d != null)
                handler.post(() -> {
                    if (!d.getAddress().equals(address)) return;
                    int state = d.getBondState();
                    if (state == BluetoothDevice.BOND_BONDED && gatt == null && !stopped)
                        connect(d);
                    else if (state == BluetoothDevice.BOND_NONE)
                        fail("Pairing cancelled. Tap the gadget to try again.", false);
                });
        }
    };
    @Override
    public void onCreate() {
        super.onCreate();
        thread = new HandlerThread("muse-relay");
        thread.start();
        handler = new Handler(thread.getLooper());
        workers = new ThreadPoolExecutor(6, 6, 30, TimeUnit.SECONDS, new ArrayBlockingQueue<>(6));
        getSystemService(NotificationManager.class)
            .createNotificationChannel(
                new NotificationChannel("relay", "Gadget relay", NotificationManager.IMPORTANCE_LOW));
        registerReceiver(bonds, new IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED));
        handler.post(new Runnable() {
            public void run() {
                for (Pipe pipe : new ArrayList<>(pipes.values())) pipe.transport.tick();
                if (!stopped)
                    handler.postDelayed(this, 1000);
            }
        });
    }
    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        startForeground(1, notification("Connecting to gadget"));
        String selected = intent == null ? null : intent.getStringExtra("address");
        handler.post(() -> {
            cleanup();
            stopped = false;
            address = selected;
            retries = 0;
            if (address == null) {
                fail("Select a gadget to connect.", false);
                return;
            }
            BluetoothAdapter adapter = getSystemService(BluetoothManager.class).getAdapter();
            if (adapter == null || !adapter.isEnabled()) {
                fail("Bluetooth is off.", false);
                return;
            }
            BluetoothDevice d = adapter.getRemoteDevice(address);
            if (d.getBondState() != BluetoothDevice.BOND_BONDED) {
                status("Pair with the passkey displayed on the gadget.");
                if (!d.createBond())
                    fail("Could not start pairing.", false);
            } else
                connect(d);
        });
        return START_NOT_STICKY;
    }
    private Notification notification(String text) {
        PendingIntent open = PendingIntent.getActivity(
            this, 0, new Intent(this, MainActivity.class), PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(this, "relay")
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentTitle("Muse Relay")
            .setContentText(text)
            .setContentIntent(open)
            .setOngoing(true)
            .build();
    }
    private void status(String message) {
        sendBroadcast(new Intent(STATUS).setPackage(getPackageName()).putExtra("status", message));
        getSystemService(NotificationManager.class).notify(1, notification(message));
    }
    private boolean internetAvailable() {
        ConnectivityManager cm = getSystemService(ConnectivityManager.class);
        NetworkCapabilities caps = cm.getNetworkCapabilities(cm.getActiveNetwork());
        return caps != null && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED);
    }
    private void connect(BluetoothDevice d) {
        status("Connecting over Bluetooth…");
        gatt = d.connectGatt(this, false, callback, BluetoothDevice.TRANSPORT_LE);
        long token = lifecycle;
        handler.postDelayed(() -> {
            if (token == lifecycle && !admitted)
                fail("Connection timed out.", true);
        }, 30000);
    }
    private boolean current(BluetoothGatt g) {
        return !stopped && g == gatt;
    }
    private final BluetoothGattCallback callback = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt g, int status, int state) {
            handler.post(() -> {
                if (!current(g))
                    return;
                if (status != BluetoothGatt.GATT_SUCCESS || state == BluetoothProfile.STATE_DISCONNECTED) {
                    fail("Bluetooth disconnected.", true);
                    return;
                }
                if (state == BluetoothProfile.STATE_CONNECTED) {
                    g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH);
                    if (!g.requestMtu(247))
                        fail("Cannot negotiate Bluetooth MTU.", true);
                }
            });
        }
        @Override
        public void onMtuChanged(BluetoothGatt g, int mtu, int status) {
            handler.post(() -> {
                if (!current(g))
                    return;
                payload = Math.min(Protocol.PAYLOAD, Math.max(12, mtu - 3 - Protocol.HEADER));
                if (!g.discoverServices())
                    fail("Cannot discover gadget services.", true);
            });
        }
        @Override
        public void onServicesDiscovered(BluetoothGatt g, int status) {
            handler.post(() -> {
                if (!current(g))
                    return;
                BluetoothGattService service = g.getService(Protocol.SERVICE);
                if (status != 0 || service == null) {
                    fail("Install the relay firmware on the gadget first.", false);
                    return;
                }
                rx = service.getCharacteristic(Protocol.RX);
                BluetoothGattCharacteristic tx = service.getCharacteristic(Protocol.TX);
                BluetoothGattDescriptor ccc = tx == null ? null : tx.getDescriptor(Protocol.CCC);
                if (rx == null || tx == null || ccc == null || !g.setCharacteristicNotification(tx, true)
                    || g.writeDescriptor(ccc, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                        != BluetoothStatusCodes.SUCCESS)
                    fail("Cannot subscribe to relay.", false);
            });
        }
        @Override
        public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int status) {
            handler.post(() -> {
                if (!current(g))
                    return;
                if (status != 0) {
                    fail("Bluetooth subscription failed.", true);
                    return;
                }
                if (!internetAvailable()) {
                    fail("Phone internet is unavailable.", true);
                    return;
                }
                do {
                    epoch = new SecureRandom().nextInt();
                } while (epoch == 0);
                send(new Protocol(Protocol.HELLO, 0, epoch, EMPTY));
            });
        }
        @Override
        public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
            handler.post(() -> {
                if (!current(g))
                    return;
                if (status != 0) {
                    fail("Bluetooth write failed.", true);
                    return;
                }
                handler.removeCallbacks(writeTimeout);
                writing = false;
                operation++;
                pump();
            });
        }
        @Override
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] value) {
            byte[] owned = value.clone();
            handler.post(() -> {
                if (current(g) && c.getUuid().equals(Protocol.TX))
                    receive(owned);
            });
        }
    };
    private void send(Protocol p) {
        if (stopped || gatt == null || p.epoch != epoch)
            return;
        if (writes.size() >= 32) {
            fail("Relay queue limit reached.", true);
            return;
        }
        writes.add(p.encode());
        pump();
    }
    private void pump() {
        if (writing || writes.isEmpty() || gatt == null || rx == null)
            return;
        byte[] p = writes.remove();
        writing = true;
        operation++;
        if (gatt.writeCharacteristic(rx, p, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
            != BluetoothStatusCodes.SUCCESS) {
            fail("Bluetooth send unavailable.", true);
            return;
        }
        handler.postDelayed(writeTimeout, 15000);
    }
    private void receive(byte[] bytes) {
        Protocol p;
        try {
            p = Protocol.parse(bytes);
        } catch (IllegalArgumentException e) {
            fail("Invalid relay frame.", true);
            return;
        }
        if (p.epoch != epoch)
            return;
        if (p.type == Protocol.READY && !admitted) {
            admitted = true;
            retries = 0;
            status("Relay connected. Internet uses this phone's connection.");
            return;
        }
        if (!admitted) {
            fail("Unexpected relay handshake.", true);
            return;
        }
        if (p.type == Protocol.OPEN) {
            if (p.data[0] == 3) {
                send(new Protocol(Protocol.CLOSE, p.id, epoch, EMPTY));
                return;
            }
            if (pipes.size() >= Protocol.STREAMS || usedIds.contains(p.id) || usedIds.size() >= 65535) {
                send(new Protocol(Protocol.CLOSE, p.id, epoch, EMPTY));
                return;
            }
            usedIds.add(p.id);
            Pipe pipe = new Pipe(p.id, epoch, p.data[0], payload);
            pipes.put(p.id, pipe);
            pipe.transport.start();
            return;
        }
        Pipe pipe = pipes.get(p.id);
        if (pipe == null)
            return;
        if (p.type == Protocol.CLOSE) {
            finish(pipe, false);
            return;
        }
        if (p.type == Protocol.DATA && pipe.transport.accept(p.data))
            return;
        if (p.type == Protocol.ACK && pipe.transport.acknowledge())
            return;
        finish(pipe, true);
    }
    private void finish(Pipe pipe, boolean notify) {
        if (pipes.get(pipe.id) != pipe)
            return;
        pipes.remove(pipe.id);
        pipe.close();
        if (notify)
            send(new Protocol(Protocol.CLOSE, pipe.id, pipe.generation, EMPTY));
    }
    private void cleanup() {
        handler.removeCallbacks(writeTimeout);
        operation++;
        lifecycle++;
        admitted = false;
        writing = false;
        writes.clear();
        usedIds.clear();
        for (Pipe p : pipes.values()) p.close();
        pipes.clear();
        BluetoothGatt old = gatt;
        gatt = null;
        rx = null;
        if (old != null) {
            old.disconnect();
            old.close();
        }
    }
    private void fail(String message, boolean retry) {
        cleanup();
        status(message);
        if (retry && !stopped && retries++ < 5 && address != null) {
            long token = operation;
            handler.postDelayed(() -> {
                if (!stopped && token == operation) {
                    BluetoothAdapter a = getSystemService(BluetoothManager.class).getAdapter();
                    if (a != null && a.isEnabled())
                        connect(a.getRemoteDevice(address));
                }
            }, Math.min(30000, 1000L << retries));
        }
    }
    private final class Pipe {
        final int id, generation;
        final SocketPipe transport;
        Pipe(int id, int generation, int endpoint, int chunk) {
            this.id = id;
            this.generation = generation;
            transport = new SocketPipe(endpoint, chunk, 30000, workers, new SocketPipe.Events() {
                public void opened() {
                    post(Protocol.OPENED, EMPTY);
                }
                public void data(byte[] data) {
                    post(Protocol.DATA, data);
                }
                public void consumed() {
                    post(Protocol.ACK, EMPTY);
                }
                public void closed() {
                    handler.post(() -> finish(Pipe.this, true));
                }
            }, SocketPipe.PUBLIC);
        }
        void post(int type, byte[] data) {
            handler.post(() -> {
                if (pipes.get(id) == this)
                    send(new Protocol(type, id, generation, data));
            });
        }
        void close() {
            transport.close();
        }
    }
    @Override
    public void onDestroy() {
        unregisterReceiver(bonds);
        handler.post(() -> {
            stopped = true;
            cleanup();
            workers.shutdownNow();
            thread.quitSafely();
        });
        super.onDestroy();
    }
    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
