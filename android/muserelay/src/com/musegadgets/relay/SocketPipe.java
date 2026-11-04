// SPDX-License-Identifier: Apache-2.0
package com.musegadgets.relay;

import java.io.*;
import java.net.*;
import java.util.Arrays;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * One reader and writer, one packet per direction, cooperative close.
 * The owner serializes Events and rejects callbacks from retired pipes.
 */
final class SocketPipe implements AutoCloseable {
    interface Events {
        void opened();
        void data(byte[] data);
        void consumed();
        void closed();
    }
    interface Connector {
        void connect(Socket socket, int endpoint) throws IOException;
    }
    static final Connector PUBLIC = (socket, endpoint) -> {
        InetAddress[] addresses = InetAddress.getAllByName(Protocol.host(endpoint));
        for (InetAddress a : addresses) {
            if (PublicEndpoint.allowed(a)) {
                socket.connect(new InetSocketAddress(a, 443), 10000);
                return;
            }
        }
        throw new IOException("No public endpoint");
    };
    private final int endpoint, chunk;
    private final long timeoutMs;
    private final Executor executor;
    private final Events events;
    private final Connector connector;
    private final ArrayBlockingQueue<byte[]> input = new ArrayBlockingQueue<>(1);
    private final Semaphore credit = new Semaphore(0);
    private final AtomicBoolean awaiting = new AtomicBoolean(), receiving = new AtomicBoolean(),
                                closed = new AtomicBoolean();
    private final AtomicBoolean finished = new AtomicBoolean();
    private final Socket socket = new Socket();
    private volatile boolean connected;
    private volatile long writeStarted;
    SocketPipe(
        int endpoint, int chunk, long timeoutMs, Executor executor, Events events, Connector connector) {
        if (chunk < 1 || chunk > Protocol.PAYLOAD || timeoutMs < 1)
            throw new IllegalArgumentException();
        this.endpoint = endpoint;
        this.chunk = chunk;
        this.timeoutMs = timeoutMs;
        this.executor = executor;
        this.events = events;
        this.connector = connector;
    }
    void start() {
        try {
            executor.execute(this::read);
        } catch (RejectedExecutionException e) {
            finish();
        }
    }
    boolean accept(byte[] data) {
        if (!connected || closed.get() || data.length == 0 || data.length > Protocol.PAYLOAD
            || !receiving.compareAndSet(false, true))
            return false;
        if (input.offer(data.clone()))
            return true;
        receiving.set(false);
        return false;
    }
    boolean acknowledge() {
        if (closed.get() || !awaiting.compareAndSet(true, false))
            return false;
        credit.release();
        return true;
    }
    /** Called by the owner's periodic watchdog; closes a blocked socket write. */
    void tick() {
        if (writeStarted != 0 && elapsedMs() - writeStarted > timeoutMs)
            finish();
    }
    private static long elapsedMs() {
        return System.nanoTime() / 1000000;
    }
    private void read() {
        try {
            if (closed.get())
                return;
            connector.connect(socket, endpoint);
            if (closed.get())
                return;
            socket.setTcpNoDelay(true);
            socket.setSoTimeout(1000);
            connected = true;
            events.opened();
            executor.execute(this::write);
            InputStream in = socket.getInputStream();
            byte[] buffer = new byte[chunk];
            while (!closed.get()) {
                int n;
                try {
                    n = in.read(buffer);
                } catch (SocketTimeoutException e) {
                    continue;
                }
                if (n < 0)
                    break;
                awaiting.set(true);
                events.data(Arrays.copyOf(buffer, n));
                if (!credit.tryAcquire(timeoutMs, TimeUnit.MILLISECONDS))
                    throw new IOException("Credit timeout");
            }
        } catch (IOException | InterruptedException | RejectedExecutionException e) {
            // Deliberately do not expose socket exception strings or payloads.
        } finally {
            finish();
        }
    }
    private void write() {
        try {
            OutputStream out = socket.getOutputStream();
            while (!closed.get()) {
                byte[] data = input.poll(1, TimeUnit.SECONDS);
                if (data == null)
                    continue;
                writeStarted = elapsedMs();
                out.write(data);
                writeStarted = 0;
                receiving.set(false);
                events.consumed();
            }
        } catch (IOException | InterruptedException e) {
        } finally {
            finish();
        }
    }
    private void finish() {
        close();
        if (finished.compareAndSet(false, true))
            events.closed();
    }
    @Override
    public void close() {
        closed.set(true);
        credit.release();
        try {
            socket.close();
        } catch (IOException ignored) {
        }
    }
}
