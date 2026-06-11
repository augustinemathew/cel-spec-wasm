#include "e2e/fuzz/compare.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

#include "absl/strings/str_cat.h"
#include "cel/expr/value.pb.h"
#include "eval/internal/cel_host.h"
#include "eval/value.h"
#include "shared/type.h"

namespace celwasm::fuzz {
namespace {

std::string KindName(Value::Kind k) {
  using K = Value::Kind;
  switch (k) {
    case K::kBool:      return "Bool";
    case K::kInt:       return "Int";
    case K::kUint:      return "Uint";
    case K::kDouble:    return "Double";
    case K::kString:    return "String";
    case K::kBytes:     return "Bytes";
    case K::kList:      return "List";
    case K::kMap:       return "Map";
    case K::kMessage:   return "Message";
    case K::kNull:      return "Null";
    case K::kError:     return "Error";
    case K::kUnknown:   return "Unknown";
    case K::kDuration:  return "Duration";
    case K::kTimestamp: return "Timestamp";
    case K::kType:      return "Type";
    default:            return absl::StrCat("<k=", static_cast<int>(k), ">");
  }
}

CompareResult WrongKind(const Value& ours, std::string oracle_render) {
  return {/*equal=*/false,
          absl::StrCat("<wrong-kind=", KindName(ours.kind()), ">"),
          std::move(oracle_render)};
}

std::string FmtScalar(const Value& v) {
  using K = Value::Kind;
  switch (v.kind()) {
    case K::kBool:   return *v.AsBool() ? "true" : "false";
    case K::kInt:    return std::to_string(*v.AsInt());
    case K::kUint:   return std::to_string(*v.AsUint()) + "u";
    case K::kDouble: return std::to_string(*v.AsDouble());
    case K::kString: return absl::StrCat("\"", *v.AsString(), "\"");
    case K::kBytes:  return absl::StrCat("b\"", *v.AsBytes(), "\"");
    default:         return absl::StrCat("<scalar=", KindName(v.kind()), ">");
  }
}

std::string FmtOracleScalar(const cel::expr::Value& v) {
  switch (v.kind_case()) {
    case cel::expr::Value::kBoolValue:
      return v.bool_value() ? "true" : "false";
    case cel::expr::Value::kInt64Value:
      return std::to_string(v.int64_value());
    case cel::expr::Value::kUint64Value:
      return std::to_string(v.uint64_value()) + "u";
    case cel::expr::Value::kDoubleValue:
      return std::to_string(v.double_value());
    case cel::expr::Value::kStringValue:
      return absl::StrCat("\"", v.string_value(), "\"");
    case cel::expr::Value::kBytesValue:
      return absl::StrCat("b\"", v.bytes_value(), "\"");
    default:
      return absl::StrCat("<oracle-kind=", static_cast<int>(v.kind_case()),
                          ">");
  }
}

// Scalar payload equality; both sides must already be the expected
// kind.  Doubles use NaN-agreement (matched NaNs agree), matching
// the conformance comparison discipline.
bool ScalarsEqual(const Value& ours, const cel::expr::Value& oracle) {
  using K = Value::Kind;
  switch (ours.kind()) {
    case K::kBool:   return *ours.AsBool() == oracle.bool_value();
    case K::kInt:    return *ours.AsInt() == oracle.int64_value();
    case K::kUint:   return *ours.AsUint() == oracle.uint64_value();
    case K::kDouble: {
      const double a = *ours.AsDouble();
      const double b = oracle.double_value();
      if (std::isnan(a) && std::isnan(b)) return true;
      return a == b;
    }
    case K::kString: return *ours.AsString() == oracle.string_value();
    case K::kBytes:  return *ours.AsBytes() == oracle.bytes_value();
    default:         return false;
  }
}

Value::Kind ExpectedKind(const CelType& t) {
  using TK = CelType::Kind;
  using VK = Value::Kind;
  switch (t.kind()) {
    case TK::kBool:   return VK::kBool;
    case TK::kInt:    return VK::kInt;
    case TK::kUint:   return VK::kUint;
    case TK::kDouble: return VK::kDouble;
    case TK::kString: return VK::kString;
    case TK::kBytes:  return VK::kBytes;
    default:          return VK::kUnknown;
  }
}

CompareResult CompareScalar(const Value& ours, const cel::expr::Value& oracle,
                            const CelType& t) {
  if (ours.kind() != ExpectedKind(t)) {
    return WrongKind(ours, FmtOracleScalar(oracle));
  }
  return {ScalarsEqual(ours, oracle), FmtScalar(ours),
          FmtOracleScalar(oracle)};
}

template <typename OracleList>
std::string RenderOracleList(const OracleList& oracle_list) {
  std::string out = "[";
  for (int i = 0; i < oracle_list.size(); ++i) {
    if (i != 0) out += ", ";
    out += FmtOracleScalar(oracle_list[i]);
  }
  return out + "]";
}

// Walk our list elements, rendering each and recursing on the
// matching oracle element; clears `equal` on any mismatch.
template <typename OracleList>
std::string CompareListElems(const HostListBacking& backing,
                             const OracleList& oracle_list, const CelType& elt,
                             bool& equal) {
  std::string out = "[";
  for (std::size_t i = 0; i < backing.Size(); ++i) {
    if (i != 0) out += ", ";
    auto el_or = backing.At(i, elt);
    if (!el_or.ok()) {
      out += "<err>";
      equal = false;
      continue;
    }
    if (static_cast<int>(i) < oracle_list.size()) {
      CompareResult sub =
          Compare(*el_or, oracle_list[static_cast<int>(i)], elt);
      out += sub.ours;
      equal = equal && sub.equal;
    } else {
      out += FmtScalar(*el_or);
    }
  }
  return out + "]";
}

CompareResult CompareList(const Value& ours, const cel::expr::Value& oracle,
                          const CelType& t) {
  const auto& oracle_list = oracle.list_value().values();
  if (ours.kind() != Value::Kind::kList) {
    return WrongKind(ours, absl::StrCat("[", oracle_list.size(), " elems]"));
  }
  auto backing_or = ours.ListBacking();
  if (!backing_or.ok()) {
    return {/*equal=*/false,
            absl::StrCat("<backing-err=", backing_or.status().message(), ">"),
            absl::StrCat("[", oracle_list.size(), " elems]")};
  }
  bool equal = static_cast<int>((*backing_or)->Size()) == oracle_list.size();
  std::string ours_render =
      CompareListElems(**backing_or, oracle_list, t.list_element(), equal);
  return {equal, std::move(ours_render), RenderOracleList(oracle_list)};
}

template <typename OracleEntries>
std::string RenderOracleMap(const OracleEntries& oracle_entries) {
  std::string out = "{";
  for (int i = 0; i < oracle_entries.size(); ++i) {
    if (i != 0) out += ", ";
    out += FmtOracleScalar(oracle_entries[i].key()) + ": " +
           FmtOracleScalar(oracle_entries[i].value());
  }
  return out + "}";
}

// Key-set comparison: for each of our entries, find the oracle
// entry with the equal key and recurse on the value; clears
// `equal` on a missing key or value mismatch.  Order is not
// compared — CEL map iteration order is not semantics.
template <typename OracleEntries>
std::string CompareMapEntries(const HostMapBacking& backing,
                              const OracleEntries& oracle_entries,
                              const CelType& value_t, bool& equal) {
  std::string out = "{";
  bool first = true;
  backing.ForEach([&](const Value& k, const Value& v) {
    if (!first) out += ", ";
    first = false;
    out += FmtScalar(k) + ": " + FmtScalar(v);
    bool found = false;
    for (const auto& oe : oracle_entries) {
      if (ScalarsEqual(k, oe.key())) {
        found = true;
        equal = equal && Compare(v, oe.value(), value_t).equal;
        break;
      }
    }
    equal = equal && found;
  });
  return out + "}";
}

CompareResult CompareMap(const Value& ours, const cel::expr::Value& oracle,
                         const CelType& t) {
  const auto& oracle_entries = oracle.map_value().entries();
  if (ours.kind() != Value::Kind::kMap) {
    return WrongKind(ours,
                     absl::StrCat("{", oracle_entries.size(), " entries}"));
  }
  auto backing_or = ours.MapBacking();
  if (!backing_or.ok()) {
    return {/*equal=*/false,
            absl::StrCat("<backing-err=", backing_or.status().message(), ">"),
            absl::StrCat("{", oracle_entries.size(), " entries}")};
  }
  bool equal = static_cast<int>((*backing_or)->Size()) == oracle_entries.size();
  std::string ours_render =
      CompareMapEntries(**backing_or, oracle_entries, t.map_value(), equal);
  return {equal, std::move(ours_render), RenderOracleMap(oracle_entries)};
}

}  // namespace

CompareResult Compare(const Value& ours, const cel::expr::Value& oracle,
                      const CelType& t) {
  using TK = CelType::Kind;
  switch (t.kind()) {
    case TK::kBool:
    case TK::kInt:
    case TK::kUint:
    case TK::kDouble:
    case TK::kString:
    case TK::kBytes:
      return CompareScalar(ours, oracle, t);
    case TK::kList:
      return CompareList(ours, oracle, t);
    case TK::kMap:
      return CompareMap(ours, oracle, t);
    default:
      return {/*equal=*/false, "<unsupported-type>", "<unsupported-type>"};
  }
}

}  // namespace celwasm::fuzz
