# The plugin / WIT pipeline

A `@plugin.` function is authored in C++, compiled to a WebAssembly
**Component**, and called from CEL. Between those two ends sit five
code generators, two toolchains, and three separate naming schemes.
This page is the map. It exists because the pipeline had a class of
bug — two generators independently deriving the *same* symbol name and
disagreeing about it — that no single file could reveal.

Scope: the `@plugin.` backend only. `@host.` functions are ordinary
wasm imports bound to C++ callbacks and never touch any of this
(`design/05-custom-functions.md`).

## 1. The stages

Everything below is driven by the `cel_wasm_plugin` Bazel macro
(`bazel/cel_wasm_plugin.bzl`) from two author-supplied inputs: a
`.idl` file of `@plugin.` declarations and a `user_fns.cc` with the
bodies.

```
  fns.idl ──┬─► cel generate ─┬─► customfn.wit      (1) WIT interface
            │                 ├─► codec.h           (2) lift/lower helpers
            │                 ├─► generated_stub.cc (3) export wrappers
            │                 └─► user_fns.h        (4) author signatures
            │
            └─► wit-bindgen c ─► customfn.{c,h}     (5) canonical-ABI glue
                                 customfn_component_type.o

  user_fns.cc + (3) + (5) ─► wasi-sdk cc_binary ─► core wasm
                                  │  (wasm32-wasip2 → already a component)
                                  ▼
                          cel embed-decls ─► <name>.wasm
                                             (component + `cel.fns` section)
```

At eval time the host walks the other way: `Plugin::Load` reads the
`cel.fns` section back, `Engine::Use` checks the component's exports
against those declarations, and `eval/internal/cel_plugin.cc` lifts CEL
values into `wasmtime_component_val_t` and lowers the results back.

## 2. Three naming schemes, one identity

This is the part that bites. A single declaration is known by three
different spellings, and every generator must agree on the mapping.

| Layer | Spelling of `bool @plugin.is_adult(proto(acme.User) u);` | Produced by |
|---|---|---|
| Overload id | `is_adult_message_acme_User` | `ArgkindSlug`, `compiler/celfn/function_library.cc` |
| WIT function | `is-adult-message-acme-user` | `SnakeToKebab`, `celfnc_emit/wit_emitter.cc` |
| C symbol | `exports_cel_customfn_fns_is_adult_message_acme_user` | wit-bindgen, from the WIT name |

The overload id is the ABI's identity for the function: it appears in
`cel.abi`'s required-functions table and is what the checker resolves
against. It preserves the proto FQN's original case, because a proto
type's name is `acme.User`, not `acme.user`.

**WIT identifiers are lowercase-only.** The WIT lexer accepts ASCII
lowercase, digits, and `-`. So `wit_emitter.cc` flattens case *and*
swaps `_` for `-`. wit-bindgen then derives the C symbol from the WIT
name, giving back underscores but keeping the lowercasing.

The trap: `celfnc_emit/cpp_stub_emitter.cc` also has to name that same
C symbol, to define the export wrapper. If it uses the overload id
verbatim, it defines `..._acme_User` while wit-bindgen declares
`..._acme_user`. Nothing complains at compile time — the wrapper is
simply an unused function, and the declared export stays undefined —
and the failure surfaces much later, from the component encoder, as:

```
failed to resolve import `env::exports_cel_customfn_fns_is_adult_message_acme_user`
module requires an import interface named `env`
```

which points at `env` and says nothing about case. Every proto-typed
plugin declaration was broken this way until 2026-07-28.

**Rule for anyone adding a generator: the C symbol is
`exports_<pkg>_<iface>_` + `AsciiStrToLower(overload_id)`.** It is
lowercase because WIT is, not because C is. Only scalar-typed decls
are accidentally immune, since their slugs are already lowercase —
which is why the whole non-proto matrix passed while protos did not.

## 3. Type mapping

Each CEL type has a WIT shape, a wit-bindgen C struct, and a C++
container the author actually writes against. `codec.h` bridges the
last two.

| CEL | WIT | C struct | Author-side C++ |
|---|---|---|---|
| `bool` / `int` / `uint` / `double` | `bool` / `s64` / `u64` / `f64` | (passed unwrapped) | `bool` / `int64_t` / `uint64_t` / `double` |
| `string` | `string` | `customfn_string_t` | `std::string_view` in, `std::string` out |
| `bytes` | `list<u8>` | `customfn_list_u8_t` | `std::vector<uint8_t>` |
| `null` | `option<u8>` | `customfn_option_u8_t` | `std::monostate` |
| `list<T>` | `list<T'>` | `customfn_list_<T>_t` | `std::vector<T''>` |
| `map<K,V>` | `list<tuple<K,V>>` | `customfn_list_tuple2_<k>_<v>_t` | `std::map<K'',V''>` |
| `Duration` / `Timestamp` | `record { seconds: s64, nanos: s32 }` | `exports_<pkg>_<iface>_{duration,timestamp}_t` | `google::protobuf::{Duration,Timestamp}` |
| `proto(X)` | `list<u8>` (serialized) | `customfn_list_u8_t` | the generated `X` class |

Two shapes deserve attention. A **map is a list of pairs** on the wire,
so its C struct is `list_tuple2_…`, not `tuple2_…` — the latter is the
element type. And a **proto crosses as serialized bytes**, sharing
`bytes`' wire shape; the codec emits a `lift_proto<M>` / `lower_proto<M>`
template pair rather than the bytes overloads, which is why
`StructFor` carries an `is_proto` flag alongside the struct name.

`optional<T>` and `type` are rejected at declaration time and never
reach any of this.

Ownership: the author never frees a return value. `lower(*ret, …)`
populates the out-param using `cabi_realloc`, and the canonical-ABI
runtime calls the generated `cabi_post_*` to free it. An **empty**
aggregate must short-circuit to a NULL pointer with `len = 0` rather
than asking `cabi_realloc` for zero bytes.

## 4. Why wasip2, and what that costs

Plugins target `wasm32-wasip2`
(`//third_party/wasi_sdk:wasm32_wasip2`) because the Component Model's
canonical ABI is a preview2-era concept: at that target the toolchain
emits a component directly (preamble `0x1000d`), with no separate
`wasm-tools component new --adapt` step and no vendored adapter
artifact. Targeting preview1 would emit a core module needing explicit
adaptation.

That platform is deliberately `wasi_threads_off` — components require
non-shared linear memory — and everything awkward about the plugin
build follows from it:

- **libc++ has no `<mutex>`, `<thread>`, or `<condition_variable>`.**
  Anything absl-dependent — which means anything protobuf-dependent —
  will not compile. Three patches (cctz's time-zone map guard,
  `absl::Mutex`'s yield hook, and the stdcpp waiter backend plus
  `spinlock.h`'s `lock_guard`) made it build end-to-end on 2026-07-28
  and were **reverted the same day**: absl does not belong in a shipped
  wasm guest. `protobuf_lite` does not help — it depends on
  `absl/time` directly.

  So this is the live constraint, not a solved one: **a proto-typed
  plugin argument does not work today.** The rest of the proto path is
  fixed and unit-pinned (the export-symbol lowercasing, the `-pthread`
  strip below); what remains is the guest runtime. Unblocking it needs
  a proto surface that does not drag absl — upb, or an IDL that passes
  fields rather than whole messages. The e2e fixture carries a skip
  recording exactly that.

  No absl patch remains. The last one,
  `abseil-cpp-wasm-sysinfo.patch` (`GetNumCPUs()` returning 1 under
  `__wasm__`/`__wasi__`), was a Phase C leftover from when the wasm
  build still pulled absl in; it was removed once nothing did.
  `somepath(//runtime:cel_runtime_wasm, @com_google_absl//absl/base:base)`
  is empty, so the wasm side never compiles absl, and abseil-cpp now
  builds stock with no `single_version_override` at all.
- **`-pthread` must not reach the link.** absl and protobuf carry it in
  their own linkopts via a select whose default arm assumes a threaded
  POSIX host. `-pthread` makes clang pass `--shared-memory` to wasm-ld,
  which then refuses the link outright. `third_party/wasi_sdk/wasm_clang.sh`
  strips `-pthread` / `-lpthread` on wasip2 links only.
- **The guest must not import `wasi:random`.** libc++ seeds its hash
  machinery lazily, and wasi-libc implements that seed through the
  `wasi:random/random@0.2.0` import. If the lazy init fires while the
  guest is inside a canonical-ABI lift or lower — which is exactly when
  an aggregate argument or return is being marshalled — wasmtime
  refuses the import call and the evaluation dies with `wasm trap:
  cannot leave component instance`. `bazel/plugin_rng_stub.c` defines
  `__imported_wasi_snapshot_preview1_random_get` in the guest, so the
  import is never emitted and there is no call left to forbid.

That last one is worth internalising: **a host-side shim cannot fix
it.** Answering the import from the host still requires *calling out
of* the component at a moment when that is illegal. The import has to
not exist. This is also why the bug looked intermittent before it was
understood — whether the seed had already been initialised depended on
what else had allocated first, so identical source passed on some
builds and trapped on others.

## 5. Where each stage is tested

| Stage | Test |
|---|---|
| Overload-id / slug derivation | `compiler/celfn/function_library_test.cc` |
| WIT emission | `compiler/celfn/celfnc_emit/wit_emitter_test.cc` |
| codec.h emission | `compiler/celfn/celfnc_emit/cpp_codec_emitter_test.cc` |
| Stub emission (incl. the export symbol) | `compiler/celfn/celfnc_emit/cpp_stub_emitter_test.cc` |
| Host lift/lower | `eval/internal/cel_plugin_test.cc` |
| The whole pipeline, per carrier | `e2e/plugin_fixtures/cel_wasm_plugin_demo/demo_plugin_e2e_test.cc` |

The unit tests above all passed throughout the period when no
proto-typed plugin could load, because each generator was
self-consistent. **Only the e2e fixture can catch a disagreement
between two generators**, and only for the type shapes it actually
declares. When you add a carrier to the type table in §3, add a row to
`KindMatrixEchoRoundTrips` — the matrix is the contract.
