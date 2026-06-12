#include "e2e/fuzz/catalog.h"

#include <string>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "e2e/fuzz/grammar.h"
#include "shared/type.h"

namespace celwasm::fuzz {

namespace {

// Timestamp / duration — leaves, accessors, comparisons, and the
// int/string conversions.  All standard library (no oracle
// extension needed).  Accessors return Int; comparisons Bool;
// `string(...)` String; `int(...)` Int.  The `_with_tz` accessor
// variants (timezone-string second arg) are deferred — they need a
// tz-string leaf set.  The max-range timestamp
// (`9999-12-31T23:59:59.999999999Z`) is left out of the leaves
// until `MaxRangeTimestampConstruction` (known_bugs) is fixed.
// Timestamp accessors — the no-tz forms and the with-tz forms (the
// tz is a FIXED valid zone baked into the template; an arbitrary
// generated string would just be an invalid-tz error, exercising no
// tz logic).  The with-tz zones rotate across IANA names, fixed
// offsets, and UTC to cover the cctz lookup + offset paths; both
// engines share the cctz database.
void RegisterTimestampAccessors(GrammarBuilder& b) {
  const CelType ts = CelType::Timestamp();
  const CelType i = CelType::Int();
  for (const char* m : {"getFullYear", "getMonth", "getDayOfMonth", "getDate",
                        "getDayOfWeek", "getDayOfYear", "getHours",
                        "getMinutes", "getSeconds", "getMilliseconds"}) {
    b.Unary(i, std::string("ts_") + m, std::string("(%0).") + m + "()", ts);
  }
  struct TzAcc {
    const char* method;
    const char* tz;
  };
  const TzAcc tzs[] = {
      {"getFullYear", "America/New_York"},
      {"getMonth", "Australia/Sydney"},
      {"getDayOfMonth", "Europe/London"},
      {"getDate", "Asia/Tokyo"},
      {"getDayOfWeek", "-05:00"},
      {"getDayOfYear", "+09:30"},
      {"getHours", "America/Los_Angeles"},
      {"getMinutes", "UTC"},
      {"getSeconds", "Europe/Paris"},
      {"getMilliseconds", "Pacific/Auckland"},
  };
  for (const TzAcc& a : tzs) {
    b.Unary(i, std::string("ts_") + a.method + "_tz",
            std::string("(%0).") + a.method + "(\"" + a.tz + "\")", ts);
  }
}

}  // namespace

void RegisterTemporal(GrammarBuilder& b) {
  const CelType ts = CelType::Timestamp();
  const CelType dur = CelType::Duration();
  const CelType i = CelType::Int();
  // Leaves — a spread of valid instants / durations.
  b.Leaf(ts, "ts_epoch", R"(timestamp("1970-01-01T00:00:00Z"))");
  b.Leaf(ts, "ts_mid", R"(timestamp("2024-03-15T13:45:30Z"))");
  b.Leaf(ts, "ts_leap", R"(timestamp("2000-02-29T23:59:59Z"))");
  b.Leaf(dur, "dur_zero", R"(duration("0s"))");
  b.Leaf(dur, "dur_hour", R"(duration("3661s"))");
  b.Leaf(dur, "dur_neg", R"(duration("-90s"))");
  RegisterTimestampAccessors(b);
  // Duration accessors → Int.
  for (const char* m :
       {"getHours", "getMinutes", "getSeconds", "getMilliseconds"}) {
    b.Unary(i, std::string("dur_") + m, std::string("(%0).") + m + "()", dur);
  }
  // Comparisons (eq/ne/lt/le/gt/ge) → Bool, same-type.
  for (const CelType& t : {ts, dur}) {
    const std::string tag = TypeKey(t);
    b.Binary(CelType::Bool(), tag + "_eq", "(%0 == %1)", t, t);
    b.Binary(CelType::Bool(), tag + "_ne", "(%0 != %1)", t, t);
    b.Binary(CelType::Bool(), tag + "_lt", "(%0 < %1)", t, t);
    b.Binary(CelType::Bool(), tag + "_le", "(%0 <= %1)", t, t);
    b.Binary(CelType::Bool(), tag + "_gt", "(%0 > %1)", t, t);
    b.Binary(CelType::Bool(), tag + "_ge", "(%0 >= %1)", t, t);
  }
  // Conversions.  `int(timestamp)` is standard (unix seconds);
  // `int(duration)` is NOT a cel-cpp overload (the fuzzer found we
  // wrongly accept it — pinned as PbtIntOfDurationOverPermissive in
  // known_bugs), so it is deliberately absent here to keep the
  // grammar emitting only conformant CEL.
  b.Unary(i, "int_from_timestamp", "int(%0)", ts);
  b.Unary(CelType::String(), "string_from_timestamp", "string(%0)", ts);
  b.Unary(CelType::String(), "string_from_duration", "string(%0)", dur);
}

}  // namespace celwasm::fuzz
