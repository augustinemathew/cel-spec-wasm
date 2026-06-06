// Regression pin for cleanup-backlog #34 (chained-grow arena)
// + #35 (proto field reads through `cel_get_field` that
// arena-copy the value at the wire transition).  Both surfaced
// 2026-06-05 from the same audit: pre-fix, the per-Eval arena
// was a fixed 64 KiB and every proto field read eagerly
// arena-copied the field via `EncodeSpan` (`cel_host.cc:737`),
// so a `customer.huge_field` binding turned every probe —
// `size()`, concat, comparison — into a clean `kArenaOverflow`
// failure.  Post-fix (chained-grow arena in `runtime/cel_arena.c`),
// the arena malloc's a new chunk on demand and these probes
// succeed.
//
// Three assertions:
//
//   1. `size(c.huge_field)` over a 70 KiB field returns the
//      correct int.  Pre-fix this would `FAILED_PRECONDITION`
//      with "arena OOM in CelMapLookupImpl"; the regression
//      this case guards against is a re-introduction of the
//      fixed-cap arena (or a regression in the chained-grow
//      chain-walk).
//
//   2. The same huge field, fed through `(c.huge_field + "")`,
//      returns the original 70 KiB string with the empty
//      suffix appended.  Catches a regression where the concat
//      path's arena alloc is still capped at the first chunk's
//      size.
//
//   3. **POSITIVE / SANITY** — normal-sized field concat
//      works end-to-end.  Catches a regression in `RunWith()`
//      itself where every test would silently OK on a broken
//      activation binding.

#include <cstdint>
#include <string>

#include "absl/log/absl_check.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler/compiler.h"
#include "eval/activation.h"
#include "eval/engine.h"
#include "eval/instance.h"
#include "eval/internal/cel_host.h"  // Value::Message(proto)
#include "eval/value.h"
#include "gtest/gtest.h"
#include "shared/type.h"
#include "testdata/e2e_fixture.pb.h"

namespace celwasm {
namespace {

using ::celwasm::testdata::Customer;

// Slightly larger than the per-Eval arena (64 KiB) so any path
// that arena-copies the field will overflow.
constexpr size_t kHugeFieldBytes = 70u * 1024u;

Engine& Eng() {
  static Engine* e = [] {
    auto eng = Engine::NewBuilder().Build();
    ABSL_CHECK_OK(eng);
    return new Engine(*std::move(eng));
  }();
  return *e;
}

absl::StatusOr<Value> RunWith(absl::string_view source, const Customer& c) {
  Compiler::Builder cb;
  cb.DeclareVariable("c", CelType::Message("celwasm.testdata.Customer"));
  auto compiler = std::move(cb).Build();
  if (!compiler.ok()) return compiler.status();
  auto program = compiler->Compile(source);
  if (!program.ok()) return program.status();
  auto instance = Eng().Plan(*program);
  if (!instance.ok()) return instance.status();
  Activation a;
  a.Bind("c", Value::Message(c));
  return instance->Eval(a);
}

// Assertion #1: chained-grow arena admits a 70 KiB field read.
TEST(ProtoArenaLazyCopyInvariant, SizeOfHugeFieldSucceedsViaChainedArena) {
  Customer c;
  c.set_name(std::string(kHugeFieldBytes, 'X'));
  auto v = RunWith("size(c.name)", c);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kInt) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsInt(), static_cast<int64_t>(kHugeFieldBytes))
      << "size(c.name) over a 70 KiB field returned the wrong int — the "
         "chained-grow arena chain-walk regressed (cleanup-backlog #34)";
}

// Assertion #2: chained-grow arena also handles concat
// allocations whose result exceeds the first chunk — the
// follow-on chunk is sized to fit the immediate allocation
// (`pick_grow_size`'s `at_least_bytes` floor).
TEST(ProtoArenaLazyCopyInvariant, ConcatHugeFieldSucceedsViaChainedArena) {
  Customer c;
  c.set_name(std::string(kHugeFieldBytes, 'X'));
  auto v = RunWith(R"((c.name + ""))", c);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kString) << static_cast<int>(v->kind());
  EXPECT_EQ(v->AsString()->size(), kHugeFieldBytes);
}

// Sanity: the test's own setup is sound — a NORMAL-sized field
// concat works through the same code path that overflows for
// the huge one.  Catches a regression where Run() or the
// activation binding broke and every test would silently OK.
TEST(ProtoArenaLazyCopyInvariant, NormalSizedFieldConcatWorks) {
  Customer c;
  c.set_name("Ada");
  auto v = RunWith(R"((c.name + " Lovelace"))", c);
  ASSERT_TRUE(v.ok()) << v.status();
  ASSERT_EQ(v->kind(), Value::Kind::kString) << static_cast<int>(v->kind());
  EXPECT_EQ(*v->AsString(), "Ada Lovelace");
}

}  // namespace
}  // namespace celwasm
