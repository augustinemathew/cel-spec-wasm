// Layer-2 unit matrix for BindTypedFunction / the ArgTrait / ReturnTrait
// machinery (m21 §5).  Two halves:
//
//   - compile-time: the canonical-type predicate (kIsCanonicalHostArg /
//     kIsCanonicalHostReturn) that drives ArgTrait / ReturnTrait's hard
//     static_assert is exercised directly — `static_assert(canonical)`
//     for every admitted spelling and `static_assert(!canonical)` for
//     the rejected ones (`int`, `float`, `char*`, by-value proto, …).
//     This is the must-not-compile coverage in testable form: a
//     non-canonical spelling flips the predicate false, which is exactly
//     what makes BindTypedFunction fail to compile.
//   - runtime: each canonical parameter / return type round-trips
//     through a typed lambda invoked over the fake HostCallContext;
//     arity is derived correctly; a wrong-kind arg / a non-OK lambda
//     surfaces as an error status.

#include "eval/typed_function.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "eval/host_call_context.h"
#include "eval/internal/cel_host.h"
#include "eval/internal/cel_host_test_fakes.h"
#include "eval/value.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"
#include "runtime/cel_data.h"
#include "testdata/e2e_fixture.pb.h"

namespace celwasm {
namespace {

using ::absl_testing::StatusIs;
using ::celwasm::test::FakeArenaAllocator;
using ::celwasm::test::FakeExternrefTable;
using ::celwasm::test::FakeMemoryView;
using ::celwasm::testdata::Customer;
using ::celwasm::typed_internal::kIsCanonicalHostArg;
using ::celwasm::typed_internal::kIsCanonicalHostReturn;

// ───────────── compile-time: canonical-type detection ──────────────

// Admitted argument spellings.
static_assert(kIsCanonicalHostArg<bool>);
static_assert(kIsCanonicalHostArg<int64_t>);
static_assert(kIsCanonicalHostArg<uint64_t>);
static_assert(kIsCanonicalHostArg<double>);
static_assert(kIsCanonicalHostArg<absl::string_view>);
static_assert(kIsCanonicalHostArg<absl::Duration>);
static_assert(kIsCanonicalHostArg<absl::Time>);
static_assert(kIsCanonicalHostArg<HostListView>);
static_assert(kIsCanonicalHostArg<HostMapView>);
static_assert(kIsCanonicalHostArg<Value>);
static_assert(kIsCanonicalHostArg<const Customer&>);  // concrete
static_assert(
    kIsCanonicalHostArg<const google::protobuf::Message&>);  // base ref
static_assert(
    kIsCanonicalHostArg<const google::protobuf::Message*>);  // base ptr

// Rejected argument spellings — the must-not-compile set.  Each of these
// flips the predicate false, so BindTypedFunction fails to compile with
// the ArgTrait static_assert.  (`long` is intentionally omitted: it
// aliases int64_t on LP64.)
static_assert(!kIsCanonicalHostArg<int>);
static_assert(!kIsCanonicalHostArg<unsigned>);
static_assert(!kIsCanonicalHostArg<float>);
static_assert(!kIsCanonicalHostArg<char*>);
static_assert(!kIsCanonicalHostArg<const char*>);
static_assert(!kIsCanonicalHostArg<std::string>);  // string_view is canonical
static_assert(!kIsCanonicalHostArg<Customer>);     // by-value proto (slicing)
// A concrete Message-derived pointer IS admitted (matches `const M*`).
static_assert(kIsCanonicalHostArg<const Customer*>);

// Admitted / rejected return spellings.
static_assert(kIsCanonicalHostReturn<bool>);
static_assert(kIsCanonicalHostReturn<int64_t>);
static_assert(kIsCanonicalHostReturn<uint64_t>);
static_assert(kIsCanonicalHostReturn<double>);
static_assert(kIsCanonicalHostReturn<std::string>);
static_assert(kIsCanonicalHostReturn<absl::Duration>);
static_assert(kIsCanonicalHostReturn<absl::Time>);
static_assert(kIsCanonicalHostReturn<std::vector<Value>>);
static_assert(kIsCanonicalHostReturn<std::vector<std::pair<Value, Value>>>);
static_assert(kIsCanonicalHostReturn<Value>);
static_assert(kIsCanonicalHostReturn<std::unique_ptr<Customer>>);
static_assert(!kIsCanonicalHostReturn<int>);
static_assert(!kIsCanonicalHostReturn<float>);
static_assert(!kIsCanonicalHostReturn<absl::string_view>);  // std::string is

// ───────────── arity derivation ──────────────

TEST(TypedFunctionArityTest, ArityIsParamCountPlusOutSlot) {
  EXPECT_EQ(BindTypedFunction([]() -> absl::StatusOr<int64_t> {
              return 0;
            }).num_args,
            1);
  EXPECT_EQ(BindTypedFunction([](int64_t a) -> absl::StatusOr<int64_t> {
              return a;
            }).num_args,
            2);
  EXPECT_EQ(
      BindTypedFunction([](int64_t a, int64_t b) -> absl::StatusOr<int64_t> {
        return a + b;
      }).num_args,
      3);
}

// ───────────── runtime round-trips over the fake context ───────────

constexpr uint32_t kOutSlot = 0;
constexpr uint32_t kArg0 = 24;
constexpr uint32_t kArg1 = 48;
constexpr uint32_t kStrArea = 0x1000;
constexpr uint32_t kArenaBase = 0x8000;
constexpr size_t kArenaCap = 0x8000;

class TypedFunctionTest : public testing::Test {
 protected:
  CelValue Int(int64_t v) {
    CelValue cv{};
    cv.kind = CEL_INT;
    cv.payload.i = v;
    return cv;
  }
  CelValue Str(uint32_t ptr, absl::string_view s) {
    if (!s.empty()) std::memcpy(mem_.data() + ptr, s.data(), s.size());
    CelValue cv{};
    cv.kind = CEL_STRING;
    cv.payload.s.ptr = ptr;
    cv.payload.s.len = static_cast<uint32_t>(s.size());
    return cv;
  }
  CelValue Msg(uint32_t slot) {
    CelValue cv{};
    cv.kind = CEL_MESSAGE;
    cv.payload.msg_slot = slot;
    return cv;
  }
  HostCallContext Ctx(std::vector<uint32_t> arg_slots) {
    arg_slots_ = std::move(arg_slots);
    return {mem_, refs_, arena_, kOutSlot, arg_slots_};
  }
  CelValue Out() const {
    return mem_.ReadCelValue(kOutSlot);
  }

  FakeMemoryView mem_;
  FakeExternrefTable refs_;
  FakeArenaAllocator arena_{&mem_, kArenaBase, kArenaCap};
  std::vector<uint32_t> arg_slots_;
};

TEST_F(TypedFunctionTest, IntParamIntReturn) {
  auto tf = BindTypedFunction([](int64_t x) -> absl::StatusOr<int64_t> {
    return x * 2;
  });
  mem_.Place(kArg0, Int(21));
  auto ctx = Ctx({kArg0});
  ASSERT_TRUE(tf.callback(ctx).ok());
  EXPECT_EQ(Out().kind, CEL_INT);
  EXPECT_EQ(Out().payload.i, 42);
}

TEST_F(TypedFunctionTest, TwoArgsDecodeInOrder) {
  auto tf =
      BindTypedFunction([](int64_t a, int64_t b) -> absl::StatusOr<int64_t> {
        return a - b;
      });
  mem_.Place(kArg0, Int(10));
  mem_.Place(kArg1, Int(3));
  auto ctx = Ctx({kArg0, kArg1});
  ASSERT_TRUE(tf.callback(ctx).ok());
  EXPECT_EQ(Out().payload.i, 7);  // 10 - 3, order preserved
}

TEST_F(TypedFunctionTest, StringParamStringReturn) {
  auto tf =
      BindTypedFunction([](absl::string_view s) -> absl::StatusOr<std::string> {
        return std::string(s) + "!";
      });
  mem_.Place(kArg0, Str(kStrArea, "hi"));
  auto ctx = Ctx({kArg0});
  ASSERT_TRUE(tf.callback(ctx).ok());
  EXPECT_EQ(Out().kind, CEL_STRING);
  EXPECT_EQ(
      std::string(mem_.ReadSpan(Out().payload.s.ptr, Out().payload.s.len)),
      "hi!");
}

TEST_F(TypedFunctionTest, DurationParamDurationReturn) {
  auto tf =
      BindTypedFunction([](absl::Duration d) -> absl::StatusOr<absl::Duration> {
        return d * 2;
      });
  CelValue cv{};
  cv.kind = CEL_DURATION;
  DecomposeAbslDuration(absl::Seconds(4), &cv.payload.dur);
  mem_.Place(kArg0, cv);
  auto ctx = Ctx({kArg0});
  ASSERT_TRUE(tf.callback(ctx).ok());
  EXPECT_EQ(Out().kind, CEL_DURATION);
  EXPECT_EQ(Out().payload.dur.seconds, 8);
}

TEST_F(TypedFunctionTest, ConcreteProtoParam) {
  auto tf = BindTypedFunction([](const Customer& c) -> absl::StatusOr<bool> {
    return c.is_premium();
  });
  Customer c;
  c.set_is_premium(true);
  const uint32_t slot = refs_.Intern(std::make_shared<ProtoBacking>(&c));
  mem_.Place(kArg0, Msg(slot));
  auto ctx = Ctx({kArg0});
  ASSERT_TRUE(tf.callback(ctx).ok());
  EXPECT_EQ(Out().kind, CEL_BOOL);
  EXPECT_NE(Out().payload.b, 0);
}

TEST_F(TypedFunctionTest, ConcreteProtoWrongTypeErrors) {
  // A bound Address fed to a lambda expecting Customer → InvalidArgument.
  auto tf = BindTypedFunction([](const Customer& c) -> absl::StatusOr<bool> {
    return c.is_premium();
  });
  testdata::Address addr;
  const uint32_t slot = refs_.Intern(std::make_shared<ProtoBacking>(&addr));
  mem_.Place(kArg0, Msg(slot));
  auto ctx = Ctx({kArg0});
  EXPECT_THAT(tf.callback(ctx), StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(TypedFunctionTest, PolymorphicProtoParam) {
  // const google::protobuf::Message* — no cast, any message accepted.
  auto tf = BindTypedFunction(
      [](const google::protobuf::Message* m) -> absl::StatusOr<std::string> {
        return std::string(m->GetTypeName());
      });
  Customer c;
  const uint32_t slot = refs_.Intern(std::make_shared<ProtoBacking>(&c));
  mem_.Place(kArg0, Msg(slot));
  auto ctx = Ctx({kArg0});
  ASSERT_TRUE(tf.callback(ctx).ok());
  EXPECT_EQ(Out().kind, CEL_STRING);
  EXPECT_EQ(
      std::string(mem_.ReadSpan(Out().payload.s.ptr, Out().payload.s.len)),
      "celwasm.testdata.Customer");
}

TEST_F(TypedFunctionTest, ProtoReturnInterns) {
  auto tf = BindTypedFunction(
      [](absl::string_view name) -> absl::StatusOr<std::unique_ptr<Customer>> {
        auto m = std::make_unique<Customer>();
        m->set_name(std::string(name));
        return m;
      });
  mem_.Place(kArg0, Str(kStrArea, "Ada"));
  auto ctx = Ctx({kArg0});
  ASSERT_TRUE(tf.callback(ctx).ok());
  EXPECT_EQ(Out().kind, CEL_MESSAGE);
  const auto* c = dynamic_cast<const Customer*>(
      refs_.Lookup(Out().payload.msg_slot)->message());
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->name(), "Ada");
}

TEST_F(TypedFunctionTest, ListParamSums) {
  auto tf = BindTypedFunction([](HostListView xs) -> absl::StatusOr<int64_t> {
    int64_t total = 0;
    for (size_t i = 0; i < xs.Size(); ++i) {
      auto e = xs.At(i);
      if (!e.ok()) return e.status();
      auto n = e->AsInt();
      if (!n.ok()) return n.status();
      total += *n;
    }
    return total;
  });
  std::vector<Value> elems = {Value::Int(2), Value::Int(5)};
  const uint32_t slot =
      refs_.InternList(std::make_shared<HostList>(std::move(elems)));
  CelValue cv{};
  cv.kind = CEL_LIST_HOST;
  cv.payload.ref_slot = slot;
  mem_.Place(kArg0, cv);
  auto ctx = Ctx({kArg0});
  ASSERT_TRUE(tf.callback(ctx).ok());
  EXPECT_EQ(Out().payload.i, 7);
}

TEST_F(TypedFunctionTest, ValueParamValueReturnEmitsUnknown) {
  // A StatusOr<Value> lambda receives the decoded arg by value and can
  // emit a function-origin unknown (overwriting the input in place).
  auto tf = BindTypedFunction([](Value v) -> absl::StatusOr<Value> {
    v = Value::Unknown(AttributeId{kFunctionUnknownSentinel});
    return v;
  });
  mem_.Place(kArg0, Int(1));
  auto ctx = Ctx({kArg0});
  ASSERT_TRUE(tf.callback(ctx).ok());
  EXPECT_EQ(Out().kind, CEL_UNKNOWN);
  EXPECT_EQ(Out().payload.unk, kFunctionUnknownSentinel);
}

TEST_F(TypedFunctionTest, WrongKindArgSurfacesError) {
  auto tf = BindTypedFunction([](int64_t x) -> absl::StatusOr<int64_t> {
    return x;
  });
  mem_.Place(kArg0, Str(kStrArea, "not an int"));
  auto ctx = Ctx({kArg0});
  EXPECT_THAT(tf.callback(ctx), StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(TypedFunctionTest, LambdaNonOkStatusPropagates) {
  auto tf = BindTypedFunction([](int64_t) -> absl::StatusOr<int64_t> {
    return absl::FailedPreconditionError("nope");
  });
  mem_.Place(kArg0, Int(1));
  auto ctx = Ctx({kArg0});
  EXPECT_THAT(tf.callback(ctx),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

// A plain function pointer (not a lambda) also binds.
absl::StatusOr<int64_t> TripleFn(int64_t x) {
  return x * 3;
}

TEST_F(TypedFunctionTest, FunctionPointerBinds) {
  auto tf = BindTypedFunction(&TripleFn);
  EXPECT_EQ(tf.num_args, 2);
  mem_.Place(kArg0, Int(4));
  auto ctx = Ctx({kArg0});
  ASSERT_TRUE(tf.callback(ctx).ok());
  EXPECT_EQ(Out().payload.i, 12);
}

}  // namespace
}  // namespace celwasm
