#include "compiler_v2/api/compiler.h"

#include <utility>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "compiler_v2/api/program.h"
#include "compiler_v2/compile.h"

namespace cel {

Compiler::Builder Compiler::NewBuilder() {
  return {};
}

absl::StatusOr<Compiler> Compiler::Builder::Build() && {
  // M1 has nothing to validate.  Future declaration setters
  // accumulate state on the Builder and Build() may then return
  // InvalidArgument for things like overload-id collisions, type
  // mismatches against the seed stdlib, etc.
  return Compiler();
}

absl::StatusOr<Program> Compiler::Compile(absl::string_view source,
                                          CompilerOptions opts) const {
  celwasm::CompileOptions inner;
  inner.mem_size_bytes = opts.mem_size_bytes;
  auto artifact_or = celwasm::Compile(source, inner);
  if (!artifact_or.ok()) return artifact_or.status();
  return Program(std::move(artifact_or->wasm_bytes));
}

}  // namespace cel
