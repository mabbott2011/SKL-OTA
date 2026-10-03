# Changelog

## 1.0.0 (2026-10-03)

First release, moved out of the Gnode firmware where it shipped on 2026-10-02.

- Signed manifests: ECDSA P-256 over `<context>|<board>|<build>|<size>|<sha256>`, checked against a compiled-in public key. Signing can't be turned off.
- Newer builds only; releases for other boards are refused.
- Streams into the other app slot with a running SHA-256; switches slots only when it matches.
- Bootloader rollback: a new build must pass your `onSelfTest()` within `selfTestTimeoutMs`, or it's rolled back. The result survives reboots (`lastResult()`).
- Checks and installs run in their own task; log lines are delivered on `loop()`.
- Automatic checks (5 min after boot, then every 12 h); installs only on request.
- `tools/ota_release.py`: `keygen`, `sign`, `verify`.
