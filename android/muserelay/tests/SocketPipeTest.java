// SPDX-License-Identifier: Apache-2.0
package com.musegadgets.relay;
import java.net.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

public final class SocketPipeTest {
    static void check(boolean condition) {
        if (!condition)
            throw new AssertionError();
    }
    static final class Events implements SocketPipe.Events {
        final CountDownLatch open = new CountDownLatch(1), close = new CountDownLatch(1);
        final BlockingQueue<byte[]> data = new LinkedBlockingQueue<>();
        final Semaphore consumed = new Semaphore(0);
        final AtomicInteger closes = new AtomicInteger();
        public void opened() {
            open.countDown();
        }
        public void data(byte[] bytes) {
            data.add(bytes);
        }
        public void consumed() {
            consumed.release();
        }
        public void closed() {
            closes.incrementAndGet();
            close.countDown();
        }
    }
    public static void main(String[] args) throws Exception {
        ExecutorService pool = Executors.newFixedThreadPool(4);
        try (ServerSocket listener = new ServerSocket(0, 1, InetAddress.getLoopbackAddress())) {
            Events e = new Events();
            SocketPipe pipe = new SocketPipe(1, 12, 1500, pool, e,
                (s, id)
                    -> s.connect(
                        new InetSocketAddress(InetAddress.getLoopbackAddress(), listener.getLocalPort())));
            check(!pipe.accept(new byte[] {1}));
            check(!pipe.acknowledge());
            pipe.start();
            try (Socket peer = listener.accept()) {
                peer.setSoTimeout(3000);
                check(e.open.await(3, TimeUnit.SECONDS));
                byte[] bytes = new byte[24];
                for (int i = 0; i < bytes.length; i++) bytes[i] = (byte) i;
                peer.getOutputStream().write(bytes);
                check(Arrays.equals(e.data.poll(3, TimeUnit.SECONDS), Arrays.copyOfRange(bytes, 0, 12)));
                check(e.data.poll(100, TimeUnit.MILLISECONDS) == null); // blocked on credit
                check(pipe.acknowledge());
                check(Arrays.equals(e.data.poll(3, TimeUnit.SECONDS), Arrays.copyOfRange(bytes, 12, 24)));
                check(pipe.acknowledge());
                check(!pipe.acknowledge());
                check(pipe.accept(bytes));
                check(!pipe.accept(new byte[181]));
                check(Arrays.equals(peer.getInputStream().readNBytes(24), bytes));
                check(e.consumed.tryAcquire(3, TimeUnit.SECONDS));
                peer.getOutputStream().write(7);
                check(e.data.poll(3, TimeUnit.SECONDS)[0] == 7);
                // Withhold ACK: bounded cleanup closes the socket and emits once.
                check(e.close.await(4, TimeUnit.SECONDS));
                check(e.closes.get() == 1);
                pipe.close();
                check(!pipe.accept(new byte[] {1}));
            }
            Events cancelled = new Events();
            SocketPipe closed = new SocketPipe(1, 180, 1000, pool, cancelled,
                (s, id) -> { throw new AssertionError("closed pipe connected"); });
            closed.close();
            closed.start();
            check(cancelled.close.await(3, TimeUnit.SECONDS));
            check(cancelled.closes.get() == 1);
            Events failed = new Events();
            SocketPipe rejected = new SocketPipe(
                1, 180, 1000, r -> { throw new RejectedExecutionException(); }, failed, (s, id) -> {});
            rejected.start();
            check(failed.close.await(1, TimeUnit.SECONDS));
        } finally {
            pool.shutdownNow();
            check(pool.awaitTermination(5, TimeUnit.SECONDS));
        }
        System.out.println("Socket flow control, timeout, close and executor rejection passed");
    }
}
