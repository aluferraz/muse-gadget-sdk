# Android phone relay proposal

Status: design investigation; no relay firmware or Android relay is implemented.
Branch: `experiment/phone-relay`, based on the working local-speech branch.

## Goal

Use the gadget away from a Wi-Fi network:

```text
ESP32-S3 <-- Bluetooth LE --> Android companion <-- cellular --> Muse
```

The phone still needs internet access. Speech synthesis continues on the
gadget. A phone hotspot is an immediate alternative, but still uses the
gadget's Wi-Fi radio and does not meet the Bluetooth-only goal.

The S3 supports Bluetooth LE, not Bluetooth Classic. Android's ordinary
Bluetooth tethering is not a transport we can simply enable on this board.
This requires an application relay, with firmware on one side and a companion
app on the other.

## Findings in this checkout

- `components/muse/muse_chat_session.cpp` directly uses ESP-TLS, upgrades to
  WebSocket, then performs the Noise handshake. Its connection admission,
  idle loop and readiness checks explicitly depend on `muse_wifi_connected()`.
- `main/noise_control.cpp` owns a separate control connection. Relaying chat
  alone is insufficient to preserve the normal device behavior.
- VM lookup and credential refresh also need internet access. A prototype
  that works only with a cached VM token does not prove independence from Wi-Fi.
- `main/ble_server.c` has setup RX/TX characteristics, fragmentation and
  notifications. These are setup protocol facilities, not an internet relay.
  Relay traffic must use a separate service and leave pairing compatible.
- `android/musegadgetstest` is a camera/video sample using local Wi-Fi.
  Its manifest and transport do not provide a Bluetooth/cellular relay.

## Proposed transport boundary

Prototype an ordered, bounded byte-stream relay beneath TLS. The Android app
opens an outbound TCP connection to an approved Muse endpoint and transfers
bytes between that socket and BLE. TLS certificate verification, account
credentials, WebSocket and Noise remain on the gadget. The app need not copy
pairing tokens or implement the Muse application protocol.

This is a design choice to validate, not an existing ESP-TLS feature switch.
Investigate a relay-backed lwIP network interface versus a custom TLS BIO
adapter. The network-interface approach may preserve more HTTP/TLS code but
adds routing and encapsulation complexity. A custom adapter must cover both
ESP-TLS sessions and account HTTP requests, including readiness and polling;
replacing only `read` and `write` is insufficient. Select the integration
after a minimal TLS connection works over BLE.

The initial relay protocol needs:

- Explicit open, data, credit, close and error messages with a version,
  connection identifier and connection generation.
- MTU-aware fragmentation, bounded reassembly and per-stream flow control.
  BLE acceptance is not evidence that the remote socket consumed the data.
- Strict ordering and separate control/chat streams, with bounded queues
  and deadlines. Slow receivers must backpressure, not accumulate audio.
- Disconnect cleanup that closes affected sockets and discards old queued
  bytes. Reconnect establishes fresh TLS/Noise sessions; do not replay
  partially delivered chat submissions automatically.
- Explicit user association and authenticated BLE bonding, with a way to
  revoke the relay phone. Forward only approved destinations and ports;
  reject private/local destinations and revalidate DNS results.
- Metadata-only diagnostics. Never log tokens, message bodies or PCM.

Keep direct Wi-Fi available as a selectable transport while developing.
Changing transports closes sessions cleanly; readiness means usable internet
through the selected transport, not merely an active Bluetooth connection.
Phone-relay mode must be demonstrably usable with the Wi-Fi radio stopped.

## Android lifecycle

Start with a user-started `connectedDevice` foreground service and visible
connection status. Request the required Bluetooth runtime permissions and
associate the selected gadget. Evaluate CompanionDeviceManager and
CompanionDeviceService for reconnection and presence handling. Follow the
platform's foreground-service launch restrictions rather than assuming the
process remains alive indefinitely.

Use Android's current network for outbound sockets; cellular must work with
the phone's Wi-Fi disabled. Handle network changes, lost internet, Bluetooth
disconnects, process death and user stop explicitly. Do not promise seamless
background operation before testing the actual phone and Android version.

## Milestones and acceptance

1. BLE transport harness: malformed fragments, minimum/negotiated MTU,
   exhausted credits, slow consumer, stale generations and repeated reconnects.
2. Android foreground relay and one certificate-verified TLS request from
   the gadget with its Wi-Fi stopped. Measure throughput, latency and memory.
3. VM lookup/refresh, control registration, chat subscription and a typed
   reply through the relay. Verify credential refresh without Wi-Fi.
4. Short voice-note upload, reply captions and existing on-device speech.
   Measure upload time before choosing compression or claiming realtime audio.
5. Phone screen off, Bluetooth range loss, cellular changes, app restart and
   revoked association. Verify bounded recovery and no duplicate submissions.

Do not include realtime voice admission, home-network tunneling, OTA or phone
TTS in the first milestone. Each adds separate requirements. Retain the
known-working deployed firmware until the transport prototype is validated.

## Platform references

- [ESP32-S3 Bluetooth support](https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32s3/api-guides/bluetooth.html)
- [Android BLE background communication](https://developer.android.com/develop/connectivity/bluetooth/ble/background)
- [Android companion association](https://developer.android.com/develop/connectivity/bluetooth/companion-device-pairing)
