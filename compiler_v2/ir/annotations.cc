#include "compiler_v2/ir/annotations.h"

#include "absl/log/absl_check.h"
#include "absl/strings/string_view.h"

namespace celwasm {

absl::string_view ReprName(Repr r) {
  switch (r) {
    case Repr::kUnknown:
      return "unknown";
    case Repr::kNull:
      return "null";
    case Repr::kBool:
      return "bool";
    case Repr::kInt:
      return "int";
    case Repr::kUint:
      return "uint";
    case Repr::kDouble:
      return "double";
    case Repr::kString:
      return "string";
    case Repr::kBytes:
      return "bytes";
    case Repr::kList:
      return "list";
    case Repr::kMap:
      return "map";
    case Repr::kMessage:
      return "message";
    case Repr::kEnum:
      return "enum";
    case Repr::kDuration:
      return "duration";
    case Repr::kTimestamp:
      return "timestamp";
    case Repr::kType:
      return "type";
  }
  return "?";
}

absl::string_view StorageKindName(StorageKind k) {
  switch (k) {
    case StorageKind::kNone:
      return "none";
    case StorageKind::kStaticRodata:
      return "static_rodata";
    case StorageKind::kWorkspaceSlot:
      return "workspace_slot";
    case StorageKind::kLocal:
      return "local";
  }
  return "?";
}

absl::string_view OriginName(Origin o) {
  switch (o) {
    case Origin::kDynamic:
      return "dynamic";
    case Origin::kArena:
      return "arena";
    case Origin::kHost:
      return "host";
  }
  // Closed enum: any other value is an invariant violation.  Silent
  // fallback would miscompile new kinds added in future milestones.
  ABSL_CHECK(false) << "OriginName: unknown Origin = " << static_cast<int>(o);
}

}  // namespace celwasm
