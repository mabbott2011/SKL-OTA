// Copyright (c) 2026 SKL (Martin Abbott)
// SPDX-License-Identifier: MIT
//
// Host test for SKLOtaInflater (src/SKLOtaInflate.h), built against miniz's
// tinfl -- the same decompressor the ESP32 has in ROM. See run.sh.
//   inflate_test <image.bin> <image.bin.zz>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#define SKL_OTA_HOST_TEST
#include "../../src/SKLOtaInflate.h"

static int failures = 0;
static void check(const char* name, bool ok) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) failures++;
}

static std::vector<uint8_t> load(const char* path) {
  std::vector<uint8_t> v;
  FILE* f = fopen(path, "rb");
  if (!f) { perror(path); exit(2); }
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
  fclose(f);
  return v;
}

// Feeds `z` in chunks of random size (1..maxChunk), like a network stream.
static SKLOtaInflater::Result run(const std::vector<uint8_t>& z, size_t maxChunk, unsigned seed,
                                  std::vector<uint8_t>& out, size_t sinkLimit = SIZE_MAX) {
  SKLOtaInflater inf;
  if (!inf.begin()) return SKLOtaInflater::FAILED;
  std::mt19937 rng(seed);
  out.clear();
  size_t off = 0;
  SKLOtaInflater::Result r = SKLOtaInflater::NEED_MORE;
  auto sink = [&](const uint8_t* d, size_t n) {
    if (out.size() + n > sinkLimit) return false;
    out.insert(out.end(), d, d + n);
    return true;
  };
  while (off < z.size()) {
    size_t n = 1 + rng() % maxChunk;
    if (n > z.size() - off) n = z.size() - off;
    bool last = off + n == z.size();
    r = inf.feed(z.data() + off, n, last, sink);
    off += n;
    if (r == SKLOtaInflater::FAILED) return r;
  }
  return r;
}

int main(int argc, char** argv) {
  if (argc != 3) { fprintf(stderr, "usage: %s image.bin image.bin.zz\n", argv[0]); return 2; }
  std::vector<uint8_t> img = load(argv[1]), z = load(argv[2]);
  std::vector<uint8_t> out;

  check("whole stream in 2 KB reads -> DONE, identical image",
        run(z, 2048, 1, out) == SKLOtaInflater::DONE && out == img);
  check("tiny reads (1-7 bytes) -> identical image", run(z, 7, 2, out) == SKLOtaInflater::DONE && out == img);
  check("large reads (up to 64 KB) -> identical image", run(z, 65536, 3, out) == SKLOtaInflater::DONE && out == img);

  std::vector<uint8_t> cut(z.begin(), z.end() - 100);
  check("truncated stream -> FAILED (not DONE)", run(cut, 2048, 4, out) == SKLOtaInflater::FAILED);

  std::vector<uint8_t> flipped = z;
  flipped[flipped.size() / 2] ^= 0x40;
  SKLOtaInflater::Result r = run(flipped, 2048, 5, out);
  check("one corrupted byte -> FAILED, or output differs (the SHA-256 check then refuses it)",
        r == SKLOtaInflater::FAILED || out != img);

  std::vector<uint8_t> badSum = z;
  badSum[badSum.size() - 1] ^= 0x01;  // last byte of the Adler-32
  check("bad Adler-32 -> FAILED", run(badSum, 2048, 6, out) == SKLOtaInflater::FAILED);

  std::vector<uint8_t> trailing = z;
  trailing.insert(trailing.end(), {1, 2, 3, 4});
  check("bytes after the end of the stream -> FAILED", run(trailing, 2048, 7, out) == SKLOtaInflater::FAILED);

  check("sink refusing (flash write failed) -> FAILED", run(z, 2048, 8, out, 100000) == SKLOtaInflater::FAILED);

  std::vector<uint8_t> junk(4096);
  std::mt19937 rng(9);
  for (auto& b : junk) b = rng();
  check("random bytes -> FAILED", run(junk, 2048, 9, out) == SKLOtaInflater::FAILED);

  printf("%s (%d failures)\n", failures ? "SOME FAILED" : "ALL PASS", failures);
  return failures ? 1 : 0;
}
