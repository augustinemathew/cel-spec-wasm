// Heavy headers shared by ~every project C++ TU.  Built into a PCH
// by scripts/build_lint_pch.sh and consumed by scripts/lint.sh via
// `-include-pch=<path>` on every clang-tidy invocation.  The list is
// the union of every absl/* and google/protobuf/* header referenced
// by any source under the project tree (regenerate with
// `grep -rh '#include "absl/\|#include "google/protobuf/' compiler eval common abi runtime`).
//
// Bazel reaches absl via `-iquote external/abseil-cpp~`, so quoted
// form is required — `<absl/...>` won't resolve.  Protobuf is reached
// the same way in this codebase by convention.
//
// gtest is intentionally absent: test TUs carry an extra `-I` into
// the gtest virtual include dir that non-test TUs don't, and clang
// rejects a PCH built with `-I` paths missing from the consumer.

#ifndef CELWASM_SCRIPTS_LINT_PCH_H_
#define CELWASM_SCRIPTS_LINT_PCH_H_

// absl
#include "absl/base/attributes.h"
#include "absl/base/nullability.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/functional/any_invocable.h"
#include "absl/functional/function_ref.h"
#include "absl/log/absl_check.h"
#include "absl/log/absl_log.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
// absl/status/status_matchers.h is intentionally absent — it
// transitively includes gmock/gmock.h, whose `-I` is only in test
// compile_commands entries; including it here would prevent the
// PCH from validating against non-test TUs.
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "absl/types/variant.h"

// google/protobuf
#include "google/protobuf/arena.h"
#include "google/protobuf/compiler/importer.h"
#include "google/protobuf/compiler/parser.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/descriptor_database.h"
#include "google/protobuf/io/tokenizer.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "google/protobuf/message.h"
#include "google/protobuf/text_format.h"

#endif  // CELWASM_SCRIPTS_LINT_PCH_H_
