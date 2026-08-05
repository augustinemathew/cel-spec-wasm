// CelType — the user-facing static type for declarations (variables,
// function args/return, map keys/values, list elements).  Mirrors the
// kinds carried on the wire in `cel.abi.CelType` (see
// `doc/implementation-plan/rewrite/cel-host-surface.md` §6) without
// depending on protobuf at the public surface.
//
// Construction is by named factory (`CelType::Int()`,
// `CelType::List(CelType::String())`, `CelType::Message("com.ex.Foo")`)
// so the type hierarchy reads top-down at the call site.

#ifndef CELWASM_SHARED_TYPE_H_
#define CELWASM_SHARED_TYPE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "absl/strings/string_view.h"

namespace celwasm {

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
    // Value 10 is deliberately vacant: it belonged to the old wire
    // CEL_TYPE tag before the type-of-types moved to 13 — never
    // reuse it.
    kDuration = 11,
    kTimestamp = 12,
    // type-of-types declarable as a variable type
    // (`Bind("t", Value::Type(name))`).  Mirrors
    // `celwasm::Value::Kind::kType = 13`.  See
    // `rewrite/m9-type-subsystem.md`.
    kType = 13,
    // CEL `null` as a signature type (the celfn grammar's `null`).
    // Not declarable as a variable type.
    kNull = 14,
    // CEL `optional<T>` as a signature type.  Element type in
    // optional_element().  Not declarable as a variable type.
    kOptional = 15,
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
  // Declare a variable typed as `type` (the type-of-types).
  // Bound values must be `Value::Type(name)`.
  static CelType Type();
  // CEL `null` as a signature type.  Signature-only: not
  // declarable as a variable type (see IsDeclarableAsVariable).
  static CelType Null();

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
  // CEL `optional<T>` (signature-only, like Null()).
  static CelType Optional(CelType element);

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

  // Only valid when kind() == kOptional.
  const CelType& optional_element() const;

  // Whether this type may appear in a
  // `Compiler::Builder::DeclareVariable` declaration.  False for
  // kUnknown (default-constructed sentinel), kNull, and kOptional
  // (signature-only kinds — the checker has no variable spelling
  // for them); true for every other kind.
  bool IsDeclarableAsVariable() const;

  // Default: Kind::kUnknown — used as a sentinel when the caller
  // hasn't populated the field yet.  Not a valid declaration.
  CelType() = default;

 private:
  Kind kind_ = Kind::kUnknown;
  std::string message_name_;                             // kMessage only
  std::shared_ptr<CelType> list_element_;                // kList only
  std::shared_ptr<std::pair<CelType, CelType>> map_kv_;  // kMap only
  std::shared_ptr<CelType> optional_element_;            // kOptional only
};

absl::string_view CelTypeKindName(CelType::Kind k);

}  // namespace celwasm

#endif  // CELWASM_SHARED_TYPE_H_
