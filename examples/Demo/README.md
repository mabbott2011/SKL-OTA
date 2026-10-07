# SKL-OTA demo: watch every safety check happen

This demo runs entirely on your desk: one ESP32, your PC as the update server, and your own Wi-Fi. In about 20 minutes you'll see a good update install, then watch the device refuse or undo six bad ones:

| # | What happens | What saves the device |
|---|---|---|
| 1 | A good update (build 1 → 2) | Nothing needed: it installs, passes its self-test, and stays |
| 2 | Someone edits the manifest in transit | The signature no longer matches, so nothing downloads |
| 3 | The download gets damaged in transit | The SHA-256 doesn't match, so the old build keeps running |
| 4 | A correctly signed build that's broken | It fails its self-test, and the bootloader puts build 2 back |
| 5 | A correctly signed build that hangs | The rollback timer fires even though `loop()` never runs |
| 6 | A correctly signed build that crashes on boot | The crash resets the chip while the build is on probation, so the bootloader goes straight back |
| 7 | Someone serves an old, validly signed build | Only newer builds install, so it's ignored |

All five builds come from the same sketch, `Demo.ino`. Each one **blinks the LED its build number of times**, then pauses, so you can tell which one is running without a serial cable.

| Build | Behavior |
|---|---|
| 1 | Healthy. The one you flash over USB to start. |
| 2 | Healthy. The "good update". |
| 3 | Installs fine, then **fails its self-test** ("pretend the light sensor is missing"). |
| 4 | **Hangs in `setup()`**, like a sensor driver stuck on I2C. LED stays solid on. |
| 5 | **Crashes in `setup()`**: reads through a null pointer, so the chip panics and resets. |

The timers are shortened for the demo: a new build must stay up 10 s and pass its self-test within 60 s. A real product should keep the defaults (1 min / 10 min).

## You need

- An ESP32 dev board (the classic `esp32dev` is what's configured) and a USB cable
- [PlatformIO](https://platformio.org/install) (the VS Code extension is fine), or the Arduino IDE (see [the end](#using-the-arduino-ide-instead))
- Python 3 with `pip install cryptography`
- Your PC and the ESP32 on the same Wi-Fi network

Run every command below from this folder (`examples/Demo`). The Serial Monitor output shown is trimmed to the interesting lines.

## Setup (once)

**1. Make a throwaway demo key.**

```sh
python demo.py keygen
```

This writes `demo-key.pem` (the private key) and `ota_pubkey.h` (the public key the firmware trusts), both here. They're git-ignored and only for this demo. **Don't use your real product key for the demo**, and don't use this one for anything else.

**2. Start the update server** in its own terminal and leave it running:

```sh
python demo.py serve
```

It prints the manifest URL to use, something like `http://192.168.1.50:8000/manifest.json`. Windows may ask whether to let Python through the firewall: allow it on private networks.

**3. Tell the sketch your Wi-Fi and that URL.** Create `demo_settings.h` next to `Demo.ino` (git-ignored, so your password never ends up in the repo):

```cpp
#define WIFI_SSID    "your-network"
#define WIFI_PASS    "your-password"
#define MANIFEST_URL "http://192.168.1.50:8000/manifest.json"
```

**4. Flash build 1 over USB.**

```sh
python demo.py flash 1
```

This builds, uploads and opens the Serial Monitor. You should see:

```
=== SKL-OTA demo: build 1 (healthy) ===
WiFi connected, IP 192.168.1.73
```

The LED blinks once, pauses, blinks once. Type `status` at any time to see what's running. (Type `help` for the other commands.)

## 1. A good update

In a second terminal, build and sign build 2:

```sh
python demo.py release 2
```

That puts `firmware-2.bin`, a compressed copy and a signed `manifest.json` in `releases/2/`. The server already offers it (it always offers the newest release). In the Serial Monitor, type `update`:

```
OTA: checking -- Checking...
[OTA INFO] Update available: build 2 (demo-2), 934656 bytes, signature OK
Installing... 10%
...
Installing... 90%
=== SKL-OTA demo: build 2 (healthy) ===
>>> New build on probation: it must pass its self-test within 60 s,
>>> or the device goes back to the previous build by itself.
[OTA INFO] Self-test passed -- build 2 is now permanent
```

The LED now blinks twice. The server's terminal shows each step from its side:

```
[14:02:11] device on build 1: asked for updates -> offering build 2
[14:02:12] device on build 1: downloading firmware-2.bin.zz (602160 bytes)
```

> The Serial Monitor may disconnect for a second while the device restarts. If it doesn't reconnect by itself, run `pio device monitor`.

## 2. Someone tampers with the manifest

Stop the server (Ctrl+C) and start it as a man-in-the-middle that edits the manifest on its way to the device. It claims build 102, a big "upgrade", but can't redo the signature without your private key:

```sh
python demo.py serve --tamper
```

Type `check`:

```
[OTA ERROR] Update build 102 rejected: Signature check FAILED
OTA: failed -- Signature check FAILED
```

Nothing was downloaded, and build 2 keeps running. Changing the board name, the size or the hash would fail the same way, because the signature covers all of them together.

## 3. The download gets damaged

The device needs something newer than build 2 to download, so release build 3 now. (It's the broken one, but that doesn't matter yet: in this step it never gets as far as running.) Then restart the server so it flips one byte in the middle of every firmware download:

```sh
python demo.py release 3
python demo.py serve --corrupt
```

Type `update`:

```
Installing... 90%
OTA: failed -- Checksum mismatch -- file corrupted
```

The manifest was genuine, so the download started. But the image was hashed as it arrived, and it didn't match the signed SHA-256, so the device never switched to it. Build 2 is still running, untouched.

## 4. A signed build that's broken

Now serve build 3 for real:

```sh
python demo.py serve
```

Type `update`. Build 3 installs, the device restarts into it, and the LED blinks three times. But its self-test always fails:

```
=== SKL-OTA demo: build 3 (BROKEN: fails its self-test) ===
>>> New build on probation: it must pass its self-test within 60 s,
...about a minute later...
[OTA ERROR] Self-test failed after 1 min (pretend the light sensor is missing) -- rolling back
>>> Self-test failed. Rebooting into the previous build...
=== SKL-OTA demo: build 2 (healthy) ===
[OTA ERROR] Update to build 3 (demo-3) didn't pass its self-test (failed, crashed or stopped responding) -- the bootloader rolled back to build 2
Last update: Build 3 failed self-test; rolled back
```

Back to two blinks, with nobody touching the device. `Last update` survives the reboot, so a real device could report it to your server.

This is the one that matters most in the field: build 3 was signed, intact and newer, so every check before installing passed. Only the self-test could tell it didn't actually work.

## 5. A signed build that hangs

```sh
python demo.py release 4
```

The server picks it up (newest wins). Type `update`. Build 4 installs and gets stuck in `setup()`, with the LED solid on:

```
=== SKL-OTA demo: build 4 (BROKEN: hangs in setup()) ===
Stuck in setup() for 0 s. Rollback timer fires at 90 s...
Stuck in setup() for 10 s. Rollback timer fires at 90 s...
...
=== SKL-OTA demo: build 2 (healthy) ===
Last update: Build 4 failed self-test; rolled back
```

Build 4 never reached `loop()`, so the self-test never even ran. The timer started by `Ota.armRollbackGuard()` (the first line of `setup()`) restarted the device after 60 s + 30 s of grace, and a restart while the build is still on probation is all the bootloader needs to go back.

## 6. A signed build that crashes on boot

```sh
python demo.py release 5
```

Type `update`. Build 5 installs, restarts, and crashes almost immediately:

```
=== SKL-OTA demo: build 5 (BROKEN: crashes in setup()) ===
Reading the light sensor's calibration table...
Guru Meditation Error: Core  1 panic'ed (LoadProhibited). Exception was unhandled.
...
Rebooting...
=== SKL-OTA demo: build 2 (healthy) ===
[OTA ERROR] Update to build 5 (demo-5) didn't pass its self-test (failed, crashed or stopped responding) -- the bootloader rolled back to build 2
Last update: Build 5 failed self-test; rolled back
```

This is the fastest rollback of all: no timer, no self-test. Build 5 was still on probation when it crashed, and the ESP32's bootloader treats any restart of a build on probation as a failure, so the very next boot is build 2. Without SKL-OTA the Arduino core would have marked build 5 good as soon as it started, and the device would crash on every boot until someone plugged in a USB cable.

## 7. Someone serves an old build

```sh
python demo.py serve --build 1
```

Build 1 is genuinely signed with your key. Type `check`:

```
OTA: up_to_date -- Up to date (build 2)
```

The device only accepts a build number higher than its own. That stops someone from re-serving an old build with a bug you've already fixed, even though its signature is valid.

## What to look at in the code

Everything that makes this work is in `Demo.ino`'s `setup()`:

- `Ota.armRollbackGuard(...)` as the very first line. Without it, build 4 would hang until someone unplugged it.
- `Ota.onSelfTest(...)` defines what "working" means. In the demo that's "Wi-Fi connected". In a real product it would be "sensors found and server reachable".
- `Ota.onRequest(...)` adds `X-Demo-Build` to each request, which is how the server's log knows which build is asking. A real server could use the same header to give beta devices a newer build.
- `cfg.board` must match `--board` when signing. A release for different hardware is refused.

And on the PC side, `demo.py` is just a wrapper around `tools/ota_release.py sign` plus a 100-line web server. Your real server can be anything that serves files.

## Starting over

Flash build 1 or 2 over USB again (`python demo.py flash 1`). To start the releases over, delete the `releases` folder. Keep `demo-key.pem` unless you also reflash, because the firmware on the device only trusts the key it was built with.

## Using the Arduino IDE instead

1. Run `python demo.py keygen` as above, then open `Demo.ino` (File > Examples > SKLOta > Demo, then save a copy). Copy `ota_pubkey.h` next to your copy, and create `demo_settings.h` there.
2. Pick your board and the **Default 4MB with spiffs** partition scheme (it has the two app slots OTA needs).
3. Flash with `#define BUILD 1` and `#define BREAK_MODE 0` at the top of the sketch.
4. For each update, change those two lines (build 2 → `0`, build 3 → `1`, build 4 → `2`, build 5 → `3`), use **Sketch > Export Compiled Binary**, and sign the result:

   ```sh
   python demo.py release 2 --bin path/to/your/copy/build/esp32.esp32.esp32/Demo.ino.bin
   ```

Everything else is the same.
