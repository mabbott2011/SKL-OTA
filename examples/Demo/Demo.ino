// SKL-OTA demo: watch every safety check happen on your own desk.
//
// One sketch, four builds. Each build blinks the LED its build number of
// times, so you can see which one is running without a serial cable:
//
//   build 1  healthy    flash this one over USB to start
//   build 2  healthy    the "good update"
//   build 3  BROKEN     installs fine, then fails its self-test -> rolled back
//   build 4  BROKEN     hangs in setup(), loop() never runs -> rolled back by the timer
//
// demo.py (next to this file) makes a throwaway key, builds and signs each
// one, and runs a tiny update server on your PC that can also play the
// attacker (--tamper, --corrupt, --build 1). README.md walks through it.
//
// Serial Monitor (115200): type  check, update, status  or  help.
#include <WiFi.h>
#include <SKLOta.h>

// ---- your settings -------------------------------------------------------------
// Put these three in demo_settings.h next to this file (it's git-ignored, so
// your Wi-Fi password stays out of the repo), or edit the defaults below.
#if __has_include("demo_settings.h")
#include "demo_settings.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID "your-ssid"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "your-password"
#endif
#ifndef MANIFEST_URL
#define MANIFEST_URL "http://192.168.1.50:8000/manifest.json"  // `python demo.py serve` prints yours
#endif

// ---- which build this is ---------------------------------------------------------
// PlatformIO sets both per environment (platformio.ini: build1 ... build4).
// Arduino IDE: edit them before each Sketch > Export Compiled Binary.
#ifndef BUILD
#define BUILD 1
#endif
#ifndef BREAK_MODE
#define BREAK_MODE 0  // 0 = healthy, 1 = fails its self-test, 2 = hangs in setup()
#endif

// The public half of the demo key (`python demo.py keygen` writes it).
#if __has_include("ota_pubkey.h")
#include "ota_pubkey.h"
#else
#define OTA_PUBKEY_PEM ""  // no key yet: updates stay off until you run keygen
#endif

#ifndef DEMO_LED
#ifdef LED_BUILTIN
#define DEMO_LED LED_BUILTIN
#else
#define DEMO_LED 2  // the blue LED on most ESP32 dev boards
#endif
#endif

const char* DEMO_BOARD = "skl-ota-demo";  // must match `--board` when signing (demo.py does)

// Short timers so the demo takes minutes, not an afternoon. A real product
// should keep the defaults (stay up 1 min, pass within 10 min).
const uint32_t SELF_TEST_MIN_UP_MS = 10UL * 1000;
const uint32_t SELF_TEST_TIMEOUT_MS = 60UL * 1000;

const char* modeName() {
  return BREAK_MODE == 1 ? "BROKEN: fails its self-test"
       : BREAK_MODE == 2 ? "BROKEN: hangs in setup()"
                         : "healthy";
}

// BUILD quick flashes, then a pause. Count them to see which build is running.
void blinkBuildNumber() {
  const uint32_t flashMs = 150, stepMs = 400, pauseMs = 1500;
  const uint32_t cycle = BUILD * stepMs + pauseMs;
  uint32_t t = millis() % cycle;
  bool lit = t < BUILD * stepMs && (t % stepMs) < flashMs;
  digitalWrite(DEMO_LED, lit ? HIGH : LOW);
}

void printHelp() {
  Serial.println("Commands: check   ask the server if there's a newer signed build");
  Serial.println("          update  install it (checks first if needed)");
  Serial.println("          status  what's running and what happened last");
}

void printStatus() {
  Serial.printf("Build %d (%s) on board '%s'\n", BUILD, modeName(), DEMO_BOARD);
  Serial.printf("  OTA:         %s -- %s\n", Ota.stateName(), Ota.message());
  Serial.printf("  Probation:   %s\n", Ota.pendingVerify() ? "yes, self-test still running" : "no, this build is permanent");
  Serial.printf("  Last update: %s\n", Ota.lastResult()[0] ? Ota.lastResult() : "(none yet)");
  Serial.printf("  WiFi:        %s\n", WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "not connected");
  Serial.printf("  Manifest:    %s\n", MANIFEST_URL);
  if (!OTA_PUBKEY_PEM[0]) Serial.println("  No public key compiled in -- run `python demo.py keygen`, then rebuild.");
}

void setup() {
  // Always the first line: if this boot is a new build on probation, start
  // the rollback deadline now, before anything that could hang.
  bool onProbation = Ota.armRollbackGuard(SELF_TEST_TIMEOUT_MS);

  Serial.begin(115200);
  delay(300);
  pinMode(DEMO_LED, OUTPUT);
  Serial.printf("\n=== SKL-OTA demo: build %d (%s) ===\n", BUILD, modeName());

#if BREAK_MODE == 2
  // Pretend a sensor driver never returns (an I2C bus stuck low, say). This
  // build never reaches loop(), so nothing in loop() can notice the problem.
  // Only the timer armed on the first line can get the old build back.
  digitalWrite(DEMO_LED, HIGH);
  if (!onProbation) {
    Serial.println("This build wasn't installed over the air, so nothing will rescue it.");
    Serial.println("Reflash build 1 or 2 over USB.");
  }
  for (uint32_t s = 0;; s += 10) {
    Serial.printf("Stuck in setup() for %lu s. Rollback timer fires at %lu s...\n",
                  (unsigned long)s, (unsigned long)(SELF_TEST_TIMEOUT_MS / 1000 + 30));
    delay(10000);
  }
#else
  (void)onProbation;
#endif

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Ota.setManifestUrl(MANIFEST_URL);
  Ota.onLog([](int sev, const char* msg) {
    Serial.printf("[OTA %s] %s\n", sev == 1 ? "ERROR" : sev == 2 ? "WARN" : "INFO", msg);
  });
  // Tell the server who's asking, so its log shows each device's build.
  // (A real server could use this to pick which build to offer.)
  Ota.onRequest([](HTTPClient& http, const char* url) {
    http.addHeader("X-Demo-Board", DEMO_BOARD);
    http.addHeader("X-Demo-Build", String(BUILD));
  });
  // What "working" means for this build.
  Ota.onSelfTest([](String& status) {
#if BREAK_MODE == 1
    status = "pretend the light sensor is missing";
    return false;
#else
    bool ok = WiFi.status() == WL_CONNECTED;
    status = ok ? "wifi ok" : "no wifi";
    return ok;
#endif
  });
  Ota.onRollback([](const char* why) {
    Serial.println(">>> Self-test failed. Rebooting into the previous build...");
  });

  SKLOtaConfig cfg;
  cfg.publicKeyPem = OTA_PUBKEY_PEM;
  cfg.board = DEMO_BOARD;
  cfg.build = BUILD;
  cfg.firstCheckMs = 15UL * 1000;  // check once, 15 s after boot...
  cfg.checkIntervalMs = 0;         // ...then only when you type `check`
  cfg.selfTestMinUptimeMs = SELF_TEST_MIN_UP_MS;
  cfg.selfTestTimeoutMs = SELF_TEST_TIMEOUT_MS;
  Ota.begin(cfg);

  if (Ota.lastResult()[0]) Serial.printf("Last update: %s\n", Ota.lastResult());
  if (Ota.pendingVerify()) {
    Serial.printf(">>> New build on probation: it must pass its self-test within %lu s,\n",
                  (unsigned long)(SELF_TEST_TIMEOUT_MS / 1000));
    Serial.println(">>> or the device goes back to the previous build by itself.");
  }
  if (!Ota.supported()) Serial.println("This board's partition table has one app slot -- OTA can't work. Use one with two.");
  printHelp();
}

void loop() {
  Ota.loop();
  blinkBuildNumber();

  static bool wifiShown = false;
  if (!wifiShown && WiFi.status() == WL_CONNECTED) {
    wifiShown = true;
    Serial.printf("WiFi connected, IP %s\n", WiFi.localIP().toString().c_str());
  }

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toLowerCase();
    if (cmd == "check") Ota.requestCheck();
    else if (cmd == "update") Ota.requestInstall();
    else if (cmd == "status") printStatus();
    else if (cmd.length()) printHelp();
  }

  // Print every state change, and install progress in 10% steps.
  static SKLOtaState shownState = SKLOtaState::Idle;
  static String shownMsg;
  if (Ota.state() != shownState || shownMsg != Ota.message()) {
    shownState = Ota.state();
    shownMsg = Ota.message();
    if (shownState != SKLOtaState::Installing) Serial.printf("OTA: %s -- %s\n", Ota.stateName(), Ota.message());
  }
  static int shownPct = -1;
  int pct = Ota.progress();
  if (pct >= 0 && pct / 10 != shownPct / 10) Serial.printf("Installing... %d%%\n", pct);
  shownPct = pct;
}
