#include "compiler/host/cel_host.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "absl/log/check.h"
#include "absl/strings/string_view.h"
#include "compiler/runtime/cel_runtime.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/dynamic_message.h"
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
      // Identity is by object address — fine for tests that don't copy.
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

// ---- Synthetic proto fixture ------------------------------------------------

// Builds a `celwasm.hosttest.HostMsg` descriptor covering every proto wire
// type plus a nested enum and a self-recursive message field.  Kept in a
// class so helpers can split the 40-statement constructor without blowing
// the function-size gate.
class HostProtoFixture {
 public:
  HostProtoFixture() {
    google::protobuf::FileDescriptorProto file;
    file.set_name("celwasm_hosttest.proto");
    file.set_package("celwasm.hosttest");
    file.set_syntax("proto2");

    google::protobuf::DescriptorProto* outer = file.add_message_type();
    outer->set_name("HostMsg");
    AddKindEnum(outer);
    AddScalarFields(outer);
    AddNestedFields(outer);

    pool_ = std::make_unique<google::protobuf::DescriptorPool>();
    CHECK(pool_->BuildFile(file) != nullptr);
    const google::protobuf::Descriptor* descriptor =
        pool_->FindMessageTypeByName("celwasm.hosttest.HostMsg");
    CHECK(descriptor != nullptr);
    factory_ = std::make_unique<google::protobuf::DynamicMessageFactory>();
    prototype_ = factory_->GetPrototype(descriptor);
    CHECK(prototype_ != nullptr);
  }

  std::unique_ptr<google::protobuf::Message> New() const {
    return std::unique_ptr<google::protobuf::Message>(prototype_->New());
  }

  const google::protobuf::Descriptor* descriptor() const {
    return prototype_->GetDescriptor();
  }

 private:
  static void AddKindEnum(google::protobuf::DescriptorProto* outer) {
    google::protobuf::EnumDescriptorProto* k = outer->add_enum_type();
    k->set_name("Kind");
    google::protobuf::EnumValueDescriptorProto* v0 = k->add_value();
    v0->set_name("KIND_UNSPECIFIED");
    v0->set_number(0);
    google::protobuf::EnumValueDescriptorProto* v7 = k->add_value();
    v7->set_name("KIND_SEVEN");
    v7->set_number(7);
  }

  static void AddScalarFields(google::protobuf::DescriptorProto* outer) {
    using Field = google::protobuf::FieldDescriptorProto;
    AddField(outer, "b", 1, Field::TYPE_BOOL);
    AddField(outer, "i32", 2, Field::TYPE_INT32);
    AddField(outer, "i64", 3, Field::TYPE_INT64);
    AddField(outer, "u32", 4, Field::TYPE_UINT32);
    AddField(outer, "u64", 5, Field::TYPE_UINT64);
    AddField(outer, "sint32", 6, Field::TYPE_SINT32);
    AddField(outer, "sint64", 7, Field::TYPE_SINT64);
    AddField(outer, "fixed32", 8, Field::TYPE_FIXED32);
    AddField(outer, "fixed64", 9, Field::TYPE_FIXED64);
    AddField(outer, "sfixed32", 10, Field::TYPE_SFIXED32);
    AddField(outer, "sfixed64", 11, Field::TYPE_SFIXED64);
    AddField(outer, "f32", 12, Field::TYPE_FLOAT);
    AddField(outer, "f64", 13, Field::TYPE_DOUBLE);
    AddField(outer, "s", 14, Field::TYPE_STRING);
    AddField(outer, "by", 15, Field::TYPE_BYTES);
  }

  static void AddNestedFields(google::protobuf::DescriptorProto* outer) {
    using Field = google::protobuf::FieldDescriptorProto;
    google::protobuf::FieldDescriptorProto* kind_f = outer->add_field();
    kind_f->set_name("kind");
    kind_f->set_number(16);
    kind_f->set_type(Field::TYPE_ENUM);
    kind_f->set_type_name(".celwasm.hosttest.HostMsg.Kind");
    kind_f->set_label(Field::LABEL_OPTIONAL);

    google::protobuf::FieldDescriptorProto* inner_f = outer->add_field();
    inner_f->set_name("inner");
    inner_f->set_number(17);
    inner_f->set_type(Field::TYPE_MESSAGE);
    inner_f->set_type_name(".celwasm.hosttest.HostMsg");
    inner_f->set_label(Field::LABEL_OPTIONAL);

    google::protobuf::FieldDescriptorProto* rep_f = outer->add_field();
    rep_f->set_name("rep_i32");
    rep_f->set_number(18);
    rep_f->set_type(Field::TYPE_INT32);
    rep_f->set_label(Field::LABEL_REPEATED);
  }

  static void AddField(google::protobuf::DescriptorProto* msg,
                       absl::string_view name, int number,
                       google::protobuf::FieldDescriptorProto::Type type) {
    google::protobuf::FieldDescriptorProto* f = msg->add_field();
    f->set_name(std::string(name));
    f->set_number(number);
    f->set_type(type);
    f->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  }

  std::unique_ptr<google::protobuf::DescriptorPool> pool_;
  std::unique_ptr<google::protobuf::DynamicMessageFactory> factory_;
  const google::protobuf::Message* prototype_ = nullptr;
};

// Sets `field` on `msg` to an arbitrary non-default value chosen to round-
// trip distinctly for every wire type (e.g. the high bit is set on fixed-
// width unsigned fields so sign-extension bugs can't pass).
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

void PopulateScalars(google::protobuf::Message* msg,
                     const google::protobuf::Descriptor* desc) {
  using FD = google::protobuf::FieldDescriptor;
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
                     const google::protobuf::Descriptor* desc,
                     const ExpectedRow& row) {
  FakeArena arena;
  SlotInterner interner;
  ArenaAllocator alloc = arena.Allocator();
  InternMessage intern = interner.Intern();
  const google::protobuf::FieldDescriptor* f =
      desc->FindFieldByName(row.field_name);
  ASSERT_NE(f, nullptr) << row.field_name;
  CelValue out{};
  ReadField(msg, f->number(), &out, alloc, intern);
  EXPECT_EQ(out.kind, row.expected_kind) << row.field_name;
  ExpectScalarPayload(out, row);
}

TEST(ReadFieldTest, EveryScalarWireType) {
  HostProtoFixture fixture;
  auto msg = fixture.New();
  const google::protobuf::Descriptor* desc = fixture.descriptor();
  PopulateScalars(msg.get(), desc);

  const ExpectedRow rows[] = {
      {"b", CEL_BOOL, 0, 0, 0.0, {}, 1},
      {"i32", CEL_INT, -7},
      {"i64", CEL_INT, -1234567890123LL},
      {"u32", CEL_UINT, 0, 0xFEDCBA98u},
      {"u64", CEL_UINT, 0, 0xFEDCBA9876543210ULL},
      {"sint32", CEL_INT, -11},
      {"sint64", CEL_INT, -13},
      {"fixed32", CEL_UINT, 0, 0x80000001u},
      {"fixed64", CEL_UINT, 0, 0x8000000000000001ULL},
      {"sfixed32", CEL_INT, -17},
      {"sfixed64", CEL_INT, -19},
      {"f32", CEL_DOUBLE, 0, 0, 0.5},
      {"f64", CEL_DOUBLE, 0, 0, -2.5},
      {"kind", CEL_INT, 7},
  };
  for (const auto& row : rows) {
    ExpectReadField(*msg, desc, row);
  }
}

TEST(ReadFieldTest, StringFieldCopiesIntoArena) {
  HostProtoFixture fixture;
  auto msg = fixture.New();
  SetScalar(msg.get(), fixture.descriptor()->FindFieldByName("s"));
  FakeArena arena;
  SlotInterner interner;
  ArenaAllocator alloc = arena.Allocator();
  InternMessage intern = interner.Intern();
  CelValue out{};
  ReadField(*msg, fixture.descriptor()->FindFieldByName("s")->number(), &out,
            alloc, intern);
  EXPECT_EQ(out.kind, CEL_STRING);
  EXPECT_EQ(out.payload.s.len, 5u);
  EXPECT_EQ(arena.View(out.payload.s.ptr, out.payload.s.len), "hello");
}

TEST(ReadFieldTest, BytesFieldCopiesIntoArenaPreservingHighBits) {
  HostProtoFixture fixture;
  auto msg = fixture.New();
  SetScalar(msg.get(), fixture.descriptor()->FindFieldByName("by"));
  FakeArena arena;
  SlotInterner interner;
  ArenaAllocator alloc = arena.Allocator();
  InternMessage intern = interner.Intern();
  CelValue out{};
  ReadField(*msg, fixture.descriptor()->FindFieldByName("by")->number(), &out,
            alloc, intern);
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
  HostProtoFixture fixture;
  auto msg = fixture.New();
  const google::protobuf::FieldDescriptor* inner_f =
      fixture.descriptor()->FindFieldByName("inner");
  // Force the message to be present so GetMessage returns a non-default
  // stable address the interner can identify.
  msg->GetReflection()->MutableMessage(msg.get(), inner_f);
  FakeArena arena;
  SlotInterner interner;
  ArenaAllocator alloc = arena.Allocator();
  InternMessage intern = interner.Intern();
  CelValue out{};
  ReadField(*msg, inner_f->number(), &out, alloc, intern);
  EXPECT_EQ(out.kind, CEL_MESSAGE);
  EXPECT_EQ(out.payload.msg_slot, 1u);
  EXPECT_EQ(interner.count(), 1u);
}

TEST(ReadFieldTest, UnknownFieldNumberSurfacesError) {
  HostProtoFixture fixture;
  auto msg = fixture.New();
  FakeArena arena;
  SlotInterner interner;
  ArenaAllocator alloc = arena.Allocator();
  InternMessage intern = interner.Intern();
  CelValue out{};
  ReadField(*msg, /*field_number=*/999, &out, alloc, intern);
  EXPECT_EQ(out.kind, CEL_ERROR);
}

TEST(ReadFieldTest, RepeatedFieldSurfacesError) {
  HostProtoFixture fixture;
  auto msg = fixture.New();
  const google::protobuf::FieldDescriptor* rep =
      fixture.descriptor()->FindFieldByName("rep_i32");
  ASSERT_NE(rep, nullptr);
  FakeArena arena;
  SlotInterner interner;
  ArenaAllocator alloc = arena.Allocator();
  InternMessage intern = interner.Intern();
  CelValue out{};
  ReadField(*msg, rep->number(), &out, alloc, intern);
  EXPECT_EQ(out.kind, CEL_ERROR);
}

TEST(ReadFieldTest, EmptyStringStillWritesSpanPayload) {
  HostProtoFixture fixture;
  auto msg = fixture.New();
  const google::protobuf::FieldDescriptor* s =
      fixture.descriptor()->FindFieldByName("s");
  msg->GetReflection()->SetString(msg.get(), s, "");
  FakeArena arena;
  SlotInterner interner;
  ArenaAllocator alloc = arena.Allocator();
  InternMessage intern = interner.Intern();
  CelValue out{};
  ReadField(*msg, s->number(), &out, alloc, intern);
  EXPECT_EQ(out.kind, CEL_STRING);
  EXPECT_EQ(out.payload.s.len, 0u);
}

// ---- HasField coverage ------------------------------------------------------

TEST(HasFieldTest, SetAndUnsetScalar) {
  HostProtoFixture fixture;
  auto msg = fixture.New();
  const google::protobuf::FieldDescriptor* i64 =
      fixture.descriptor()->FindFieldByName("i64");
  EXPECT_FALSE(HasField(*msg, i64->number()));
  msg->GetReflection()->SetInt64(msg.get(), i64, 42);
  EXPECT_TRUE(HasField(*msg, i64->number()));
}

TEST(HasFieldTest, SetAndUnsetMessage) {
  HostProtoFixture fixture;
  auto msg = fixture.New();
  const google::protobuf::FieldDescriptor* inner =
      fixture.descriptor()->FindFieldByName("inner");
  EXPECT_FALSE(HasField(*msg, inner->number()));
  msg->GetReflection()->MutableMessage(msg.get(), inner);
  EXPECT_TRUE(HasField(*msg, inner->number()));
}

TEST(HasFieldTest, RepeatedFieldCountsElements) {
  HostProtoFixture fixture;
  auto msg = fixture.New();
  const google::protobuf::FieldDescriptor* rep =
      fixture.descriptor()->FindFieldByName("rep_i32");
  EXPECT_FALSE(HasField(*msg, rep->number()));
  msg->GetReflection()->AddInt32(msg.get(), rep, 1);
  EXPECT_TRUE(HasField(*msg, rep->number()));
}

TEST(HasFieldTest, UnknownFieldNumberReturnsFalse) {
  HostProtoFixture fixture;
  auto msg = fixture.New();
  EXPECT_FALSE(HasField(*msg, /*field_number=*/999));
}

// ---- MessageEq coverage -----------------------------------------------------

TEST(MessageEqTest, IdenticalMessagesAreEqual) {
  HostProtoFixture fixture;
  auto a = fixture.New();
  auto b = fixture.New();
  SetScalar(a.get(), fixture.descriptor()->FindFieldByName("i64"));
  SetScalar(b.get(), fixture.descriptor()->FindFieldByName("i64"));
  EXPECT_TRUE(MessageEq(*a, *b));
}

TEST(MessageEqTest, DifferentFieldsAreUnequal) {
  HostProtoFixture fixture;
  auto a = fixture.New();
  auto b = fixture.New();
  const google::protobuf::FieldDescriptor* i64 =
      fixture.descriptor()->FindFieldByName("i64");
  a->GetReflection()->SetInt64(a.get(), i64, 1);
  b->GetReflection()->SetInt64(b.get(), i64, 2);
  EXPECT_FALSE(MessageEq(*a, *b));
}

TEST(MessageEqTest, NestedMessageEqualityRecurses) {
  HostProtoFixture fixture;
  auto a = fixture.New();
  auto b = fixture.New();
  const google::protobuf::FieldDescriptor* inner =
      fixture.descriptor()->FindFieldByName("inner");
  const google::protobuf::FieldDescriptor* i64 =
      fixture.descriptor()->FindFieldByName("i64");
  google::protobuf::Message* a_inner =
      a->GetReflection()->MutableMessage(a.get(), inner);
  google::protobuf::Message* b_inner =
      b->GetReflection()->MutableMessage(b.get(), inner);
  a_inner->GetReflection()->SetInt64(a_inner, i64, 42);
  b_inner->GetReflection()->SetInt64(b_inner, i64, 42);
  EXPECT_TRUE(MessageEq(*a, *b));
  b_inner->GetReflection()->SetInt64(b_inner, i64, 43);
  EXPECT_FALSE(MessageEq(*a, *b));
}

}  // namespace
}  // namespace celwasm
