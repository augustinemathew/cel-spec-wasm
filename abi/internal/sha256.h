// First-party SHA-256 (FIPS 180-4).  Exists because the repo has no
// crypto dependency and deliberately does not add one for the single
// digest the plugin surface needs (content-hashing plugin binaries;
// see doc/implementation-plan/rewrite/m35-plugin-ergonomics.md).
// Not a general crypto library: one-shot digest over a byte span,
// nothing else.

#ifndef CELWASM_ABI_INTERNAL_SHA256_H_
#define CELWASM_ABI_INTERNAL_SHA256_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "absl/types/span.h"

namespace celwasm {

// Byte length of a SHA-256 digest.
inline constexpr size_t kSha256DigestSize = 32;

// Computes the SHA-256 digest of `data` per FIPS 180-4.
std::array<uint8_t, kSha256DigestSize> Sha256(absl::Span<const uint8_t> data);

// The digest of `data` as 64 lowercase hex characters — the form
// embedder-facing hash strings present.
std::string Sha256Hex(absl::Span<const uint8_t> data);

}  // namespace celwasm

#endif  // CELWASM_ABI_INTERNAL_SHA256_H_
