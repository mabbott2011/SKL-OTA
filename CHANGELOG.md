# Changelog

## Unreleased

- New `examples/Demo`: an end-to-end walkthrough with one sketch and five builds (healthy, healthy, fails its self-test, hangs in `setup()`, crashes in `setup()`), each blinking its build number. `demo.py` makes a throwaway key, builds and signs releases, and runs a small update server that can also tamper with the manifest, corrupt the download, or serve an old build. Compiled in CI.

## 1.1.0 (2026-10-03)

- Filesystem images (SPIFFS / LittleFS / FAT) shipped with a release: an optional `"fs"` block in the manifest, signed separately over `<context>-fs|<board>|<build>|<size>|<sha256>`.
- Written only after the new build passes its self-test, so a rolled-back build never touches the filesystem. Interrupted or failed writes are retried automatically (1, 2, 4… min, at most hourly). An unchanged image (same SHA-256) isn't rewritten.
- New: `onFilesystemUpdate()`, `filesystemPending()`, `SKLOtaConfig.updateFilesystem`, `SKLOtaRelease.fs`.
- A release whose filesystem image fails its signature or doesn't fit the partition isn't installed.
- The "report soon" flag survives restarts, so a server hears about an install even when the device restarts right after it.
- `ota_release.py sign --fs-bin` (and `verify` checks the filesystem image).
- Fix: a new build that **hangs** (stuck in `loop()` or `setup()` without resetting) is now rolled back. The self-test deadline used to be checked only inside `loop()`, and the Arduino loop watchdog is off by default, so a hung build stayed installed. An `esp_timer` now restarts the device 30 s after `selfTestTimeoutMs` if the build hasn't passed; a restart while pending makes the bootloader go back.
- New: `armRollbackGuard()`. Call it first in `setup()` so hangs during startup are covered; `begin()` arms the same timer otherwise.

## 1.0.0 (2026-10-03)

First release, moved out of the Gnode firmware where it shipped on 2026-10-02.

- Signed manifests: ECDSA P-256 over `<context>|<board>|<build>|<size>|<sha256>`, checked against a compiled-in public key. Signing can't be turned off.
- Newer builds only; releases for other boards are refused.
- Streams into the other app slot with a running SHA-256; switches slots only when it matches.
- Bootloader rollback: a new build must pass your `onSelfTest()` within `selfTestTimeoutMs`, or it's rolled back. The result survives reboots (`lastResult()`).
- Checks and installs run in their own task; log lines are delivered on `loop()`.
- Automatic checks (5 min after boot, then every 12 h); installs only on request.
- Compressed downloads: an optional zlib copy of the image (`compressed_url`), inflated as it arrives with the ESP32's ROM decompressor. It must still match the signed size and SHA-256. Falls back to the plain image if the compressed download fails before anything is written.
- `tools/ota_release.py`: `keygen`, `sign` (writes the compressed copy too; `--no-compress` to skip), `verify`.
- Host tests for the streaming decompressor (`test/host`).
