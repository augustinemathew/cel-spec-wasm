// SHA-256 per FIPS 180-4 (§5.3.3 initial hash value, §4.2.2 round
// constants, §6.2 hash computation).  Straightforward one-shot
// implementation; correctness is pinned by the published-vector
// suite in sha256_test.cc.

#include "abi/internal/sha256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "absl/strings/escaping.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace celwasm {

namespace {

constexpr size_t kBlockSize = 64;

// FIPS 180-4 §4.2.2 — the first 32 bits of the fractional parts of
// the cube roots of the first 64 primes.
constexpr std::array<uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

// FIPS 180-4 §5.3.3 — the first 32 bits of the fractional parts of
// the square roots of the first 8 primes.
constexpr std::array<uint32_t, 8> kInitialState = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

uint32_t RotR(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

// §6.2.2 step 1: prepare the 64-word message schedule from a block.
void LoadSchedule(const uint8_t* block, std::array<uint32_t, 64>& w) {
  for (size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[4 * i]) << 24) |
           (static_cast<uint32_t>(block[(4 * i) + 1]) << 16) |
           (static_cast<uint32_t>(block[(4 * i) + 2]) << 8) |
           static_cast<uint32_t>(block[(4 * i) + 3]);
  }
  for (size_t i = 16; i < 64; ++i) {
    const uint32_t s0 =
        RotR(w[i - 15], 7) ^ RotR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 =
        RotR(w[i - 2], 17) ^ RotR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
}

// §6.2.2 steps 2-4: one compression round over a 64-byte block.
void ProcessBlock(const uint8_t* block, std::array<uint32_t, 8>& state) {
  std::array<uint32_t, 64> w = {};
  LoadSchedule(block, w);
  uint32_t a = state[0];
  uint32_t b = state[1];
  uint32_t c = state[2];
  uint32_t d = state[3];
  uint32_t e = state[4];
  uint32_t f = state[5];
  uint32_t g = state[6];
  uint32_t h = state[7];
  for (size_t i = 0; i < 64; ++i) {
    const uint32_t s1 = RotR(e, 6) ^ RotR(e, 11) ^ RotR(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = h + s1 + ch + kRoundConstants[i] + w[i];
    const uint32_t s0 = RotR(a, 2) ^ RotR(a, 13) ^ RotR(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

// §5.1.1 padding: 0x80, zeros, then the 64-bit big-endian bit
// length — one or two final blocks depending on the tail length.
void ProcessFinalBlocks(absl::Span<const uint8_t> tail, uint64_t total_bytes,
                        std::array<uint32_t, 8>& state) {
  std::array<uint8_t, 2 * kBlockSize> buf = {};
  if (!tail.empty()) std::memcpy(buf.data(), tail.data(), tail.size());
  buf[tail.size()] = 0x80;
  const size_t num_blocks = (tail.size() + 1 + 8 <= kBlockSize) ? 1 : 2;
  const uint64_t bit_len = total_bytes * 8;
  const size_t len_off = (num_blocks * kBlockSize) - 8;
  for (size_t i = 0; i < 8; ++i) {
    buf[len_off + i] = static_cast<uint8_t>(bit_len >> (56 - (8 * i)));
  }
  for (size_t b = 0; b < num_blocks; ++b) {
    ProcessBlock(buf.data() + (b * kBlockSize), state);
  }
}

}  // namespace

std::array<uint8_t, kSha256DigestSize> Sha256(absl::Span<const uint8_t> data) {
  std::array<uint32_t, 8> state = kInitialState;
  const size_t full_end = (data.size() / kBlockSize) * kBlockSize;
  for (size_t off = 0; off < full_end; off += kBlockSize) {
    ProcessBlock(data.data() + off, state);
  }
  ProcessFinalBlocks(data.subspan(full_end), data.size(), state);
  std::array<uint8_t, kSha256DigestSize> digest = {};
  for (size_t i = 0; i < state.size(); ++i) {
    digest[4 * i] = static_cast<uint8_t>(state[i] >> 24);
    digest[(4 * i) + 1] = static_cast<uint8_t>(state[i] >> 16);
    digest[(4 * i) + 2] = static_cast<uint8_t>(state[i] >> 8);
    digest[(4 * i) + 3] = static_cast<uint8_t>(state[i]);
  }
  return digest;
}

std::string Sha256Hex(absl::Span<const uint8_t> data) {
  const auto digest = Sha256(data);
  return absl::BytesToHexString(absl::string_view(
      reinterpret_cast<const char*>(digest.data()), digest.size()));
}

}  // namespace celwasm
