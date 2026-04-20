# CelHost — Embedding Surface Design

Status: **draft v1** (2026-04-19). Complements — does not replace —
`doc/wasm-compiler-design.md`, which remains the normative reference
for the host ABI and module shape.

This doc specifies **CelHost**: the embedding SDK that wraps a
compiled CEL → WASM module so an application can register custom
functions, bind host values to variables, and run an evaluation. It
answers "what does an embedder see" at the level of detail a C++ or
Go engineer can implement from.

The host ABI (`cel_host.*`, `cel_fn.*`, imports/exports, `cel.abi`
custom section) is defined in `doc/wasm-compiler-design.md` §8. This
document does not redefine those — it describes the SDK that sits
**above** them and realises them for application code.

### Organisation

- §1 Purpose & non-goals — one paragraph, scoping the SDK.
- §2 Shape of the embedding — lifecycle and where things live.
- §3 Canonical C++ API — `class CelHost` header sketch.
- §4 Canonical Go API — idiomatic `celhost` package.
- §5 Custom function ABI — the load-bearing section: how
  `cel_fn.*` reaches a user-registered callable.
- §6 Variable binding ABI — how `SetVariable("request.user",
  userMsg)` is seen by the module.
- §7 End-to-end examples — one C++, one Go, both running
  `greet(request.user.name)`.
- §8 Error / unknown propagation — pointer to §9 of the main
  design doc, plus the language mapping.
- §9 Implementation milestones — slice list, referenced against
  the existing M6 plan.
- Open questions — surfaced for the user to decide before the
  first slice lands.

---

## 1. Purpose & non-goals

**CelHost** is the embedding surface around the compiled WASM
module. It owns a `runtime.wasm` instance, owns one or more compiled
`expression.wasm` modules, provides the host side of the
`cel_host.*` imports defined in main §8.2, implements the `cel_fn.*`
trampolines so application code can register custom functions, lets
the application bind host-provided variables, and calls `eval`. It
is **not** a compiler (that's `celwasmc`, main §4) and it is **not**
an evaluator (the WASM module evaluates — CelHost only wires inputs
and extracts outputs). It is the thinnest possible layer that lets
"I have a `.wasm` from `celwasmc` and a proto message" become "I
have a `CelValue`."

## 2. Shape of the embedding

Lifecycle, in order:

1. **Load compiled bytes.** The application reads
   `expression.wasm` (the per-expression module) from disk, a
   blob, or a build-time embed. The shared `runtime.wasm` bytes
   live inside the CelHost library — embedders don't ship it
   separately. Main §7.0 is the reference for why the split
   exists.
2. **Create a CelHost instance.** Construction compiles the
   runtime once (cached across instances), compiles the
   expression module, and sets up the wasmtime (or equivalent)
   engine + store + linker. The `cel.abi` custom section is
   parsed eagerly so type/attribute/pattern IDs are resolved
   before any `Eval`.
3. **Register custom functions.** Any `cel_fn.<overload_id>`
   import the module declares must be satisfied before the
   module is instantiated against the linker. Registration
   happens against the `CelHost`, which keeps a
   `overload_id → callable` registry and exposes one trampoline
   per overload.
4. **Bind variables.** `SetVariable` stashes the host value in a
   per-instance map keyed by the declared variable name. Values
   are not copied into the arena at registration time — they
   live on the host, and the eval path reaches them either as
   `externref` (for messages) or by reading host memory at
   eval-start (for scalars and strings). See §6.
5. **Call Eval.** `Eval()` calls `cel_reset` on the runtime
   (rewinds the arena), packs bound variables into the eval
   signature's parameter list, invokes the exported `eval`, and
   translates the returned `CelValue*` (arena-relative offset)
   into a host-language value. Custom function callbacks fire
   re-entrantly during this call.
6. **Repeat Eval.** A single CelHost can be evaluated many
   times with different variable bindings. `Eval` is the only
   point that touches the arena; registrations are
   instantiation-time state and do not change between calls.

What lives where:

- **WASM module.** Code that computes the expression.
- **Wasmtime engine + store.** Owned by CelHost. One store per
  CelHost; a CelHost is single-threaded (main §7.0.1).
- **Runtime instance.** One per CelHost. Exports `memory`,
  `cel_alloc`, `cel_make_*`, `cel_reset`, `cel_mem_base`.
- **Eval instance.** One per CelHost. Exports `eval` and the
  per-module `$cel_refs` externref-table helpers
  (`cel_ref_intern`, `cel_unwrap_message`).
- **CelHost-owned registries (host-side C++/Go data).**
  - `fn_table_` — `overload_id → FunctionCallable`. Consulted
    from the `cel_fn.*` trampolines via a closure capture or
    wasmtime callback-data pointer.
  - `var_table_` — `name → BoundValue`. Consulted from
    `Eval` when packing parameters.
  - `abi_` — a parsed `CelAbi` proto, providing type IDs,
    attribute IDs, pattern IDs.

## 3. Canonical C++ API

The C++ SDK lives under `compiler/host/`, extending the existing
`cel_host_wasmtime.{h,cc}` trampoline layer with a higher-level
class. Placeholder header sketch — types are approximate, the
intent is the shape.

```cpp
// compiler/host/cel_host.h
#ifndef CELWASM_COMPILER_HOST_CEL_HOST_SDK_H_
#define CELWASM_COMPILER_HOST_CEL_HOST_SDK_H_

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "compiler/host/cel_value_view.h"  // new; see §8
#include "google/protobuf/message.h"

namespace celwasm {

// A host-language value that can be (a) bound to a CEL variable or
// (b) passed in/out of a custom function.  Discriminated sum — the
// Maker methods (Bool/Int/…/Message/Unknown/Error) construct and
// assert the kind.
class HostValue { /* tagged union over bool/int/uint/double/string/
                     bytes/Message*/unknown/error */ };

// Signature for user-registered function bodies.  Args arrive already
// decoded to HostValue; return is wrapped on the way back into the
// arena.  Unknown/Error propagation: a callable may return
// HostValue::Unknown(attribute_id) or HostValue::Error(code, msg),
// which the SDK packs into a CelValue of the appropriate kind.
using FunctionCallable =
    std::function<absl::StatusOr<HostValue>(absl::Span<const HostValue>)>;

// Declares one overload signature.  Must agree with what the
// compiler expects — the `cel.abi.function_set` in the compiled
// module is checked against this at construction.  Mismatch → hard
// error at Eval-instantiation time so a type-skew never shows up as
// a silent miscompute.
struct OverloadSpec {
  std::string   overload_id;     // the `cel_fn.<id>` import name
  std::vector<CelType> params;   // each element is a static CEL type
  CelType       result;
  bool          is_member_call = false;  // cf. langdef §receiver
};

class CelHost {
 public:
  // Loads a compiled expression module.  Parses the `cel.abi`
  // custom section, compiles runtime.wasm (once per process) and
  // expr.wasm, but does NOT instantiate the eval module yet —
  // instantiation waits until after all custom functions are
  // registered, because wasmtime resolves imports at instantiation
  // time.
  //
  // Rationale: a staged "Load → Register → Build" would force the
  // engineer to hold a builder object; a single-stage constructor
  // that defers instantiation until the first Eval keeps the
  // happy path to three lines of code (ctor, RegisterFunction,
  // Eval).  The first Eval validates that every declared
  // `cel_fn.*` import has a matching registration, else returns
  // `FailedPreconditionError`.
  static absl::StatusOr<std::unique_ptr<CelHost>> Load(
      absl::Span<const uint8_t> expression_wasm);

  // Registers one or more overloads for the CEL-visible function
  // `name`.  The variadic form handles overload sets: a single
  // user call site `size(x)` can dispatch to `size_string`,
  // `size_bytes`, or `size_list` as the checker resolved.  Only
  // overloads the expression module declared as `cel_fn.*`
  // imports need to be registered; extras are silently ignored
  // (useful when one registration bundle serves many expressions).
  //
  // The wrapper from FunctionCallable → cel_fn.<id> trampoline is
  // built at this call — scalar args decode from CelValue
  // offsets, messages decode from externref.  See §5.
  template <typename... Overloads>
  absl::Status RegisterFunction(std::string_view name,
                                Overloads... overloads);
  // Convenience overload: single-callable, single-overload.
  absl::Status RegisterFunction(std::string_view name,
                                const OverloadSpec& spec,
                                FunctionCallable callable);

  // Binds a host value to the variable named `name`.  The name must
  // match a variable declared at compile time (in cel.abi's attribute
  // table) and the static type must agree.  Messages are stored as
  // `const google::protobuf::Message*` — CelHost does NOT take
  // ownership; the caller must keep the message alive across Eval.
  //
  // Why per-variable SetVariable instead of a bag-of-values
  // `Eval(Bindings)`: common case is one compiled expression run
  // many times with one or two inputs changing.  SetVariable +
  // Eval lets the stable bindings stay parked between calls; a
  // single-shot Eval(bindings) would force re-validating the
  // whole bag on every call.
  absl::Status SetVariable(std::string_view name, HostValue value);

  // Runs the expression.  Sequences:
  //   1. `cel_reset` on the runtime (rewind arena, ref-table).
  //   2. For each eval parameter, materialise the bound variable
  //      as its ABI-shaped wasm value (i64 / f64 / externref / i32
  //      CelValue offset — see main §8.1).
  //   3. `wasmtime_func_call(eval, args, &result)`.
  //   4. Decode result per the root `Repr` from cel.abi.
  //
  // Failure modes surfaced as StatusOr: missing binding, type
  // mismatch in a binding, trap during eval, trap inside a
  // user function, validation failure parsing cel.abi.
  //
  // The returned CelValueView is a non-owning reference into the
  // runtime arena.  Valid until the NEXT `Eval()` call (which
  // invokes cel_reset); an embedder that wants to keep the value
  // across evals must `Copy` it into a CelValueOwned.
  absl::StatusOr<CelValueView> Eval();

  // Escape hatch: the wasmtime store, exposed for tests that need
  // to poke linear memory by hand.  Production embedders should
  // not reach for this.
  wasmtime_context_t* context() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace celwasm
#endif
```

Surface notes:

- **Why `Load` returns a `unique_ptr`, not a value type.** The
  wasmtime linker holds raw pointers back into CelHost-owned data
  (trampoline callback-data, the bound-variable table); move
  semantics would invalidate those. `unique_ptr` is the minimal
  shape that preserves pinning.
- **Why `RegisterFunction` takes overloads variadically.** cel-cpp
  routinely emits multiple overloads per name (`size(string)`,
  `size(bytes)`, `size(list)`). A variadic signature lets the
  embedder write one `host.RegisterFunction("size", s1, s2, s3)`;
  without variadics the code pattern is repeated
  `RegisterFunction` calls that must share the same `name` string.
- **Why `HostValue` is a tagged union, not `std::variant`.** The
  kinds include `const google::protobuf::Message*` (non-owning),
  `std::string_view` (non-owning), `absl::Span<const uint8_t>`
  (non-owning), and two boxing kinds (Unknown, Error). A
  hand-rolled tagged union matches the on-wire `CelValue` one-to-one
  and gives the SDK a clean place to hang `View` / `Copy`
  conversions. Using `std::variant` would force every getter to go
  through `std::get_if` and lose the 1-to-1 `CelKind` alignment.

## 4. Canonical Go API

```go
// celhost/celhost.go
package celhost

import (
    "errors"
    "fmt"
    "google.golang.org/protobuf/proto"
)

// Value mirrors CelValue over Go types.  Messages are proto.Message;
// strings are native Go strings; unknown/error propagate via
// Value.IsUnknown / Value.IsError.
type Value struct { /* kind + payload */ }

// OverloadSpec describes one overload; mirrors C++ OverloadSpec.
type OverloadSpec struct {
    OverloadID    string
    Params        []Type
    Result        Type
    IsMemberCall  bool
}

// FuncHandler is the registrable shape.  Generic overloads are
// expressed via type-specific helpers (RegisterFunc1, …) below —
// they keep the hot path reflection-free when the arg count is
// known.  Reflective registration is available as a fallback.
type FuncHandler func(args []Value) (Value, error)

// Host is the Go-side equivalent of C++ CelHost.  One Host holds
// one runtime + eval instance pair; not goroutine-safe (see
// design §7.0.1).
type Host struct {
    // unexported: engine, store, runtime instance, eval instance,
    //             linker, callable registry, var table, cel.abi
}

// New loads a compiled expression module and prepares a Host.  As in
// C++, instantiation of the eval module is deferred until first
// Eval so registrations can be applied.
//
// Why []byte, not io.Reader: compiled .wasm is fetched whole once
// and held in memory for the life of the Host; a streaming API
// would force a Finalize step we'd rather skip.
func New(expressionWasm []byte) (*Host, error) { /* … */ }

// RegisterFunc registers `handler` for every overload in `specs`.
// Variadic for the same reason as the C++ API: a single CEL-visible
// name `size` can have multiple overload IDs.
//
// Signature of handler is the uniform []Value shape.  Helpers
// below (RegisterFunc1[A,R], RegisterFunc2[A,B,R]) wrap a
// type-safe Go function via generics for common cases:
//
//     h.RegisterFunc1("greet", greetSpec,
//         func(s string) (string, error) { return "hi " + s, nil })
//
// The reflection-free path is preferred because CEL custom
// functions are hot-path — reflection on every call in a rule
// engine hurts.  The []Value fallback exists for the small
// fraction of functions whose arity depends on data.
func (h *Host) RegisterFunc(name string, specs []OverloadSpec,
    handler FuncHandler) error

// Typed registration helpers (generics, Go 1.21+):
func RegisterFunc1[A, R any](h *Host, name string, spec OverloadSpec,
    fn func(A) (R, error)) error

func RegisterFunc2[A, B, R any](h *Host, name string, spec OverloadSpec,
    fn func(A, B) (R, error)) error

// SetVariable binds name to v.  Messages are passed as
// proto.Message; Host holds a reference for the duration of Eval.
// Go has no const/ownership story, so the contract is "don't mutate
// the message while an Eval is running"; this matches the
// single-threaded Host contract anyway.
func (h *Host) SetVariable(name string, v Value) error

// Convenience variadic (for the common "four primitives" case):
func (h *Host) SetInt(name string, v int64) error
func (h *Host) SetString(name, v string) error
func (h *Host) SetMessage(name string, v proto.Message) error
// …

// Eval runs the expression and returns the decoded result.  The
// returned Value is owned by the caller (strings copied out of
// wasm memory, messages held as proto.Message) — no lifetime
// coupling to the next Eval, so idiomatic Go.
//
// The extra cost of copying on the way out vs. the C++ View model
// is intentional: Go's escape-analysis + GC make a borrowed-view
// API unusable in practice (the caller has no way to opt out of
// GC visibility of the wasm arena).
func (h *Host) Eval() (Value, error)

// Close releases the wasmtime store and engine.  After Close,
// Eval / SetVariable / RegisterFunc all return ErrClosed.
func (h *Host) Close() error
```

Surface notes:

- **Dual registration path (typed generics + untyped `[]Value`).**
  Go generics (1.21+) let us type-check the happy path without
  reflection on every call. The `FuncHandler` fallback stays for
  variadic-arg or runtime-typed functions.
- **Go returns copied `Value`s, C++ returns a `View`.** C++
  callers control lifetime, so handing back a view into the wasm
  arena is fine — it expires on the next `Eval`. Go's GC has no
  way to keep wasm memory alive, and attempting to hand back a
  view would mean either pinning the store across Go's GC passes
  or surprising the caller with invalidation. Copying costs one
  string allocation per string result; acceptable.

## 5. Custom function ABI

The load-bearing section. When the WASM module calls a
`cel_fn.<id>` import, how does the call reach a C++ / Go function
the embedder registered?

Main §8.3 specifies the wasm-side shape: each custom function is
imported as

```wat
(import "cel_fn" "<overload_id>"
        (func (param i32 i32 …) (result i32)))
```

…where each param is a `CelValue*` (arena-relative offset) and the
return is a `CelValue*`. Message params retain their externref
identity — they travel as a `CEL_MESSAGE` CelValue whose `msg_slot`
indexes the `$cel_refs` table. The SDK implements this import.

### 5.1 Flow

```
  (inside eval)                            (CelHost, host process)
  ─────────────                            ────────────────────────
  call cel_fn.greet_string      ─────▶    trampoline(greet_string)
        arg0 = CelValue*                     │
                                             │  (1) decode CelValue*
                                             │      → HostValue
                                             │  (2) look up "greet_string"
                                             │      in fn_table_
                                             │  (3) invoke FunctionCallable
                                             │  (4) receive HostValue result
                                             │      (or Unknown/Error/status)
                                             │  (5) allocate in arena
                                             │      via cel_alloc + write
                                             │  (6) return CelValue* offset
                                             ▼
        result (CelValue*)       ◀─────  result offset
```

### 5.2 Argument marshalling

Main §8.2 already standardises on an **out-parameter CelValue**
(24-byte arena-allocated scratch) for the host ABI. Custom
functions do the symmetric thing but inverted: arguments arrive
as input `CelValue*` offsets. The trampoline:

1. Reads `cel_mem_base()` once (cached by the trampoline's init).
2. For each declared argument: reads 24 bytes from
   `abs = cel_mem_base + offset`, decodes `CelKind` + payload into
   a `HostValue`:
   - Scalars (`bool`, `int`, `uint`, `double`) copy-out the
     inline payload.
   - `string` / `bytes`: copy the span `[mem_base + ptr,
     mem_base + ptr + len)` into a host-owned buffer (or
     `string_view` if the callable is known-synchronous and the
     arena won't reset under it — which is the default; eval is
     single-threaded per §7.0.1 of main).
   - `message`: load `payload.msg_slot`, call
     `cel_unwrap_message(offset)` via the eval module's export
     to get the externref back, unwrap to the host `Message*`
     pointer the embedder originally handed in.
   - `unknown` / `error`: decode and wrap as
     `HostValue::Unknown(ids)` / `HostValue::Error(code, msg)`
     for the callable to observe.
3. Invokes the registered `FunctionCallable` with the decoded
   vector. No arena allocation happens on the host side for
   scalar args; the string/bytes copy only happens if the
   callable captures across `Eval`.

### 5.3 Return marshalling

The trampoline builds the return CelValue on the module side via
the already-exported constructors (main §8.1):

| `HostValue` kind  | Constructor                     |
| ----------------- | ------------------------------- |
| `bool`            | `cel_make_bool`                 |
| `int`             | `cel_make_int`                  |
| `uint`            | `cel_make_uint`                 |
| `double`          | `cel_make_double`               |
| `string`          | `cel_alloc`+memcpy+`cel_make_string_view` |
| `bytes`           | `cel_alloc`+memcpy+`cel_make_bytes_view` |
| `message`         | `cel_ref_intern` (eval mod) + `cel_make_message` |
| `unknown`         | `cel_make_unknown(attribute_id)` |
| `error`           | `cel_make_error(code, msg_ptr, msg_len)` |

This is exactly the shape the host-side `cel_host.get_field`
trampoline already uses (see `compiler/host/cel_host.cc`'s
`WriteSpanPayload`), so the implementation is a straight reuse of
that helper plus a thin "allocate + fill" wrapper.

### 5.4 Error / trap handling

Three failure modes, each with a normative outcome:

1. **Callable returns `HostValue::Error(code, msg)`.** Trampoline
   packs as `CEL_ERROR`; three-valued-logic propagation takes it
   from there.
2. **Callable returns `HostValue::Unknown(ids)`.** Trampoline
   packs as `CEL_UNKNOWN`; same as partial-eval inputs.
3. **Callable throws / panics / returns a non-CEL error status.**
   Trampoline catches (C++: `catch(...)`; Go: `recover`) and
   packs as `CEL_ERROR` with code `CEL_ERR_HOST_CALLBACK`
   (new, to be added to `cel_runtime.h` alongside the existing
   `CEL_ERR_OVERFLOW` family). The original exception message is
   copied into the error's `msg` span so the caller can observe
   it when decoding the top-level result. Callables that want to
   surface a trap-style "abort the whole eval, not a CEL error"
   can return a sentinel
   `FunctionCallable::abort_eval(reason)` — translated to a
   wasmtime trap, which propagates through `Eval` as
   `absl::AbortedError`. Aborting is rare; the common case is
   CEL-visible ERROR.

### 5.5 Registration timing

Wasmtime resolves imports at **instantiation time**, and the
module declares every `cel_fn.*` import unconditionally (main §8.2
"don't gate on AST inspection"). CelHost therefore instantiates
the eval module lazily on first `Eval`, after
`RegisterFunction` calls have populated `fn_table_`. Attempting
`Eval` with unregistered `cel_fn.*` imports fails with a
`FailedPreconditionError` listing the missing overload IDs.

## 6. Variable binding ABI

Variables declared at compile time appear two ways in the emitted
module:

- **Top-level identifiers that the expression reads as leaves.**
  Surfaced as parameters on the exported `eval` function (main
  §8.1 `func (export "eval") (param $arg_msg externref) …`).
  One param per declared variable, ordered by declaration
  (see `compiler/codegen/expr_lower.cc` param layout), typed by
  the variable's ABI (`Repr`): `i64` for int/uint, `f64` for
  double, `i32` for bool/string/bytes, `externref` for message.
- **Nested field paths the expression reaches via
  `cel_host.get_field`.** Those are read from the message
  externref handed in as a top-level param; the field-number
  immediates are baked in at compile time.

CelHost maps its per-variable `SetVariable` registry onto the
first bucket: at `Eval`, each declared eval parameter is looked up
by name in `var_table_`, converted to the ABI-shaped wasmtime
value, and pushed into the call.

**Scalars and strings are materialised at Eval time, not at
`SetVariable` time.** Rationale: the runtime arena is wiped by
`cel_reset` on every `Eval` call, so any string/bytes view
allocated earlier would dangle. Messages bypass this because
they're externrefs — the host already owns them — so
`SetVariable("user", msg)` really does nothing more than park a
`Message*` pointer.

**Strings flow via the existing `cel_alloc + cel_string_view`
dance** shown in main Appendix B:

```cpp
uint32_t rel = cel_alloc(len);
uint32_t abs_ = cel_mem_base() + rel;
memcpy(mem.data() + abs_, src, len);
uint32_t cv = cel_string_view(rel, len);
// push wasmtime_val_t{WASMTIME_I32, cv} as the eval param
```

No bespoke host API; this is literally what the C++ SDK's
`BindScalarToEvalParam` helper does internally.

**Field reads (`request.user.name`) do NOT see `SetVariable` data
except via the enclosing message.** The compiled module makes
`cel_host.get_field(externref_for_request, field#_user, out_cv)`
calls; the SDK satisfies those through the existing
`CelHostEnv` wasmtime glue (`compiler/host/cel_host_wasmtime.h`).
No new plumbing — the existing `ReadField` path in
`compiler/host/cel_host.cc` already handles every scalar +
message case. CelHost is the *embedder's* door to that pipe.

**Unknowns at variable granularity.** The embedder binds a
variable to `HostValue::Unknown(attribute_id)` when the input is
not yet resolved. The SDK packs it as `cel_make_unknown(id)` and
passes the resulting `CelValue*` as the i32 eval parameter.
Three-valued propagation (main §9) then carries UNKNOWN through
the expression.

## 7. Canonical end-to-end examples

Both examples run the expression `greet(request.user.name)`,
where `greet(string) -> string` is a user-registered custom
function and `request` is a host-provided message. Both presume
a `.wasm` compiled by:

```
celwasmc -e "greet(request.user.name)" \
    --schema-descriptorset=fixtures.pb \
    --var "request:demo.Request" \
    --functions functions.pb \
    --emit-wasm=expr.wasm
```

### 7.1 C++

```cpp
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "compiler/host/cel_host.h"
#include "fixtures/demo.pb.h"

int main() {
  std::ifstream in("expr.wasm", std::ios::binary);
  std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>()};

  auto host_or = celwasm::CelHost::Load(bytes);
  if (!host_or.ok()) { std::fprintf(stderr, "%s\n",
      std::string(host_or.status().message()).c_str()); return 1; }
  auto& host = *host_or.value();

  // Register greet(string) -> string.
  celwasm::OverloadSpec greet_spec{
      .overload_id = "greet_string",
      .params = {celwasm::CelType::String()},
      .result = celwasm::CelType::String(),
  };
  auto greet =
      [](absl::Span<const celwasm::HostValue> args)
          -> absl::StatusOr<celwasm::HostValue> {
    if (args.size() != 1 || !args[0].IsString()) {
      return celwasm::HostValue::Error(
          celwasm::CEL_ERR_NO_MATCHING_OVERLOAD, "greet expects 1 string");
    }
    return celwasm::HostValue::String("hello, " + std::string(args[0].AsStringView()));
  };
  auto s = host.RegisterFunction("greet", greet_spec, greet);
  if (!s.ok()) { /* log and exit */ return 1; }

  // Build the request message and bind it.
  demo::Request req;
  req.mutable_user()->set_name("Augustine");
  if (auto s = host.SetVariable("request", celwasm::HostValue::Message(&req));
      !s.ok()) { return 1; }

  // Evaluate.
  auto result = host.Eval();
  if (!result.ok()) { return 1; }
  if (result->IsString()) {
    std::printf("%s\n", std::string(result->AsStringView()).c_str());
  } else if (result->IsError()) {
    std::printf("ERROR: %s\n", std::string(result->ErrorMessage()).c_str());
  } else {
    std::printf("(unexpected kind %u)\n", result->kind());
  }
  return 0;
}
```

### 7.2 Go

```go
package main

import (
    "fmt"
    "os"

    "cel.dev/expr/celhost"
    demopb "cel.dev/expr/examples/demo"
)

func main() {
    bytes, err := os.ReadFile("expr.wasm")
    if err != nil { panic(err) }

    h, err := celhost.New(bytes)
    if err != nil { panic(err) }
    defer h.Close()

    // Typed registration via generics — no reflection on the hot
    // path.
    greetSpec := celhost.OverloadSpec{
        OverloadID: "greet_string",
        Params:     []celhost.Type{celhost.TString},
        Result:     celhost.TString,
    }
    err = celhost.RegisterFunc1[string, string](h, "greet", greetSpec,
        func(name string) (string, error) {
            return "hello, " + name, nil
        })
    if err != nil { panic(err) }

    req := &demopb.Request{
        User: &demopb.User{Name: "Augustine"},
    }
    if err := h.SetMessage("request", req); err != nil { panic(err) }

    v, err := h.Eval()
    if err != nil { panic(err) }
    switch {
    case v.IsString():
        fmt.Println(v.String())
    case v.IsError():
        fmt.Println("ERROR:", v.ErrorMessage())
    case v.IsUnknown():
        fmt.Println("UNKNOWN attrs:", v.UnknownAttrs())
    default:
        fmt.Printf("(unexpected kind %v)\n", v.Kind())
    }
}
```

## 8. Error and unknown propagation to the host language

CEL three-valued logic (main §9) means every `CelValue` can be
`CEL_UNKNOWN` or `CEL_ERROR` in addition to its static type.
Decoding to host types must preserve this:

- **C++.** `CelValueView::IsError()` / `IsUnknown()` return
  `true` iff the kind is `CEL_ERROR` / `CEL_UNKNOWN`; otherwise
  type-specific accessors (`AsInt`, `AsStringView`,
  `AsMessage` …) return the payload. `IsOk()` is shorthand for
  "neither unknown nor error." The `Eval()` result is an
  `absl::StatusOr<CelValueView>`; Status is reserved for
  SDK-level failures (missing binding, trap, instantiation
  failure) — CEL-visible Unknown / Error ride inside the
  CelValueView on the OK path.
- **Go.** `Value.IsError()` / `Value.IsUnknown()` mirror C++.
  `Value.Error()` returns `(code uint32, msg string)` —
  deliberately not wrapped in a Go `error` interface so a caller
  can distinguish "the expression evaluated to ERROR(div by
  zero)" from "the Host failed to evaluate at all." The `Eval`
  return pair is `(Value, error)`: `error` is SDK-level failure,
  `Value.IsError()` is CEL-level.

The split (SDK error vs CEL error) follows the same split the
existing `LoadedEval::CallEval` makes today.

## 9. Implementation milestones

This doc is a design, not a plan. The landing sequence below is
the suggested slicing for whoever picks this up. M6 (`m6-custom-fns.md`)
already reserves the `cel_fn.*` ABI and a stub generator; it does
**not** claim the public SDK surface, so this doc complements it
rather than duplicating it.

| Slice | Scope | Reference |
| ----- | ----- | --------- |
| **H1 — C++ skeleton + variable binding.** | `CelHost::Load`, `SetVariable` for scalars/strings/messages, `Eval` returning `CelValueView`. No custom functions yet. Reuses existing `LoadedEval`/`CelHostEnv` plumbing. | extends `compiler/host/host_loader.{h,cc}` |
| **H2 — C++ custom functions.** | `RegisterFunction`, `cel_fn.*` trampolines, Unknown/Error returns, trap-catching. Comes after M6 codegen lands the `cel_fn.*` imports. | complements `m6-custom-fns.md` Deliverables / Codegen |
| **H3 — Go port (scalar subset).** | `celhost.New`, typed `RegisterFunc1/2`, `SetVariable` for scalars/strings, `Eval`. Uses wasmtime-go. Message support in H4. | new `celhost/` package at repo root |
| **H4 — Go custom functions + messages.** | Message binding via `proto.Message`, `cel_fn.*` trampolines in Go, message-typed args/returns via externref round-trip. | reuses M6's IDL + stub generator for the `celfnc --go` target |

H1 can land against the current M3 surface (no custom
functions, scalars + messages). H2 waits on M6. H3 can start
in parallel with H2 (no dependency). H4 waits on H2.

Non-goals for this milestone set (explicitly): async streaming
eval, lazy variable registration (on-demand callbacks for
unbound variables), pluggable wasm runtimes (wasmer, V8) —
wasmtime-only for both C++ and Go.

## Open questions

Items the user should resolve before H1 starts. Listed with a
recommended default so a non-answer still lets implementation
proceed.

1. **Shared runtime instance across CelHosts?** Today
   `LoadedEval` instantiates one `runtime.wasm` per load. If the
   app loads ten expressions, that's ten runtime instances
   (~66 KiB each, per main §7.0.1). Is it worth letting a
   `CelHostPool` share one runtime across many expression
   modules? Trade-off: shared-arena means `cel_reset` between
   two expressions running on the same pool wipes both; the
   single-threaded contract makes that enforceable but forces a
   "one eval at a time per pool" rule that's not otherwise
   present. **Default: no pooling in v1.** Revisit if profiling
   shows runtime-per-host as a bottleneck.
2. **`HostValue::String` ownership model.** C++ can offer
   `StringView` (no copy, valid until next Eval) or `String`
   (owned copy, unbounded lifetime). Today's code would
   naturally choose View; that forces discipline on callers
   keeping results across Eval. **Default: View only, with a
   documented `Copy()` method.** Or should the SDK always copy?
   Relevant to main §7.0.1's single-thread contract.
3. **How are `OverloadSpec` types specified?** The sketch uses a
   `CelType` builder (`CelType::String()`, `CelType::Message("demo.User")`,
   `CelType::List(CelType::Int())`). Alternative: reuse
   `cel::expr::Type` proto directly. **Default: lightweight
   `CelType` wrapper over `cel::expr::Type` so embedders don't
   link the full cel-cpp proto.**
4. **Does the SDK validate `cel.abi.function_set` vs
   `RegisterFunction` calls?** Hard-error on mismatch feels
   right (signature skew is a known-bad failure mode), but it
   requires embedders to register every overload the module
   declares, even ones their expression doesn't reach. Or do
   we validate only on first call? **Default: validate at
   first `Eval`, list missing overloads in the error message.**
5. **Does `SetVariable` take ownership of a proto message in
   the C++ API?** Current sketch says no — embedder owns the
   lifetime. But embedders routinely have the opposite
   expectation in other SDKs. Decide between:
   (a) `Message*` non-owning (current sketch);
   (b) `shared_ptr<Message>` with ownership transfer;
   (c) support both via overload.
   **Default: (a).** It matches the existing
   `CelHostEnv::InternMessage` contract.
6. **Go `Value` mutability on the return path.** The sketch
   returns a `Value` that copies strings out of wasm memory. Do
   we want a cheaper `ValueView` for callers that promise not
   to outlive the Host? **Default: no view type in Go —
   single-shot copy keeps the API debuggable.** Revisit for
   perf-sensitive workloads.
7. **Registration of variadic or generic functions** —
   e.g. `size(list(A)) -> int`. H2 can register one overload
   per concrete element type (which is what the checker sees
   anyway), but that's awkward for user code. Is a
   "type-variable-aware" registration API worth it, or does
   the generated `cel_fn.<id>` convention (main §12.2,
   `matchesRegex_string_string` style) already flatten this?
   **Default: flattened IDs only.**
8. **Hot-reload of expressions** — replace the loaded
   `.wasm` without re-creating the CelHost? Useful for rule
   engines pushing updates. **Default: out of scope for v1;
   callers drop and re-create.**
