// SKL-OTA basic example: check for a signed update every 12 hours, and
// install one when you type "update" in the Serial Monitor.
//
// Before flashing:
//   1. pip install cryptography
//   2. python tools/ota_release.py keygen --header examples/Basic/ota_pubkey.h
//      (keeps the private key in ~/.skl/, writes the public key next to this sketch)
//   3. Set your WiFi and manifest URL below.
//   4. Use a partition scheme with two app slots (Arduino IDE: "Default 4MB
//      with spiffs"; PlatformIO: the default table, or any with ota_0/ota_1).
// To release: bump BUILD, build, then
//   python tools/ota_release.py sign --bin <the .bin> --board my-board --build 2 \
//       --header examples/Basic/ota_pubkey.h --url-base https://example.com/fw
// and upload releases/2/manifest.json + firmware-2.bin to that server.
#include <WiFi.h>
#include <SKLOta.h>

#if __has_include("ota_pubkey.h")
#include "ota_pubkey.h"
#else
#define OTA_PUBKEY_PEM ""  // run keygen (step 2) -- without a key, updates stay off
#endif

#define BUILD 1  // raise with every release; devices only accept newer builds

const char* WIFI_SSID = "your-ssid";
const char* WIFI_PASS = "your-password";

void setup() {
  // First thing: if this is a new build on probation, start the rollback
  // deadline before anything that could hang. Does nothing otherwise.
  Ota.armRollbackGuard();

  Serial.begin(115200);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Ota.setManifestUrl("https://example.com/fw/manifest.json");
  Ota.onLog([](int sev, const char* msg) {
    Serial.printf("[%s] %s\n", sev == 1 ? "ERROR" : sev == 2 ? "WARN" : "INFO", msg);
  });
  // Keep the new build only if it can still reach WiFi. Anything your
  // product needs to work belongs here (sensors found, server reachable...).
  Ota.onSelfTest([](String& status) {
    bool ok = WiFi.status() == WL_CONNECTED;
    status = ok ? "wifi ok" : "no wifi";
    return ok;
  });

  // If your release also ships a LittleFS image (sign --fs-bin), let
  // SKL-OTA unmount it while it's rewritten:
  //   Ota.onFilesystemUpdate([](bool starting, bool ok) {
  //     if (starting) LittleFS.end(); else if (ok) ESP.restart(); else LittleFS.begin(true);
  //   });

  SKLOtaConfig cfg;
  cfg.publicKeyPem = OTA_PUBKEY_PEM;
  cfg.board = "my-board";
  cfg.build = BUILD;
  Ota.begin(cfg);
  Serial.printf("Build %d running. Type 'check' or 'update'.\n", BUILD);
}

void loop() {
  Ota.loop();

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "check") Ota.requestCheck();
    if (cmd == "update") Ota.requestInstall();
  }

  static SKLOtaState shown = SKLOtaState::Idle;
  if (Ota.state() != shown) {
    shown = Ota.state();
    Serial.printf("OTA: %s -- %s\n", Ota.stateName(), Ota.message());
  }
}
