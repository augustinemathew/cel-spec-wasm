#include "compiler/ir/annotations.h"

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

}  // namespace celwasm
