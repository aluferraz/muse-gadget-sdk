#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
sdk=${ANDROID_HOME:-$HOME/Library/Android/sdk}
bt=$(ls -d "$sdk"/build-tools/* | sort -V | tail -1)
jar="$sdk/platforms/android-36/android.jar"
if ! javac -version >/dev/null 2>&1; then
    export PATH="/Applications/Android Studio.app/Contents/jbr/Contents/Home/bin:$PATH"
fi
out=$(mktemp -d "${TMPDIR:-/tmp}/muse-relay-build.XXXXXX")
mkdir -p "$out/classes" "$out/dex"
javac -source 17 -target 17 -Xlint:-options -classpath "$jar" -d "$out/classes" "$here"/src/com/musegadgets/relay/*.java
"$bt/d8" --min-api 33 --lib "$jar" --output "$out/dex" "$out"/classes/com/musegadgets/relay/*.class
"$bt/aapt2" link -o "$out/base.apk" -I "$jar" --manifest "$here/AndroidManifest.xml" \
    --min-sdk-version 33 --target-sdk-version 34 --version-code 1 --version-name 0.1 --debug-mode
(cd "$out/dex" && zip -q "$out/base.apk" classes.dex)
"$bt/zipalign" -f -p 4 "$out/base.apk" "$out/aligned.apk"
"$bt/apksigner" sign --ks "$HOME/.android/debug.keystore" --ks-pass pass:android --out "$out/MuseRelay.apk" "$out/aligned.apk"
"$bt/apksigner" verify "$out/MuseRelay.apk"
printf '%s\n' "$out/MuseRelay.apk"
if [ "${1:-}" = install ]; then "$sdk/platform-tools/adb" install -r "$out/MuseRelay.apk"; fi
