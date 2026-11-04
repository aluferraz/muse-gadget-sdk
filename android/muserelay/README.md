# Muse Relay for Android

Experimental companion for the ESP32-S3 Bluetooth internet relay. The gadget
keeps its Muse credentials, TLS verification, Noise sessions, microphone,
screen and local speech. The phone forwards encrypted socket bytes using
its current internet connection. No Muse login or token is entered here.

Requires Android 13 or later and a gadget built with `CONFIG_MUSE_PHONE_RELAY`.
The app is separate from the Muse app and the repository's camera sample.

## Build and install

On macOS with the Android SDK (platform 36, build tools), a JDK 17 or newer,
and an existing Android debug signing key at `~/.android/debug.keystore`:

```sh
./build.sh
./build.sh install
```

`ANDROID_HOME` overrides `~/Library/Android/sdk`. Android Studio's bundled
JDK is used if the system has no working `javac`. Each build uses a fresh
output directory and prints the signed APK path. Signing credentials and
APKs are not stored in the repository. This is a locally debug-signed test
app, not a production distribution build.

For a USB install, enable Developer options and USB debugging on the phone,
connect it, and accept the computer's authorization prompt. `adb devices`
must show `device`, not `unauthorized`.

## Connect

1. Pair the gadget to Muse using the ordinary firmware first. Preserve that
   pairing when flashing the relay build described in
   [`../../esp32/docs/phone-relay.md`](../../esp32/docs/phone-relay.md).
2. Open **Muse Relay**, tap **Find gadget**, and grant nearby-device access.
   If this is the first permission grant, tap **Find gadget** again.
3. Select the gadget. Enter the passkey displayed on its screen when Android
   asks to pair. A previous bond can be reused.
4. Keep the foreground-service notification active. The app reports relay
   connection status; the gadget separately establishes its Muse sessions.
5. For the cellular test, turn off the phone's Wi-Fi and keep mobile data on.
   The relay firmware never starts the gadget's Wi-Fi radio.
6. Tap **Disconnect** to stop the service. This closes the relay sockets.

After five failed reconnect attempts, select the gadget again. This first
prototype does not auto-start at boot or use CompanionDeviceService. Android
process termination, radio changes and manufacturer power management still
need validation on the physical phone. Do not infer background reliability
from the host tests.

## Tests

From the repository root:

```sh
python3 -m unittest discover -s esp32/tests -p test_phone_relay.py -v
```

Tests need C and Java compilers and permission to bind local loopback
sockets. They compare C/Java wire parsing, test destination restrictions,
and exercise the production socket carrier and firmware worker for credits,
timeouts, disconnects and stale generations. They do not emulate Android's
Bluetooth stack or prove a cellular connection to Muse.

Phone-generated TTS is not implemented in this prototype.
