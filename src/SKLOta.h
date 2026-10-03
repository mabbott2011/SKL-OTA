// Copyright (c) 2026 SKL (Martin Abbott)
// SPDX-License-Identifier: MIT
//
// SKL-OTA -- signed, rollback-safe OTA firmware updates for ESP32.
// https://github.com/mabbott2011/SKL-OTA
//
// How an update gets onto a device:
//   1. CHECK   GET a small JSON manifest from your update server:
//                {"board":"my-board","build":143,"version":"cc800ce",
//                 "url":"/fw/firmware-143.bin","size":1315181,
//                 "sha256":"<64 hex>","sig":"<hex DER ECDSA>"}
//              Only a build NEWER than the running one is offered, so an
//              old (validly signed) build can never be pushed back on.
//   2. TRUST   "sig" is an ECDSA P-256 signature, made offline with
//              tools/ota_release.py, over
//                "<context>|<board>|<build>|<size>|<sha256>"
//              and checked against the public key compiled into the
//              firmware. Where the files are hosted doesn't matter:
//              nothing unsigned installs. There is no switch to turn this off.
//   3. INSTALL The .bin streams into the OTHER app slot (the running one is
//              never touched) and is hashed as it goes. Only if the SHA-256
//              matches the signed one does the device switch slots and
//              restart. A failure anywhere leaves the old firmware running.
//   4. PROVE   The new firmware boots "pending verify". It marks itself good
//              only after your self-test passes. A crash or reset before
//              that, or no pass within selfTestTimeoutMs, and the bootloader
//              goes back to the previous build by itself.
//
// Checks and installs run in their own FreeRTOS task (TLS + ECDSA need far
// more stack than loop() has spare). Log messages from that task are handed
// to loop() and delivered through onLog() there, so your logger never runs
// on the update task.
//
// Requirements: an ESP32 partition table with two OTA app slots (ota_0 /
// ota_1 + otadata), and bootloader rollback enabled
// (CONFIG_APP_ROLLBACK_ENABLE -- on in the Arduino-ESP32 2.x core).
#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <functional>

#define SKL_OTA_VERSION "1.0.0"

enum class SKLOtaState : uint8_t {
  Idle,        // nothing happening (or never checked)
  Checking,    // fetching / verifying the manifest
  UpToDate,    // no newer build (or none published)
  Available,   // a newer, correctly signed build is on offer -- see offer()
  Installing,  // downloading / writing / restarting
  Failed       // last check or install failed -- see message()
};

// A release the device has fetched and verified the signature of.
struct SKLOtaRelease {
  uint32_t build = 0;
  uint32_t size = 0;
  char version[24] = "";   // free text, e.g. a git commit
  char url[224] = "";      // absolute URL of the .bin
  char zurl[224] = "";     // absolute URL of the zlib-compressed .bin, "" = none offered
  uint32_t zsize = 0;      // its size in bytes
  char sha256[65] = "";    // lowercase hex
  char sig[160] = "";      // hex DER ECDSA signature
};

struct SKLOtaConfig {
  // --- required ---------------------------------------------------------
  const char* publicKeyPem = nullptr;  // OTA_PUBKEY_PEM from tools/ota_release.py keygen
  const char* board = nullptr;         // must equal the manifest's "board"
  uint32_t build = 0;                  // the running firmware's build number (monotonic)

  // --- optional -----------------------------------------------------------
  // First field of the signed message. Use a name of your own so a
  // signature made for one product can never be replayed on another that
  // happens to share a key. Must match `ota_release.py sign --context`.
  const char* signContext = "skl-ota";
  const char* nvsNamespace = "skl_ota";  // where the install record + last result live
  uint32_t firstCheckMs = 5UL * 60 * 1000;          // first automatic check after boot (0 = never)
  uint32_t checkIntervalMs = 12UL * 60 * 60 * 1000; // then every this often (0 = only the first)
  uint32_t selfTestMinUptimeMs = 60UL * 1000;       // a new build must stay up this long...
  uint32_t selfTestTimeoutMs = 10UL * 60 * 1000;    // ...and pass its self-test within this
  uint32_t taskStack = 12288;                       // bytes, for the check/install task
  // Download the compressed image when the manifest offers one (about a
  // third smaller). Costs ~44 KB of heap during the install, no flash.
  bool allowCompressed = true;
};

class SKLOta {
 public:
  using LogFn = std::function<void(int severity, const char* msg)>;   // 1 error, 2 warning, 3 info
  using UrlFn = std::function<String()>;
  using RequestFn = std::function<void(HTTPClient& http, const char* url)>;
  using SelfTestFn = std::function<bool(String& status)>;
  using BoolFn = std::function<bool()>;
  using VoidFn = std::function<void()>;
  using RollbackFn = std::function<void(const char* why)>;

  // --- setup ---------------------------------------------------------------
  // Where the manifest is. A function is re-evaluated at every check, so a
  // URL from your settings can change at runtime.
  void setManifestUrl(const char* url);
  void setManifestUrl(UrlFn fn);
  // Delivered on loop()'s task (from begin()/loop()), never on the update task.
  void onLog(LogFn fn);
  // Add headers (device identity, API key...) to the manifest and .bin
  // requests. Runs on the update task. `url` lets you send credentials only
  // to your own server.
  void onRequest(RequestFn fn);
  // Return true once the new build is healthy (sensors found, server
  // reachable...). Fill `status` with a short summary for the failure log.
  // Called from loop() only while a new build is pending verification.
  // Not set: the build passes once it has stayed up selfTestMinUptimeMs.
  void onSelfTest(SelfTestFn fn);
  // Whether automatic checks may run now. Default: WiFi is connected.
  void onNetworkReady(BoolFn fn);
  // Something changed (state, message, progress) -- e.g. redraw a screen.
  // Called from ANY task: keep it to setting a flag.
  void onChange(VoidFn fn);
  // The new image is written and verified; the device must restart into it.
  // Runs on the update task. Default: wait 2 s, then ESP.restart(). Replace
  // it to restart from loop() after saving state.
  void onRestart(VoidFn fn);
  // The self-test failed and the device is about to reboot into the
  // previous build (e.g. save state, show a message). Runs on loop().
  void onRollback(RollbackFn fn);

  // Call once from setup(), after your logger is ready. Returns false when
  // the config is incomplete (updates then always fail with a message).
  bool begin(const SKLOtaConfig& config);
  // Call every loop() pass. Cheap when there's nothing to do.
  void loop();

  // --- actions (safe from any task: web handlers, console, buttons) ----------
  void requestCheck();
  void requestInstall();  // checks first if nothing verified is on offer

  // --- status ------------------------------------------------------------------
  SKLOtaState state() const { return state_; }
  const char* stateName() const;      // "idle", "checking", "up_to_date", "available", "installing", "failed"
  const char* message() const { return message_; }    // one line for a screen / status page
  int progress() const { return progress_; }          // install %, -1 when not installing
  bool busy() const { return busy_ || request_; }
  bool supported() const;             // false = single app slot, needs one USB flash
  bool pendingVerify() const { return pendingVerify_; }  // running a new build that hasn't passed yet
  const SKLOtaRelease& offer() const { return offer_; }  // valid when state() == Available
  const char* lastResult() const { return lastResult_; } // outcome of the last install, survives reboots
  uint32_t build() const { return cfg_.build; }

 private:
  static void taskEntry(void* arg);
  void doCheck();
  void doInstall();
  bool download(const char* url, uint32_t expectBytes, bool compressed, uint32_t& got, const char*& failWhy,
                void* hashCtx, char* msg, size_t msgLen);
  void fail(const char* why);
  void selfTestLoop();
  bool fetchManifest(SKLOtaRelease& m, bool& nonePublished, char* why, size_t whyLen);
  bool verifySignature(const SKLOtaRelease& m, char* why, size_t whyLen);
  void setMessage(SKLOtaState st, const char* msg);
  void saveResult(const char* msg);
  void clearInstallRecord();
  void postLog(int sev, const char* msg);  // from the update task
  void log(int sev, const char* msg);      // on loop()
  bool configured() const { return cfg_.publicKeyPem && cfg_.publicKeyPem[0] && cfg_.board && cfg_.board[0]; }

  SKLOtaConfig cfg_;
  String manifestUrl_;
  UrlFn manifestUrlFn_;
  LogFn logFn_;
  RequestFn requestFn_;
  SelfTestFn selfTestFn_;
  BoolFn networkReadyFn_;
  VoidFn changeFn_;
  VoidFn restartFn_;
  RollbackFn rollbackFn_;

  volatile SKLOtaState state_ = SKLOtaState::Idle;
  volatile int progress_ = -1;
  volatile bool busy_ = false;
  volatile uint8_t request_ = 0;  // 1 = check, 2 = check + install
  bool pendingVerify_ = false;
  bool reportPending_ = false;
  unsigned long lastCheckMs_ = 0;
  SKLOtaRelease offer_;
  SKLOtaRelease scratch_;
  char message_[64] = "";
  char lastResult_[64] = "";
  char logText_[160] = "";
  volatile int logSev_ = -1;
};

// The one instance, like Serial.
extern SKLOta Ota;
