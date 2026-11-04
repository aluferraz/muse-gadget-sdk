# Experimental Android Bluetooth relay

Branch: `experiment/phone-relay`. The initial firmware and Android companion
are implemented and host-tested. The relay image also passed a board startup
check. Android-to-gadget interoperability and cellular Muse access still
require a phone run.

```text
ESP32-S3 <-- authenticated Bluetooth LE --> Android <-- cellular --> Muse
     TLS, credentials and Noise remain on the gadget
```

The phone needs internet. A hotspot remains an alternative for the normal
firmware, but this experimental build does not start the gadget's Wi-Fi radio.
The S3 supports BLE only; this is an application relay, not Classic Bluetooth
internet tethering. Existing local speech remains available.

## Build and rollback

Use an already Muse-paired Waveshare ESP32-S3-Touch-AMOLED-1.75 and ESP-IDF
6.0.3. Create a separate build directory, copy the working ignored
`sdkconfig` into it, and set:

```text
CONFIG_HOMEHUB_TUNNEL=n
CONFIG_MUSE_PHONE_RELAY=y
CONFIG_LWIP_NETIF_LOOPBACK=y
CONFIG_LWIP_MAX_SOCKETS=16
```

Keep the existing board, PSRAM, SDK token and local-TTS settings. Configure
with the same board defaults and the new build's explicit SDKCONFIG path:

```sh
idf.py -B build-muse-waveshare-s3-175-relay \
  -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-muse-waveshare-s3-175-relay/sdkconfig \
  '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-175' build
```

Check that the resulting sdkconfig actually enables `MUSE_PHONE_RELAY`.
The Kconfig option requires the home-network tunnel to be disabled. Use the
normal flash recipe from that build directory, without erasing NVS. Install
[`android/muserelay`](../../android/muserelay/README.md) on the phone.

To roll back, flash the separately retained normal Wi-Fi build. Partition
offsets and stored Muse/Wi-Fi credentials are unchanged. Relay mode is a
build-time choice in this prototype, not a runtime transport toggle.

## Integration

`main/phone_relay/phone_relay.c` registers a separate authenticated BLE GATT
service alongside existing setup and companion services. It reuses the
existing passkey display and authenticated bonding configuration. Setup's
RX/TX characteristics and protocol are unchanged.

The relay worker owns two loopback listeners and at most three active
socket streams. `socket_routes.c` uses link-time adapters for
`lwip_getaddrinfo` and `lwip_connect`: exactly `api.muse.ai` and
`hatch.metaaivm.com`, port 443, resolve to synthetic endpoint addresses and
connect to the matching loopback listener. Other destinations fail closed.
The TLS caller still receives its original hostname for certificate checks
and SNI. ESP-TLS, account HTTP, WebSocket and Noise code retain their existing
protocols and TLS policy; this change does not disable certificate checks.

Android receives only endpoint IDs and encrypted socket bytes, connects to
one of the same two fixed hosts, and rejects private/reserved DNS answers.
It does not terminate TLS or hold gadget account credentials. The phone can
observe traffic sizes and timing and can interrupt service.

`muse_network_ready()` separates transport availability from physical Wi-Fi
status. Relay availability gates chat admission, control startup and account
credential maintenance. Android checks for a validated internet connection
before relay admission; successful Muse access is established separately by
the gadget's existing TLS and session handshakes. Relay readiness does not
prove the server is reachable. Wi-Fi joins/scans are disabled in this build;
the normal build retains its existing behavior.

## Wire protocol v1

Service UUID: `9e390001-7a46-4c8e-a950-1a00c0de0001`.
RX (phone writes): `9e390002-7a46-4c8e-a950-1a00c0de0001`.
TX (gadget notifies): `9e390003-7a46-4c8e-a950-1a00c0de0001`.

Every GATT value is one complete frame. Its eight-byte header is:

| Offset | Size | Meaning |
|---|---|---|
| 0 | 1 | Version, currently 1 |
| 1 | 1 | Type |
| 2 | 2 | Stream ID, unsigned little endian |
| 4 | 4 | Nonzero connection epoch, little endian |

Payload is at most `min(180, ATT_MTU - 3 - 8)` bytes. Default MTU 23 works
with 12-byte data chunks; Android requests a larger MTU. This is deliberately
conservative stop-and-wait flow control, not a throughput-optimized protocol.

| Type | Direction | Meaning |
|---|---|---|
| 1 HELLO | Phone to gadget | Stream 0, fresh random epoch, empty payload |
| 2 READY | Gadget to phone | Echo epoch, stream 0, empty payload |
| 3 OPEN | Gadget to phone | New stream, one endpoint byte: 1 API, 2 VM |
| 4 OPENED | Phone to gadget | TCP connected, empty payload |
| 5 DATA | Either | Nonempty socket bytes |
| 6 ACK | Either | Previous DATA consumed by the local socket |
| 7 CLOSE | Either | Close stream, empty payload |

There is one outstanding DATA per stream per direction. ACK grants the next
packet; GATT delivery alone does not grant credit. Phone writes use ATT write
responses and a single serialized queue. TCP handles fragmentation above the
carrier, so TLS record boundaries need not match BLE chunks.

A disconnect resets streams and queued bytes. Epochs fence old application
frames; firmware additionally tags queued writes with a physical-connection
generation to handle reused BLE handles. Stream IDs are never reused in an
epoch. Reconnecting starts new TLS/Noise sessions and does not replay partial
submissions. The existing application decides what to do with its voice-note
backlog.

## Bounds and failure behavior

- Three streams, two listeners, at most one 180-byte receive chunk per stream.
- Firmware ingress: 32 bounded frames. Android GATT writes: 32 pending frames.
- Android socket workers: six threads and six pending tasks; each stream has
  one reader and one writer, with one queued inbound chunk.
- Firmware open, stalled socket delivery, notification admission and credit
  waits: 30 seconds. Android socket connect: 10 seconds; credit and blocked
  socket-write waits: 30 seconds. Bluetooth write: 15 seconds; admission:
  30 seconds. Existing TLS/application deadlines can expire earlier.
- Invalid frames and unexpected stream transitions fail the request or
  connection. Queue exhaustion fails closed. No message bodies, audio or
  tokens are logged by the relay.
- Android uses a user-started `connectedDevice` foreground service and five
  bounded reconnect attempts. Process death and phone restart require opening
  the app again. Background continuity is not yet established.

## Validation and remaining work

Host checks exercise the actual firmware worker with fake BLE callbacks and
real loopback sockets, plus the production Java socket carrier. They cover
unauthenticated input, default-MTU chunking, credit backpressure, timeouts,
stale epochs, disconnect/reconnect, rejected workers, C/Java parser agreement
and confined socket routing. Android APK and both relay/normal firmware must
build before a device trial.

Validation on 2026-11-04:

- Full host suite: 1,068 tests run, no failures, one optional power-profiler
  dependency skip. Final targeted relay suite: five passing tests, including
  an additional firmware-worker test added after the full run started.
- Android APK builds and signature verification passes.
- Both Waveshare 1.75 firmware variants build on ESP-IDF 6.0.3; each image is
  0x3e1000 bytes, within the existing 4 MB slot.
- Relay image flashed for a startup check: reached `Muse Gadget starting`,
  advertised the device, reported `Wi-Fi radio stopped`, and reached UI ready
  without a panic or reboot during the 20-second capture. No Android phone
  was connected, so no remote TLS/Noise or audio transfer was exercised.
- Restored the normal Wi-Fi/local-speech build afterward; boot logs confirmed
  control registration and the chat connection to Muse. Pairing was retained.

Required hardware acceptance: pairing passkey; control registration and chat
with both Wi-Fi radios off; short voice-note upload and local spoken reply;
account refresh; Bluetooth range loss; phone screen off; cellular handover;
app stop/restart; observed throughput and memory. Do not claim these pass
until measured on the actual phone.

Custom Muse endpoints, home-network tunneling, OTA, realtime voice and
phone-generated TTS are outside this initial prototype. DNS/connect adapters
must be rechecked when upgrading ESP-IDF; they target its lwIP socket path.

## Platform references

- [ESP32-S3 Bluetooth support](https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32s3/api-guides/bluetooth.html)
- [Android BLE background communication](https://developer.android.com/develop/connectivity/bluetooth/ble/background)
- [Android companion association](https://developer.android.com/develop/connectivity/bluetooth/companion-device-pairing)
