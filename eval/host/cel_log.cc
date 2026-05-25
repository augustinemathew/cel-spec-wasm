#include "eval/host/cel_log.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/nullability.h"
#include "absl/status/status.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "runtime/cel_runtime.h"
#include "wasm.h"
#include "wasmtime.h"

namespace celwasm {
namespace {

// Byte layout of one `argv` slot.  Two 8-byte words: the first's low
// 32 bits carry the tag (high 32 reserved), the second is the
// per-tag payload.  Keep in sync with the `CEL_LOG_*` call-site
// macros in cel_runtime.h.
constexpr uint32_t kArgvSlotBytes = 16;

// Process-wide sink.  Defaults to the stderr sink.
CelLogSink* g_sink = nullptr;

// Stderr sink — one static instance, returned by DefaultCelLogSink.
class StderrCelLogSink : public CelLogSink {
 public:
  void Emit(absl::string_view file, absl::string_view fn, uint32_t line_no,
            absl::string_view message) override {
    std::fprintf(stderr, "[%.*s:%u %.*s] %.*s\n", static_cast<int>(file.size()),
                 file.data(), line_no, static_cast<int>(fn.size()), fn.data(),
                 static_cast<int>(message.size()), message.data());
  }
};

StderrCelLogSink* StderrSingleton() {
  static StderrCelLogSink kInstance;
  return &kInstance;
}

// Returns a byte span into `mem` for `[ptr, ptr+len)`.  Empty on OOB.
absl::string_view SafeSpan(absl::Span<const uint8_t> mem, uint32_t ptr,
                           uint32_t len) {
  if (static_cast<uint64_t>(ptr) + len > mem.size()) return {};
  return {reinterpret_cast<const char*>(mem.data()) + ptr, len};
}

// Same as SafeSpan but returns a raw byte span (for CelValue reads).
absl::Span<const uint8_t> SafeBytes(absl::Span<const uint8_t> mem, uint32_t ptr,
                                    uint32_t len) {
  if (static_cast<uint64_t>(ptr) + len > mem.size()) return {};
  return mem.subspan(ptr, len);
}

// Reads one argv slot at offset `slot_off` into `mem`.  On OOB returns
// a zeroed slot and sets *ok=false.
struct ArgvSlot {
  uint32_t tag = 0;
  uint64_t payload = 0;
};
ArgvSlot ReadSlot(absl::Span<const uint8_t> mem, uint32_t slot_off, bool* ok) {
  ArgvSlot s;
  if (static_cast<uint64_t>(slot_off) + kArgvSlotBytes > mem.size()) {
    *ok = false;
    return s;
  }
  *ok = true;
  std::memcpy(&s.tag, mem.data() + slot_off, sizeof(uint32_t));
  std::memcpy(&s.payload, mem.data() + slot_off + 8, sizeof(uint64_t));
  return s;
}

// ---- Per-directive formatters --------------------------------------------

void FormatStr(absl::Span<const uint8_t> mem, uint64_t payload,
               std::string* out) {
  const auto ptr = static_cast<uint32_t>(payload & 0xFFFFFFFFull);
  const auto len = static_cast<uint32_t>(payload >> 32);
  absl::string_view s = SafeSpan(mem, ptr, len);
  if (s.empty() && (ptr != 0 || len != 0) &&
      static_cast<uint64_t>(ptr) + len > mem.size()) {
    absl::StrAppend(out, "<oob>");
    return;
  }
  absl::StrAppend(out, s);
}

void FormatInt(uint64_t payload, std::string* out) {
  absl::StrAppend(out, static_cast<int64_t>(payload));
}

void FormatUint(uint64_t payload, std::string* out) {
  absl::StrAppend(out, payload);
}

void FormatDouble(uint64_t payload, std::string* out) {
  double d = 0.0;
  std::memcpy(&d, &payload, sizeof(double));
  absl::StrAppendFormat(out, "%g", d);
}

void FormatBool(uint64_t payload, std::string* out) {
  absl::StrAppend(out, payload ? "true" : "false");
}

// ---- %v (CelValue) --------------------------------------------------------

// Reads the 24-byte CelValue at `off`.  Returns false on OOB.
bool ReadCelValue(absl::Span<const uint8_t> mem, uint32_t off, CelValue* out) {
  if (static_cast<uint64_t>(off) + sizeof(CelValue) > mem.size()) return false;
  std::memcpy(out, mem.data() + off, sizeof(CelValue));
  return true;
}

void FormatSpanPayload(absl::Span<const uint8_t> mem, const CelSpan& s,
                       absl::string_view kind, std::string* out) {
  absl::Span<const uint8_t> bytes = SafeBytes(mem, s.ptr, s.len);
  if (kind == "string" || kind == "type") {
    // Type-name strings are pure ASCII (per the spec type-name
    // set in `rewrite/m9-type-subsystem.md` §3.1) — render verbatim like a
    // string but with the `kind` label.
    absl::string_view sv(reinterpret_cast<const char*>(bytes.data()),
                         bytes.size());
    absl::StrAppendFormat(out, "%s(%s)", kind, absl::CEscape(sv));
    return;
  }
  absl::StrAppendFormat(out, "bytes(%u bytes)", s.len);
}

// Formats a CEL_UNKNOWN's AttributeSet.  Payload points at an
// `UnknownSet` descriptor in memory: `(ids_off, len)` — two u32s.
// Unreadable / missing set renders as `unknown([])`.
void FormatUnknown(absl::Span<const uint8_t> mem, uint32_t set_off,
                   std::string* out) {
  if (set_off == 0 ||
      static_cast<uint64_t>(set_off) + (2 * sizeof(uint32_t)) > mem.size()) {
    absl::StrAppend(out, "unknown([])");
    return;
  }
  uint32_t ids_off = 0;
  uint32_t len = 0;
  std::memcpy(&ids_off, mem.data() + set_off, sizeof(uint32_t));
  std::memcpy(&len, mem.data() + set_off + 4, sizeof(uint32_t));
  absl::Span<const uint8_t> ids_bytes =
      SafeBytes(mem, ids_off, len * sizeof(uint32_t));
  absl::StrAppend(out, "unknown([");
  for (uint32_t i = 0;
       i < len && ids_bytes.size() >= (static_cast<size_t>(i) + 1) * 4; ++i) {
    uint32_t id = 0;
    std::memcpy(&id, ids_bytes.data() + (static_cast<size_t>(i) * 4),
                sizeof(uint32_t));
    if (i > 0) {
      absl::StrAppend(out, ",");
    }
    absl::StrAppend(out, id);
  }
  absl::StrAppend(out, "])");
}

// Formats a CEL_ERROR descriptor.  Payload points at a 16-byte struct
// `(code, msg_ptr, msg_len, _pad)`.
void FormatError(absl::Span<const uint8_t> mem, uint32_t err_off,
                 std::string* out) {
  if (err_off == 0 || static_cast<uint64_t>(err_off) + 12 > mem.size()) {
    absl::StrAppend(out, "error(code=0)");
    return;
  }
  uint32_t code = 0;
  uint32_t msg_ptr = 0;
  uint32_t msg_len = 0;
  std::memcpy(&code, mem.data() + err_off, sizeof(uint32_t));
  std::memcpy(&msg_ptr, mem.data() + err_off + 4, sizeof(uint32_t));
  std::memcpy(&msg_len, mem.data() + err_off + 8, sizeof(uint32_t));
  absl::string_view msg = SafeSpan(mem, msg_ptr, msg_len);
  absl::StrAppendFormat(out, "error(code=%u,\"%s\")", code, absl::CEscape(msg));
}

// Dispatch on kind for `%v`.  Each branch is trivial; factored for
// the function-size gate.
void FormatValueKind(absl::Span<const uint8_t> mem, const CelValue& cv,
                     std::string* out) {
  switch (cv.kind) {
    case CEL_NULL:
      absl::StrAppend(out, "null");
      return;
    case CEL_BOOL:
      absl::StrAppendFormat(out, "bool(%s)", cv.payload.b ? "true" : "false");
      return;
    case CEL_INT:
      absl::StrAppendFormat(out, "int(%d)", cv.payload.i);
      return;
    case CEL_UINT:
      absl::StrAppendFormat(out, "uint(%u)", cv.payload.u);
      return;
    case CEL_DOUBLE:
      absl::StrAppendFormat(out, "double(%g)", cv.payload.d);
      return;
    case CEL_STRING:
      FormatSpanPayload(mem, cv.payload.s, "string", out);
      return;
    case CEL_BYTES:
      FormatSpanPayload(mem, cv.payload.s, "bytes", out);
      return;
    case CEL_MESSAGE:
      absl::StrAppendFormat(out, "message(slot=%u)", cv.payload.msg_slot);
      return;
    case CEL_TYPE:
      // M9: payload.s carries (ptr, len) of the type-name string in
      // linear memory.  Pretty-print as `type(<name>)`.
      FormatSpanPayload(mem, cv.payload.s, "type", out);
      return;
    case CEL_DURATION:
      absl::StrAppendFormat(out, "duration(s=%d,ns=%d)", cv.payload.dur.seconds,
                            cv.payload.dur.nanos);
      return;
    case CEL_TIMESTAMP:
      absl::StrAppendFormat(out, "timestamp(s=%d,ns=%d)", cv.payload.ts.seconds,
                            cv.payload.ts.nanos);
      return;
    case CEL_OPTIONAL:
      absl::StrAppendFormat(out, "optional(inner=%u)", cv.payload.opt);
      return;
    case CEL_UNKNOWN:
      FormatUnknown(mem, cv.payload.unk, out);
      return;
    case CEL_ERROR:
      FormatError(mem, cv.payload.err, out);
      return;
    default:
      absl::StrAppendFormat(out, "<bad-kind=%u>", cv.kind);
  }
}

void FormatValue(absl::Span<const uint8_t> mem, uint64_t payload,
                 std::string* out) {
  const auto off = static_cast<uint32_t>(payload & 0xFFFFFFFFull);
  if (off == 0) {
    absl::StrAppend(out, "<null-offset>");
    return;
  }
  CelValue cv;
  if (!ReadCelValue(mem, off, &cv)) {
    absl::StrAppend(out, "<oob>");
    return;
  }
  FormatValueKind(mem, cv, out);
}

// ---- Directive dispatch ---------------------------------------------------

// Applies one directive (c, payload) to the output buffer.  Tag
// mismatches (e.g. `%d` when the call site sent a string slot) still
// try to render — CEL_LOG is diagnostic; silently dropping a bad
// arg would hide bugs more than it would hide noise.
void ApplyDirective(char c, absl::Span<const uint8_t> mem, const ArgvSlot& slot,
                    std::string* out) {
  switch (c) {
    case 's':
      FormatStr(mem, slot.payload, out);
      return;
    case 'd':
      FormatInt(slot.payload, out);
      return;
    case 'u':
      FormatUint(slot.payload, out);
      return;
    case 'f':
      FormatDouble(slot.payload, out);
      return;
    case 'b':
      FormatBool(slot.payload, out);
      return;
    case 'v':
      FormatValue(mem, slot.payload, out);
      return;
    default:
      // Unknown directive: emit the two bytes verbatim.  Keeps
      // `%z` etc. visible rather than silently dropped.
      absl::StrAppend(out, "%", absl::string_view(&c, 1));
  }
}

// Walks the format string, pulling one argv slot per non-%% directive.
// Returns the fully formatted message body (no prefix, no newline).
std::string FormatMessage(absl::Span<const uint8_t> mem, absl::string_view fmt,
                          uint32_t argv_ptr, uint32_t argc) {
  std::string out;
  out.reserve(fmt.size() + (size_t{16} * argc));
  uint32_t arg_idx = 0;
  for (size_t i = 0; i < fmt.size(); ++i) {
    const char c = fmt[i];
    if (c != '%') {
      out.push_back(c);
      continue;
    }
    if (i + 1 >= fmt.size()) {
      out.push_back('%');  // trailing `%` — keep literal
      break;
    }
    const char next = fmt[++i];
    if (next == '%') {
      out.push_back('%');
      continue;
    }
    if (arg_idx >= argc) {
      absl::StrAppend(&out, "%", absl::string_view(&next, 1));
      continue;
    }
    bool ok = false;
    ArgvSlot slot = ReadSlot(mem, argv_ptr + (arg_idx * kArgvSlotBytes), &ok);
    if (!ok) {
      absl::StrAppend(&out, "<oob-arg>");
      ++arg_idx;
      continue;
    }
    ApplyDirective(next, mem, slot, &out);
    ++arg_idx;
  }
  return out;
}

}  // namespace

CelLogSink* DefaultCelLogSink() {
  return StderrSingleton();
}

CelLogSink* SetCelLogSink(CelLogSink* sink) {
  CelLogSink* prev = g_sink;
  g_sink = sink;
  return prev;
}

namespace {
CelLogSink* ActiveSink() {
  return g_sink != nullptr ? g_sink : DefaultCelLogSink();
}
}  // namespace

void CapturingCelLogSink::Emit(absl::string_view file, absl::string_view fn,
                               uint32_t line_no, absl::string_view message) {
  lines_.push_back(
      absl::StrCat("[", file, ":", line_no, " ", fn, "] ", message));
}

void DecodeCelLog(absl::Span<const uint8_t> linear_memory,
                  const CelLogWireArgs& args, CelLogSink* sink) {
  absl::string_view file =
      SafeSpan(linear_memory, args.file_ptr, args.file_len);
  absl::string_view fn = SafeSpan(linear_memory, args.fn_ptr, args.fn_len);
  absl::string_view fmt = SafeSpan(linear_memory, args.fmt_ptr, args.fmt_len);
  std::string body =
      FormatMessage(linear_memory, fmt, args.argv_ptr, args.argc);
  sink->Emit(file, fn, args.line, body);
}

namespace {

// Reads the 9 i32 args off the wasmtime args array into a typed struct.
// Args shorter than 9 => fill remainder with zero.  The trampoline
// never traps; missing args become zeroed offsets and DecodeCelLog
// handles the resulting `<oob>` gracefully.
CelLogWireArgs ArgsFromWasmtime(const wasmtime_val_t* args, size_t nargs) {
  CelLogWireArgs out;
  auto at = [&](size_t i) -> uint32_t {
    if (i >= nargs) return 0;
    return static_cast<uint32_t>(args[i].of.i32);
  };
  out.file_ptr = at(0);
  out.file_len = at(1);
  out.fn_ptr = at(2);
  out.fn_len = at(3);
  out.line = at(4);
  out.fmt_ptr = at(5);
  out.fmt_len = at(6);
  out.argv_ptr = at(7);
  out.argc = at(8);
  return out;
}

// Pulls the caller's exported memory into a byte span for decoding.
// Returns an empty span when the caller has no memory export (the
// trampoline then no-ops) — logging is diagnostic, not load-bearing.
absl::Span<const uint8_t> CallerMemory(wasmtime_caller_t* caller) {
  wasmtime_context_t* ctx = wasmtime_caller_context(caller);
  wasmtime_extern_t ext;
  const char kName[] = "memory";
  if (!wasmtime_caller_export_get(caller, kName, sizeof(kName) - 1, &ext)) {
    return {};
  }
  if (ext.kind != WASMTIME_EXTERN_MEMORY) {
    wasmtime_extern_delete(&ext);
    return {};
  }
  const uint8_t* data = wasmtime_memory_data(ctx, &ext.of.memory);
  const size_t size = wasmtime_memory_data_size(ctx, &ext.of.memory);
  return {data, size};
}

wasm_trap_t* CelLogTrampoline(void* /*data*/, wasmtime_caller_t* caller,
                              const wasmtime_val_t* args, size_t nargs,
                              wasmtime_val_t* /*results*/,
                              size_t /*nresults*/) {
  absl::Span<const uint8_t> mem = CallerMemory(caller);
  if (mem.empty()) return nullptr;
  const CelLogWireArgs wire = ArgsFromWasmtime(args, nargs);
  DecodeCelLog(mem, wire, ActiveSink());
  return nullptr;
}

wasm_functype_t* CelLogType() {
  wasm_valtype_vec_t params;
  wasm_valtype_vec_t results;
  wasm_valtype_t* param_arr[9];
  for (auto& p : param_arr) {
    p = wasm_valtype_new(WASM_I32);
  }
  wasm_valtype_vec_new(&params, 9, param_arr);
  wasm_valtype_vec_new_empty(&results);
  return wasm_functype_new(&params, &results);
}

}  // namespace

absl::Status RegisterCelLog(wasmtime_linker_t* linker) {
  wasm_functype_t* type = CelLogType();
  const char kModule[] = "cel_env";
  const char kName[] = "cel_log";
  wasmtime_error_t* err = wasmtime_linker_define_func(
      linker, kModule, sizeof(kModule) - 1, kName, sizeof(kName) - 1, type,
      CelLogTrampoline, /*data=*/nullptr, /*finalizer=*/nullptr);
  wasm_functype_delete(type);
  if (err != nullptr) {
    wasm_byte_vec_t msg;
    wasmtime_error_message(err, &msg);
    std::string text(msg.data, msg.size);
    wasm_byte_vec_delete(&msg);
    wasmtime_error_delete(err);
    return absl::InternalError(
        absl::StrCat("wasmtime_linker_define_func(cel_env.cel_log): ", text));
  }
  return absl::OkStatus();
}

}  // namespace celwasm
