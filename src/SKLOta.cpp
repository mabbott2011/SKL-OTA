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

void SKLOta::saveResult(const char* msg) {
  strncpy(lastResult_, msg, sizeof(lastResult_) - 1);
  lastResult_[sizeof(lastResult_) - 1] = '\0';
  Preferences prefs;
  if (prefs.begin(cfg_.nvsNamespace, false)) {
    prefs.putString("result", lastResult_);
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

// ---- manifest ------------------------------------------------------------------
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
  const char* fileUrl = doc["url"] | "";
  const char* sha = doc["sha256"] | "";
  const char* sig = doc["sig"] | "";
  const char* ver = doc["version"] | "";
  m.build = doc["build"] | 0UL;
  m.size = doc["size"] | 0UL;
  if (strcmp(board, cfg_.board) != 0) { snprintf(why, whyLen, "Update is for other hardware"); return false; }
  if (!m.build || !m.size || !fileUrl[0] || !isHex(sha, 64, 64) || !isHex(sig, 16, sizeof(m.sig) - 1)) {
    snprintf(why, whyLen, "Update info incomplete");
    return false;
  }
  resolveUrl(url, fileUrl, m.url, sizeof(m.url));
  if (strncasecmp(m.url, "http", 4) != 0) { snprintf(why, whyLen, "Update file URL invalid"); return false; }
  snprintf(m.sha256, sizeof(m.sha256), "%s", sha);
  for (char* c = m.sha256; *c; c++) *c = tolower(*c);
  snprintf(m.sig, sizeof(m.sig), "%s", sig);
  snprintf(m.version, sizeof(m.version), "%s", ver);

  // Optional zlib-compressed copy of the same image. Not signed itself: what
  // it inflates to must still match the signed size and SHA-256.
  m.zurl[0] = '\0';
  m.zsize = 0;
  const char* comp = doc["compression"] | "";
  const char* zurl = doc["compressed_url"] | "";
  uint32_t zsize = doc["compressed_size"] | 0UL;
  if (strcmp(comp, "zlib") == 0 && zurl[0] && zsize) {
    resolveUrl(url, zurl, m.zurl, sizeof(m.zurl));
    if (strncasecmp(m.zurl, "http", 4) == 0) m.zsize = zsize;
    else m.zurl[0] = '\0';
  }
  return true;
}

// ---- signature -----------------------------------------------------------------
bool SKLOta::verifySignature(const SKLOtaRelease& m, char* why, size_t whyLen) {
  if (!cfg_.publicKeyPem || !cfg_.publicKeyPem[0]) {
    snprintf(why, whyLen, "No signing key in this build");
    return false;
  }
  char msg[200];
  snprintf(msg, sizeof(msg), "%s|%s|%lu|%lu|%s", cfg_.signContext, cfg_.board,
           (unsigned long)m.build, (unsigned long)m.size, m.sha256);
  uint8_t hash[32];
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md || mbedtls_md(md, (const unsigned char*)msg, strlen(msg), hash) != 0) {
    snprintf(why, whyLen, "Hash error");
    return false;
  }
  uint8_t sig[80];
  size_t sigLen = strlen(m.sig) / 2;
  if (sigLen > sizeof(sig) || !hexToBytes(m.sig, sig, sigLen)) {
    snprintf(why, whyLen, "Signature malformed");
    return false;
  }
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  int rc = mbedtls_pk_parse_public_key(&pk, (const unsigned char*)cfg_.publicKeyPem, strlen(cfg_.publicKeyPem) + 1);
  if (rc == 0) rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sigLen);
  mbedtls_pk_free(&pk);
  if (rc != 0) {
    snprintf(why, whyLen, "Signature check FAILED");
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
  SKLOtaRelease& m = scratch_;
  bool none = false;
  char why[64];
  if (!fetchManifest(m, none, why, sizeof(why))) {
    setMessage(none ? SKLOtaState::UpToDate : SKLOtaState::Failed, why);
    if (!none) {
      char msg[120];
      snprintf(msg, sizeof(msg), "Update check failed: %s", why);
      postLog(2, msg);
    }
    return;
  }
  char msg[160];
  if (m.build <= cfg_.build) {
    snprintf(msg, sizeof(msg), "Up to date (build %lu)", (unsigned long)cfg_.build);
    setMessage(SKLOtaState::UpToDate, msg);
    return;
  }
  if (m.size > next->size) {
    setMessage(SKLOtaState::Failed, "Update too big for slot");
    snprintf(msg, sizeof(msg), "Update build %lu is %lu bytes; the update slot holds %lu",
             (unsigned long)m.build, (unsigned long)m.size, (unsigned long)next->size);
    postLog(1, msg);
    return;
  }
  if (!verifySignature(m, why, sizeof(why))) {
    setMessage(SKLOtaState::Failed, why);
    snprintf(msg, sizeof(msg), "Update build %lu rejected: %s", (unsigned long)m.build, why);
    postLog(1, msg);
    return;
  }
  offer_ = m;
  snprintf(msg, sizeof(msg), "Build %lu available", (unsigned long)m.build);
  setMessage(SKLOtaState::Available, msg);
  snprintf(msg, sizeof(msg), "Update available: build %lu (%s), %lu bytes, signature OK",
           (unsigned long)m.build, m.version, (unsigned long)m.size);
  postLog(3, msg);
}

// ---- install --------------------------------------------------------------------------
void SKLOta::fail(const char* why) {
  char msg[120];
  snprintf(msg, sizeof(msg), "Update failed: %s -- still running build %lu", why, (unsigned long)cfg_.build);
  postLog(1, msg);
  progress_ = -1;
  setMessage(SKLOtaState::Failed, why);
}

// Streams one URL into the update slot, hashing the image as it's written.
// compressed: the body is a zlib stream that inflates to the image.
bool SKLOta::download(const char* url, uint32_t expectBytes, bool compressed, uint32_t& got,
                      const char*& failWhy, void* hashCtx, char* msg, size_t msgLen) {
  mbedtls_md_context_t* ctx = (mbedtls_md_context_t*)hashCtx;
  const uint32_t imageSize = offer_.size;
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

  if (!Update.begin(m.size, U_FLASH)) {
    snprintf(msg, sizeof(msg), "Can't start update: %s", Update.errorString());
    fail(msg);
    return;
  }
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  if (mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0) != 0 || mbedtls_md_starts(&ctx) != 0) {
    mbedtls_md_free(&ctx);
    Update.abort();
    fail("Out of memory");
    return;
  }
  uint32_t got = 0;
  const char* failWhy = nullptr;
  char why[48];
  bool ok = download(compressed ? m.zurl : m.url, compressed ? m.zsize : m.size, compressed, got, failWhy, &ctx,
                     why, sizeof(why));
  if (!ok && compressed && got == 0) {
    // Nothing written yet (e.g. the compressed file isn't there): fall back
    // to the plain image rather than fail the update.
    snprintf(msg, sizeof(msg), "Compressed download failed (%s) -- trying the uncompressed image", failWhy ? failWhy : "?");
    postLog(2, msg);
    mbedtls_md_starts(&ctx);
    failWhy = nullptr;
    ok = download(m.url, m.size, false, got, failWhy, &ctx, why, sizeof(why));
  }
  uint8_t hash[32];
  bool hashed = ok && mbedtls_md_finish(&ctx, hash) == 0;
  mbedtls_md_free(&ctx);

  if (!ok || !hashed || got != m.size) {
    Update.abort();
    fail(failWhy ? failWhy : "Download incomplete");
    return;
  }
  char hex[65];
  bytesToHexLower(hash, sizeof(hash), hex);
  if (strcmp(hex, m.sha256) != 0) {
    Update.abort();
    fail("Checksum mismatch -- file corrupted");
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

void SKLOta::taskEntry(void* arg) {
  uint8_t what = (uint8_t)(uintptr_t)arg;
  if (what == 2) Ota.doInstall();
  else Ota.doCheck();
  Ota.busy_ = false;
  vTaskDelete(nullptr);
}

// ---- boot / self-test --------------------------------------------------------------
bool SKLOta::begin(const SKLOtaConfig& config) {
  cfg_ = config;
  if (!cfg_.signContext) cfg_.signContext = "skl-ota";
  if (!cfg_.nvsNamespace) cfg_.nvsNamespace = "skl_ota";

  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  pendingVerify_ = running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
                   st == ESP_OTA_IMG_PENDING_VERIFY;

  uint32_t from = 0, to = 0;
  String toVer;
  Preferences prefs;
  if (prefs.begin(cfg_.nvsNamespace, true)) {
    from = prefs.getULong("from", 0);
    to = prefs.getULong("to", 0);
    toVer = prefs.getString("to_ver", "");
    String r = prefs.getString("result", "");
    strncpy(lastResult_, r.c_str(), sizeof(lastResult_) - 1);
    prefs.end();
  }
  char msg[160];
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
    reportPending_ = true;
    snprintf(msg, sizeof(msg), "Update to build %lu (%s) didn't pass its self-test -- the bootloader rolled back to build %lu",
             (unsigned long)to, toVer.c_str(), (unsigned long)from);
    log(1, msg);
    clearInstallRecord();
  } else if (to) {
    clearInstallRecord();  // a different build entirely (flashed over USB)
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
    esp_ota_mark_app_valid_cancel_rollback();
    pendingVerify_ = false;
    char msg[64];
    snprintf(msg, sizeof(msg), "Updated to build %lu", (unsigned long)cfg_.build);
    saveResult(msg);
    clearInstallRecord();
    setMessage(SKLOtaState::Idle, msg);
    reportPending_ = true;
    char line[96];
    snprintf(line, sizeof(line), "Self-test passed -- build %lu is now permanent", (unsigned long)cfg_.build);
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
  }
}

void SKLOta::loop() {
  if (logSev_ >= 0) {  // deliver the update task's message on this task
    log(logSev_, logText_);
    logSev_ = -1;
  }
  selfTestLoop();
  bool netReady = networkReadyFn_ ? networkReadyFn_() : WiFi.status() == WL_CONNECTED;

  // After an update finished (or rolled back), check once right away, so a
  // server that records each device's build / result (see onRequest) hears
  // about it within a minute instead of at the next scheduled check.
  if (reportPending_ && !busy_ && !request_ && netReady && configured()) {
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

  if (request_ && !busy_) {
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
