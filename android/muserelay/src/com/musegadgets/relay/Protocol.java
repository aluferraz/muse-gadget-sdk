// SPDX-License-Identifier: Apache-2.0
package com.musegadgets.relay;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.UUID;

final class Protocol {
    static final int HEADER = 8, PAYLOAD = 180, STREAMS = 3;
    static final int HELLO = 1, READY = 2, OPEN = 3, OPENED = 4, DATA = 5, ACK = 6, CLOSE = 7;
    static final UUID SERVICE = UUID.fromString("9e390001-7a46-4c8e-a950-1a00c0de0001");
    static final UUID RX = UUID.fromString("9e390002-7a46-4c8e-a950-1a00c0de0001");
    static final UUID TX = UUID.fromString("9e390003-7a46-4c8e-a950-1a00c0de0001");
    static final UUID CCC = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    final int type, id, epoch;
    final byte[] data;
    Protocol(int type, int id, int epoch, byte[] data) {
        this.type = type;
        this.id = id;
        this.epoch = epoch;
        this.data = data.clone();
    }
    byte[] encode() {
        ByteBuffer b = ByteBuffer.allocate(HEADER + data.length).order(ByteOrder.LITTLE_ENDIAN);
        b.put((byte) 1).put((byte) type).putShort((short) id).putInt(epoch).put(data);
        byte[] out = b.array();
        parse(out);
        return out;
    }
    static Protocol parse(byte[] p) {
        if (p == null || p.length < HEADER || p.length > HEADER + PAYLOAD || p[0] != 1)
            throw new IllegalArgumentException("Frame size/version");
        ByteBuffer b = ByteBuffer.wrap(p).order(ByteOrder.LITTLE_ENDIAN);
        int type = p[1] & 255, id = b.getShort(2) & 65535, epoch = b.getInt(4), len = p.length - HEADER;
        if (epoch == 0 || type < HELLO || type > CLOSE)
            throw new IllegalArgumentException("Frame type/epoch");
        if (type == HELLO || type == READY) {
            if (id != 0 || len != 0)
                throw new IllegalArgumentException("Control frame");
        } else {
            if (id == 0)
                throw new IllegalArgumentException("Stream ID");
            if (type == OPEN) {
                if (len != 1 || p[8] < 1 || p[8] > 2)
                    throw new IllegalArgumentException("Endpoint");
            } else if (type == DATA) {
                if (len == 0)
                    throw new IllegalArgumentException("Empty data");
            } else if (len != 0)
                throw new IllegalArgumentException("Unexpected payload");
        }
        return new Protocol(type, id, epoch, Arrays.copyOfRange(p, HEADER, p.length));
    }
    static String host(int endpoint) {
        if (endpoint == 1)
            return "api.muse.ai";
        if (endpoint == 2)
            return "hatch.metaaivm.com";
        throw new IllegalArgumentException("Endpoint");
    }
}
