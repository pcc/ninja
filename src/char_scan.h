// Copyright 2026 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NINJA_CHAR_SCAN_H_
#define NINJA_CHAR_SCAN_H_

// SIMD helpers for finding characters in NUL-terminated buffers.
//
// The scanning functions read 16 bytes at a time, so the buffer must remain
// readable for at least kCharScanPadding bytes after the terminating NUL.
// Callers include '\0' in the set of characters to find, so scans never go
// past the end of the data.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// (MSVC defines _M_ARM64, not __aarch64__.)
#if (defined(__aarch64__) && defined(__ARM_NEON)) || defined(_M_ARM64)
#include <arm_neon.h>
#define NINJA_CHAR_SCAN_NEON 1
#ifdef _MSC_VER
#include <intrin.h>
#endif
#elif defined(__SSE2__) || defined(_M_X64) || \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define NINJA_CHAR_SCAN_SSE2 1
#ifdef _MSC_VER
#include <intrin.h>
#endif
#endif

/// The number of readable bytes required after the terminating NUL.
static const size_t kCharScanPadding = 16;

/// Return a pointer to the first character at or after \a p that is one of
/// \a Cs, which must include '\0'.
template <char... Cs>
inline const char* FindFirstOf(const char* p) {
#if defined(NINJA_CHAR_SCAN_NEON)
  for (;;) {
    uint8x16_t chars = vld1q_u8(reinterpret_cast<const uint8_t*>(p));
    uint8x16_t mask = vdupq_n_u8(0);
    ((mask = vorrq_u8(mask, vceqq_u8(chars, vdupq_n_u8(Cs)))), ...);
    // Narrow each 8-bit lane of the mask to 4 bits.
    uint64_t bits = vget_lane_u64(
        vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(mask), 4)), 0);
    if (bits) {
#ifdef _MSC_VER
      unsigned long index;
      _BitScanForward64(&index, bits);
      return p + (index >> 2);
#else
      return p + (__builtin_ctzll(bits) >> 2);
#endif
    }
    p += 16;
  }
#elif defined(NINJA_CHAR_SCAN_SSE2)
  for (;;) {
    __m128i chars = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    __m128i mask = _mm_setzero_si128();
    ((mask = _mm_or_si128(mask, _mm_cmpeq_epi8(chars, _mm_set1_epi8(Cs)))),
     ...);
    unsigned bits = static_cast<unsigned>(_mm_movemask_epi8(mask));
    if (bits) {
#ifdef _MSC_VER
      unsigned long index;
      _BitScanForward(&index, bits);
      return p + index;
#else
      return p + __builtin_ctz(bits);
#endif
    }
    p += 16;
  }
#else
  for (;; ++p) {
    char c = *p;
    if (((c == Cs) || ...))
      return p;
  }
#endif
}

#endif  // NINJA_CHAR_SCAN_H_
