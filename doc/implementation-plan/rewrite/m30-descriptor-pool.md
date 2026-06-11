# M30 — descriptor-pool plumbing through the C++ compiler APIs

Status: plan — drafted 2026-06-11, not yet started.

## 1. Why

Killing the internal CLI compile backend (the `@cel-wasm/compiler`
binding's `CliBackend`, which shells out to the native `cel` binary) is
the last open m29 follow-up. The blocker is **protobuf descriptors**:

  - M29 conformance reached 1710 pass (+264) by type-checking proto
    expressions against a `FileDescriptorSet` — but it does so through
    the CLI's `--descriptor_set <path>` flag.
  - The in-browser `compiler.wasm` (and the C ABI it wraps) **cannot
    accept a descriptor set at all** today. So the moment we route
    `compile()` / conformance through the wasm backend, every proto row
    regresses from pass back to skip.

So before the CLI backend can die, the wasm compiler must learn to
type-check against a caller-supplied descriptor set. This doc designs
that plumbing — and it is the *prerequisite* for the CLI removal, not
the removal itself (that is the closing slice here).

## 2. Current state (grounded in the source)

The schema carrier is a variant on `CheckOptions`
(`compiler/frontend/parse_and_check.h:35-58`):

```cpp
// compiler/frontend/parse_and_check.h
struct SchemaProtoSource   { std::string path; };  // a .proto source file
struct SchemaDescriptorSet { std::string path; };  // a binary FileDescriptorSet

struct CheckOptions {
  std::variant<std::monostate, SchemaProtoSource, SchemaDescriptorSet> schema;
  std::vector<std::string> variable_specs;
  // ...
};
```

Both variants hold a **filesystem path**. The descriptor-set path is
read with `std::ifstream` and parsed into a `FileDescriptorSet`
(`compiler/frontend/parse_and_check.cc:137-161`,
`RegisterSchemaDescriptorSet`), then `LoadDescriptorPool`
(`parse_and_check.cc:164+`) merges the parsed files with the
process-wide generated pool via a `MergedDescriptorDatabase` and hands
the checker an owned `DescriptorPool`.

Two consumers build `CheckOptions::schema`:

  - **The CLI** (`tools/cel/cel.cc:233-247`, `BuildCompileOptions`) sets
    `opts.check.schema = SchemaDescriptorSet{fds_path}` from
    `--descriptor_set` (or `SchemaProtoSource{path}` from `--proto`).
  - **The C ABI** (`bindings/c/cel_capi.cc:231-237`,
    `MakeCompilerOptions`) sets `container`, `optimize_level`,
    `link_mode` — and **nothing for schema**. The C ABI has no schema
    surface, so `compiler_wasm_exports.cc` / `cew_compile_opts` cannot
    pass one.

**The browser constraint.** `compiler.wasm` runs under the hand-written
WASI shim in `bindings/ts/compiler/src/internal/wasm-backend.ts`, which
has **no filesystem** (`fd_prestat_get` → `EBADF`, no preopens). A
path-based `SchemaDescriptorSet` is therefore unusable in the browser —
the descriptor set has to arrive as **bytes through linear memory**, not
a path.

## 3. Design

### 3.1 A bytes-based schema variant

Add a third schema alternative that carries the serialized
`FileDescriptorSet` in memory:

```cpp
// compiler/frontend/parse_and_check.h
// A binary-serialized google.protobuf.FileDescriptorSet held in memory
// (no filesystem). The path-based SchemaDescriptorSet stays for the CLI;
// this is what the C ABI / compiler.wasm use, where there is no FS.
struct SchemaDescriptorSetBytes {
  std::string bytes;
};

struct CheckOptions {
  std::variant<std::monostate,
               SchemaProtoSource,
               SchemaDescriptorSet,
               SchemaDescriptorSetBytes> schema;
  // ...
};
```

Factor the parse-and-register half out of `RegisterSchemaDescriptorSet`
so both the file path and the in-memory bytes share it:

```cpp
// Registers every file of a serialized FileDescriptorSet into schema_db.
absl::Status RegisterFileDescriptorSetBytes(
    absl::string_view bytes,
    google::protobuf::SimpleDescriptorDatabase& schema_db);
```

The existing path variant reads the file then delegates; the new bytes
variant delegates directly. `LoadDescriptorPool`'s `std::visit` gains
one `else if constexpr` arm. The merged-pool construction is unchanged —
the new variant only changes *where the bytes come from*, not how the
pool is built, so WKT resolution and the generated-pool merge are
identical to the path case (this preserves the byte-exact conformance
behaviour the CLI path already has).

### 3.2 C ABI surface

```c
// bindings/c/cel_capi.h
// Supply a binary-serialized google.protobuf.FileDescriptorSet (the bytes
// `protoc --descriptor_set_out` emits) describing the message types a
// proto-typed expression references. Copied into `opts`; the caller may
// free `fds` after the call. Overrides any previously set descriptor set.
void cel_compile_opts_set_descriptor_set(
    CelCompileOpts* opts, const uint8_t* fds, int len);
```

`CelCompileOpts` gains a `std::string descriptor_set` field;
`MakeCompilerOptions` (`cel_capi.cc`) sets
`out.check.schema = SchemaDescriptorSetBytes{opts->descriptor_set}` when
it is non-empty (and leaves `std::monostate` otherwise — so a
no-descriptor compile is unchanged).

### 3.3 `cew_compile_opts` records entry

The structured records format already added in m29
(`compiler_wasm_exports.cc`, `ApplyOptions`) gains one kind:

| kind | value | meaning |
|------|-------|---------|
| `'d'` | raw FDS bytes (`len`-delimited) | descriptor set → `cel_compile_opts_set_descriptor_set` |

`ApplyOneOption` gets a `case 'd'`. No new boundary shape — the records
format was designed length-prefixed precisely so a binary blob like an
FDS rides through with no escaping.

### 3.4 TypeScript backend

`CompileRequest.descriptorSet` is today an absolute **path** (consumed by
the CLI's `--descriptor_set`). The wasm backend needs **bytes**. Two
clean options — pick during implementation:

  - **(a)** Keep `descriptorSet: string` (path) in `CompileRequest`;
    `wasm-backend.ts` reads the file → bytes in Node, and the SPA passes
    a second `descriptorSetBytes?: Uint8Array`. Asymmetric.
  - **(b, preferred)** Add `descriptorSetBytes?: Uint8Array` to
    `CompileRequest`; the wasm backend encodes it as a `'d'` record. The
    CLI backend keeps using the path form; a small helper reads a path
    to bytes where a caller only has a path. Symmetric and
    browser-honest — the SPA fetches the FDS like it fetches the
    runtime.

The conformance harness already loads the FDS into memory
(`conformance/src/harness.ts`), so it has the bytes in hand — option (b)
lets it pass them straight to the wasm backend with no temp file.

### 3.5 Closing slice — retire the CLI backend

Once descriptors flow through the wasm path:

  1. `compile()`'s default backend (`compiler/src/index.ts`,
     `getDefaultBackend`) switches from `CliBackend` to
     `WasmCompileBackend`, loading the compiler.wasm asset. (Move the
     54 MB `compiler.wasm` to live with the `@cel-wasm/compiler` package
     rather than `web/public/`, so the compiler binding owns its own
     artifact and the web package depends on it — fixing the current
     compiler↔web layering inversion.)
  2. The conformance harness compiles via the wasm backend with
     `descriptorSetBytes`.
  3. Delete `cli-backend.ts` + `cli-backend.test.ts`; move the
     `CompileBackend` / `CompileRequest` interfaces to a backend-neutral
     module (they currently live in `cli-backend.ts`).
  4. Accept the known diagnostic gap: the stock-wasi-sdk `compiler.wasm`
     gives generic messages on *syntax* errors (the EH wall). Type-check
     errors keep full diagnostics. This is the already-accepted "wasm
     now, emscripten later" tradeoff; emscripten restores syntax
     diagnostics in a later milestone.

## 4. Testing

Per repo rules — probe the real behaviour before asserting it, and pin
it with the oracle where a value is in question.

  - **C++ unit** — `parse_and_check_test.cc`: a `SchemaDescriptorSetBytes`
    built from an in-memory FDS type-checks a `Foo{...}` literal and a
    message-typed var identically to the path variant (same pool, same
    result). Negative: malformed bytes → `InvalidArgumentError`; a file
    with a duplicate `name` → the same duplicate error the path variant
    gives.
  - **C ABI** — `cel_capi_test.cc`: `cel_compile_opts_set_descriptor_set`
    then `cel_compile` of a proto expression succeeds and the emitted
    Program decodes; null/zero-len leaves the schema at `monostate`.
  - **wasm e2e** — extend `web/src/run.test.ts` (or a compiler-package
    test once the asset moves): compile a proto expression through
    `compiler.wasm` with a `'d'` record and assert it type-checks where
    it previously failed `undeclared reference`.
  - **Conformance** — the existing corpus run is the integration gate:
    flipping the harness to the wasm backend must hold 1710 pass / 0
    fail (the ratchet enforces it).
  - **Oracle** — for any "does this proto expression type-check / what
    does it evaluate to" question that the bytes-vs-path change could
    perturb, add a case to `testdata/cel_cpp_oracle_test.cc` rather than
    reasoning it out.

## 5. Work breakdown (slices)

  - **A — C++ core.** `SchemaDescriptorSetBytes` variant +
    `RegisterFileDescriptorSetBytes` + `LoadDescriptorPool` arm + tests.
    No ABI/TS change; fully testable in isolation.
  - **B — C ABI + wasm export.** `cel_compile_opts_set_descriptor_set`,
    the `'d'` record in `ApplyOneOption`, rebuild `compiler.wasm`, C ABI
    test. End-to-end testable via a Node harness driving the rebuilt
    wasm (the m29 pattern).
  - **C — TS backend.** `descriptorSetBytes` on `CompileRequest`,
    `wasm-backend.ts` encoder, unit test.
  - **D — retire the CLI.** Move the compiler.wasm asset into the
    compiler package, switch `compile()` + conformance to the wasm
    backend, delete `cli-backend.ts`, re-home the backend interfaces.
    Gate: full conformance corpus holds 1710/0.

Slices A–C are additive and regression-free (nothing routes through the
new path until D). D is the behavioural switch and carries the
conformance gate.

## 6. Open questions

  - **Asset home & size.** `compiler.wasm` is 54 MB. Moving it into the
    published `@cel-wasm/compiler` package makes the npm tarball large;
    do we ship it as an optional/peer asset, fetch-on-demand, or accept
    the size? (The browser already lazy-fetches it; Node could too.)
  - **`SchemaProtoSource` for wasm?** Source-`.proto` compilation also
    needs an in-memory variant for the browser, but no current consumer
    needs it (conformance uses FDS). Deferred unless a driver appears.
  - **Descriptor-set merge order.** The CLI merges schema-over-generated;
    confirm the bytes path keeps the same precedence so a schema that
    re-declares a WKT can't shadow the generated one (probe before
    assuming).

## 7. Relationship to other docs

  - `m29-typescript-bindings.md` — the descriptorSet path-based wiring
    (CLI) shipped there; this doc supersedes it for the wasm/browser
    path and closes the CLI-backend-removal follow-up it opened.
  - `design.md` — the C ABI surface section gains
    `cel_compile_opts_set_descriptor_set` when slice B lands.
