// Thin wrapper over BoringSSL's one-shot SHA256 (FIPS 180-4).
// Correctness is pinned by the published-vector suite in
// sha256_test.cc, which exercises the wrapper end-to-end.

#include "abi/internal/sha256.h"

#include <array>
#include <cstdint>
#include <string>

#include "absl/strings/escaping.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "openssl/sha.h"

namespace celwasm {

std::array<uint8_t, kSha256DigestSize> Sha256(absl::Span<const uint8_t> data) {
  static_assert(kSha256DigestSize == SHA256_DIGEST_LENGTH,
                "kSha256DigestSize must match BoringSSL's digest length");
  std::array<uint8_t, kSha256DigestSize> digest = {};
  SHA256(data.data(), data.size(), digest.data());
  return digest;
}

std::string Sha256Hex(absl::Span<const uint8_t> data) {
  const auto digest = Sha256(data);
  return absl::BytesToHexString(absl::string_view(
      reinterpret_cast<const char*>(digest.data()), digest.size()));
}

}  // namespace celwasm
