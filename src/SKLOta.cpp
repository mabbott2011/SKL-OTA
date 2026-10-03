// Copyright (c) 2026 SKL (Martin Abbott)
// SPDX-License-Identifier: MIT
//
// SKL-OTA -- see SKLOta.h for how an update flows.
#include "SKLOta.h"
#include "SKLOtaInflate.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <new>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>

SKLOta Ota;

// Take over from the Arduino core's automatic "mark valid on boot": with
// rollback enabled (CONFIG_APP_ROLLBACK_ENABLE), returning true leaves a
// freshly updated image in PENDING_VERIFY until the self-test decides.
extern "C" bool verifyRollbackLater() { return true; }

namespace {

// How long after selfTestTimeoutMs the hang guard waits before forcing the
// rollback. While loop() is alive, selfTestLoop() handles the timeout itself
// (logging it and calling onRollback()); the guard is only the backstop for
// a build where loop() is stuck.
constexpr uint32_t GUARD_GRACE_MS = 30UL * 1000;

bool runningIsPendingVerify() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  return running && esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

// Runs on the esp_timer task, which a stuck loop() can't block. A restart
// while the image is still PENDING_VERIFY is all the bootloader needs: it
// marks the image aborted and boots the previous slot. Kept to that one
// call on purpose -- no logging, NVS or flash work from here.
void rollbackGuardFired(void*) {
  if (!esp_ota_check_rollback_is_possible()) return;  // nothing to go back to
  esp_restart();
}

// The slot an update would go into, or nullptr with a single-app layout.
const esp_partition_t* nextSlot() {
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  if (!next || next == esp_ota_get_running_partition()) return nullptr;
  return next;
}

bool isHex(const char* s, size_t minLen, size_t maxLen) {
  size_t n = strlen(s);
  if (n < minLen || n > maxLen || (n % 2)) return false;
  for (size_t i = 0; i < n; i++)
    if (!isxdigit((unsigned char)s[i])) return false;
  return true;
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool hexToBytes(const char* in, uint8_t* out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    int hi = hexNibble(in[2 * i]), lo = hexNibble(in[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

void bytesToHexLower(const uint8_t* in, size_t n, char* out) {
  static const char* digits = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = digits[in[i] >> 4];
    out[2 * i + 1] = digits[in[i] & 0x0F];
  }
  out[2 * n] = '\0';
}

// "/fw/x.bin" relative to the manifest's own origin -> absolute URL
void resolveUrl(const char* manifestUrl, const char* url, char* out, size_t len) {
  if (url[0] != '/') { snprintf(out, len, "%s", url); return; }
  const char* p = strstr(manifestUrl, "://");
  size_t originLen = strlen(manifestUrl);
  if (p) {
    const char* slash = strchr(p + 3, '/');
    if (slash) originLen = slash - manifestUrl;
  }
  snprintf(out, len, "%.*s%s", (int)originLen, manifestUrl, url);
}

// The data partition a filesystem image is written to (what Update's
// U_SPIFFS writes): the SPIFFS/LittleFS partition, else a FAT one.
const esp_partition_t* filesystemPartition() {
  const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  if (!p) p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, nullptr);
  return p;
}

}  // namespace

// ---- setup -----------------------------------------------------------------
void SKLOta::setManifestUrl(const char* url) { manifestUrl_ = url ? url : ""; manifestUrlFn_ = nullptr; }
void SKLOta::setManifestUrl(UrlFn fn) { manifestUrlFn_ = fn; }
void SKLOta::onLog(LogFn fn) { logFn_ = fn; }
void SKLOta::onRequest(RequestFn fn) { requestFn_ = fn; }
void SKLOta::onSelfTest(SelfTestFn fn) { selfTestFn_ = fn; }
void SKLOta::onNetworkReady(BoolFn fn) { networkReadyFn_ = fn; }
void SKLOta::onChange(VoidFn fn) { changeFn_ = fn; }
void SKLOta::onRestart(VoidFn fn) { restartFn_ = fn; }
void SKLOta::onRollback(RollbackFn fn) { rollbackFn_ = fn; }
void SKLOta::onFilesystemUpdate(FilesystemFn fn) { filesystemFn_ = fn; }

void SKLOta::requestCheck() { if (request_ < 1) request_ = 1; }
void SKLOta::requestInstall() { request_ = 2; }

bool SKLOta::supported() const { return nextSlot() != nullptr; }

const char* SKLOta::stateName() const {
  switch (state_) {
    case SKLOtaState::Checking:   return "checking";
    case SKLOtaState::UpToDate:   return "up_to_date";
    case SKLOtaState::Available:  return "available";
    case SKLOtaState::Installing: return "installing";
    case SKLOtaState::Failed:     return "failed";
    default:                      return "idle";
  }
}

// ---- logging / state ---------------------------------------------------------
void SKLOta::log(int sev, const char* msg) {
  if (logFn_) logFn_(sev, msg);
}

// The update task leaves one message at a time for loop() to deliver.
void SKLOta::postLog(int sev, const char* msg) {
  for (int i = 0; i < 50 && logSev_ >= 0; i++) vTaskDelay(pdMS_TO_TICKS(10));  // previous one still queued
  strncpy(logText_, msg, sizeof(logText_) - 1);
  logText_[sizeof(logText_) - 1] = '\0';
  logSev_ = sev;
}

void SKLOta::setMessage(SKLOtaState st, const char* msg) {
  strncpy(message_, msg, sizeof(message_) - 1);
  message_[sizeof(message_) - 1] = '\0';
  state_ = st;
  if (changeFn_) changeFn_();
}

// The outcome of an install, kept across reboots. "report" asks for a check
// soon (even after a restart), so a server that records results hears it.
void SKLOta::saveResult(const char* msg) {
  strncpy(lastResult_, msg, sizeof(lastResult_) - 1);
  lastResult_[sizeof(lastResult_) - 1] = '\0';
  reportPending_ = true;
  Preferences prefs;
  if (prefs.begin(cfg_.nvsNamespace, false)) {
    prefs.putString("result", lastResult_);
    prefs.putBool("report", true);
    prefs.end();
  }
}

void SKLOta::clearInstallRecord() {
  Preferences prefs;
  if (prefs.begin(cfg_.nvsNamespace, false)) {
    prefs.remove("to");
    prefs.remove("from");
    prefs.end();
  }
}

// ---- filesystem record (NVS) --------------------------------------------------------
//   fs_for   build the pending image belongs to     fs_busy  a write started, not finished
//   fs_url / fs_zurl / fs_size / fs_zsize / fs_sha  the image (signature checked before saving)
//   fs_done  SHA-256 of the image last written (so an unchanged image isn't rewritten)
void SKLOta::rememberFilesystem(const SKLOtaRelease& m) {
  Preferences prefs;
  if (!prefs.begin(cfg_.nvsNamespace, false)) return;
  String done = prefs.getString("fs_done", "");
  if (!m.fs.size || !cfg_.updateFilesystem || done == m.fs.sha256) {
    prefs.remove("fs_for");  // nothing to write for this build
  } else {
    prefs.putULong("fs_for", m.build);
    prefs.putString("fs_url", m.fs.url);
    prefs.putString("fs_zurl", m.fs.zurl);
    prefs.putULong("fs_size", m.fs.size);
    prefs.putULong("fs_zsize", m.fs.zsize);
    prefs.putString("fs_sha", m.fs.sha256);
  }
  prefs.end();
}

void SKLOta::clearFilesystemRecord() {
  Preferences prefs;
  if (prefs.begin(cfg_.nvsNamespace, false)) {
    for (const char* k : {"fs_for", "fs_url", "fs_zurl", "fs_size", "fs_zsize", "fs_sha"}) prefs.remove(k);
    prefs.end();
  }
}

// ---- manifest ------------------------------------------------------------------
static bool parseImageUrls(JsonVariantConst o, const char* manifestUrl, char* url, size_t urlLen, char* zurl,
                           size_t zurlLen, uint32_t& zsize) {
  const char* u = o["url"] | "";
  if (!u[0]) return false;
  resolveUrl(manifestUrl, u, url, urlLen);
  if (strncasecmp(url, "http", 4) != 0) return false;
  // Optional zlib-compressed copy of the same image. Not signed itself: what
  // it inflates to must still match the signed size and SHA-256.
  zurl[0] = '\0';
  zsize = 0;
  const char* comp = o["compression"] | "";
  const char* z = o["compressed_url"] | "";
  uint32_t zs = o["compressed_size"] | 0UL;
  if (strcmp(comp, "zlib") == 0 && z[0] && zs) {
    resolveUrl(manifestUrl, z, zurl, zurlLen);
    if (strncasecmp(zurl, "http", 4) == 0) zsize = zs;
    else zurl[0] = '\0';
  }
  return true;
}

bool SKLOta::fetchManifest(SKLOtaRelease& m, bool& nonePublished, char* why, size_t whyLen) {
  nonePublished = false;
  static char url[224];
  String u = manifestUrlFn_ ? manifestUrlFn_() : manifestUrl_;
  snprintf(url, sizeof(url), "%s", u.c_str());
  if (!url[0]) { snprintf(why, whyLen, "No update URL set"); return false; }
  HTTPClient http;
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(url)) { snprintf(why, whyLen, "Bad update URL"); return false; }
  if (requestFn_) requestFn_(http, url);
  int code = http.GET();
  if (code == 404 || code == 204) {
    http.end();
    nonePublished = true;
    snprintf(why, whyLen, "No updates published");
    return false;
  }
  if (code != 200) {
    http.end();
    if (code < 0) snprintf(why, whyLen, "Can't reach update server");
    else snprintf(why, whyLen, "Update server error (HTTP %d)", code);
    return false;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) { snprintf(why, whyLen, "Update info unreadable"); return false; }

  const char* board = doc["board"] | "";
  const char* sha = doc["sha256"] | "";
  const char* sig = doc["sig"] | "";
  const char* ver = doc["version"] | "";
  m.build = doc["build"] | 0UL;
  m.size = doc["size"] | 0UL;
  if (strcmp(board, cfg_.board) != 0) { snprintf(why, whyLen, "Update is for other hardware"); return false; }
  if (!m.build || !m.size || !isHex(sha, 64, 64) || !isHex(sig, 16, sizeof(m.sig) - 1) ||
      !parseImageUrls(doc.as<JsonVariantConst>(), url, m.url, sizeof(m.url), m.zurl, sizeof(m.zurl), m.zsize)) {
    snprintf(why, whyLen, "Update info incomplete");
    return false;
  }
  snprintf(m.sha256, sizeof(m.sha256), "%s", sha);
  for (char* c = m.sha256; *c; c++) *c = tolower(*c);
  snprintf(m.sig, sizeof(m.sig), "%s", sig);
  snprintf(m.version, sizeof(m.version), "%s", ver);

  // Optional filesystem image for this build.
  m.fs = SKLOtaImage();
  JsonVariantConst fs = doc["fs"];
  if (!fs.isNull()) {
    const char* fsha = fs["sha256"] | "";
    const char* fsig = fs["sig"] | "";
    m.fs.size = fs["size"] | 0UL;
    if (!m.fs.size || !isHex(fsha, 64, 64) || !isHex(fsig, 16, sizeof(m.fs.sig) - 1) ||
        !parseImageUrls(fs, url, m.fs.url, sizeof(m.fs.url), m.fs.zurl, sizeof(m.fs.zurl), m.fs.zsize)) {
      snprintf(why, whyLen, "Filesystem info incomplete");
      return false;
    }
    snprintf(m.fs.sha256, sizeof(m.fs.sha256), "%s", fsha);
    for (char* c = m.fs.sha256; *c; c++) *c = tolower(*c);
    snprintf(m.fs.sig, sizeof(m.fs.sig), "%s", fsig);
  }
  return true;
}

// ---- signature -----------------------------------------------------------------
bool SKLOta::verifySignature(const char* contextSuffix, uint32_t build, uint32_t size, const char* sha256,
                             const char* sigHex, char* why, size_t whyLen) {
  if (!cfg_.publicKeyPem || !cfg_.publicKeyPem[0]) {
    snprintf(why, whyLen, "No signing key in this build");
    return false;
  }
  char msg[220];
  snprintf(msg, sizeof(msg), "%s%s|%s|%lu|%lu|%s", cfg_.signContext, contextSuffix, cfg_.board,
           (unsigned long)build, (unsigned long)size, sha256);
  uint8_t hash[32];
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md || mbedtls_md(md, (const unsigned char*)msg, strlen(msg), hash) != 0) {
    snprintf(why, whyLen, "Hash error");
    return false;
  }
  uint8_t sig[80];
  size_t sigLen = strlen(sigHex) / 2;
  if (sigLen > sizeof(sig) || !hexToBytes(sigHex, sig, sigLen)) {
    snprintf(why, whyLen, "Signature malformed");
    return false;
  }
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  int rc = mbedtls_pk_parse_public_key(&pk, (const unsigned char*)cfg_.publicKeyPem, strlen(cfg_.publicKeyPem) + 1);
  if (rc == 0) rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sigLen);
  mbedtls_pk_free(&pk);
  if (rc != 0) {
    snprintf(why, whyLen, contextSuffix[0] ? "Filesystem signature check FAILED" : "Signature check FAILED");
    return false;
  }
  return true;
}

// ---- check -------------------------------------------------------------------------
void SKLOta::doCheck() {
  lastCheckMs_ = millis();
  setMessage(SKLOtaState::Checking, "Checking...");
  if (WiFi.status() != WL_CONNECTED) { setMessage(SKLOtaState::Failed, "No WiFi"); return; }
  const esp_partition_t* next = nextSlot();
  if (!next) {
    setMessage(SKLOtaState::Failed, "Needs USB flash w/ OTA layout");
    postLog(2, "Update check: single app slot -- flash once over USB with a two-slot (OTA) partition table");
    return;
  }
  SKLOtaRelease* mp = new (std::nothrow) SKLOtaRelease();
  if (!mp) { setMessage(SKLOtaState::Failed, "Out of memory"); return; }
  SKLOtaRelease& m = *mp;
  bool none = false;
  char why[64];
  char msg[160];
  bool fetched = fetchManifest(m, none, why, sizeof(why));
  if (fetched || none) {  // the server heard this check (and any result headers)
    Preferences prefs;
    if (prefs.begin(cfg_.nvsNamespace, false)) { prefs.remove("report"); prefs.end(); }
  }
  if (!fetched) {
    setMessage(none ? SKLOtaState::UpToDate : SKLOtaState::Failed, why);
    if (!none) {
      snprintf(msg, sizeof(msg), "Update check failed: %s", why);
      postLog(2, msg);
    }
  } else if (m.build <= cfg_.build) {
    snprintf(msg, sizeof(msg), "Up to date (build %lu)", (unsigned long)cfg_.build);
    setMessage(SKLOtaState::UpToDate, msg);
  } else if (m.size > next->size) {
    setMessage(SKLOtaState::Failed, "Update too big for slot");
    snprintf(msg, sizeof(msg), "Update build %lu is %lu bytes; the update slot holds %lu",
             (unsigned long)m.build, (unsigned long)m.size, (unsigned long)next->size);
    postLog(1, msg);
  } else if (!verifySignature("", m.build, m.size, m.sha256, m.sig, why, sizeof(why))) {
    setMessage(SKLOtaState::Failed, why);
    snprintf(msg, sizeof(msg), "Update build %lu rejected: %s", (unsigned long)m.build, why);
    postLog(1, msg);
  } else {
    bool ok = true;
    if (m.fs.size && !cfg_.updateFilesystem) {
      m.fs = SKLOtaImage();  // filesystem updates are off: install the app only
    } else if (m.fs.size) {
      // The pages/files must arrive with their build, so a release whose
      // filesystem image can't be used isn't installed at all.
      const esp_partition_t* fsPart = filesystemPartition();
      if (!fsPart || m.fs.size > fsPart->size) {
        snprintf(why, sizeof(why), fsPart ? "Filesystem image too big" : "No filesystem partition");
        ok = false;
      } else if (!verifySignature("-fs", m.build, m.fs.size, m.fs.sha256, m.fs.sig, why, sizeof(why))) {
        ok = false;
      }
      if (!ok) {
        setMessage(SKLOtaState::Failed, why);
        snprintf(msg, sizeof(msg), "Update build %lu rejected: %s", (unsigned long)m.build, why);
        postLog(1, msg);
      }
    }
    if (ok) {
      offer_ = m;
      snprintf(msg, sizeof(msg), "Build %lu available", (unsigned long)m.build);
      setMessage(SKLOtaState::Available, msg);
      snprintf(msg, sizeof(msg), "Update available: build %lu (%s), %lu bytes%s, signature OK",
               (unsigned long)m.build, m.version, (unsigned long)m.size, m.fs.size ? " + filesystem" : "");
      postLog(3, msg);
    }
  }
  delete mp;
}

// ---- install --------------------------------------------------------------------------
void SKLOta::fail(const char* why) {
  char msg[120];
  snprintf(msg, sizeof(msg), "Update failed: %s -- still running build %lu", why, (unsigned long)cfg_.build);
  postLog(1, msg);
  progress_ = -1;
  setMessage(SKLOtaState::Failed, why);
}

// Streams one URL into the open Update, hashing the image as it's written.
// compressed: the body is a zlib stream that inflates to the image.
bool SKLOta::download(const char* url, uint32_t expectBytes, bool compressed, uint32_t imageSize, uint32_t& got,
                      const char*& failWhy, void* hashCtx, char* msg, size_t msgLen) {
  mbedtls_md_context_t* ctx = (mbedtls_md_context_t*)hashCtx;
  got = 0;
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);  // e.g. GitHub release assets
  if (!http.begin(url)) { failWhy = "Bad download URL"; return false; }
  if (requestFn_) requestFn_(http, url);
  int code = http.GET();
  if (code != 200) {
    http.end();
    snprintf(msg, msgLen, "Download error (HTTP %d)", code);
    failWhy = msg;
    return false;
  }
  int len = http.getSize();
  if (len > 0 && (uint32_t)len != expectBytes) { http.end(); failWhy = "Download size mismatch"; return false; }

  // Every image byte, compressed download or not, goes through here.
  auto sink = [&](const uint8_t* data, size_t n) -> bool {
    if (got + n > imageSize) { failWhy = "Image larger than signed"; return false; }
    mbedtls_md_update(ctx, data, n);
    if (Update.write((uint8_t*)data, n) != n) { failWhy = "Flash write failed"; return false; }
    got += n;
    int pct = (int)((uint64_t)got * 100 / imageSize);
    if (pct != progress_) {
      progress_ = pct;
      if (changeFn_) changeFn_();
    }
    return true;
  };

  SKLOtaInflater inflater;
  if (compressed && !inflater.begin()) { http.end(); failWhy = "Out of memory"; return false; }
  const size_t BUF = 2048;
  uint8_t* buf = (uint8_t*)malloc(BUF);
  if (!buf) { http.end(); failWhy = "Out of memory"; return false; }
  WiFiClient* stream = http.getStreamPtr();
  uint32_t received = 0;
  unsigned long lastData = millis();
  bool ok = true, finished = false;
  while (ok && received < expectBytes) {
    size_t avail = stream->available();
    if (avail) {
      size_t want = min(min(avail, BUF), (size_t)(expectBytes - received));
      int n = stream->readBytes(buf, want);
      if (n <= 0) continue;
      received += n;
      lastData = millis();
      if (compressed) {
        SKLOtaInflater::Result r = inflater.feed(buf, n, received == expectBytes, sink);
        if (r == SKLOtaInflater::FAILED) {
          if (!failWhy) failWhy = "Compressed image corrupt";
          ok = false;
        } else if (r == SKLOtaInflater::DONE) {
          finished = true;
        }
      } else {
        ok = sink(buf, n);
      }
    } else {
      if (!http.connected()) { failWhy = "Connection dropped"; ok = false; break; }
      if (millis() - lastData > 20000) { failWhy = "Download stalled"; ok = false; break; }
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
  http.end();
  free(buf);
  inflater.end();
  if (ok && compressed && !finished) { failWhy = "Compressed image incomplete"; ok = false; }
  return ok;
}

// Writes one image (app slot: U_FLASH, filesystem: U_SPIFFS) and checks it
// against the signed SHA-256. Update.end() is the caller's (for U_FLASH it
// switches the boot slot).
bool SKLOta::writeImage(int command, uint32_t size, const char* url, const char* zurl, uint32_t zsize,
                        const char* sha256, const char*& failWhy, char* why, size_t whyLen) {
  failWhy = nullptr;
  if (!Update.begin(size, command)) {
    snprintf(why, whyLen, "Can't start update: %s", Update.errorString());
    failWhy = why;
    return false;
  }
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  if (mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0) != 0 || mbedtls_md_starts(&ctx) != 0) {
    mbedtls_md_free(&ctx);
    Update.abort();
    failWhy = "Out of memory";
    return false;
  }
  bool compressed = cfg_.allowCompressed && zurl[0] && zsize;
  uint32_t got = 0;
  bool ok = download(compressed ? zurl : url, compressed ? zsize : size, compressed, size, got, failWhy, &ctx,
                     why, whyLen);
  if (!ok && compressed && got == 0) {
    // Nothing written yet (e.g. the compressed file isn't there): fall back
    // to the plain image rather than fail the update.
    char msg[160];
    snprintf(msg, sizeof(msg), "Compressed download failed (%s) -- trying the uncompressed image", failWhy ? failWhy : "?");
    postLog(2, msg);
    mbedtls_md_starts(&ctx);
    failWhy = nullptr;
    ok = download(url, size, false, size, got, failWhy, &ctx, why, whyLen);
  }
  uint8_t hash[32];
  bool hashed = ok && mbedtls_md_finish(&ctx, hash) == 0;
  mbedtls_md_free(&ctx);
  if (!ok || !hashed || got != size) {
    Update.abort();
    if (!failWhy) failWhy = "Download incomplete";
    return false;
  }
  char hex[65];
  bytesToHexLower(hash, sizeof(hash), hex);
  if (strcmp(hex, sha256) != 0) {
    Update.abort();
    failWhy = "Checksum mismatch -- file corrupted";
    return false;
  }
  return true;
}

void SKLOta::doInstall() {
  if (state_ != SKLOtaState::Available) {
    doCheck();
    if (state_ != SKLOtaState::Available) return;
  }
  const SKLOtaRelease& m = offer_;
  bool compressed = cfg_.allowCompressed && m.zurl[0] && m.zsize;
  char msg[200];
  snprintf(msg, sizeof(msg), "Installing build %lu from %s%s", (unsigned long)m.build,
           compressed ? m.zurl : m.url, compressed ? " (compressed)" : "");
  postLog(3, msg);
  progress_ = 0;
  setMessage(SKLOtaState::Installing, "Downloading...");

  const char* failWhy = nullptr;
  char why[64];
  if (!writeImage(U_FLASH, m.size, m.url, m.zurl, m.zsize, m.sha256, failWhy, why, sizeof(why))) {
    fail(failWhy);
    return;
  }
  setMessage(SKLOtaState::Installing, "Verifying...");
  if (!Update.end()) {  // validates the image and points the bootloader at it
    snprintf(msg, sizeof(msg), "Image rejected: %s", Update.errorString());
    fail(msg);
    return;
  }

  Preferences prefs;
  if (prefs.begin(cfg_.nvsNamespace, false)) {
    prefs.putULong("from", cfg_.build);
    prefs.putULong("to", m.build);
    prefs.putString("to_ver", m.version);
    prefs.end();
  }
  rememberFilesystem(m);  // written by the new build once it passes its self-test
  progress_ = 100;
  snprintf(msg, sizeof(msg), "Update to build %lu written and checksum-verified -- restarting", (unsigned long)m.build);
  postLog(3, msg);
  setMessage(SKLOtaState::Installing, "Restarting...");
  if (restartFn_) {
    restartFn_();
  } else {
    vTaskDelay(pdMS_TO_TICKS(2000));  // let loop() deliver the log line
    ESP.restart();
  }
}

// Writes this build's filesystem image. loop() has already called
// onFilesystemUpdate(true), so nothing is using the filesystem.
void SKLOta::doFilesystemInstall() {
  fsOk_ = false;
  String url, zurl, sha;
  uint32_t size = 0, zsize = 0;
  Preferences prefs;
  if (prefs.begin(cfg_.nvsNamespace, false)) {
    url = prefs.getString("fs_url", "");
    zurl = prefs.getString("fs_zurl", "");
    sha = prefs.getString("fs_sha", "");
    size = prefs.getULong("fs_size", 0);
    zsize = prefs.getULong("fs_zsize", 0);
    prefs.putBool("fs_busy", true);  // cleared only when the image is fully written
    prefs.end();
  }
  char msg[200];
  if (!size || url.isEmpty() || sha.length() != 64) {
    postLog(1, "Filesystem update record is incomplete -- skipped");
    clearFilesystemRecord();
    fsPending_ = false;
    setMessage(SKLOtaState::Failed, "Files update skipped");
    return;
  }
  bool compressed = cfg_.allowCompressed && zurl.length() && zsize;
  snprintf(msg, sizeof(msg), "Writing the filesystem image for build %lu from %s%s", (unsigned long)cfg_.build,
           compressed ? zurl.c_str() : url.c_str(), compressed ? " (compressed)" : "");
  postLog(3, msg);
  progress_ = 0;
  setMessage(SKLOtaState::Installing, "Updating files");

  const char* failWhy = nullptr;
  char why[64];
  bool ok = writeImage(U_SPIFFS, size, url.c_str(), zurl.c_str(), zsize, sha.c_str(), failWhy, why, sizeof(why));
  if (ok && !Update.end()) {
    snprintf(why, sizeof(why), "Write not finished: %s", Update.errorString());
    failWhy = why;
    ok = false;
  }
  progress_ = -1;
  if (!ok) {
    snprintf(msg, sizeof(msg), "Filesystem update failed: %s -- will retry", failWhy ? failWhy : "?");
    postLog(1, msg);
    setMessage(SKLOtaState::Failed, "Files update failed");
    return;
  }
  if (prefs.begin(cfg_.nvsNamespace, false)) {
    prefs.putString("fs_done", sha);
    prefs.remove("fs_busy");
    prefs.end();
  }
  clearFilesystemRecord();
  fsPending_ = false;
  fsOk_ = true;
  snprintf(msg, sizeof(msg), "Updated to build %lu + files", (unsigned long)cfg_.build);
  saveResult(msg);
  setMessage(SKLOtaState::Idle, msg);
  snprintf(msg, sizeof(msg), "Filesystem image for build %lu written and checksum-verified", (unsigned long)cfg_.build);
  postLog(3, msg);
}

void SKLOta::taskEntry(void* arg) {
  uint8_t what = (uint8_t)(uintptr_t)arg;
  if (what == 3) Ota.doFilesystemInstall();
  else if (what == 2) Ota.doInstall();
  else Ota.doCheck();
  Ota.busy_ = false;
  vTaskDelete(nullptr);
}

// ---- boot / self-test --------------------------------------------------------------
bool SKLOta::armRollbackGuard(uint32_t selfTestTimeoutMs) {
  if (!runningIsPendingVerify()) return false;
  if (!guardTimer_) {
    esp_timer_create_args_t args = {};
    args.callback = rollbackGuardFired;
    args.name = "skl_ota_guard";
    if (esp_timer_create(&args, &guardTimer_) != ESP_OK) {
      guardTimer_ = nullptr;
      return false;
    }
  } else {
    esp_timer_stop(guardTimer_);  // re-arm with this deadline (fine if it wasn't running)
  }
  // The deadline counts from boot, the same clock selfTestLoop() uses.
  uint64_t deadlineMs = (uint64_t)selfTestTimeoutMs + GUARD_GRACE_MS;
  uint64_t upMs = millis();
  uint64_t leftMs = deadlineMs > upMs ? deadlineMs - upMs : 1000;
  return esp_timer_start_once(guardTimer_, leftMs * 1000ULL) == ESP_OK;
}

void SKLOta::disarmRollbackGuard() {
  if (!guardTimer_) return;
  esp_timer_stop(guardTimer_);
  esp_timer_delete(guardTimer_);
  guardTimer_ = nullptr;
}

bool SKLOta::begin(const SKLOtaConfig& config) {
  cfg_ = config;
  if (!cfg_.signContext) cfg_.signContext = "skl-ota";
  if (!cfg_.nvsNamespace) cfg_.nvsNamespace = "skl_ota";

  pendingVerify_ = runningIsPendingVerify();
  // Hang protection from here on (or re-armed with this config's timeout,
  // if armRollbackGuard() already ran earlier in setup()).
  if (pendingVerify_ && !armRollbackGuard(cfg_.selfTestTimeoutMs)) {
    log(2, "Couldn't start the rollback timer -- a new build that hangs won't be rolled back");
  }

  uint32_t from = 0, to = 0, fsFor = 0;
  bool fsBusy = false;
  String toVer;
  Preferences prefs;
  if (prefs.begin(cfg_.nvsNamespace, true)) {
    from = prefs.getULong("from", 0);
    to = prefs.getULong("to", 0);
    toVer = prefs.getString("to_ver", "");
    String r = prefs.getString("result", "");
    strncpy(lastResult_, r.c_str(), sizeof(lastResult_) - 1);
    reportPending_ = prefs.getBool("report", false);
    fsFor = prefs.getULong("fs_for", 0);
    fsBusy = prefs.getBool("fs_busy", false);
    prefs.end();
  }
  char msg[200];
  if (to && cfg_.build == to) {
    // Booted the new build; selfTestLoop() decides whether it stays.
    if (!pendingVerify_) {
      clearInstallRecord();  // already validated on an earlier boot
    } else {
      snprintf(msg, sizeof(msg), "Running new build %lu -- self-test started (%lu min to pass)",
               (unsigned long)to, (unsigned long)(cfg_.selfTestTimeoutMs / 60000UL));
      log(3, msg);
    }
  } else if (to && from && cfg_.build == from) {
    snprintf(msg, sizeof(msg), "Build %lu failed self-test; rolled back", (unsigned long)to);
    saveResult(msg);
    snprintf(msg, sizeof(msg), "Update to build %lu (%s) didn't pass its self-test (failed, crashed or stopped responding) -- the bootloader rolled back to build %lu",
             (unsigned long)to, toVer.c_str(), (unsigned long)from);
    log(1, msg);
    clearInstallRecord();
  } else if (to) {
    clearInstallRecord();  // a different build entirely (flashed over USB)
  }

  // A filesystem image waiting for this build (or one whose write was cut off)?
  if (fsFor && fsFor == cfg_.build && cfg_.updateFilesystem) {
    fsPending_ = true;
    if (fsBusy) log(2, "The last filesystem update was interrupted -- it will be written again");
  } else if (fsFor) {
    clearFilesystemRecord();  // belongs to a build that isn't running (rolled back, or reflashed)
  }

  if (pendingVerify_) setMessage(SKLOtaState::Idle, "Self-testing new build");
  if (!supported()) {
    log(2, "OTA updates unavailable: single app slot (flash once over USB with a two-slot partition table)");
  }
  if (!configured()) {
    log(1, "OTA updates disabled: SKLOtaConfig needs publicKeyPem and board");
    return false;
  }
  return true;
}

void SKLOta::selfTestLoop() {
  if (!pendingVerify_) return;
  unsigned long up = millis();
  String status;
  bool healthy = selfTestFn_ ? selfTestFn_(status) : true;

  if (healthy && up >= cfg_.selfTestMinUptimeMs) {
    disarmRollbackGuard();  // first, so it can't fire between here and "valid"
    esp_ota_mark_app_valid_cancel_rollback();
    pendingVerify_ = false;
    char msg[64];
    snprintf(msg, sizeof(msg), "Updated to build %lu", (unsigned long)cfg_.build);
    saveResult(msg);
    clearInstallRecord();
    setMessage(SKLOtaState::Idle, msg);
    char line[120];
    snprintf(line, sizeof(line), "Self-test passed -- build %lu is now permanent%s", (unsigned long)cfg_.build,
             fsPending_ ? "; writing its filesystem image next" : "");
    log(3, line);
    return;
  }
  if (up >= cfg_.selfTestTimeoutMs) {
    char msg[200];
    snprintf(msg, sizeof(msg), "Self-test failed after %lu min (%s) -- rolling back",
             (unsigned long)(cfg_.selfTestTimeoutMs / 60000UL), status.length() ? status.c_str() : "not healthy");
    log(1, msg);
    if (rollbackFn_) rollbackFn_(msg);
    esp_ota_mark_app_invalid_rollback_and_reboot();  // doesn't return when a previous build exists
    pendingVerify_ = false;                          // (nothing to go back to -- keep running)
    disarmRollbackGuard();
  }
}

void SKLOta::loop() {
  // A filesystem write just finished: hand the filesystem back.
  if (fsRunning_ && !busy_) {
    fsRunning_ = false;
    if (filesystemFn_) filesystemFn_(false, fsOk_);
    if (!fsOk_ && fsPending_) {
      // Retry after 1, 2, 4 ... min, at most hourly.
      uint32_t delayMs = 60000UL << (fsAttempts_ < 6 ? fsAttempts_ : 6);
      if (delayMs > 3600000UL) delayMs = 3600000UL;
      if (fsAttempts_ < 255) fsAttempts_++;
      fsNextTryMs_ = millis() + delayMs;
    }
  }
  if (logSev_ >= 0) {  // deliver the update task's message on this task
    log(logSev_, logText_);
    logSev_ = -1;
  }
  selfTestLoop();
  bool netReady = networkReadyFn_ ? networkReadyFn_() : WiFi.status() == WL_CONNECTED;

  // This build's filesystem image, once the build has proven itself.
  if (fsPending_ && !fsRunning_ && !pendingVerify_ && !busy_ && !request_ && netReady && configured() &&
      (long)(millis() - fsNextTryMs_) >= 0) {
    if (!filesystemFn_) {
      if (!fsHookWarned_) {
        log(2, "This build comes with a filesystem image, but onFilesystemUpdate() isn't set -- not written");
        fsHookWarned_ = true;
      }
    } else {
      filesystemFn_(true, false);
      fsRunning_ = true;
      busy_ = true;
      if (xTaskCreate(taskEntry, "skl_ota", cfg_.taskStack, (void*)(uintptr_t)3, 1, nullptr) != pdPASS) {
        busy_ = false;  // finished (failed) -- handed back above on the next pass
        fsOk_ = false;
        log(1, "Couldn't start the filesystem update task (out of memory)");
      }
      return;
    }
  }

  // After an install finished (or rolled back), check once right away, so a
  // server that records each device's build / result (see onRequest) hears
  // about it within a minute instead of at the next scheduled check.
  if (reportPending_ && !busy_ && !request_ && !fsRunning_ && netReady && configured()) {
    reportPending_ = false;
    request_ = 1;
  }

  // Automatic checks (never automatic installs): firstCheckMs after boot,
  // then every checkIntervalMs. Skipped while a new build is still proving
  // itself.
  if (cfg_.firstCheckMs && !busy_ && !request_ && !pendingVerify_ && netReady && configured() &&
      state_ != SKLOtaState::Available && supported()) {
    unsigned long now = millis();
    if ((lastCheckMs_ == 0 && now >= cfg_.firstCheckMs) ||
        (lastCheckMs_ != 0 && cfg_.checkIntervalMs && now - lastCheckMs_ >= cfg_.checkIntervalMs)) {
      request_ = 1;
    }
  }

  if (request_ && !busy_ && !fsRunning_) {
    uint8_t what = request_;
    request_ = 0;
    if (!configured()) {
      setMessage(SKLOtaState::Failed, "Updates not configured");
      return;
    }
    if (what == 2 && pendingVerify_) {
      log(2, "Update not started: the current build is still in its self-test");
      return;
    }
    busy_ = true;
    if (what == 2) setMessage(SKLOtaState::Installing, "Starting...");
    else setMessage(SKLOtaState::Checking, "Checking...");
    if (xTaskCreate(taskEntry, "skl_ota", cfg_.taskStack, (void*)(uintptr_t)what, 1, nullptr) != pdPASS) {
      busy_ = false;
      setMessage(SKLOtaState::Failed, "Out of memory");
      log(1, "Couldn't start the update task (out of memory)");
    }
  }
}
