#include "shared/type.h"

#include <memory>
#include <string>
#include <utility>

#include "absl/log/absl_check.h"
#include "absl/strings/string_view.h"

namespace celwasm {

CelType CelType::Bool() {
  CelType t;
  t.kind_ = Kind::kBool;
  return t;
}
CelType CelType::Int() {
  CelType t;
  t.kind_ = Kind::kInt;
  return t;
}
CelType CelType::Uint() {
  CelType t;
  t.kind_ = Kind::kUint;
  return t;
}
CelType CelType::Double() {
  CelType t;
  t.kind_ = Kind::kDouble;
  return t;
}
CelType CelType::String() {
  CelType t;
  t.kind_ = Kind::kString;
  return t;
}
CelType CelType::Bytes() {
  CelType t;
  t.kind_ = Kind::kBytes;
  return t;
}
CelType CelType::Duration() {
  CelType t;
  t.kind_ = Kind::kDuration;
  return t;
}
CelType CelType::Timestamp() {
  CelType t;
  t.kind_ = Kind::kTimestamp;
  return t;
}
CelType CelType::Type() {
  CelType t;
  t.kind_ = Kind::kType;
  return t;
}
CelType CelType::Null() {
  CelType t;
  t.kind_ = Kind::kNull;
  return t;
}

CelType CelType::Message(std::string fully_qualified_name) {
  CelType t;
  t.kind_ = Kind::kMessage;
  t.message_name_ = std::move(fully_qualified_name);
  return t;
}

CelType CelType::List(CelType element) {
  CelType t;
  t.kind_ = Kind::kList;
  t.list_element_ = std::make_shared<CelType>(std::move(element));
  return t;
}

CelType CelType::Map(CelType key, CelType value) {
  CelType t;
  t.kind_ = Kind::kMap;
  t.map_kv_ = std::make_shared<std::pair<CelType, CelType>>(std::move(key),
                                                            std::move(value));
  return t;
}

CelType CelType::Optional(CelType element) {
  CelType t;
  t.kind_ = Kind::kOptional;
  t.optional_element_ = std::make_shared<CelType>(std::move(element));
  return t;
}

absl::string_view CelType::message_fully_qualified_name() const {
  ABSL_CHECK(kind_ == Kind::kMessage)
      << "message_fully_qualified_name on a " << CelTypeKindName(kind_);
  return message_name_;
}

const CelType& CelType::list_element() const {
  ABSL_CHECK(kind_ == Kind::kList)
      << "list_element on a " << CelTypeKindName(kind_);
  return *list_element_;
}

const CelType& CelType::map_key() const {
  ABSL_CHECK(kind_ == Kind::kMap) << "map_key on a " << CelTypeKindName(kind_);
  return map_kv_->first;
}

const CelType& CelType::map_value() const {
  ABSL_CHECK(kind_ == Kind::kMap)
      << "map_value on a " << CelTypeKindName(kind_);
  return map_kv_->second;
}

const CelType& CelType::optional_element() const {
  ABSL_CHECK(kind_ == Kind::kOptional)
      << "optional_element on a " << CelTypeKindName(kind_);
  return *optional_element_;
}

bool CelType::IsDeclarableAsVariable() const {
  return kind_ != Kind::kUnknown && kind_ != Kind::kNull &&
         kind_ != Kind::kOptional;
}

bool CelType::operator==(const CelType& other) const {
  if (kind_ != other.kind_) return false;
  // Scalars (kBool / kInt / ... / kType) carry no extra state; the
  // kind tag alone determines equality.  Container kinds compare their
  // payload.  An if/else chain (rather than a switch) keeps
  // `bugprone-branch-clone` from flattening the three structurally-
  // similar `return _ == _` arms into one warning.
  if (kind_ == Kind::kMessage) {
    return message_name_ == other.message_name_;
  }
  if (kind_ == Kind::kList) {
    return *list_element_ == *other.list_element_;
  }
  if (kind_ == Kind::kMap) {
    return map_kv_->first == other.map_kv_->first &&
           map_kv_->second == other.map_kv_->second;
  }
  if (kind_ == Kind::kOptional) {
    return *optional_element_ == *other.optional_element_;
  }
  return true;
}

absl::string_view CelTypeKindName(CelType::Kind k) {
  switch (k) {
    case CelType::Kind::kUnknown:
      return "unknown";
    case CelType::Kind::kBool:
      return "bool";
    case CelType::Kind::kInt:
      return "int";
    case CelType::Kind::kUint:
      return "uint";
    case CelType::Kind::kDouble:
      return "double";
    case CelType::Kind::kString:
      return "string";
    case CelType::Kind::kBytes:
      return "bytes";
    case CelType::Kind::kList:
      return "list";
    case CelType::Kind::kMap:
      return "map";
    case CelType::Kind::kMessage:
      return "message";
    case CelType::Kind::kDuration:
      return "duration";
    case CelType::Kind::kTimestamp:
      return "timestamp";
    case CelType::Kind::kType:
      return "type";
    case CelType::Kind::kNull:
      return "null";
    case CelType::Kind::kOptional:
      return "optional";
  }
  ABSL_CHECK(false) << "unhandled CelType::Kind = " << static_cast<int>(k);
}

}  // namespace celwasm
