#include "compiler/host/cel_host.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "absl/log/check.h"
#include "absl/strings/string_view.h"
#include "compiler/runtime/cel_runtime.h"
#include "compiler/testdata/host_fixture_proto2.pb.h"
#include "compiler/testdata/host_fixture_proto3.pb.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"

namespace celwasm {
namespace {

// A heap-backed arena that stands in for the module's wasm-memory arena in
// unit tests.  `Allocator()` hands back a callback matching `ArenaAllocator`;
// offsets start at 1 so zero can remain the "absent" sentinel the runtime
// reserves.
class FakeArena {
 public:
  ArenaAllocator Allocator() {
    return [this](size_t len, uint32_t* out_offset) -> uint8_t* {
      *out_offset = static_cast<uint32_t>(bytes_.size());
      const size_t before = bytes_.size();
      bytes_.resize(before + len);
      return len == 0 ? nullptr : bytes_.data() + before;
    };
  }

  absl::string_view View(uint32_t ptr, uint32_t len) const {
    CHECK_LE(static_cast<size_t>(ptr) + len, bytes_.size());
    return {reinterpret_cast<const char*>(bytes_.data()) + ptr, len};
  }

 private:
  // Offset 0 is reserved as the runtime's "absent" sentinel, so start the
  // arena with a throwaway byte.
  std::vector<uint8_t> bytes_{0};
};

// Trivial interning callback: each unique submessage gets the next slot.
// Slot 0 mirrors the runtime's "null sentinel" convention so a test that
// accidentally interns a value it expected to be unset shows up loudly.
class SlotInterner {
 public:
  InternMessage Intern() {
    return [this](const google::protobuf::Message& m) -> uint32_t {
      for (uint32_t i = 0; i < interned_.size(); ++i) {
        if (interned_.at(i) == &m) return i + 1;
      }
      interned_.push_back(&m);
      return static_cast<uint32_t>(interned_.size());
    };
  }

  uint32_t count() const {
    return static_cast<uint32_t>(interned_.size());
  }

 private:
  std::vector<const google::protobuf::Message*> interned_;
};

// Sets `field` on `msg` to an arbitrary non-default value chosen to round-
// trip distinctly for every wire type (e.g. the high bit is set on fixed-
// width unsigned fields so sign-extension bugs can't pass).  Dispatches on
// the field descriptor's proto wire type, so it works against any message
// regardless of whether it came from a proto2 or proto3 .proto file.
void SetScalar(google::protobuf::Message* msg,
               const google::protobuf::FieldDescriptor* f) {
  using FD = google::protobuf::FieldDescriptor;
  const google::protobuf::Reflection* r = msg->GetReflection();
  switch (f->type()) {
    case FD::TYPE_BOOL:
      r->SetBool(msg, f, true);
      break;
    case FD::TYPE_INT32:
      r->SetInt32(msg, f, -7);
      break;
    case FD::TYPE_INT64:
      r->SetInt64(msg, f, -1234567890123LL);
      break;
    case FD::TYPE_UINT32:
      r->SetUInt32(msg, f, 0xFEDCBA98u);
      break;
    case FD::TYPE_UINT64:
      r->SetUInt64(msg, f, 0xFEDCBA9876543210ULL);
      break;
    case FD::TYPE_SINT32:
      r->SetInt32(msg, f, -11);
      break;
    case FD::TYPE_SINT64:
      r->SetInt64(msg, f, -13);
      break;
    case FD::TYPE_FIXED32:
      r->SetUInt32(msg, f, 0x80000001u);
      break;
    case FD::TYPE_FIXED64:
      r->SetUInt64(msg, f, 0x8000000000000001ULL);
      break;
    case FD::TYPE_SFIXED32:
      r->SetInt32(msg, f, -17);
      break;
    case FD::TYPE_SFIXED64:
      r->SetInt64(msg, f, -19);
      break;
    case FD::TYPE_FLOAT:
      r->SetFloat(msg, f, 0.5f);
      break;
    case FD::TYPE_DOUBLE:
      r->SetDouble(msg, f, -2.5);
      break;
    case FD::TYPE_STRING:
      r->SetString(msg, f, "hello");
      break;
    case FD::TYPE_BYTES:
      r->SetString(msg, f, std::string("\x00\xFFok", 4));
      break;
    default:
      FAIL() << "unhandled scalar " << f->type();
  }
}

// ---- ReadField coverage -----------------------------------------------------

// Test-only fixture — the readable "name, kind, value variants, bool"
// layout matters more than packing, so suppress the padding hint locally.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct ExpectedRow {
  const char* field_name;
  uint32_t expected_kind;
  // Exactly one of the following is meaningful per row, based on kind.
  int64_t i = 0;
  uint64_t u = 0;
  double d = 0.0;
  absl::string_view span;
  int32_t b = 0;
};

void PopulateScalars(google::protobuf::Message* msg) {
  using FD = google::protobuf::FieldDescriptor;
  const google::protobuf::Descriptor* desc = msg->GetDescriptor();
  for (int i = 0; i < desc->field_count(); ++i) {
    const FD* f = desc->field(i);
    if (f->is_repeated()) continue;
    if (f->cpp_type() == FD::CPPTYPE_MESSAGE) continue;
    if (f->cpp_type() == FD::CPPTYPE_ENUM) {
      msg->GetReflection()->SetEnumValue(msg, f, 7);
      continue;
    }
    SetScalar(msg, f);
  }
}

void ExpectScalarPayload(const CelValue& out, const ExpectedRow& row) {
  switch (row.expected_kind) {
    case CEL_BOOL:
      EXPECT_EQ(out.payload.b, row.b) << row.field_name;
      break;
    case CEL_INT:
      EXPECT_EQ(out.payload.i, row.i) << row.field_name;
      break;
    case CEL_UINT:
      EXPECT_EQ(out.payload.u, row.u) << row.field_name;
      break;
    case CEL_DOUBLE:
      EXPECT_DOUBLE_EQ(out.payload.d, row.d) << row.field_name;
      break;
    default:
      FAIL() << "unhandled expected kind for " << row.field_name;
  }
}

void ExpectReadField(const google::protobuf::Message& msg,
                     const ExpectedRow& row) {
  FakeArena arena;
  SlotInterner interner;
  ArenaAllocator alloc = arena.Allocator();
  InternMessage intern = interner.Intern();
  const google::protobuf::FieldDescriptor* f =
      msg.GetDescriptor()->FindFieldByName(row.field_name);
  ASSERT_NE(f, nullptr) << row.field_name;
  CelValue out{};
  ReadField(msg, f->number(), &out, alloc, intern);
  EXPECT_EQ(out.kind, row.expected_kind) << row.field_name;
  ExpectScalarPayload(out, row);
}

// Value matrix shared by the proto3 and proto2 scalar-read tests — both
// messages carry the same fields under the same names, so this table
// drives both.  Field-ordering tolerance is explicit: `ExpectReadField`
// looks up by name, not by position.
const ExpectedRow kScalarRows[] = {
    {"b", CEL_BOOL, 0, 0, 0.0, {}, 1},
    {"i32", CEL_INT, -7},
    {"i64", CEL_INT, -1234567890123LL},
    {"u32", CEL_UINT, 0, 0xFEDCBA98u},
    {"u64", CEL_UINT, 0, 0xFEDCBA9876543210ULL},
    {"si32", CEL_INT, -11},
    {"si64", CEL_INT, -13},
    {"fx32", CEL_UINT, 0, 0x80000001u},
    {"fx64", CEL_UINT, 0, 0x8000000000000001ULL},
    {"sfx32", CEL_INT, -17},
    {"sfx64", CEL_INT, -19},
    {"f32", CEL_DOUBLE, 0, 0, 0.5},
    {"f64", CEL_DOUBLE, 0, 0, -2.5},
    {"kind", CEL_INT, 7},
};

TEST(ReadFieldTest, EveryScalarWireTypeProto3) {
  testdata::HostMsg3 msg;
  PopulateScalars(&msg);
  for (const auto& row : kScalarRows) {
    ExpectReadField(msg, row);
  }
}

TEST(ReadFieldTest, EveryScalarWireTypeProto2) {
  testdata::HostMsg2 msg;
  PopulateScalars(&msg);
  for (const auto& row : kScalarRows) {
    ExpectReadField(msg, row);
  }
}

TEST(ReadFieldTest, StringFieldCopiesIntoArena) {
  testdata::HostMsg3 msg;
  msg.set_s("hello");
  FakeArena arena;
  SlotInterner interner;
  ArenaAllocator alloc = arena.Allocator();
  InternMessage intern = interner.Intern();
  const google::protobuf::FieldDescriptor* f =
      msg.GetDescriptor()->FindFieldByName("s");
  CelValue out{};
  ReadField(msg, f->number(), &out, alloc, intern);
  EXPECT_EQ(out.kind, CEL_STRING);
  EXPECT_EQ(out.payload.s.len, 5u);
  EXPECT_EQ(arena.View(out.payload.s.ptr, out.payload.s.len), "hello");
}

TEST(ReadFieldTest, BytesFieldCopiesIntoArenaPreservingHighBits) {
  testdata::HostMsg3 msg;
  msg.set_by(std::string("\x00\xFFok", 4));
  FakeArena arena;
  SlotInterner interner;
  ArenaAllocator alloc = arena.Allocator();
  InternMessage intern = interner.Intern();
  const google::protobuf::FieldDescriptor* f =
      msg.GetDescriptor()->FindFieldByName("by");
  CelValue out{};
  ReadField(msg, f->number(), &out, alloc, intern);
  EXPECT_EQ(out.kind, CEL_BYTES);
  EXPECT_EQ(out.payload.bytes.len, 4u);
  absl::string_view span =
      arena.View(out.payload.bytes.ptr, out.payload.bytes.len);
  ASSERT_EQ(span.size(), 4u);
  EXPECT_EQ(static_cast<uint8_t>(span.at(0)), 0x00);
  EXPECT_EQ(static_cast<uint8_t>(span.at(1)), 0xFF);
  EXPECT_EQ(span.substr(2), "ok");
}

TEST(ReadFieldTest, MessageFieldInternsSubmessage) {
  testdata::HostMsg3 msg;
  // Force the submessage to be present so GetMessage returns a stable
  // object address that the interner can identify.
  msg.mutable_inner();
  FakeArena arena;
  SlotInterner interner;
  ArenaAllocator alloc = arena.Allocator();
  InternMessage intern = interner.Intern();
  const google::protobuf::FieldDescriptor* inner_f =
      msg.GetDescriptor()->FindFieldByName("inner");
  CelValue out{};
  ReadField(msg, inner_f->number(), &out, alloc, intern);
  EXPECT_EQ(out.kind, CEL_MESSAGE);
  EXPECT_EQ(out.payload.msg_slot, 1u);
  EXPECT_EQ(interner.count(), 1u);
}

TEST(ReadFieldTest, UnknownFieldNumberSurfacesError) {
  testdata::HostMsg3 msg;
  FakeArena arena;
  SlotInterner interner;
  ArenaAllocator alloc = arena.Allocator();
  InternMessage intern = interner.Intern();
  CelValue out{};
  ReadField(msg, /*field_number=*/999, &out, alloc, intern);
  EXPECT_EQ(out.kind, CEL_ERROR);
}

TEST(ReadFieldTest, RepeatedFieldSurfacesError) {
  testdata::HostMsg3 msg;
  const google::protobuf::FieldDescriptor* rep =
      msg.GetDescriptor()->FindFieldByName("rep_i32");
  ASSERT_NE(rep, nullptr);
  FakeArena arena;
  SlotInterner interner;
  ArenaAllocator alloc = arena.Allocator();
  InternMessage intern = interner.Intern();
  CelValue out{};
  ReadField(msg, rep->number(), &out, alloc, intern);
  EXPECT_EQ(out.kind, CEL_ERROR);
}

TEST(ReadFieldTest, EmptyStringStillWritesSpanPayload) {
  testdata::HostMsg3 msg;
  msg.set_s("");
  FakeArena arena;
  SlotInterner interner;
  ArenaAllocator alloc = arena.Allocator();
  InternMessage intern = interner.Intern();
  const google::protobuf::FieldDescriptor* s =
      msg.GetDescriptor()->FindFieldByName("s");
  CelValue out{};
  ReadField(msg, s->number(), &out, alloc, intern);
  EXPECT_EQ(out.kind, CEL_STRING);
  EXPECT_EQ(out.payload.s.len, 0u);
}

// ---- HasField coverage ------------------------------------------------------

TEST(HasFieldTest, Proto3SingularScalarSetToNonDefaultIsPresent) {
  testdata::HostMsg3 msg;
  const google::protobuf::FieldDescriptor* i64 =
      msg.GetDescriptor()->FindFieldByName("i64");
  EXPECT_FALSE(HasField(msg, i64->number()));
  msg.set_i64(42);
  EXPECT_TRUE(HasField(msg, i64->number()));
}

TEST(HasFieldTest, Proto3SingularScalarAtDefaultIsAbsent) {
  // Proto3 tracks presence by value-equals-default for singular scalars
  // without the `optional` label.  Explicitly setting a scalar to its
  // default therefore shows up as absent — this is the behaviour the
  // G3 e2e test `HasProtoBoolFieldSetToFalseIsFalse` pins at the wasm
  // boundary; here it's pinned at the host-function boundary.
  testdata::HostMsg3 msg;
  const google::protobuf::FieldDescriptor* b =
      msg.GetDescriptor()->FindFieldByName("b");
  msg.set_b(false);
  EXPECT_FALSE(HasField(msg, b->number()));
}

TEST(HasFieldTest, Proto2SingularScalarAtDefaultIsStillPresent) {
  // Proto2 with `optional` tracks presence independently of value.
  // Setting the field — even to the type's default — flips HasField to
  // true.  This is the behaviour that distinguishes proto2's explicit
  // presence from proto3's value-based presence.
  testdata::HostMsg2 msg;
  const google::protobuf::FieldDescriptor* b =
      msg.GetDescriptor()->FindFieldByName("b");
  EXPECT_FALSE(HasField(msg, b->number()));
  msg.set_b(false);
  EXPECT_TRUE(HasField(msg, b->number()));
}

TEST(HasFieldTest, Proto2UnsetSingularScalarIsAbsent) {
  testdata::HostMsg2 msg;
  const google::protobuf::FieldDescriptor* i64 =
      msg.GetDescriptor()->FindFieldByName("i64");
  EXPECT_FALSE(HasField(msg, i64->number()));
  msg.set_i64(0);
  EXPECT_TRUE(HasField(msg, i64->number()));
}

TEST(HasFieldTest, SetAndUnsetMessage) {
  testdata::HostMsg3 msg;
  const google::protobuf::FieldDescriptor* inner =
      msg.GetDescriptor()->FindFieldByName("inner");
  EXPECT_FALSE(HasField(msg, inner->number()));
  msg.mutable_inner();
  EXPECT_TRUE(HasField(msg, inner->number()));
}

TEST(HasFieldTest, RepeatedFieldCountsElements) {
  testdata::HostMsg3 msg;
  const google::protobuf::FieldDescriptor* rep =
      msg.GetDescriptor()->FindFieldByName("rep_i32");
  EXPECT_FALSE(HasField(msg, rep->number()));
  msg.add_rep_i32(1);
  EXPECT_TRUE(HasField(msg, rep->number()));
}

TEST(HasFieldTest, UnknownFieldNumberReturnsFalse) {
  testdata::HostMsg3 msg;
  EXPECT_FALSE(HasField(msg, /*field_number=*/999));
}

// ---- MessageEq coverage -----------------------------------------------------

TEST(MessageEqTest, IdenticalMessagesAreEqual) {
  testdata::HostMsg3 a;
  testdata::HostMsg3 b;
  a.set_i64(-1234567890123LL);
  b.set_i64(-1234567890123LL);
  EXPECT_TRUE(MessageEq(a, b));
}

TEST(MessageEqTest, DifferentFieldsAreUnequal) {
  testdata::HostMsg3 a;
  testdata::HostMsg3 b;
  a.set_i64(1);
  b.set_i64(2);
  EXPECT_FALSE(MessageEq(a, b));
}

TEST(MessageEqTest, NestedMessageEqualityRecurses) {
  testdata::HostMsg3 a;
  testdata::HostMsg3 b;
  a.mutable_inner()->set_i64(42);
  b.mutable_inner()->set_i64(42);
  EXPECT_TRUE(MessageEq(a, b));
  b.mutable_inner()->set_i64(43);
  EXPECT_FALSE(MessageEq(a, b));
}

}  // namespace
}  // namespace celwasm
