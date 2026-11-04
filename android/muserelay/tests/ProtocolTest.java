// SPDX-License-Identifier: Apache-2.0
package com.musegadgets.relay;
import java.net.InetAddress;
import java.util.*;
public final class ProtocolTest {
    static void check(boolean condition) {
        if (!condition)
            throw new AssertionError();
    }
    static boolean valid(byte[] p) {
        try {
            Protocol.parse(p);
            return true;
        } catch (IllegalArgumentException e) {
            return false;
        }
    }
    public static void main(String[] args) throws Exception {
        if (args.length > 0 && args[0].equals("corpus")) {
            Scanner in = new Scanner(System.in);
            while (in.hasNextLine())
                System.out.println(valid(HexFormat.of().parseHex(in.nextLine())) ? 1 : 0);
            return;
        }
        for (int type = 1; type <= 7; type++) {
            int id = type <= 2 ? 0 : 65535;
            byte[] data = type == 3 ? new byte[] {2} : type == 5 ? new byte[180] : new byte[0];
            Protocol p = new Protocol(type, id, 0xdeadbeef, data), q = Protocol.parse(p.encode());
            check(q.type == type && q.id == id && q.epoch == p.epoch && Arrays.equals(q.data, data));
        }
        check(!valid(new byte[0]));
        check(!valid(new byte[189]));
        check(Protocol.host(1).equals("api.muse.ai"));
        check(Protocol.host(2).equals("hatch.metaaivm.com"));
        for (String host : new String[] {"0.0.0.0", "127.0.0.1", "10.1.2.3", "192.168.1.2", "172.16.1.1",
                 "169.254.1.2", "100.64.0.1", "198.18.0.1", "192.0.2.1", "198.51.100.1", "203.0.113.1",
                 "224.1.2.3", "255.255.255.255", "::1", "fc00::1", "fe80::1", "2001:db8::1"})
            check(!PublicEndpoint.allowed(InetAddress.getByName(host)));
        check(PublicEndpoint.allowed(InetAddress.getByName("1.1.1.1")));
        check(PublicEndpoint.allowed(InetAddress.getByName("2606:4700:4700::1111")));
        System.out.println("Protocol roundtrips and endpoint restrictions passed");
    }
}
