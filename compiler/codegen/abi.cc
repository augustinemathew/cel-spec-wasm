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
#include "compiler/codegen/field_name_pool.h"
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

  // Emit the `(field_number, name)` intern table used by
  // `cel_host.get_field` / `cel_host.has_field` call sites.  The
  // walk order in `FromTypedAst` matches the one in
  // `LowerToEvalFunction`, so the intern IDs the host sees here are
  // the same ones the emitted wasm passes at call time.
  const FieldNamePool pool = FieldNamePool::FromTypedAst(typed);
  uint32_t id = 0;
  for (const FieldNamePool::Entry& e : pool.entries()) {
    FieldEntry* row = abi.add_fields();
    row->set_id(id++);
    row->set_field_number(e.field_number);
    row->set_name(e.name);
  }

  // `function_set` and `layout` stay at their defaults — the
  // codegen MVP never emits references to a custom `FunctionSet`,
  // and `MemoryLayout` mirrors the runtime's single-page arena.
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
