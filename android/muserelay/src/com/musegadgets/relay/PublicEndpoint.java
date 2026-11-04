// SPDX-License-Identifier: Apache-2.0
package com.musegadgets.relay;
import java.net.InetAddress;

final class PublicEndpoint {
    static boolean allowed(InetAddress a) {
        if (a.isAnyLocalAddress() || a.isLoopbackAddress() || a.isLinkLocalAddress() || a.isSiteLocalAddress()
            || a.isMulticastAddress())
            return false;
        byte[] b = a.getAddress();
        if (b.length == 4) {
            int x = b[0] & 255, y = b[1] & 255, z = b[2] & 255;
            return x != 0 && x != 127 && x < 224 && !(x == 100 && y >= 64 && y <= 127)
                && !(x == 169 && y == 254) && !(x == 192 && (y == 0 || y == 168 || (y == 88 && z == 99)))
                && !(x == 198 && (y == 18 || y == 19 || (y == 51 && z == 100)))
                && !(x == 203 && y == 0 && z == 113);
        }
        // Global unicast only, excluding the documentation prefix.
        return b.length == 16 && (b[0] & 0xe0) == 0x20
            && !((b[0] & 255) == 0x20 && (b[1] & 255) == 1 && (b[2] & 255) == 0x0d && (b[3] & 255) == 0xb8);
    }
}
