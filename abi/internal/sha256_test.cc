// Tests for the first-party SHA-256 against published FIPS 180-4 /
// NIST vectors.  Every digest constant below is copied verbatim from
// a published source (cited per case) — never computed by the
// implementation under test.

#include "abi/internal/sha256.h"

#include <cstdint>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

absl::Span<const uint8_t> AsBytes(absl::string_view s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

std::string HexOf(absl::string_view s) {
  return Sha256Hex(AsBytes(s));
}

// NIST CAVP SHA256ShortMsg.rsp, Len = 0 (the empty-message digest;
// also RFC 6234 / de-facto universal).
TEST(Sha256Test, EmptyString) {
  EXPECT_EQ(HexOf(""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

// FIPS 180-2/180-4 example B.1: one-block message "abc".
TEST(Sha256Test, Abc) {
  EXPECT_EQ(HexOf("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

// FIPS 180-2/180-4 example B.2: two-block message (448 bits — the
// padding boundary where 0x80 + the 64-bit length no longer fit in
// the final block, forcing a second block).
TEST(Sha256Test, TwoBlockMessage) {
  EXPECT_EQ(HexOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

// RFC 6234 §8.5 TEST4 / NIST 896-bit message (four blocks of
// schedule expansion; exercises multi-block chaining).
TEST(Sha256Test, FourBlock896BitMessage) {
  EXPECT_EQ(HexOf("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                  "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
            "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

// FIPS 180-2/180-4 example B.3: one million repetitions of 'a'
// (10^6 bytes = 1 MB) — the published large-input vector.
TEST(Sha256Test, OneMillionA) {
  const std::string input(1000000, 'a');
  EXPECT_EQ(HexOf(input),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// Padding boundaries around the 55/56-byte cliff (55 bytes is the
// longest message whose 0x80 + 64-bit length padding fits in one
// block) and the 63/64/65-byte block edge.  Digests cross-checked
// against `shasum -a 256` (Perl Digest::SHA, macOS), which itself
// reproduces the FIPS vectors above.
struct BoundaryCase {
  size_t len;
  absl::string_view digest;
};

class Sha256BoundaryTest : public ::testing::TestWithParam<BoundaryCase> {};

TEST_P(Sha256BoundaryTest, RepeatedAOfBoundaryLength) {
  const std::string input(GetParam().len, 'a');
  EXPECT_EQ(HexOf(input), GetParam().digest);
}

INSTANTIATE_TEST_SUITE_P(
    PaddingCliffs, Sha256BoundaryTest,
    ::testing::Values(
        BoundaryCase{
            55,
            "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        BoundaryCase{
            56,
            "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        BoundaryCase{
            63,
            "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        BoundaryCase{
            64,
            "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        BoundaryCase{65,
                     "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f1"
                     "7eb0ae0"}));

// The digest bytes returned by Sha256() match Sha256Hex()'s encoding
// (pins the two entry points to one another and the array form).
TEST(Sha256Test, DigestBytesMatchHexForm) {
  const auto digest = Sha256(AsBytes("abc"));
  ASSERT_EQ(digest.size(), kSha256DigestSize);
  EXPECT_EQ(digest[0], 0xba);
  EXPECT_EQ(digest[1], 0x78);
  EXPECT_EQ(digest[31], 0xad);
}

// Determinism: same input, same digest across calls.
TEST(Sha256Test, Deterministic) {
  EXPECT_EQ(HexOf("celwasm"), HexOf("celwasm"));
}

// Distinct inputs produce distinct digests (smoke — not a
// cryptographic claim).
TEST(Sha256Test, DistinctInputsDistinctDigests) {
  EXPECT_NE(HexOf("abc"), HexOf("abd"));
  EXPECT_NE(HexOf(""), HexOf(absl::string_view("\0", 1)));
}

// Embedded NUL bytes are hashed, not treated as terminators.
TEST(Sha256Test, EmbeddedNulBytes) {
  const std::vector<uint8_t> with_nul = {'a', 0x00, 'b'};
  const std::vector<uint8_t> without = {'a', 'b'};
  EXPECT_NE(Sha256Hex(with_nul), Sha256Hex(without));
}

}  // namespace
}  // namespace celwasm
