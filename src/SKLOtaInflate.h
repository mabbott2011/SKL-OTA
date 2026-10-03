// Copyright (c) 2026 SKL (Martin Abbott)
// SPDX-License-Identifier: MIT
//
// Streaming zlib inflater for compressed OTA downloads (internal to SKL-OTA).
// Uses the tinfl decompressor that's already in the ESP32's ROM, so it adds
// no code size -- only RAM while an install runs: the 32 KB window tinfl
// needs plus its ~11 KB state, both freed afterwards.
//
// Feed it the compressed bytes as they arrive; it hands each run of
// decompressed bytes to `sink` (write to flash + hash). The host tests build
// this same file against miniz (SKL_OTA_HOST_TEST).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#ifdef SKL_OTA_HOST_TEST
#include "miniz.h"
#else
#include "rom/miniz.h"
#endif

class SKLOtaInflater {
 public:
  enum Result { NEED_MORE, DONE, FAILED };

  ~SKLOtaInflater() { end(); }

  bool begin() {
    end();
    state_ = (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
    window_ = (uint8_t*)malloc(TINFL_LZ_DICT_SIZE);
    if (!state_ || !window_) { end(); return false; }
    tinfl_init(state_);
    pos_ = 0;
    done_ = false;
    return true;
  }

  void end() {
    free(state_); state_ = nullptr;
    free(window_); window_ = nullptr;
  }

  // `last` = no compressed bytes will follow this call. `sink(data, len)`
  // returns false to abort (e.g. a flash write failed). DONE means the zlib
  // stream ended cleanly, including its Adler-32 check.
  template <typename Sink>
  Result feed(const uint8_t* in, size_t len, bool last, Sink sink) {
    if (!state_ || !window_) return FAILED;
    if (done_) return len ? FAILED : DONE;  // bytes after the end of the stream
    for (;;) {
      size_t inBytes = len;
      size_t outBytes = TINFL_LZ_DICT_SIZE - pos_;
      mz_uint32 flags = TINFL_FLAG_PARSE_ZLIB_HEADER | (last ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
      tinfl_status st = tinfl_decompress(state_, in, &inBytes, window_, window_ + pos_, &outBytes, flags);
      if (inBytes == 0 && outBytes == 0 && len && st != TINFL_STATUS_DONE) return FAILED;  // no progress
      in += inBytes;
      len -= inBytes;
      if (outBytes) {
        if (!sink(window_ + pos_, outBytes)) return FAILED;
        pos_ = (pos_ + outBytes) & (TINFL_LZ_DICT_SIZE - 1);
      }
      if (st == TINFL_STATUS_DONE) {
        done_ = true;
        return len ? FAILED : DONE;
      }
      if (st < 0) return FAILED;                    // corrupt stream / bad checksum
      if (st == TINFL_STATUS_HAS_MORE_OUTPUT) continue;
      // NEEDS_MORE_INPUT
      if (len) continue;                            // it still has some of this chunk
      return last ? FAILED : NEED_MORE;             // truncated if that was the last chunk
    }
  }

 private:
  tinfl_decompressor* state_ = nullptr;
  uint8_t* window_ = nullptr;
  size_t pos_ = 0;
  bool done_ = false;
};
