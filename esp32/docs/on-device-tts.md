# Experimental on-device English speech

This fork adds `CONFIG_MUSE_ON_DEVICE_TTS` to the existing voice-note reply
path. Muse still receives voice notes over Wi-Fi and returns text; the gadget
phonemizes and synthesizes that text locally. No phone, TTS HTTP service, or
voice-realtime endpoint is involved in playback.

The engine is sanoTTS's **E12 nano** English model (294,642 parameters,
24 kHz), with its dictionary-first C phonemizer, primary dictionary only.
It is not the similarly named heart-nano model. Source revision and original
licenses are in `components/muse/local_tts/vendor/UPSTREAM.md`.

## Build

Use ESP-IDF 6.0.3 and your normal board profile. In that build directory's
ignored `sdkconfig`, enable `CONFIG_MUSE_ON_DEVICE_TTS=y`. For the tested
Waveshare ESP32-S3-Touch-AMOLED-1.75 profile, disable
`CONFIG_MUSE_REALTIME_VOICE` to use the voice-note interaction and save space.
Keep `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512` on this board to leave internal
RAM for the codec DMA buffers. The SDK token belongs only in ignored config.

The normal 4 MB app slots and all partition offsets are unchanged. Initial
build: 0x3e1000 bytes, 0x1f000 bytes spare. Check each build's size result.
An ordinary reflash preserves NVS pairing and Wi-Fi settings.

## Test

Send the single lowercase character `t` to the USB console while idle and
awake. It synthesizes: "Hello. My voice is generated on this device. No phone
or server is needed." This test does not send a message to Muse.

For end-to-end use, hold the microphone, speak, and release. With the speaker
on, the returned text is synthesized locally and uses the existing volume
and caption path. Muting skips synthesis. A new turn cancels the old worker;
if it has not stopped yet, the next reply falls back to captions.

Host tests:

```
python3 -m unittest discover -s tests -p test_muse_local_tts.py
```

These compile the actual dictionary and neural engine with address/undefined
behavior sanitizers, verify non-silent finite PCM, exercise callback abort,
and check phrase splitting at word and UTF-8 boundaries. They do not prove
speaker audibility or device timing. Hardware observations are recorded below
when available.

## Bounds and limitations

- English only. Unknown words can be omitted by the upstream phonemizer;
  names, code, URLs and other languages are not reliable speech inputs.
- Only the text already retained for captions is spoken (at most 1023 bytes).
  Longer answers remain available in the Muse app.
- Phrases are split at punctuation or word boundaries, at most 80 bytes.
- One persistent synthesis worker, 16 KB PSRAM stack, 96 KB arena and
  128 KB PCM ring plus a bounded 512 KB phrase buffer. Static neural/G2P scratch is placed in PSRAM.
- The arena uses internal RAM only when at least 48 KB would remain free;
  otherwise it uses PSRAM and the scalar kernels. This may be slower than
  playback. Complete phrases are buffered before playback to prevent
  gaps within words; there can still be a pause between phrases.
- Synthesis has a 150-second deadline checked at PCM callbacks; the existing
  chat turn cap also applies. Cancellation is cooperative at those callbacks.
- Failures before audio fall back to captions. A failure after partial speech
  can leave the remainder unspoken. This is an experiment, not a complete
  accessible read-aloud implementation.
- System tones and the MP3 test remain available. Realtime voice is a separate
  feature and its observed HTTP 404 is not fixed by this change.

## Third-party licensing

Upstream sanoTTS has mixed licensing: an explicit MIT runtime subset,
per-file notices, Apache-2.0 dictionary data, and GPL-3.0-or-later for files
outside the stated exceptions. All notices and license texts are preserved.
This optional combined experimental build includes GPL-covered source; the
fork supplies the corresponding source. The original Muse files retain their
Apache-2.0 notices. Do not assume all vendored files are MIT merely because
the core inference runtime is MIT.

## Hardware run (2026-11-04)

Waveshare ESP32-S3-Touch-AMOLED-1.75 (non-C), ESP-IDF 6.0.3:
booted, initialized audio, and reconnected to Muse after flash. USB `t` test
generated three phrases successfully, then delivered 101,206 PCM16 samples
at 16 kHz (6.33 seconds) to the speaker path. Phrase synthesis took 1.83,
4.73, and 4.31 seconds using the PSRAM scalar path; peak arena use 48,320 B.
The test completed without a reported synthesis failure or panic. This
verifies generation and driver delivery, not subjective audibility. A real
Muse reply and user listening confirmation are still needed.
