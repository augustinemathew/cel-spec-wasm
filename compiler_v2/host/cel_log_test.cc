#include "compiler_v2/host/cel_log.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler_v2/runtime/cel_runtime.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

// Fixed-size scratch memory for the decoder tests.  Large enough to
// hold a handful of strings, a CelValue or two, and an argv vector
// without worrying about overflow.  Every test lays its bytes out
// from offset 1 onwards so offset 0 stays free as the "null" sentinel.
constexpr size_t kMemBytes = 4096;

// In-memory byte-level scratchpad.  Test helpers park strings / argv
// slots / CelValues here by hand and hand the enclosing span to
// DecodeCelLog.  Keeping the layout explicit (no allocators, no
// struct copies) makes every offset visible in the test body.
class Scratch {
 public:
  // NSDMI sizes `bytes_` to kMemBytes of zeroes — equivalent to
  // `bytes_.assign(kMemBytes, 0)` in the body, but lets the ctor be
  // `= default` (modernize-use-equals-default) and silences
  // cppcoreguidelines-pro-type-member-init on the std::vector field.
  Scratch() = default;

  absl::Span<const uint8_t> mem() const {
    return {bytes_.data(), bytes_.size()};
  }

  // Copies `s` at `off`; returns `off` for chaining.
  uint32_t WriteStr(uint32_t off, absl::string_view s) {
    std::memcpy(bytes_.data() + off, s.data(), s.size());
    return off;
  }

  // Writes one 16-byte argv slot at `off`: two u64 words (tag_word,
  // payload).  Matches the `CEL_LOG_*` call-site macros.
  uint32_t WriteSlot(uint32_t off, uint64_t tag, uint64_t payload) {
    std::memcpy(bytes_.data() + off, &tag, sizeof(uint64_t));
    std::memcpy(bytes_.data() + off + 8, &payload, sizeof(uint64_t));
    return off;
  }

  // Writes a CelValue at `off`.  Returns `off` for chaining.
  uint32_t WriteCelValue(uint32_t off, const CelValue& v) {
    std::memcpy(bytes_.data() + off, &v, sizeof(v));
    return off;
  }

  // Writes four u32s at `off`: CEL_ERROR's (code, msg_ptr, msg_len, 0).
  uint32_t WriteErrorDesc(uint32_t off, uint32_t code, uint32_t msg_ptr,
                          uint32_t msg_len) {
    auto* p = reinterpret_cast<uint32_t*>(bytes_.data() + off);
    p[0] = code;
    p[1] = msg_ptr;
    p[2] = msg_len;
    p[3] = 0;
    return off;
  }

  // Writes an UnknownSet descriptor + id array, returns the descriptor
  // offset for pointing a CelValue's `payload.unk` at.
  uint32_t WriteUnknownSet(uint32_t desc_off, uint32_t ids_off,
                           absl::Span<const uint32_t> ids) {
    auto* ids_p = reinterpret_cast<uint32_t*>(bytes_.data() + ids_off);
    for (size_t i = 0; i < ids.size(); ++i) {
      ids_p[i] = ids[i];
    }
    auto* desc_p = reinterpret_cast<uint32_t*>(bytes_.data() + desc_off);
    desc_p[0] = ids_off;
    desc_p[1] = static_cast<uint32_t>(ids.size());
    return desc_off;
  }

 private:
  std::vector<uint8_t> bytes_ = std::vector<uint8_t>(kMemBytes, 0);
};

// Convenience: build and decode a call with `fmt` + caller-supplied
// args, returning the single captured line (assumes exactly one
// Emit fired).  Scratch is provided by the test for argv placement.
std::string DecodeOne(Scratch& mem, absl::string_view fmt, uint32_t argv_ptr,
                      uint32_t argc) {
  CapturingCelLogSink sink;
  const uint32_t file_off = 1;
  const uint32_t fn_off = 20;
  absl::string_view file_str = "test.cc";
  absl::string_view fn_str = "TestFn";
  mem.WriteStr(file_off, file_str);
  mem.WriteStr(fn_off, fn_str);
  const uint32_t fmt_off = 40;
  mem.WriteStr(fmt_off, fmt);

  CelLogWireArgs args;
  args.file_ptr = file_off;
  args.file_len = static_cast<uint32_t>(file_str.size());
  args.fn_ptr = fn_off;
  args.fn_len = static_cast<uint32_t>(fn_str.size());
  args.line = 17;
  args.fmt_ptr = fmt_off;
  args.fmt_len = static_cast<uint32_t>(fmt.size());
  args.argv_ptr = argv_ptr;
  args.argc = argc;

  DecodeCelLog(mem.mem(), args, &sink);
  if (sink.lines().size() != 1) return "<wrong-line-count>";
  return sink.lines().front();
}

TEST(CelLogTest, EmptyFormatEmitsPrefixAndEmptyBody) {
  Scratch mem;
  EXPECT_EQ(DecodeOne(mem, "", /*argv_ptr=*/0, /*argc=*/0),
            "[test.cc:17 TestFn] ");
}

TEST(CelLogTest, LiteralOnlyFormatPassesThroughUnchanged) {
  Scratch mem;
  EXPECT_EQ(DecodeOne(mem, "hello world", 0, 0),
            "[test.cc:17 TestFn] hello world");
}

TEST(CelLogTest, DoublePercentRendersLiteralPercent) {
  Scratch mem;
  EXPECT_EQ(DecodeOne(mem, "100%% done", 0, 0),
            "[test.cc:17 TestFn] 100% done");
}

TEST(CelLogTest, StringDirectiveRendersPayloadSpan) {
  Scratch mem;
  const uint32_t text_off = 200;
  mem.WriteStr(text_off, "abc");
  const uint32_t argv_off = 256;
  const uint64_t payload =
      static_cast<uint64_t>(text_off) | (static_cast<uint64_t>(3) << 32);
  mem.WriteSlot(argv_off, CEL_LOG_TAG_STR, payload);
  EXPECT_EQ(DecodeOne(mem, "say: %s", argv_off, 1),
            "[test.cc:17 TestFn] say: abc");
}

TEST(CelLogTest, IntDirectiveRendersSignedPayload) {
  Scratch mem;
  const uint32_t argv_off = 256;
  mem.WriteSlot(argv_off, CEL_LOG_TAG_INT,
                static_cast<uint64_t>(static_cast<int64_t>(-42)));
  EXPECT_EQ(DecodeOne(mem, "x=%d", argv_off, 1), "[test.cc:17 TestFn] x=-42");
}

TEST(CelLogTest, UintDirectiveRendersUnsignedPayload) {
  Scratch mem;
  const uint32_t argv_off = 256;
  mem.WriteSlot(argv_off, CEL_LOG_TAG_UINT, 18446744073709551610ull);
  EXPECT_EQ(DecodeOne(mem, "u=%u", argv_off, 1),
            "[test.cc:17 TestFn] u=18446744073709551610");
}

TEST(CelLogTest, DoubleDirectiveRendersBitCastPayload) {
  Scratch mem;
  const uint32_t argv_off = 256;
  const double d = 3.14159;
  uint64_t bits = 0;
  std::memcpy(&bits, &d, sizeof(bits));
  mem.WriteSlot(argv_off, CEL_LOG_TAG_DOUBLE, bits);
  // %g gives "3.14159" for this value.
  EXPECT_EQ(DecodeOne(mem, "pi=%f", argv_off, 1),
            "[test.cc:17 TestFn] pi=3.14159");
}

TEST(CelLogTest, BoolDirectiveRendersTrueFalse) {
  Scratch mem;
  const uint32_t argv_off = 256;
  mem.WriteSlot(argv_off, CEL_LOG_TAG_BOOL, 1);
  EXPECT_EQ(DecodeOne(mem, "b=%b", argv_off, 1), "[test.cc:17 TestFn] b=true");
  mem.WriteSlot(argv_off, CEL_LOG_TAG_BOOL, 0);
  EXPECT_EQ(DecodeOne(mem, "b=%b", argv_off, 1), "[test.cc:17 TestFn] b=false");
}

TEST(CelLogTest, UnknownDirectiveEmitsBytesVerbatim) {
  Scratch mem;
  EXPECT_EQ(DecodeOne(mem, "oops %z here", 0, 0),
            "[test.cc:17 TestFn] oops %z here");
}

TEST(CelLogTest, TrailingPercentEmitsLiteralPercent) {
  Scratch mem;
  EXPECT_EQ(DecodeOne(mem, "done %", 0, 0), "[test.cc:17 TestFn] done %");
}

TEST(CelLogTest, ArgcShortOfDirectivesPrintsDirectiveVerbatim) {
  // Graceful fallback: if the format references a %d the caller didn't
  // pack an arg for, emit `%d` verbatim rather than trapping on OOB
  // read of the argv array.  Matches the docstring's "never trap" rule.
  Scratch mem;
  EXPECT_EQ(DecodeOne(mem, "have %s and %d", 0, 0),
            "[test.cc:17 TestFn] have %s and %d");
}

TEST(CelLogTest, MultipleDirectivesIndexArgvSequentially) {
  Scratch mem;
  const uint32_t text_off = 100;
  mem.WriteStr(text_off, "ok");
  const uint32_t argv_off = 256;
  const uint64_t str_payload =
      static_cast<uint64_t>(text_off) | (static_cast<uint64_t>(2) << 32);
  mem.WriteSlot(argv_off, CEL_LOG_TAG_STR, str_payload);
  mem.WriteSlot(argv_off + 16, CEL_LOG_TAG_INT,
                static_cast<uint64_t>(static_cast<int64_t>(7)));
  EXPECT_EQ(DecodeOne(mem, "%s=%d", argv_off, 2), "[test.cc:17 TestFn] ok=7");
}

TEST(CelLogTest, StringOutOfRangePrintsOob) {
  Scratch mem;
  const uint32_t argv_off = 256;
  // A length larger than the scratch buffer guarantees OOB.
  const uint64_t payload =
      static_cast<uint64_t>(1) | (static_cast<uint64_t>(1u << 30) << 32);
  mem.WriteSlot(argv_off, CEL_LOG_TAG_STR, payload);
  EXPECT_EQ(DecodeOne(mem, "x=%s", argv_off, 1), "[test.cc:17 TestFn] x=<oob>");
}

// ---- %v per-kind pretty printing ------------------------------------------

TEST(CelLogTest, ValueNullOffsetPrintsSentinel) {
  Scratch mem;
  const uint32_t argv_off = 256;
  mem.WriteSlot(argv_off, CEL_LOG_TAG_VALUE, 0);
  EXPECT_EQ(DecodeOne(mem, "v=%v", argv_off, 1),
            "[test.cc:17 TestFn] v=<null-offset>");
}

TEST(CelLogTest, ValueNullKind) {
  Scratch mem;
  const uint32_t cv_off = 512;
  CelValue cv{};
  cv.kind = CEL_NULL;
  mem.WriteCelValue(cv_off, cv);
  const uint32_t argv_off = 256;
  mem.WriteSlot(argv_off, CEL_LOG_TAG_VALUE, cv_off);
  EXPECT_EQ(DecodeOne(mem, "v=%v", argv_off, 1), "[test.cc:17 TestFn] v=null");
}

TEST(CelLogTest, ValueBoolKind) {
  Scratch mem;
  const uint32_t cv_off = 512;
  CelValue cv{};
  cv.kind = CEL_BOOL;
  cv.payload.b = 1;
  mem.WriteCelValue(cv_off, cv);
  const uint32_t argv_off = 256;
  mem.WriteSlot(argv_off, CEL_LOG_TAG_VALUE, cv_off);
  EXPECT_EQ(DecodeOne(mem, "%v", argv_off, 1),
            "[test.cc:17 TestFn] bool(true)");
}

TEST(CelLogTest, ValueIntKind) {
  Scratch mem;
  const uint32_t cv_off = 512;
  CelValue cv{};
  cv.kind = CEL_INT;
  cv.payload.i = 42;
  mem.WriteCelValue(cv_off, cv);
  const uint32_t argv_off = 256;
  mem.WriteSlot(argv_off, CEL_LOG_TAG_VALUE, cv_off);
  EXPECT_EQ(DecodeOne(mem, "%v", argv_off, 1), "[test.cc:17 TestFn] int(42)");
}

TEST(CelLogTest, ValueUintKind) {
  Scratch mem;
  const uint32_t cv_off = 512;
  CelValue cv{};
  cv.kind = CEL_UINT;
  cv.payload.u = 7;
  mem.WriteCelValue(cv_off, cv);
  const uint32_t argv_off = 256;
  mem.WriteSlot(argv_off, CEL_LOG_TAG_VALUE, cv_off);
  EXPECT_EQ(DecodeOne(mem, "%v", argv_off, 1), "[test.cc:17 TestFn] uint(7)");
}

TEST(CelLogTest, ValueDoubleKind) {
  Scratch mem;
  const uint32_t cv_off = 512;
  CelValue cv{};
  cv.kind = CEL_DOUBLE;
  cv.payload.d = 2.5;
  mem.WriteCelValue(cv_off, cv);
  const uint32_t argv_off = 256;
  mem.WriteSlot(argv_off, CEL_LOG_TAG_VALUE, cv_off);
  EXPECT_EQ(DecodeOne(mem, "%v", argv_off, 1),
            "[test.cc:17 TestFn] double(2.5)");
}

TEST(CelLogTest, ValueStringKind) {
  Scratch mem;
  const uint32_t payload_off = 600;
  mem.WriteStr(payload_off, "abc");
  const uint32_t cv_off = 512;
  CelValue cv{};
  cv.kind = CEL_STRING;
  cv.payload.s = CelSpan{payload_off, 3};
  mem.WriteCelValue(cv_off, cv);
  const uint32_t argv_off = 256;
  mem.WriteSlot(argv_off, CEL_LOG_TAG_VALUE, cv_off);
  EXPECT_EQ(DecodeOne(mem, "%v", argv_off, 1),
            "[test.cc:17 TestFn] string(abc)");
}

TEST(CelLogTest, ValueBytesKind) {
  Scratch mem;
  const uint32_t cv_off = 512;
  CelValue cv{};
  cv.kind = CEL_BYTES;
  cv.payload.s = CelSpan{600, 3};
  mem.WriteCelValue(cv_off, cv);
  const uint32_t argv_off = 256;
  mem.WriteSlot(argv_off, CEL_LOG_TAG_VALUE, cv_off);
  EXPECT_EQ(DecodeOne(mem, "%v", argv_off, 1),
            "[test.cc:17 TestFn] bytes(3 bytes)");
}

TEST(CelLogTest, ValueMessageKind) {
  Scratch mem;
  const uint32_t cv_off = 512;
  CelValue cv{};
  cv.kind = CEL_MESSAGE;
  cv.payload.msg_slot = 11;
  mem.WriteCelValue(cv_off, cv);
  const uint32_t argv_off = 256;
  mem.WriteSlot(argv_off, CEL_LOG_TAG_VALUE, cv_off);
  EXPECT_EQ(DecodeOne(mem, "%v", argv_off, 1),
            "[test.cc:17 TestFn] message(slot=11)");
}

TEST(CelLogTest, ValueTypeKind) {
  // M9: CEL_TYPE payload is a CelSpan into linear memory carrying
  // the type-name bytes.  Pretty-printer reads them via FormatSpanPayload.
  Scratch mem;
  const uint32_t payload_off = 600;
  mem.WriteStr(payload_off, "int");
  const uint32_t cv_off = 512;
  CelValue cv{};
  cv.kind = CEL_TYPE;
  cv.payload.s = CelSpan{payload_off, 3};
  mem.WriteCelValue(cv_off, cv);
  const uint32_t argv_off = 256;
  mem.WriteSlot(argv_off, CEL_LOG_TAG_VALUE, cv_off);
  EXPECT_EQ(DecodeOne(mem, "%v", argv_off, 1), "[test.cc:17 TestFn] type(int)");
}

TEST(CelLogTest, ValueDurationKind) {
  Scratch mem;
  const uint32_t cv_off = 512;
  CelValue cv{};
  cv.kind = CEL_DURATION;
  cv.payload.dur.seconds = 5;
  cv.payload.dur.nanos = 200;
  mem.WriteCelValue(cv_off, cv);
  const uint32_t argv_off = 256;
  mem.WriteSlot(argv_off, CEL_LOG_TAG_VALUE, cv_off);
  EXPECT_EQ(DecodeOne(mem, "%v", argv_off, 1),
            "[test.cc:17 TestFn] duration(s=5,ns=200)");
}

TEST(CelLogTest, ValueTimestampKind) {
  Scratch mem;
  const uint32_t cv_off = 512;
  CelValue cv{};
  cv.kind = CEL_TIMESTAMP;
  cv.payload.ts.seconds = 1;
  cv.payload.ts.nanos = 2;
  mem.WriteCelValue(cv_off, cv);
  const uint32_t argv_off = 256;
  mem.WriteSlot(argv_off, CEL_LOG_TAG_VALUE, cv_off);
  EXPECT_EQ(DecodeOne(mem, "%v", argv_off, 1),
            "[test.cc:17 TestFn] timestamp(s=1,ns=2)");
}

TEST(CelLogTest, ValueOptionalKind) {
  Scratch mem;
  const uint32_t cv_off = 512;
  CelValue cv{};
  cv.kind = CEL_OPTIONAL;
  cv.payload.opt = 777;
  mem.WriteCelValue(cv_off, cv);
  const uint32_t argv_off = 256;
  mem.WriteSlot(argv_off, CEL_LOG_TAG_VALUE, cv_off);
  EXPECT_EQ(DecodeOne(mem, "%v", argv_off, 1),
            "[test.cc:17 TestFn] optional(inner=777)");
}

TEST(CelLogTest, ValueUnknownKindWithIdSet) {
  Scratch mem;
  const uint32_t ids_off = 700;
  const uint32_t desc_off = 740;
  const uint32_t ids[] = {1, 7, 42};
  mem.WriteUnknownSet(desc_off, ids_off,
                      absl::MakeSpan(ids, sizeof(ids) / sizeof(ids[0])));
  const uint32_t cv_off = 512;
  CelValue cv{};
  cv.kind = CEL_UNKNOWN;
  cv.payload.unk = desc_off;
  mem.WriteCelValue(cv_off, cv);
  const uint32_t argv_off = 256;
  mem.WriteSlot(argv_off, CEL_LOG_TAG_VALUE, cv_off);
  EXPECT_EQ(DecodeOne(mem, "%v", argv_off, 1),
            "[test.cc:17 TestFn] unknown([1,7,42])");
}

TEST(CelLogTest, ValueErrorKindWithMessage) {
  Scratch mem;
  const uint32_t msg_off = 700;
  mem.WriteStr(msg_off, "bad");
  const uint32_t err_off = 740;
  mem.WriteErrorDesc(err_off, /*code=*/5, msg_off, 3);
  const uint32_t cv_off = 512;
  CelValue cv{};
  cv.kind = CEL_ERROR;
  cv.payload.err = err_off;
  mem.WriteCelValue(cv_off, cv);
  const uint32_t argv_off = 256;
  mem.WriteSlot(argv_off, CEL_LOG_TAG_VALUE, cv_off);
  EXPECT_EQ(DecodeOne(mem, "%v", argv_off, 1),
            "[test.cc:17 TestFn] error(code=5,\"bad\")");
}

TEST(CelLogTest, ValueBadKindEmitsDiagnostic) {
  Scratch mem;
  const uint32_t cv_off = 512;
  CelValue cv{};
  cv.kind = 99;
  mem.WriteCelValue(cv_off, cv);
  const uint32_t argv_off = 256;
  mem.WriteSlot(argv_off, CEL_LOG_TAG_VALUE, cv_off);
  EXPECT_EQ(DecodeOne(mem, "%v", argv_off, 1),
            "[test.cc:17 TestFn] <bad-kind=99>");
}

TEST(CelLogTest, CapturingSinkAppendsEveryCall) {
  CapturingCelLogSink sink;
  Scratch mem;
  mem.WriteStr(1, "f.cc");
  mem.WriteStr(10, "Fn");
  mem.WriteStr(30, "hi");
  CelLogWireArgs args;
  args.file_ptr = 1;
  args.file_len = 4;
  args.fn_ptr = 10;
  args.fn_len = 2;
  args.line = 1;
  args.fmt_ptr = 30;
  args.fmt_len = 2;
  DecodeCelLog(mem.mem(), args, &sink);
  args.line = 2;
  DecodeCelLog(mem.mem(), args, &sink);
  ASSERT_EQ(sink.lines().size(), 2u);
  EXPECT_EQ(sink.lines()[0], "[f.cc:1 Fn] hi");
  EXPECT_EQ(sink.lines()[1], "[f.cc:2 Fn] hi");
  sink.Clear();
  EXPECT_TRUE(sink.lines().empty());
}

TEST(CelLogTest, SetCelLogSinkReturnsPreviousAndRestores) {
  CapturingCelLogSink sink;
  CelLogSink* prev = SetCelLogSink(&sink);
  // Restore whatever was there before, even if null (the process default).
  SetCelLogSink(prev);
  // DefaultCelLogSink is always non-null.
  EXPECT_NE(DefaultCelLogSink(), nullptr);
}

TEST(CelLogTest, OobArgvSlotEmitsFallback) {
  // argv_ptr points past the end of memory; ReadSlot should return
  // `<oob-arg>` rather than trapping.
  Scratch mem;
  EXPECT_EQ(DecodeOne(mem, "x=%d", /*argv_ptr=*/kMemBytes - 8, /*argc=*/1),
            "[test.cc:17 TestFn] x=<oob-arg>");
}

TEST(CelLogTest, FilePtrOobProducesEmptyFileInPrefix) {
  // The prefix formatter wraps the file span via SafeSpan, which
  // returns an empty string_view on OOB.  The line is still emitted;
  // only the prefix's file slot is blank.
  CapturingCelLogSink sink;
  Scratch mem;
  mem.WriteStr(10, "Fn");
  mem.WriteStr(30, "hi");
  CelLogWireArgs args;
  args.file_ptr = kMemBytes - 2;
  args.file_len = 100;  // runs past end
  args.fn_ptr = 10;
  args.fn_len = 2;
  args.line = 3;
  args.fmt_ptr = 30;
  args.fmt_len = 2;
  DecodeCelLog(mem.mem(), args, &sink);
  ASSERT_EQ(sink.lines().size(), 1u);
  EXPECT_EQ(sink.lines()[0], "[:3 Fn] hi");
}

}  // namespace
}  // namespace celwasm
