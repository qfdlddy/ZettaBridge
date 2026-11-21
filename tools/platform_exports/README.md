# Platform export baselines

Arm32 plugins are linked with `BIND_NOW`: one strong undefined symbol that the platform library
provides and our guest stub library does not stops the whole library from loading. The only
symptom on the device is `UnsatisfiedLinkError: JNI_ERR returned from JNI_OnLoad in
.../proxy/libX.so`, which says nothing about the missing name.

This directory records, per library, what the Android platform exports, so
`tools/check_platform_exports.py` can fail the build when the gap grows.

| File | Meaning |
|---|---|
| `<lib>.baseline.txt` | Every dynamic symbol the platform's `<lib>.so` exports |
| `<lib>.gap.txt` | The subset our arm32 guest stub does not export (a known gap) |

The check runs from `tools/build_guest.sh` after the guest libraries are built, and as
`ctest platform_exports_check`. It fails when a baseline name is neither exported nor listed in
the gap, when a gap entry is exported now (stale), and when a gap entry is not a platform name at
all.

## Provenance

Captured 2026-11-20 from the Android 15 (API 35) emulator image. Symbol names are independent of
the CPU ABI, so an x86_64 image is a valid name baseline for the arm32 guest.

| Source | SHA-256 |
|---|---|
| `libGLESv1_CM.so` | `be9f997b200498de81b9599d0ee9384924e2b817ad863865659e4ec178b571da` |
| `libGLESv2.so` | `6aa319017cf9da7201096ea89b3bd3300cf436493ebc54857244b3bb10a3fb2a` |
| `libOpenSLES.so` | `b7a7cd23abe326be508f5be8e6037ef8c960bf5c34ca645362fd185945661339` |

Regenerate the baseline files from a new image only deliberately, and re-review the gap: a
platform update that adds entry points is a new set of names a future plugin may import.

## What the gaps mean today

- `libGLESv1_CM.gap.txt` is mostly the GLES 1 fixed-function pipeline (`glMatrixMode`,
  `glVertexPointer`, `glOrthof`, ...) plus the OES entry points that `libGLESv2.so` serves
  instead. A guest that links only `libGLESv1_CM` cannot resolve those OES names yet; a guest
  that also links `libGLESv2` (Cocos2D-x does) can.
- `libGLESv2.gap.txt` is the GLES 3.x and extension surface the bridge does not implement.
- `libOpenSLES.gap.txt` holds `slQueryNumSupportedEngineInterfaces` and
  `slQuerySupportedEngineInterfaces`, which no guest we have seen imports.
