#include "compiler/codegen/abi.h"

#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "binaryen-c.h"
#include "cel/expr/checked.pb.h"
#include "common/ast_proto.h"
#include "compiler/codegen/cel_abi.pb.h"
#include "compiler/codegen/module.h"
#include "compiler/ir/typed_ast.h"

namespace celwasm {

absl::StatusOr<CelAbi> BuildCelAbi(const TypedAst& typed,
                                   absl::string_view cel_source) {
  CelAbi abi;
  abi.set_version(kCelAbiVersion);
  abi.set_cel_source(std::string(cel_source));

  if (auto s = cel::AstToCheckedExpr(typed.ast(), abi.mutable_checked());
      !s.ok()) {
    return absl::InvalidArgumentError(
        absl::StrCat("AstToCheckedExpr failed: ", s.message()));
  }

  // `function_set`, tables, and `layout` stay at their defaults for
  // M2 — the codegen MVP never emits references to any of them.
  // M3+ will populate these as features land.
  abi.mutable_function_set();  // materialise as empty rather than unset.
  MemoryLayout* layout = abi.mutable_layout();
  layout->set_initial_pages(1);
  layout->set_max_pages(0);

  return abi;
}

absl::Status AttachCelAbiSection(WasmModule& mod, const CelAbi& abi) {
  std::string bytes;
  if (!abi.SerializeToString(&bytes)) {
    return absl::InternalError("CelAbi::SerializeToString returned false");
  }
  // Binaryen copies the bytes internally, so stack-allocated
  // storage is fine.  The name is likewise copied.
  const std::string section_name(kCelAbiSectionName);
  BinaryenAddCustomSection(mod.raw(), section_name.c_str(), bytes.data(),
                           static_cast<BinaryenIndex>(bytes.size()));
  return absl::OkStatus();
}

}  // namespace celwasm
