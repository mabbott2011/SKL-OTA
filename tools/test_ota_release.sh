#!/bin/bash
# Copyright (c) 2026 SKL (Martin Abbott)
# SPDX-License-Identifier: MIT
#
# End-to-end check of tools/ota_release.py with a throwaway key: keygen,
# sign (firmware + filesystem image, compressed copies), verify, and that
# verify REJECTS tampered releases. Needs: pip install cryptography
set -euo pipefail
TOOL="$(cd "$(dirname "$0")" && pwd)/ota_release.py"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"
fail() { echo "FAIL $*"; exit 1; }
pass() { echo "PASS $*"; }

python3 "$TOOL" keygen --key key.pem --header include/ota_pubkey.h >/dev/null
[ -f key.pem ] && grep -q OTA_PUBKEY_PEM include/ota_pubkey.h && pass "keygen writes the key and the public header"
python3 "$TOOL" keygen --key key.pem --header include/ota_pubkey.h >/dev/null 2>&1 && fail "keygen overwrote an existing key"
pass "keygen refuses to overwrite an existing key"

python3 - <<'PY'
import random
random.seed(1)
open("fw.bin", "wb").write(b"\xe9" + bytes(random.choice(b"ESP32 app\x00\xff") for _ in range(300000)))
open("fs.bin", "wb").write(b"littlefs" + b"\xff" * (64 * 1024 - 8))
open("notfw.bin", "wb").write(b"hello")
PY
python3 "$TOOL" sign --key key.pem --bin notfw.bin --board b --build 1 --url-base /fw >/dev/null 2>&1 && fail "signed a non-ESP32 image"
pass "sign refuses a file that isn't an ESP32 app image"
python3 "$TOOL" sign --key key.pem --bin fw.bin --board b --build 2 --version abc-dirty --url-base /fw >/dev/null 2>&1 && fail "signed a dirty build"
pass "sign refuses a -dirty version"

python3 "$TOOL" sign --key key.pem --bin fw.bin --board b --build 7 --version abc --fs-bin fs.bin --url-base /fw >/dev/null
[ -f releases/7/firmware-7.bin.zz ] && [ -f releases/7/filesystem-7.bin.zz ] && pass "sign writes firmware + filesystem images and compressed copies"
python3 "$TOOL" verify releases/7/manifest.json >/dev/null && pass "verify accepts the release it just signed"
python3 "$TOOL" verify releases/7/manifest.json --context other >/dev/null 2>&1 && fail "verify accepted the wrong signing context"
pass "verify rejects the wrong signing context"

tamper() {  # $1 = python expression run on the manifest dict m
  cp -r releases/7 t; python3 - "$1" <<'PY'
import json, sys
m = json.load(open("t/manifest.json")); exec(sys.argv[1]); json.dump(m, open("t/manifest.json", "w"))
PY
  python3 "$TOOL" verify t/manifest.json >/dev/null 2>&1 && { rm -rf t; return 1; }; rm -rf t; return 0
}
tamper 'm["build"] = 8' && pass "verify rejects a changed build number" || fail "accepted a changed build number"
tamper 'm["board"] = "other"' && pass "verify rejects a changed board" || fail "accepted a changed board"
tamper 'm["fs"]["sig"] = m["sig"]' && pass "verify rejects the firmware signature passed off as the filesystem's" || fail "accepted a swapped signature"
cp -r releases/7 t; printf 'X' | dd of=t/firmware-7.bin bs=1 seek=1000 conv=notrunc 2>/dev/null
python3 "$TOOL" verify t/manifest.json >/dev/null 2>&1 && fail "accepted a modified firmware image"; rm -rf t
pass "verify rejects a modified firmware image"
cp -r releases/7 t; printf 'X' | dd of=t/filesystem-7.bin.zz bs=1 seek=100 conv=notrunc 2>/dev/null
python3 "$TOOL" verify t/manifest.json >/dev/null 2>&1 && fail "accepted a corrupted compressed copy"; rm -rf t
pass "verify rejects a corrupted compressed copy"

echo "ALL PASS"
