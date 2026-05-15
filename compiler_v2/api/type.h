// CelType — the user-facing static type for declarations (variables,
// function args/return, map keys/values, list elements).  Mirrors the
// kinds carried on the wire in `cel.abi.CelType` (see
// `doc/implementation-plan/rewrite/cel-host-surface.md` §6) without
// depending on protobuf at the public surface.
//
// Construction is by named factory (`CelType::Int()`,
// `CelType::List(CelType::String())`, `CelType::Message("com.ex.Foo")`)
// so the type hierarchy reads top-down at the call site.

#ifndef CELWASM_COMPILER_V2_API_TYPE_H_
#define CELWASM_COMPILER_V2_API_TYPE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "absl/strings/string_view.h"

namespace cel {

class CelType {
 public:
  enum class Kind : uint8_t {
    kUnknown = 0,
    kBool = 1,
    kInt = 2,
    kUint = 3,
    kDouble = 4,
    kString = 5,
    kBytes = 6,
    kList = 7,
    kMap = 8,
    kMessage = 9,
    kDuration = 11,
    kTimestamp = 12,
    // M9.A: type-of-types declarable as a variable type
    // (`Bind("t", Value::Type(name))`).  Mirrors
    // `cel::Value::Kind::kType = 13`.
    kType = 13,
  };

  // ——— Scalar factories ———
  static CelType Bool();
  static CelType Int();
  static CelType Uint();
  static CelType Double();
  static CelType String();
  static CelType Bytes();
  static CelType Duration();
  static CelType Timestamp();
  // M9.A: declare a variable typed as `type` (the type-of-types).
  // Bound values must be `Value::Type(name)`.
  static CelType Type();

  // ——— Container factories ———
  // Message by fully-qualified name.  The FQN is resolved to a
  // Descriptor at Compile time via the process-wide generated
  // descriptor pool (any statically-linked `cc_proto_library`
  // descriptor is reachable there).  The type itself is pure
  // metadata.
  static CelType Message(std::string fully_qualified_name);

  // List / Map element types.  Shallow copies held by shared_ptr
  // so CelType stays value-semantic and cheap to pass by value.
  static CelType List(CelType element);
  static CelType Map(CelType key, CelType value);

  // Accessors.
  Kind kind() const {
    return kind_;
  }
  bool operator==(const CelType& other) const;
  bool operator!=(const CelType& other) const {
    return !(*this == other);
  }

  // Only valid when kind() == kMessage.
  absl::string_view message_fully_qualified_name() const;

  // Only valid when kind() == kList.
  const CelType& list_element() const;

  // Only valid when kind() == kMap.
  const CelType& map_key() const;
  const CelType& map_value() const;

  // Default: Kind::kUnknown — used as a sentinel when the caller
  // hasn't populated the field yet.  Not a valid declaration.
  CelType() = default;

 private:
  Kind kind_ = Kind::kUnknown;
  std::string message_name_;                             // kMessage only
  std::shared_ptr<CelType> list_element_;                // kList only
  std::shared_ptr<std::pair<CelType, CelType>> map_kv_;  // kMap only
};

absl::string_view CelTypeKindName(CelType::Kind k);

}  // namespace cel

#endif  // CELWASM_COMPILER_V2_API_TYPE_H_
