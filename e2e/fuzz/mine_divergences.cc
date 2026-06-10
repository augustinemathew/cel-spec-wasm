// Mine oracle divergences over a target type by sequential seed.
// Prints each divergence with its seed + source + ours-vs-oracle
// values; intended for triaging what the property test catches
// when fuzztest's unit-test mode buffers the EXPECT_EQ message.
// Run as:
//   bazel run //e2e/fuzz:mine_divergences -- <target> <max_seeds> <depth> [stop_after]
// `target` is one of bool / int / uint / double / string / bytes.
// `stop_after` (default 5) caps how many divergences before exit.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "cel/expr/value.pb.h"
#include "e2e/fuzz/oracle_harness.h"
#include "eval/internal/cel_host.h"
#include "eval/value.h"
#include "shared/type.h"

using ::celwasm::CelType;
using ::celwasm::HostListBacking;
using ::celwasm::HostMapBacking;
using ::celwasm::Value;
using ::celwasm::fuzz::GenAndEvalResult;
using ::celwasm::fuzz::GenAndEvalSliceC;
using ::celwasm::fuzz::GenAndEvalStatus;

namespace {

CelType ParseTarget(absl::string_view s) {
  if (s == "bool")   return CelType::Bool();
  if (s == "int")    return CelType::Int();
  if (s == "uint")   return CelType::Uint();
  if (s == "double") return CelType::Double();
  if (s == "string") return CelType::String();
  if (s == "bytes")  return CelType::Bytes();
  if (s == "list_int")    return CelType::List(CelType::Int());
  if (s == "list_bool")   return CelType::List(CelType::Bool());
  if (s == "list_double") return CelType::List(CelType::Double());
  if (s == "list_string") return CelType::List(CelType::String());
  if (s == "map_string_int")
    return CelType::Map(CelType::String(), CelType::Int());
  std::cerr << "unknown target `" << s << "`\n";
  std::exit(2);
}

// Returns a human-readable label for ours.kind() so `<wrong-kind>`
// failures actually name the kind we got.
std::string KindName(::celwasm::Value::Kind k) {
  using K = ::celwasm::Value::Kind;
  switch (k) {
    case K::kBool: return "Bool";
    case K::kInt: return "Int";
    case K::kUint: return "Uint";
    case K::kDouble: return "Double";
    case K::kString: return "String";
    case K::kBytes: return "Bytes";
    case K::kList: return "List";
    case K::kMap: return "Map";
    case K::kMessage: return "Message";
    case K::kNull: return "Null";
    case K::kError: return "Error";
    case K::kUnknown: return "Unknown";
    case K::kDuration: return "Duration";
    case K::kTimestamp: return "Timestamp";
    case K::kType: return "Type";
    default: return absl::StrCat("<k=", static_cast<int>(k), ">");
  }
}

// Render a single scalar leaf to a string for the divergence log.
// Used by list/map comparison — never produces a `<wrong-kind>` for
// the OUTER kind (the list/map check handles that); a wrong-kind
// scalar inside the container is rendered as `<scalar=Kind>`.
std::string FmtScalar(const ::celwasm::Value& v) {
  using K = ::celwasm::Value::Kind;
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
      return absl::StrCat("<oracle-kind=", static_cast<int>(v.kind_case()), ">");
  }
}

// Element-wise scalar equality between our Value and a cel-cpp Value.
// Both must already be of the same expected scalar kind.  Doubles
// use NaN-equality (matched NaNs agree).
bool ScalarsEqual(const ::celwasm::Value& ours, const cel::expr::Value& oracle) {
  using K = ::celwasm::Value::Kind;
  switch (ours.kind()) {
    case K::kBool:   return *ours.AsBool() == oracle.bool_value();
    case K::kInt:    return *ours.AsInt() == oracle.int64_value();
    case K::kUint:   return *ours.AsUint() == oracle.uint64_value();
    case K::kDouble: {
      const double a = *ours.AsDouble(), b = oracle.double_value();
      if (std::isnan(a) && std::isnan(b)) return true;
      return a == b;
    }
    case K::kString: return *ours.AsString() == oracle.string_value();
    case K::kBytes:  return *ours.AsBytes() == oracle.bytes_value();
    default:         return false;
  }
}

// list / map comparison.  `element_type` is the CEL type of list
// elements (or map values when comparing values).  Returns true on
// divergence; sets `ours_str` / `oracle_str` to a one-line render.
bool CompareList(const ::celwasm::Value& ours,
                 const cel::expr::Value& oracle,
                 const CelType& element_type,
                 std::string& ours_str, std::string& oracle_str) {
  if (ours.kind() != ::celwasm::Value::Kind::kList) {
    ours_str = absl::StrCat("<wrong-kind=", KindName(ours.kind()), ">");
    oracle_str = absl::StrCat("[", oracle.list_value().values_size(), " elems]");
    return true;
  }
  auto backing_or = ours.ListBacking();
  if (!backing_or.ok()) {
    ours_str = absl::StrCat("<backing-err=", backing_or.status().message(), ">");
    return true;
  }
  const HostListBacking* backing = *backing_or;
  const auto& oracle_list = oracle.list_value().values();
  const size_t our_n = backing->Size();
  const int oracle_n = oracle_list.size();

  std::string our_render = "[";
  std::string oracle_render = "[";
  bool diverges = (static_cast<int>(our_n) != oracle_n);
  for (size_t i = 0; i < our_n; ++i) {
    if (i != 0) our_render += ", ";
    auto el_or = backing->At(i, element_type);
    if (!el_or.ok()) { our_render += "<err>"; diverges = true; continue; }
    our_render += FmtScalar(*el_or);
    if (static_cast<int>(i) < oracle_n) {
      if (!ScalarsEqual(*el_or, oracle_list[i])) diverges = true;
    }
  }
  for (int i = 0; i < oracle_n; ++i) {
    if (i != 0) oracle_render += ", ";
    oracle_render += FmtOracleScalar(oracle_list[i]);
  }
  our_render += "]";
  oracle_render += "]";
  ours_str = our_render;
  oracle_str = oracle_render;
  return diverges;
}

// Map comparison.  Keys are compared as a set (we forEach our side,
// look up oracle's by key).  Map iteration order is not part of CEL
// semantics, so we don't compare order — only contents.
bool CompareMap(const ::celwasm::Value& ours,
                const cel::expr::Value& oracle,
                const CelType& key_type, const CelType& value_type,
                std::string& ours_str, std::string& oracle_str) {
  if (ours.kind() != ::celwasm::Value::Kind::kMap) {
    ours_str = absl::StrCat("<wrong-kind=", KindName(ours.kind()), ">");
    return true;
  }
  auto backing_or = ours.MapBacking();
  if (!backing_or.ok()) {
    ours_str = absl::StrCat("<backing-err=", backing_or.status().message(), ">");
    return true;
  }
  const HostMapBacking* backing = *backing_or;
  const auto& oracle_entries = oracle.map_value().entries();

  bool diverges = (static_cast<int>(backing->Size()) != oracle_entries.size());

  // Materialise our entries into a string + a key→value lookup we
  // then cross-check against the oracle.
  std::string our_render = "{";
  bool first = true;
  std::vector<std::pair<::celwasm::Value, ::celwasm::Value>> ours_entries;
  backing->ForEach([&](const ::celwasm::Value& k, const ::celwasm::Value& v) {
    if (!first) our_render += ", ";
    first = false;
    our_render += FmtScalar(k) + ": " + FmtScalar(v);
    ours_entries.emplace_back(k, v);
  });
  our_render += "}";

  std::string oracle_render = "{";
  for (int i = 0; i < oracle_entries.size(); ++i) {
    if (i != 0) oracle_render += ", ";
    oracle_render += FmtOracleScalar(oracle_entries[i].key()) + ": " +
                     FmtOracleScalar(oracle_entries[i].value());
  }
  oracle_render += "}";

  // For each of our entries, look up in oracle.
  for (const auto& [k, v] : ours_entries) {
    bool found = false;
    for (const auto& oe : oracle_entries) {
      if (ScalarsEqual(k, oe.key())) {
        if (!ScalarsEqual(v, oe.value())) diverges = true;
        found = true; break;
      }
    }
    if (!found) diverges = true;
  }
  (void)key_type; (void)value_type;
  ours_str = our_render;
  oracle_str = oracle_render;
  return diverges;
}

// Returns true on divergence (printed to stdout); false otherwise.
bool CompareAndReport(absl::string_view kind_label, uint64_t seed,
                      const GenAndEvalResult& r) {
  const Value& ours = r.ours;
  const cel::expr::Value& oracle = r.oracle;

  std::string ours_str, oracle_str;
  bool diverges = false;
  auto wrong_kind = [&] {
    return absl::StrCat("<wrong-kind=", KindName(ours.kind()), ">");
  };

  if (kind_label == "bool") {
    if (ours.kind() != Value::Kind::kBool) { ours_str = wrong_kind(); diverges = true; }
    else ours_str = *ours.AsBool() ? "true" : "false";
    oracle_str = oracle.bool_value() ? "true" : "false";
    if (!diverges) diverges = (*ours.AsBool() != oracle.bool_value());
  } else if (kind_label == "int") {
    if (ours.kind() != Value::Kind::kInt) { ours_str = wrong_kind(); diverges = true; }
    else ours_str = std::to_string(*ours.AsInt());
    oracle_str = std::to_string(oracle.int64_value());
    if (!diverges) diverges = (*ours.AsInt() != oracle.int64_value());
  } else if (kind_label == "uint") {
    if (ours.kind() != Value::Kind::kUint) { ours_str = wrong_kind(); diverges = true; }
    else ours_str = std::to_string(*ours.AsUint());
    oracle_str = std::to_string(oracle.uint64_value());
    if (!diverges) diverges = (*ours.AsUint() != oracle.uint64_value());
  } else if (kind_label == "double") {
    if (ours.kind() != Value::Kind::kDouble) { ours_str = wrong_kind(); diverges = true; }
    else ours_str = std::to_string(*ours.AsDouble());
    oracle_str = std::to_string(oracle.double_value());
    if (!diverges) {
      const double a = *ours.AsDouble(), b = oracle.double_value();
      if (std::isnan(a) && std::isnan(b)) diverges = false;
      else diverges = (a != b);
    }
  } else if (kind_label == "string") {
    if (ours.kind() != Value::Kind::kString) { ours_str = wrong_kind(); diverges = true; }
    else ours_str = std::string(*ours.AsString());
    oracle_str = oracle.string_value();
    if (!diverges) diverges = (*ours.AsString() != oracle.string_value());
  } else if (kind_label == "bytes") {
    if (ours.kind() != Value::Kind::kBytes) { ours_str = wrong_kind(); diverges = true; }
    else ours_str = std::string(*ours.AsBytes());
    oracle_str = oracle.bytes_value();
    if (!diverges) diverges = (*ours.AsBytes() != oracle.bytes_value());
  } else if (kind_label == "list_int") {
    diverges = CompareList(ours, oracle, CelType::Int(), ours_str, oracle_str);
  } else if (kind_label == "list_bool") {
    diverges = CompareList(ours, oracle, CelType::Bool(), ours_str, oracle_str);
  } else if (kind_label == "list_double") {
    diverges = CompareList(ours, oracle, CelType::Double(), ours_str, oracle_str);
  } else if (kind_label == "list_string") {
    diverges = CompareList(ours, oracle, CelType::String(), ours_str, oracle_str);
  } else if (kind_label == "map_string_int") {
    diverges = CompareMap(ours, oracle, CelType::String(), CelType::Int(),
                          ours_str, oracle_str);
  }

  if (diverges) {
    std::printf("DIVERGE [%s seed=%llu]\n", std::string(kind_label).c_str(),
                static_cast<unsigned long long>(seed));
    std::printf("  source = %s\n", r.source.c_str());
    std::printf("  ours   = %s\n", ours_str.c_str());
    std::printf("  oracle = %s\n", oracle_str.c_str());
    std::fflush(stdout);
  }
  return diverges;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4 || argc > 5) {
    std::cerr << "usage: " << argv[0]
              << " <target> <max_seeds> <depth> [stop_after=5]\n";
    return 1;
  }
  const absl::string_view target_str = argv[1];
  const CelType target = ParseTarget(target_str);
  const uint64_t max_seeds = std::strtoull(argv[2], nullptr, 10);
  const int depth = std::atoi(argv[3]);
  const int stop_after = (argc == 5) ? std::atoi(argv[4]) : 5;

  int diverged = 0;
  int our_rejected = 0;
  int oracle_rejected = 0;
  int oracle_error = 0;
  int too_large = 0;
  int agreed = 0;

  for (uint64_t seed = 1; seed <= max_seeds; ++seed) {
    GenAndEvalResult r;
    std::string err;
    GenAndEvalStatus st = GenAndEvalSliceC(target, seed, depth, r, &err);
    switch (st) {
      case GenAndEvalStatus::kOk:
        if (CompareAndReport(target_str, seed, r)) ++diverged;
        else ++agreed;
        break;
      case GenAndEvalStatus::kSourceTooLarge:
        ++too_large; break;
      case GenAndEvalStatus::kOurPipelineRejected:
        ++our_rejected;
        std::printf("OUR-REJECT [%s seed=%llu] %s\n  source = %s\n",
                    std::string(target_str).c_str(),
                    static_cast<unsigned long long>(seed), err.c_str(),
                    r.source.c_str());
        std::fflush(stdout);
        break;
      case GenAndEvalStatus::kOracleRejected:
        ++oracle_rejected;
        std::printf("ORACLE-REJECT [%s seed=%llu] %s\n  source = %s\n",
                    std::string(target_str).c_str(),
                    static_cast<unsigned long long>(seed), err.c_str(),
                    r.source.c_str());
        std::fflush(stdout);
        break;
      case GenAndEvalStatus::kOracleErrorValue:
        ++oracle_error;
        std::printf("ORACLE-ERR-VAL [%s seed=%llu] %s\n  source = %s\n",
                    std::string(target_str).c_str(),
                    static_cast<unsigned long long>(seed), err.c_str(),
                    r.source.c_str());
        std::fflush(stdout);
        break;
    }
    if (diverged + our_rejected >= stop_after) break;
  }

  std::printf("\n--- summary [%s, depth=%d] ---\n",
              std::string(target_str).c_str(), depth);
  std::printf("agreed=%d  diverged=%d  our_rejected=%d  oracle_rejected=%d  "
              "oracle_error=%d  too_large=%d\n",
              agreed, diverged, our_rejected, oracle_rejected,
              oracle_error, too_large);
  return 0;
}
