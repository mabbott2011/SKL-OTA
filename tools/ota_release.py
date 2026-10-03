#!/usr/bin/env python3
# Copyright (c) 2026 SKL (Martin Abbott)
# SPDX-License-Identifier: MIT
"""SKL-OTA release tool: make a signing key, sign a firmware build, check a
release the way the device will. Run it yourself from your project folder;
it is not a PlatformIO build script. Needs:  pip install cryptography

  keygen   Make the signing key pair, once per product:
             python ota_release.py keygen
           The PRIVATE key goes outside the repo (default
           ~/.skl/ota-signing-key.pem) -- back it up offline and never commit
           it. The public half is written to include/ota_pubkey.h as
           OTA_PUBKEY_PEM; pass that to SKLOtaConfig.publicKeyPem.

  sign     After building, sign the firmware:
             python ota_release.py sign --board my-board --build 143 \\
                 --bin .pio/build/esp32dev/firmware.bin --url-base https://example.com/fw
           Writes releases/<build>/firmware-<build>.bin + manifest.json.
           Host both so the manifest is what your manifest URL returns and the
           .bin is at the manifest's "url" (a "/path" is relative to the
           manifest's host).

  verify   Check a release against include/ota_pubkey.h, as the device will:
             python ota_release.py verify releases/143/manifest.json --context skl-ota

The signed message is "<context>|<board>|<build>|<size>|<sha256 hex>", where
context is SKLOtaConfig.signContext (default "skl-ota").
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import sys

try:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.exceptions import InvalidSignature
except ImportError:
    sys.exit("This tool needs the 'cryptography' package:  pip install cryptography")

DEFAULT_KEY = os.path.join(os.path.expanduser("~"), ".skl", "ota-signing-key.pem")
DEFAULT_HEADER = os.path.join("include", "ota_pubkey.h")
DEFAULT_CONTEXT = "skl-ota"


def signed_message(context, board, build, size, sha256_hex):
    return "{0}|{1}|{2}|{3}|{4}".format(context, board, build, size, sha256_hex).encode("ascii")


def read_define(path, name):
    if not path or not os.path.exists(path):
        return None
    with open(path, "r") as f:
        for line in f:
            if line.startswith("#define " + name + " "):
                return line.split(" ", 2)[2].strip().strip('"')
    return None


def header_pubkey_pem(path):
    if not os.path.exists(path):
        return None
    text = open(path).read()
    m = re.search(r'#define OTA_PUBKEY_PEM[\s\\]+((?:"(?:[^"\\]|\\.)*"[\s\\]*)+)', text)
    if not m:
        return None
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
    pem = "".join(parts).encode("ascii").decode("unicode_escape")
    return pem or None


def write_pubkey_header(path, public_key):
    pem = public_key.public_bytes(serialization.Encoding.PEM,
                                  serialization.PublicFormat.SubjectPublicKeyInfo).decode("ascii")
    lines = ['  "' + line + '\\n"' for line in pem.strip().splitlines()]
    if os.path.dirname(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write("// Public half of the OTA signing key (ECDSA P-256), made by SKL-OTA's\n"
                "// tools/ota_release.py keygen -- safe to commit. The private key lives\n"
                "// outside this repo and must never be committed.\n"
                "#pragma once\n"
                "#define OTA_PUBKEY_PEM \\\n" + " \\\n".join(lines) + "\n")


def load_private_key(path):
    if not os.path.exists(path):
        sys.exit("No signing key at {0} (run keygen first, or pass --key)".format(path))
    with open(path, "rb") as f:
        return serialization.load_pem_private_key(f.read(), password=None)


def public_pem(key):
    return key.public_key().public_bytes(serialization.Encoding.PEM,
                                         serialization.PublicFormat.SubjectPublicKeyInfo).decode("ascii")


def cmd_keygen(args):
    if os.path.exists(args.key):
        if args.header_only:
            key = load_private_key(args.key)
            write_pubkey_header(args.header, key.public_key())
            print("Public key:  {0}  (re-written from {1})".format(args.header, args.key))
            return
        sys.exit("Refusing to overwrite the existing key at {0}.\n"
                 "Devices already trust it -- a new key would lock them out of updates.\n"
                 "(--header-only re-writes the public header from it.)".format(args.key))
    os.makedirs(os.path.dirname(args.key), exist_ok=True)
    key = ec.generate_private_key(ec.SECP256R1())
    with open(args.key, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM,
                                  serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))
    try:
        os.chmod(args.key, stat.S_IRUSR | stat.S_IWUSR)
    except OSError:
        pass
    write_pubkey_header(args.header, key.public_key())
    print("Private key: {0}  <-- back this up somewhere safe and offline; never commit it".format(args.key))
    print("Public key:  {0}  <-- commit this and build it into the firmware".format(args.header))


def cmd_sign(args):
    build = args.build
    version = args.version
    if args.build_info:
        build = build or read_define(args.build_info, "BUILD_NUMBER")
        version = version or read_define(args.build_info, "GIT_COMMIT")
    if not build:
        sys.exit("No build number: pass --build N (or --build-info with a BUILD_NUMBER define)")
    build = int(build)
    if build <= 0:
        sys.exit("The build number must be a positive integer that grows with every release")
    version = version or ""
    if version.endswith("-dirty") and not args.allow_dirty:
        sys.exit("Build {0} is from a dirty working tree ({1}). Commit, rebuild, then sign.".format(build, version))

    data = open(args.bin, "rb").read()
    if data[:1] != b"\xe9":
        sys.exit("{0} doesn't look like an ESP32 app image".format(args.bin))
    if args.slot_size and len(data) > args.slot_size:
        sys.exit("firmware is {0} bytes; the OTA slot holds {1}".format(len(data), args.slot_size))

    key = load_private_key(args.key)
    header_pem = header_pubkey_pem(args.header)
    if header_pem is None:
        print("note: no {0} to cross-check against -- make sure the firmware trusts this key".format(args.header))
    elif header_pem.strip() != public_pem(key).strip():
        sys.exit("{0} doesn't match {1} -- devices running this build couldn't verify the NEXT update. "
                 "Fix the key/header before releasing.".format(args.header, args.key))

    sha = hashlib.sha256(data).hexdigest()
    sig = key.sign(signed_message(args.context, args.board, build, len(data), sha),
                   ec.ECDSA(hashes.SHA256())).hex()

    out = os.path.join(args.out, str(build))
    os.makedirs(out, exist_ok=True)
    fname = "firmware-{0}.bin".format(build)
    shutil.copyfile(args.bin, os.path.join(out, fname))
    url = args.url if args.url else args.url_base.rstrip("/") + "/" + fname
    manifest = {"board": args.board, "build": build, "version": version, "url": url,
                "size": len(data), "sha256": sha, "sig": sig}
    with open(os.path.join(out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print("Signed build {0} ({1}), {2} bytes, context '{3}'".format(build, version or "no version", len(data), args.context))
    print("  {0}".format(os.path.join(out, fname)))
    print("  {0}  (url: {1})".format(os.path.join(out, "manifest.json"), url))


def cmd_verify(args):
    manifest = json.load(open(args.manifest))
    binpath = args.bin or os.path.join(os.path.dirname(os.path.abspath(args.manifest)),
                                       os.path.basename(manifest["url"]))
    data = open(binpath, "rb").read()
    ok = True
    if len(data) != manifest["size"]:
        print("FAIL size: file {0}, manifest {1}".format(len(data), manifest["size"])); ok = False
    sha = hashlib.sha256(data).hexdigest()
    if sha != manifest["sha256"].lower():
        print("FAIL sha256 mismatch"); ok = False
    pem = header_pubkey_pem(args.header)
    if not pem:
        sys.exit("No public key in {0}".format(args.header))
    pub = serialization.load_pem_public_key(pem.encode("ascii"))
    try:
        pub.verify(bytes.fromhex(manifest["sig"]),
                   signed_message(args.context, manifest["board"], manifest["build"], manifest["size"],
                                  manifest["sha256"].lower()),
                   ec.ECDSA(hashes.SHA256()))
        print("signature OK")
    except InvalidSignature:
        print("FAIL signature (wrong key, or wrong --context?)"); ok = False
    print("PASS" if ok else "REJECTED -- a device would refuse this update")
    sys.exit(0 if ok else 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    k = sub.add_parser("keygen", help="make the signing key pair (once)")
    k.add_argument("--key", default=DEFAULT_KEY, help="private key path (default %(default)s)")
    k.add_argument("--header", default=DEFAULT_HEADER, help="public key header to write (default %(default)s)")
    k.add_argument("--header-only", action="store_true", help="re-write the header from an existing key")

    s = sub.add_parser("sign", help="sign a firmware .bin and write its manifest")
    s.add_argument("--bin", required=True, help="the built app image, e.g. .pio/build/<env>/firmware.bin")
    s.add_argument("--board", required=True, help="must equal SKLOtaConfig.board")
    s.add_argument("--build", type=int, help="build number (must grow with every release)")
    s.add_argument("--version", help="free text shown on the device, e.g. a git commit")
    s.add_argument("--build-info", help="header with BUILD_NUMBER / GIT_COMMIT defines to read instead")
    s.add_argument("--context", default=DEFAULT_CONTEXT, help="must equal SKLOtaConfig.signContext (default %(default)s)")
    s.add_argument("--key", default=DEFAULT_KEY)
    s.add_argument("--header", default=DEFAULT_HEADER, help="checked against the key (default %(default)s)")
    s.add_argument("--slot-size", type=lambda v: int(v, 0), help="refuse images bigger than this, e.g. 0x180000")
    g = s.add_mutually_exclusive_group(required=True)
    g.add_argument("--url-base", help="where the .bin will be hosted; firmware-<build>.bin is appended")
    g.add_argument("--url", help="exact URL of the .bin (or /path relative to the manifest's host)")
    s.add_argument("--out", default="releases", help="output folder (default %(default)s)")
    s.add_argument("--allow-dirty", action="store_true", help="sign a '-dirty' version (testing only)")

    v = sub.add_parser("verify", help="check a release like the device will")
    v.add_argument("manifest")
    v.add_argument("--bin")
    v.add_argument("--context", default=DEFAULT_CONTEXT)
    v.add_argument("--header", default=DEFAULT_HEADER)

    args = ap.parse_args()
    {"keygen": cmd_keygen, "sign": cmd_sign, "verify": cmd_verify}[args.cmd](args)


if __name__ == "__main__":
    main()
