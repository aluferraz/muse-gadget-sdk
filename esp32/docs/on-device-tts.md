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

**Settings > Sound > Speak replies** toggles local speech, independently of
speaker volume and system sounds. It defaults on, survives restarts, and
applies to the next reply. Off keeps the existing text/caption behavior.

For end-to-end use, hold the microphone, speak, and release. With the speaker
on, the returned text is synthesized locally and uses the existing volume
and caption path. Muting skips synthesis. A new turn cancels the old worker;
if it has not stopped yet, the next reply falls back to captions.

Host tests:

```
python3 tools/muse/check_local_tts_vendor.py
python3 -m unittest discover -s tests -p 'test_muse_local_tts*.py'
```

The tests compile the real dictionary and neural engine, plus the production
worker against deterministic platform fakes, with address/undefined-behavior
sanitizers. They cover input bounds, malformed UTF-8, PCM chunk equivalence,
non-finite samples, allocation failures, task creation failure, cancellation
before and during synthesis, stalled consumers, deadline expiry, concurrent
admission, request isolation, repeated cleanup, and the reply/caption path.
The saved toggle is covered by `test_muse_ptt_ui.py`. Vendored file checksums
are verified against the pinned manifest. Host tests do not establish speaker
audibility, device scheduling, or power consumption.

## Ownership and failure handling

`muse_local_tts_start()` returns an opaque request handle, or NULL if the
worker is busy, the input is invalid, or resources cannot be allocated. The
consumer owns one reference and serializes reads, status queries and release.
The worker owns the other reference. Release signals cancellation and returns
without waiting; resources disappear when the last reference is released.
The worker and its notification slot are persistent, but request buffers are
freed after use. No producer resets a buffer another request can still read.

The chat session and diagnostic test keep distinct handles. A cancelled
request cannot cancel a subsequent request or contribute audio to it. One
admission gate serializes the upstream engine's static scratch and lazy task
creation. Terminal status is published after the last PCM write.

If synthesis fails after some speech has played, caption pacing restarts for
the full retained answer after queued audio. This preserves the text rather
than silently treating a partial spoken answer as complete. The phonemizer's
unknown-word and dropped-symbol counters are checked: a phrase with omitted
content falls back to text instead of knowingly speaking a changed answer.
No reply text is written to diagnostic logs.

## Bounds and limitations

- English only. Names, code, URLs and other languages may trigger caption
  fallback. The dictionary does not provide a general pronunciation model.
- Only the text already retained for captions is spoken (at most 1023 bytes).
  Longer answers remain available in the Muse app.
- Phrases are split at punctuation or word boundaries, at most 80 bytes.
- One persistent worker with a 16 KB PSRAM stack; each live request owns a
  128 KB PCM ring and at most 1023 bytes of text. The active worker holds a
  96 KB arena and a bounded 512 KB phrase buffer. Neural/G2P static scratch
  is placed in PSRAM. Retaining a finished handle retains its ring; consumers
  release it promptly after draining.
- The arena uses internal RAM only when at least 48 KB would remain free;
  otherwise it uses PSRAM and the scalar kernels. This may be slower than
  playback. Complete phrases are buffered before playback to prevent
  gaps within words; there can still be a pause between phrases.
- Each request has a 150-second deadline checked before phrases, at PCM
  callbacks and during bounded ring writes. The chat turn cap also applies.
  Cancellation is cooperative; it does not preempt an in-progress phonemizer
  or engine call before its next callback.
- Failures fall back to full retained captions, including after partial speech.
  This remains an experiment, not a complete accessible read-aloud implementation.
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
verifies generation and driver delivery, not subjective audibility. The user subsequently confirmed that spoken replies work.
These measurements describe the earlier prototype. The ownership/failure-path
review has been built and host-tested but has not been flashed or remeasured.

## Review validation

The enabled and disabled Waveshare 1.75 builds pass on ESP-IDF 6.0.3. All
28 focused local-TTS tests pass; the native harnesses use AddressSanitizer
and UndefinedBehaviorSanitizer. The full ESP32 host suite ran 1,060 tests
successfully with one skip for the optional `ppk2-api==0.9.2` benchmark
dependency. Four additional focused cases were added and passed separately
after that full-suite run started. The macOS run used Ninja, a pinned FreeInk checkout,
and host compiler flag adaptations for Darwin. The enabled build remains
0x3e1000 bytes, leaving 0x1f000 bytes in the existing 4 MB slot. The working
device firmware was left unchanged during this source review.
