// celwasmc-eval: compile a CEL expression and run it under wasmtime.
//
// The "compile" half is the same pipeline `celwasmc --emit-wasm` drives
// (parse → check → lower → serialize); the "eval" half is
// `compiler/host/host_loader`'s `LoadEval` + `CallNullaryEval` /
// `CallEval`.  Adds one flag on top of the compile CLI:
//
//   --unknown_attrs=var.q1.q2[,var2.*.q]...
//
// Each comma-separated pattern is parsed via
// `ParseUnknownAttributePattern` and installed with
// `LoadedEval::SetUnknownPatterns`; any `cel_host.get_field` call whose
// rooted attribute path FULL-matches a pattern produces CEL_UNKNOWN.
//
// Scope and non-goals:
//   - Only builds on darwin-arm64 today (same constraint as every other
//     host_loader consumer — we vendor wasmtime for that triple only).
//   - Variables are bound to zeroed / null arguments.  That's fine for
//     partial-eval testing — if every access to a variable FULL-matches
//     an unknown pattern, the trampoline never reads the null externref
//     — but an eval that actually touches a variable's value without a
//     covering pattern will trap.  A future slice can grow a
//     `--var_value=name=json` flag if the need materialises.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "compiler/codegen/abi.h"
#include "compiler/codegen/expr_lower.h"
#include "compiler/codegen/module.h"
#include "compiler/frontend/parse_and_check.h"
#include "compiler/host/attribute.h"
#include "compiler/host/host_loader.h"
#include "compiler/ir/static_subset.h"
#include "compiler/ir/typed_ast.h"
#include "wasm.h"
#include "wasmtime.h"

// ABSL_FLAG expands to a file-scope variable plus two macro-named
// helper structs.  clang-tidy flags all of them:
//   - `misc-use-internal-linkage` — wants `static` / anon-namespace,
//     but the macro is designed around external linkage.
//   - `bugprone-throwing-static-initialization` — `std::string` flag
//     defaults can throw on bad_alloc, but the same risk applies to
//     every absl-based binary; ABSL accepts it.
// Both are tracked in doc/implementation-plan/lint-backlog.md for the
// other CLI (celwasmc_main.cc); suppressing here keeps the new
// binary consistent rather than expanding the backlog further.
// NOLINTBEGIN(misc-use-internal-linkage,bugprone-throwing-static-initialization)
ABSL_FLAG(std::string, e, "", "CEL expression to compile and evaluate");
ABSL_FLAG(std::string, description, "<input>",
          "Source description used in parser/checker diagnostics");
ABSL_FLAG(bool, reject_dyn, true,
          "Reject expressions with any DYN-typed nodes");
ABSL_FLAG(std::string, schema, "",
          "Path to a textual .proto source file describing proto message "
          "types referenced by --var.  Mutually exclusive with "
          "--schema_descriptorset.");
ABSL_FLAG(std::string, schema_descriptorset, "",
          "Path to a binary google.protobuf.FileDescriptorSet describing "
          "proto message types referenced by --var.");
ABSL_FLAG(std::string, var, "",
          "Variable declarations 'name:Type' separated by ';'.  Variables "
          "are bound to zero / null-externref at eval time; use "
          "--unknown_attrs to force every access through the UNKNOWN "
          "path if you do not want the null-externref read to trap.");
ABSL_FLAG(std::string, container, "",
          "Name resolution container (like CEL-Go's container option)");
ABSL_FLAG(std::string, unknown_attrs, "",
          "Comma-separated list of attribute patterns to treat as UNKNOWN. "
          "Each entry is a dotted path `variable.qualifier[.qualifier...]` "
          "with `*` as the wildcard (matches any qualifier at that "
          "position).  Any `cel_host.get_field` call whose rooted "
          "attribute path FULL-matches a pattern produces a CelValue "
          "with kind=CEL_UNKNOWN.");
// NOLINTEND(misc-use-internal-linkage,bugprone-throwing-static-initialization)

namespace {

absl::StatusOr<std::vector<uint8_t>> CompileToBytes(
    absl::string_view expression, const celwasm::CheckOptions& opts,
    bool reject_dyn) {
  auto typed = celwasm::ParseAndCheck(expression, opts);
  if (!typed.ok()) return typed.status();
  if (reject_dyn) {
    if (auto s = celwasm::RejectDyn(typed->ast()); !s.ok()) return s;
  }
  celwasm::WasmModule mod;
  auto fn = celwasm::LowerToEvalFunction(*typed, "eval", mod);
  if (!fn.ok()) return fn.status();
  mod.ExportFunction("eval", "eval");
  auto abi = celwasm::BuildCelAbi(*typed, expression);
  if (!abi.ok()) return abi.status();
  if (auto s = celwasm::AttachCelAbiSection(mod, *abi); !s.ok()) return s;
  if (auto s = mod.Validate(); !s.ok()) return s;
  return mod.Serialize();
}

absl::StatusOr<std::vector<celwasm::AttributePattern>> ParsePatternList(
    absl::string_view flag_value) {
  std::vector<celwasm::AttributePattern> out;
  for (absl::string_view text :
       absl::StrSplit(flag_value, ',', absl::SkipEmpty{})) {
    auto p = celwasm::ParseUnknownAttributePattern(text);
    if (!p.ok()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "--unknown_attrs: ", p.status().message(), " (at `", text, "`)"));
    }
    out.push_back(*std::move(p));
  }
  return out;
}

// Builds one `wasmtime_val_t` per declared variable, in the same order
// codegen uses when shaping the `$eval` function's parameter list.
// kMessage → null externref; scalar Reprs → zero-valued i32 / i64 / f64.
std::vector<wasmtime_val_t> ZeroArgsFor(
    const std::vector<celwasm::Variable>& variables) {
  std::vector<wasmtime_val_t> out;
  out.reserve(variables.size());
  for (const celwasm::Variable& v : variables) {
    wasmtime_val_t arg{};
    switch (v.repr) {
      case celwasm::Repr::kInt:
      case celwasm::Repr::kUint:
        arg.kind = WASMTIME_I64;
        arg.of.i64 = 0;
        break;
      case celwasm::Repr::kDouble:
        arg.kind = WASMTIME_F64;
        arg.of.f64 = 0.0;
        break;
      case celwasm::Repr::kMessage:
        // `arg.of.externref` was value-initialized to zero via the
        // `wasmtime_val_t arg{}` above; `wasmtime_externref_is_null`
        // treats that as null (see `compiler/e2e/cel_refs_e2e_test.cc`
        // line 219 for the canonical null-check shape).
        arg.kind = WASMTIME_EXTERNREF;
        break;
      default:
        // bool, string, bytes, etc. all travel as CelValue arena offsets
        // today (i32).  A zero offset is the runtime's null sentinel.
        arg.kind = WASMTIME_I32;
        arg.of.i32 = 0;
        break;
    }
    out.push_back(arg);
  }
  return out;
}

// Prints `v` in a shape suitable for CLI consumption (one line, no
// trailing newline added — caller handles).  `v` must come from
// `DecodeSlot`, so string / bytes / message kinds arrive as an
// arena-relative i32 offset; we print those as `@<offset>` rather than
// dereferencing, because the runtime arena gets reset between evals and
// the caller has no stable handle to dump the payload.
void PrintValue(const wasmtime_val_t& v) {
  switch (v.kind) {
    case WASMTIME_I32:
      std::fprintf(stdout, "%d\n", static_cast<int>(v.of.i32));
      break;
    case WASMTIME_I64:
      std::fprintf(stdout, "%lld\n", static_cast<long long>(v.of.i64));
      break;
    case WASMTIME_F64:
      std::fprintf(stdout, "%g\n", v.of.f64);
      break;
    default:
      std::fprintf(stdout, "<wasmtime kind %d>\n", static_cast<int>(v.kind));
      break;
  }
}

// Maps the `absl::InternalError` text `DecodeSlot` emits for non-OK
// result kinds back to a stable CLI-visible token.  Anything we do not
// recognise is surfaced verbatim under an `error:` prefix so the user
// can tell a CEL-level ERROR apart from, say, a wasmtime trap.
int PrintErrorOrUnknown(const absl::Status& s) {
  const std::string msg(s.message());
  if (absl::StrContains(msg, "UNKNOWN")) {
    std::fprintf(stdout, "unknown\n");
    return 0;  // unknown is a valid CEL result, not a CLI failure.
  }
  if (absl::StrContains(msg, "ERROR")) {
    std::fprintf(stdout, "error\n");
    return 0;
  }
  std::fprintf(stderr, "eval error: %s\n", msg.c_str());
  return 1;
}

// Installs `--unknown_attrs` patterns on `loaded`.  Returns 0 on
// success; on failure prints the diagnostic on stderr and returns the
// CLI exit code (always 1 — parse / installation failures are hard
// config errors, not CEL-level UNKNOWN / ERROR).
int InstallUnknownPatterns(celwasm::LoadedEval& loaded,
                           absl::string_view unknown_attrs_flag) {
  if (unknown_attrs_flag.empty()) return 0;
  auto patterns = ParsePatternList(unknown_attrs_flag);
  if (!patterns.ok()) {
    std::fprintf(stderr, "%s\n",
                 std::string(patterns.status().message()).c_str());
    return 1;
  }
  if (auto s = loaded.SetUnknownPatterns(*std::move(patterns)); !s.ok()) {
    std::fprintf(stderr, "SetUnknownPatterns: %s\n",
                 std::string(s.message()).c_str());
    return 1;
  }
  return 0;
}

int Run(absl::string_view expression, const celwasm::CheckOptions& opts,
        bool reject_dyn, absl::string_view unknown_attrs_flag) {
  auto typed = celwasm::ParseAndCheck(expression, opts);
  if (!typed.ok()) {
    std::fprintf(stderr, "%s\n", std::string(typed.status().message()).c_str());
    return 1;
  }
  if (reject_dyn) {
    if (auto s = celwasm::RejectDyn(typed->ast()); !s.ok()) {
      std::fprintf(stderr, "%s\n", std::string(s.message()).c_str());
      return 1;
    }
  }
  const std::vector<celwasm::Variable> vars = typed->variables();

  auto bytes = CompileToBytes(expression, opts, reject_dyn);
  if (!bytes.ok()) {
    std::fprintf(stderr, "%s\n", std::string(bytes.status().message()).c_str());
    return 1;
  }

  auto loaded = celwasm::LoadEval(*bytes);
  if (!loaded.ok()) {
    std::fprintf(stderr, "load error: %s\n",
                 std::string(loaded.status().message()).c_str());
    return 1;
  }

  if (int rc = InstallUnknownPatterns(*loaded, unknown_attrs_flag); rc != 0) {
    return rc;
  }

  const std::vector<wasmtime_val_t> args = ZeroArgsFor(vars);
  auto result = args.empty() ? loaded->CallNullaryEval()
                             : loaded->CallEval(absl::MakeConstSpan(args));
  if (!result.ok()) return PrintErrorOrUnknown(result.status());
  PrintValue(*result);
  return 0;
}

}  // namespace

// absl/proto allocation may throw, but we let the runtime terminate
// rather than catching in a CLI entry point.
int main(int argc, char** argv) {  // NOLINT(bugprone-exception-escape)
  absl::ParseCommandLine(argc, argv);

  const std::string& expression = absl::GetFlag(FLAGS_e);
  if (expression.empty()) {
    std::fprintf(stderr,
                 "usage: celwasmc-eval -e \"<cel expression>\" "
                 "[--schema=<source.proto> | "
                 "--schema_descriptorset=<fds.pb.bin>] "
                 "[--var=name:Type[;...]] [--container=<pkg>] "
                 "[--unknown_attrs=var.q[,...]]\n");
    return 2;
  }

  celwasm::CheckOptions opts;
  opts.schema_proto_path = absl::GetFlag(FLAGS_schema);
  opts.schema_descriptor_set_path = absl::GetFlag(FLAGS_schema_descriptorset);
  const std::string var_flag = absl::GetFlag(FLAGS_var);
  if (!var_flag.empty()) {
    for (absl::string_view spec :
         absl::StrSplit(var_flag, ';', absl::SkipEmpty{})) {
      opts.variable_specs.emplace_back(spec);
    }
  }
  opts.container = absl::GetFlag(FLAGS_container);
  opts.description = absl::GetFlag(FLAGS_description);

  return Run(expression, opts, absl::GetFlag(FLAGS_reject_dyn),
             absl::GetFlag(FLAGS_unknown_attrs));
}
