#!/usr/bin/env python3
# Copyright (c) 2026 SKL (Martin Abbott)
# SPDX-License-Identifier: MIT
"""SKL-OTA demo helper. Run it from anywhere; it works in its own folder.
Needs:  pip install cryptography   (and PlatformIO for flash/release)

  python demo.py keygen          make a throwaway demo key (never your real one)
  python demo.py flash 1         build 1 over USB, then open the Serial Monitor
  python demo.py release 2       build 2, sign it, put it in releases/2/
  python demo.py serve           be the update server for the newest release

The server can also play the attacker / the unlucky network:
  python demo.py serve --tamper  edit the manifest in transit (claims build +100)
  python demo.py serve --corrupt flip one byte of the firmware in transit
  python demo.py serve --build 1 offer an OLD (validly signed) build

Arduino IDE instead of PlatformIO? Export the compiled binary for each build
and pass it in:  python demo.py release 2 --bin path/to/Demo.ino.bin
"""
import argparse
import http.server
import json
import os
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.normpath(os.path.join(HERE, "..", "..", "tools", "ota_release.py"))
KEY = os.path.join(HERE, "demo-key.pem")       # *.pem is git-ignored
HEADER = os.path.join(HERE, "ota_pubkey.h")    # git-ignored for examples
RELEASES = os.path.join(HERE, "releases")      # git-ignored
BOARD = "skl-ota-demo"                         # must match DEMO_BOARD in Demo.ino
WHAT = {1: "healthy", 2: "healthy", 3: "BROKEN: fails its self-test", 4: "BROKEN: hangs in setup()"}


def run(cmd):
    print("> " + " ".join(cmd))
    r = subprocess.call(cmd, cwd=HERE)
    if r != 0:
        sys.exit(r)


def pio():
    """PlatformIO's CLI: on PATH, or where the VS Code extension installs it."""
    found = shutil.which("pio") or shutil.which("platformio")
    if found:
        return found
    home = os.path.join(os.path.expanduser("~"), ".platformio", "penv")
    for p in (os.path.join(home, "Scripts", "pio.exe"), os.path.join(home, "bin", "pio")):
        if os.path.exists(p):
            return p
    sys.exit("Can't find PlatformIO's `pio` command. Install PlatformIO, or use the Arduino IDE and --bin.")


def need_key():
    if not os.path.exists(HEADER):
        sys.exit("No demo key yet. Run:  python demo.py keygen")


# ---- commands ------------------------------------------------------------------------
def cmd_keygen(args):
    if os.path.exists(KEY):
        print("A demo key already exists ({0}); keeping it.".format(KEY))
        return
    run([sys.executable, TOOL, "keygen", "--key", KEY, "--header", HEADER])
    print("\nThis key is only for the demo. Your real products get their own,")
    print("kept outside the repo (python tools/ota_release.py keygen).")


def cmd_flash(args):
    need_key()
    if args.build in (3, 4):
        print("Note: build {0} is meant to arrive OVER THE AIR. Flashed over USB it isn't on".format(args.build))
        print("probation, so there's no previous build to roll back to.")
    run([pio(), "run", "-e", "build{0}".format(args.build), "-t", "upload", "-t", "monitor"])


def cmd_release(args):
    need_key()
    if args.bin:
        binpath = os.path.abspath(args.bin)
    else:
        env = "build{0}".format(args.build)
        run([pio(), "run", "-e", env])
        binpath = os.path.join(HERE, ".pio", "build", env, "firmware.bin")
    run([sys.executable, TOOL, "sign", "--key", KEY, "--header", HEADER, "--bin", binpath,
         "--board", BOARD, "--build", str(args.build), "--version", "demo-{0}".format(args.build),
         "--url-base", "/fw", "--out", RELEASES])
    print("\nReleased build {0} ({1}). `python demo.py serve` offers the newest release.".format(
        args.build, WHAT.get(args.build, "custom")))


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))  # no packet is sent; just picks the LAN interface
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def available_builds():
    if not os.path.isdir(RELEASES):
        return []
    return sorted(int(d) for d in os.listdir(RELEASES)
                  if d.isdigit() and os.path.exists(os.path.join(RELEASES, d, "manifest.json")))


class Handler(http.server.BaseHTTPRequestHandler):
    opts = None

    def log_message(self, fmt, *a):
        pass  # we print our own, friendlier lines

    def say(self, text):
        who = self.headers.get("X-Demo-Build")
        who = "device on build {0}".format(who) if who else self.client_address[0]
        print("[{0}] {1}: {2}".format(time.strftime("%H:%M:%S"), who, text))

    def reply(self, code, body=b"", ctype="application/octet-stream"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if body and self.command != "HEAD":
            self.wfile.write(body)

    def do_HEAD(self):
        self.do_GET()

    def pick_build(self):
        builds = available_builds()
        if not builds:
            return None
        if self.opts.build:
            return self.opts.build if self.opts.build in builds else None
        return builds[-1]

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/manifest.json":
            build = self.pick_build()
            if build is None:
                self.say("asked for updates -> nothing released (204)")
                return self.reply(204)
            with open(os.path.join(RELEASES, str(build), "manifest.json")) as f:
                m = json.load(f)
            note = "offering build {0}".format(build)
            if self.opts.tamper:
                m["build"] = build + 100
                note += ", TAMPERED to claim build {0} (signature not redone)".format(m["build"])
            self.say("asked for updates -> " + note)
            return self.reply(200, json.dumps(m, indent=2).encode(), "application/json")

        if path.startswith("/fw/"):
            name = os.path.basename(path)
            if self.opts.corrupt and name.endswith(".zz"):
                # Hide the compressed copy so the device takes the plain image,
                # which we then damage: the clearest way to see the hash check.
                self.say("asked for {0} -> 404 (corrupt mode serves the plain image)".format(name))
                return self.reply(404)
            for b in available_builds():
                p = os.path.join(RELEASES, str(b), name)
                if os.path.exists(p):
                    with open(p, "rb") as f:
                        data = bytearray(f.read())
                    note = "{0} bytes".format(len(data))
                    if self.opts.corrupt:
                        data[len(data) // 2] ^= 0xFF
                        note += ", ONE BYTE FLIPPED at {0}".format(len(data) // 2)
                    self.say("downloading {0} ({1})".format(name, note))
                    return self.reply(200, bytes(data))
            self.say("asked for {0} -> 404".format(name))
            return self.reply(404)

        self.reply(404, b"SKL-OTA demo server: try /manifest.json\n", "text/plain")


def cmd_serve(args):
    builds = available_builds()
    Handler.opts = args
    ip = lan_ip()
    print("SKL-OTA demo server")
    print("  Manifest URL for Demo.ino:  http://{0}:{1}/manifest.json".format(ip, args.port))
    print("  Releases on hand:           {0}".format(", ".join(map(str, builds)) or "none yet (python demo.py release 2)"))
    if args.build:
        print("  Offering:                   build {0} only{1}".format(
            args.build, "" if args.build in builds else "  (not released!)"))
    elif builds:
        print("  Offering:                   build {0} (the newest)".format(builds[-1]))
    if args.tamper:
        print("  MODE: --tamper   the manifest is edited in transit (build number raised, not re-signed)")
    if args.corrupt:
        print("  MODE: --corrupt  one byte of every firmware download is flipped")
    print("Ctrl+C to stop. (Windows may ask to let Python through the firewall: allow it on private networks.)\n")
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("keygen", help="make the throwaway demo key")
    f = sub.add_parser("flash", help="build N, upload over USB, open the Serial Monitor")
    f.add_argument("build", type=int, choices=[1, 2, 3, 4])
    r = sub.add_parser("release", help="build N and sign it into releases/N/")
    r.add_argument("build", type=int)
    r.add_argument("--bin", help="an already-built .bin (Arduino IDE: Sketch > Export Compiled Binary)")
    s = sub.add_parser("serve", help="run the update server")
    s.add_argument("--port", type=int, default=8000)
    s.add_argument("--build", type=int, help="offer this build instead of the newest")
    s.add_argument("--tamper", action="store_true", help="edit the manifest in transit")
    s.add_argument("--corrupt", action="store_true", help="flip a byte of the firmware in transit")
    args = ap.parse_args()
    {"keygen": cmd_keygen, "flash": cmd_flash, "release": cmd_release, "serve": cmd_serve}[args.cmd](args)


if __name__ == "__main__":
    main()
