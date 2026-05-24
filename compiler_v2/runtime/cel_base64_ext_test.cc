// M17 Slice A — unit tests for the `encoders` extension kernels
// (`cel_base64_encode_at_v` / `cel_base64_decode_at_v`).
//
// Exercises the kernels directly against the slot ABI via
// `StringExtFixture` (shared with the string_ext tests).  Scenario
// matrix per `m17-encoders-ext.md` §5.1: encode/decode happy paths +
// padding shapes, the unpadded-decode corpus case, decode errors,
// round-trip over a 256-byte blob, and the 3VL / kind-mismatch
// envelope over both kernels.

#include "compiler_v2/runtime/cel_base64_ext.h"

#include <string>

#include "compiler_v2/runtime/cel_data.h"
#include "compiler_v2/runtime/string_ext_test_helpers.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

using Base64Test = StringExtFixture;

// ──────────────────────────────────────────────────────────────
// encode — bytes -> string.
// ──────────────────────────────────────────────────────────────

TEST_F(Base64Test, EncodeHello) {
  // encoders_ext.textproto::encode/hello.
  uint32_t out = MakeOut();
  cel_base64_encode_at_v(out, MakeBytes("hello"));
  ExpectStr(out, "aGVsbG8=");
}

TEST_F(Base64Test, EncodeEmpty) {
  uint32_t out = MakeOut();
  cel_base64_encode_at_v(out, MakeBytes(""));
  ExpectStr(out, "");
}

TEST_F(Base64Test, EncodeHelloWorld) {
  uint32_t out = MakeOut();
  cel_base64_encode_at_v(out, MakeBytes("Hello World!"));
  ExpectStr(out, "SGVsbG8gV29ybGQh");
}

TEST_F(Base64Test, EncodePaddingShapes) {
  // 3-byte input → no '='; 2 bytes → one '='; 1 byte → two '='.
  uint32_t a = MakeOut();
  cel_base64_encode_at_v(a, MakeBytes("abc"));
  ExpectStr(a, "YWJj");
  uint32_t b = MakeOut();
  cel_base64_encode_at_v(b, MakeBytes("ab"));
  ExpectStr(b, "YWI=");
  uint32_t c = MakeOut();
  cel_base64_encode_at_v(c, MakeBytes("a"));
  ExpectStr(c, "YQ==");
}

TEST_F(Base64Test, EncodeRawHighBytes) {
  // Non-UTF-8 input bytes encode fine; output is ASCII base64.
  const std::string raw("\xff\xfe\xfd", 3);
  uint32_t out = MakeOut();
  cel_base64_encode_at_v(out, MakeBytes(raw));
  ExpectStr(out, "//79");
}

// ──────────────────────────────────────────────────────────────
// decode — string -> bytes.
// ──────────────────────────────────────────────────────────────

TEST_F(Base64Test, DecodeHello) {
  // encoders_ext.textproto::decode/hello.
  uint32_t out = MakeOut();
  cel_base64_decode_at_v(out, MakeStr("aGVsbG8="));
  ExpectBytes(out, "hello");
}

TEST_F(Base64Test, DecodeHelloWithoutPadding) {
  // encoders_ext.textproto::decode/hello_without_padding — the
  // load-bearing unpadded-input contract (§7 risk).
  uint32_t out = MakeOut();
  cel_base64_decode_at_v(out, MakeStr("aGVsbG8"));
  ExpectBytes(out, "hello");
}

TEST_F(Base64Test, DecodeEmpty) {
  uint32_t out = MakeOut();
  cel_base64_decode_at_v(out, MakeStr(""));
  ExpectBytes(out, "");
}

TEST_F(Base64Test, DecodeProducesNonUtf8Bytes) {
  // "//79" decodes to the raw 0xff 0xfe 0xfd byte sequence.
  uint32_t out = MakeOut();
  cel_base64_decode_at_v(out, MakeStr("//79"));
  ExpectBytes(out, std::string("\xff\xfe\xfd", 3));
}

TEST_F(Base64Test, DecodeInvalidAlphabet) {
  uint32_t out = MakeOut();
  cel_base64_decode_at_v(out, MakeStr("!!!!"));
  ExpectError(out, CEL_ERR_INVALID_ARGUMENT);
}

// ──────────────────────────────────────────────────────────────
// round trip — decode(encode(b)) == b.
// ──────────────────────────────────────────────────────────────

TEST_F(Base64Test, RoundTripHelloWorld) {
  // encoders_ext.textproto::round_trip/hello.
  uint32_t enc = MakeOut();
  cel_base64_encode_at_v(enc, MakeBytes("Hello World!"));
  std::string encoded = StringAt(enc);
  uint32_t dec = MakeOut();
  cel_base64_decode_at_v(dec, MakeStr(encoded.c_str()));
  ExpectBytes(dec, "Hello World!");
}

TEST_F(Base64Test, RoundTripAll256Bytes) {
  std::string blob;
  blob.reserve(256);
  for (int i = 0; i < 256; ++i) {
    blob.push_back(static_cast<char>(i));
  }
  uint32_t enc = MakeOut();
  cel_base64_encode_at_v(enc, MakeBytes(blob));
  std::string encoded = StringAt(enc);
  uint32_t dec = MakeOut();
  cel_base64_decode_at_v(dec, MakeStr(encoded.c_str()));
  ExpectBytes(dec, blob);
}

// ──────────────────────────────────────────────────────────────
// 3VL + kind-mismatch envelope over both kernels.
// ──────────────────────────────────────────────────────────────

TEST_F(Base64Test, EncodeAbsorbsError) {
  uint32_t out = MakeOut();
  cel_base64_encode_at_v(out, MakeError());
  ExpectError(out, CEL_ERR_DIVIDE_BY_ZERO);  // propagated verbatim.
}

TEST_F(Base64Test, EncodeAbsorbsUnknown) {
  uint32_t out = MakeOut();
  cel_base64_encode_at_v(out, MakeUnknown());
  ExpectKind(out, CEL_UNKNOWN);
}

TEST_F(Base64Test, EncodeRejectsNonBytes) {
  // encode wants CEL_BYTES; a CEL_STRING arg is a kind mismatch.
  uint32_t out = MakeOut();
  cel_base64_encode_at_v(out, MakeStr("hello"));
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

TEST_F(Base64Test, DecodeAbsorbsError) {
  uint32_t out = MakeOut();
  cel_base64_decode_at_v(out, MakeError());
  ExpectError(out, CEL_ERR_DIVIDE_BY_ZERO);
}

TEST_F(Base64Test, DecodeAbsorbsUnknown) {
  uint32_t out = MakeOut();
  cel_base64_decode_at_v(out, MakeUnknown());
  ExpectKind(out, CEL_UNKNOWN);
}

TEST_F(Base64Test, DecodeRejectsNonString) {
  // decode wants CEL_STRING; a CEL_BYTES arg is a kind mismatch.
  uint32_t out = MakeOut();
  cel_base64_decode_at_v(out, MakeBytes("hello"));
  ExpectError(out, CEL_ERR_TYPE_MISMATCH);
}

}  // namespace
}  // namespace celwasm
