# Vendored local speech engine

Source: https://github.com/Ampixa/sanoTTS
Revision: `87282046baca9db01af80103a981524ae45dcf18`
Source directory: `esphome/components/sanotts/`

The C/C++ files, headers and embedded model arrays are copied unchanged.
The voice is **E12 nano**, not heart-nano. The build selects the primary
English dictionary with `NANO_LEX_WITH_SILVER=0`.

`manifest.json` pins the complete file inventory and SHA-256 digests, including
model arrays and license files. Run from `esp32/`:

```
python3 tools/muse/check_local_tts_vendor.py
```

For an update, review the upstream revision and licensing first, copy only the
explicit source list used by `components/muse/CMakeLists.txt`, update the
manifest, run the sanitizer tests and both firmware configurations, and repeat
the device timing/playback checks. Do not edit or reformat vendored files to
silence warnings. The model arrays must stay paired with `nano_q8_meta.h` from
the same revision and lineage.

## Notices

- `LICENSE` and `LICENSE.MIT` are copied from the upstream repository root.
- `DICTIONARY-NOTICE.md` is upstream `pypkg/sanotts/g2p_data/NOTICE.md`.
- Per-file license declarations are preserved.
- The upstream license split is not a blanket MIT grant: files outside its
  explicit permissive exceptions retain GPL-3.0-or-later terms. The optional
  combined experimental build includes GPL-covered source.
