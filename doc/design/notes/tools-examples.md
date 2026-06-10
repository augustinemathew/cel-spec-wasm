# tools-examples — design notes (undefined)

Scope: `tools/cel/` (the `cel` CLI), `tools/wat_runner/` (WAT-first harness),
`examples/` (9 runnable embeds + smoke test). Paired docs:
`doc/implementation-plan/rewrite/cel-cli-design.md`, `tools/cel/README.md`,
`examples/README.md` (all exist and were read).

## 1. Verified architecture

### 1.1 `cel` CLI (`tools/cel/cel.cc`)

- One binary, **four** subcommands: `eval`, `check`, `compile`, `generate`
  (cel.cc:544-545 gate; dispatch cel.cc:569-593). There is **no `cel run`**,
  no `inspect`, no `--celfn`, no `--module`, no `--activation` — the entire
  run-a-precompiled-Program half of the design doc is unimplemented
  (cel-cli-design.md:40-43 marks them ⛔ planned; code agrees).
- Argv handling: the subcommand is peeled out of argv (cel.cc:553-558), then
  `ExtractRepeated` pulls every `--var` / `--format` occurrence (both
  `--f=V` and `--f V` forms) out of argv **before** `absl::ParseCommandLine`
  runs (cel.cc:471-493, 559-564). Reason: their values legally contain
  commas/`=`, and absl's repeatable-flag parser comma-splits then overwrites
  (cel.cc:54-58). Consequence documented in README.md:326-329: always quote
  the whole `--var` argument.
- Flags: `--proto` / `--descriptor_set` (mutually exclusive, enforced twice:
  cel.cc:194-197 and 239-242), `--container`, `--O` (Binaryen level 0..3 →
  `CompileOptions.optimize_level`, cel.cc:236), `--mem_size_bytes` (default
  128 KiB, cel.cc:79-82, 235), `--output` (compile only); generate-only
  `--idl/--language/--out_dir/--package/--include` (cel.cc:90-104).
- Schema loading (`BuildPool`, cel.cc:191-225):
  - `--proto` parses ONE `.proto` source in-process via
    `google::protobuf::compiler::Parser` (cel.cc:144-166); imports are
    recorded but NOT recursively loaded — README.md:100-164 documents the
    resulting atomic-file-rejection failure mode accurately.
  - `--descriptor_set` parses a binary `FileDescriptorSet` into a
    `SimpleDescriptorDatabase`, rejecting duplicate file names
    (cel.cc:168-189).
  - Either is merged with `DescriptorPool::generated_pool()` via
    `MergedDescriptorDatabase` (cel.cc:217-224); with neither flag the
    generated pool is used directly (cel.cc:199-201).
- `eval` pipeline (cel.cc:327-399): BuildPool → ParseAllVars →
  BuildCompileOptions → `celwasm::Compile` (the **internal** pipeline facade
  `compiler/internal/compile.h`, not the public `Compiler` class —
  cel.cc:32, BUILD.bazel:106) → `Program` → `Engine::NewBuilder().Build()` →
  `Plan` → bind `--var` values into an `Activation` → `Eval` →
  `FormatScalar` (non-message) or `FormatMessage` (message + `--format`s).
  With zero vars it calls the no-activation `Eval()` overload (cel.cc:365).
- `check` runs `celwasm::ParseAndCheck` only and prints `OK`
  (cel.cc:418-424). `compile` writes wasm bytes to `--output` or raw to
  stdout, byte-count diagnostic to stderr (cel.cc:449-463).
- `generate` (run_generate.cc): reads the `.idl`, `ParseCelfnSource`, then
  drives four emitters — `fns.wit`, `codec.h`, `generated_stub.cc`,
  `user_fns.h` — into `--out_dir` (run_generate.cc:63-82). `--language` cpp
  only; anything else exits 2 (run_generate.cc:96-100). Default WIT package
  `cel:<module>` with fallback `cel:customfn` (run_generate.cc:52-58).
  `generate` takes no positional expr (cel.cc:569-578).
- Exit codes (README.md:24): 0 success; 1 compile/eval/IDL-parse/emit
  failure; 2 usage error. In code, 2 also covers pool/var-parse/engine/Plan
  failures (cel.cc:328-359) — i.e. "2" is really "anything before/around the
  compile-eval core", not strictly usage.
- Variable spec round-trip: `BuildCompileOptions` re-formats each parsed
  `CelType` back into the `name:TypeSpec` string `parse_and_check.cc`
  consumes (cel.cc:248-288) — deliberate single-source-of-truth for the
  type grammar.

### 1.2 `--var` parser (`tools/cel/var_parser.{h,cc}`)

- Grammar (var_parser.h:8-36): `name:Type[=value]`; scalar types bool, int,
  uint (optional `u` suffix), double, string, bytes, duration (absl/Go
  syntax), timestamp (RFC3339); `list<T>`, `map<K,V>` (recursive); anything
  else is treated as a message FQN without second-guessing
  (var_parser.cc:130-134).
- Type-directed and coercion-free: `:int=3.14` errors with offset
  (var_parser.cc:636-639). `kType`/`kUnknown` cannot be bound
  (var_parser.cc:565-569).
- Message values: `@path` with extension dispatch (.txtpb/.textproto/
  .pbtxt/.prototxt → TextFormat, .json → JSON, .pb/.bin → binary;
  var_parser.cc:429-443) or explicit `txtpb:`/`json:`/`pb:` inline prefix
  (var_parser.cc:501-507); inline without prefix is rejected
  (var_parser.cc:525-528). Instances are `DynamicMessage`s built from the
  supplied pool; `Value::OwnedMessage` owns them (var_parser.cc:534-536).
  Lifetime contract: factory + parsed vars must outlive every Eval
  (var_parser.h:56-61, 76-80).
- Declaration-only form `name:Type` sets `has_value=false`
  (var_parser.cc:623-627); `BindActivation` skips those (cel.cc:295).
- String escapes are deliberately narrower than CEL's grammar (JSON-ish:
  \n \t \r \\ \" \' \0 \xHH; var_parser.cc:149-224). NOTE: var_parser.h:25
  advertises `\uXXXX` + surrogate pairs, but the `.cc` switch has no `u`
  arm — see §2.8.
- `bytes` accepts `@path` raw-file form (var_parser.cc:317-325).

### 1.3 Output formatter (`tools/cel/value_format.{h,cc}`)

- `FormatScalar(v)`: one-line CEL-literal form for every non-message kind
  including list/map/null/duration("3s")/timestamp("...")/type(...)/
  `<unknown:id>`/`error: <code-name> <msg>` (value_format.cc:222-282).
  Unhandled kind → `ABSL_CHECK(false)` (closed-switch rule).
- `FormatMessage(v, formats)`: textproto (TextFormat, `SetExpandAny(true)`),
  json (`MessageToJsonString`, `preserve_proto_field_names=true`), cel
  (hand-rolled shallow literal; repeated fields render as `[...]`,
  value_format.cc:172-174). Empty `formats` → `{kTextproto}`; >1 →
  `--- <name> ---` headers (value_format.cc:326-352).
- `ParseFormatName` accepts aliases `txtpb`/`pbtxt` for textproto
  (value_format.cc:299) — the CLI usage text only advertises
  `textproto|json|cel` (cel.cc:509).

### 1.4 `wat_runner` (`tools/wat_runner/`)

Role: the executable half of the repo's WAT-first discipline — assemble a
hand-written WAT expr module and run it through the **real**
`cel_runtime.wasm` under wasmtime's C API, with caller-supplied stubs for
`cel_host.*` imports whose production trampolines don't exist yet
(wat_runner.h:1-39). Explicit non-goals: not production code, shares **no
code** with `Engine::Plan` on purpose (so it can prototype changes to
Plan's own shape), ships no assertions (wat_runner.h:31-39).

`RunWat(WatRunInput) -> WatRunOutput` (wat_runner.h:152, cc:892-907):
1. `wasmtime_wat2wasm` (InvalidArgument on assembly error, cc:399-413).
2. Engine config: tail-call + threads + shared-memory proposals enabled —
   required because the runtime's dispatchers use `return_call` and its
   memory is shared (cc:444-456).
3. Store gets a minimal WASI context (the runtime links absl+cctz →
   wasi-libc imports) (cc:477-498).
4. Linker: `define_wasi`, `RegisterCelLog`, optional 3-arg stubs with no-op
   fallbacks (`cel_map_lookup`, `cel_list_at`, `cel_wkt_unwrap_wrapper` —
   third arg semantically repurposed as wrapper_kind — `cel_set_field`),
   bulk no-ops for aggregate kHost dispatchers + `cel_make_message` +
   `resolve_message_type_name` + iter-open trampolines, no-op
   duration/timestamp ("M7B") imports, and the optional 4-arg
   `cel_get_field`/`cel_has_field` stubs (cc:503-734).
5. Instantiate the embedded `kCelRuntimeWasmBytes`; **adopt** the runtime's
   exported shared memory as `cel.memory` (cc:759-778); bind **all 115**
   names in `kRuntimeExports` onto the linker — the append-only "always
   link the runtime fully" mirror of Engine::Plan (cc:28-33, 820-825;
   entry count verified == declared 115); seed the arena via
   `arena_init(CELWASM_ARENA_CAPACITY_BYTES)` (cc:784-805).
6. Instantiate the expr module, require an `eval` func export (cc:829-847).
7. Apply `pre_writes` (simulating Activation marshal) AFTER instantiation,
   BEFORE `$eval` (cc:853-865); call `$eval` (0-arg → i32); snapshot the
   whole shared memory into `memory_after` (cc:883-887).

Contract pins (from wat_runner_test.cc): a WAT importing
`cel_host.cel_get_field` with no stub supplied **fails instantiation** —
codegen can't sneak an unimplemented host import past the harness
(test:316-326). 4-arg/3-arg stub trampolines trap on arity/type mismatch
(cc:200-215, 345-357). Stubs get the raw memory buffer and write CelValues
at out_slot.

Known harness limitation: wasmtime's C API **panics** on the
`return_call`-from-runtime → imported-host-trampoline path, so the two
kDynamic dispatcher WATs (09, 14) are reasoned `GTEST_SKIP`s pointing at
production coverage in m3/m4/instance tests (test:487-518, 661-671).

Both `:wat_runner` and `:wat_runner_test` are tagged `manual`
(tools/wat_runner/BUILD.bazel:13, 35) — they do NOT run under a bare
`bazel test //...`; the test data-deps the
`//doc/implementation-plan/rewrite/wat:wat_traces` filegroup (BUILD:34).

### 1.5 `examples/`

Nine `cc_binary` examples (01-09), each restricted (with one exception, see
§2.6) to public API targets, plus `examples_smoke_test.sh` which runs every
binary and asserts on documented output substrings — the doc-snippet rot
gate (examples/BUILD.bazel:186-200, examples_smoke_test.sh:27-44). Coverage
arc: 01 hello-world pipeline; 02 typed variables + Activation reuse; 03
Program-bytes save/reload in a compiler-free "serving box"
(03:58-78 — `Program(std::move(bytes))`, "no validation until Plan");
04 `@host` fn via one `.celfn` decl string + `Engine::BindFunction`
signature-validated lambda (04:34-58); 05 `PartialEval` +
`AttributePattern::Parse` (05:46-61, 80-86); 06 proto message vars from the
generated pool, non-owning `Value::Message` (06:52-56); 07 the three
failure layers (compile Status / CEL error value / accessor StatusOr);
08 host-fn returning Value::Error vs Value::Unknown
(`kFunctionUnknownSentinel`) vs non-OK Status (08:50-68), registered via
`Engine::AddTypedFunction("quota_string", ...)` (08:82); 09 sandboxed
`@component.` fn — `adder.idl` + `adder_fns.cc` →
`cel_wasm_component` macro (`//bazel:cel_wasm_component.bzl`,
BUILD:17,160-164) → component bytes loaded via bazel runfiles →
`Engine::AddComponent(bytes, lib)` with an embedder-side `FunctionLibrary`
mirror of the IDL (09:65-83). 09 is scalar-only on purpose: string-returning
component fns trap (pinned skip in
e2e/foreign_component_fixtures/.../demo_component_e2e_test.cc;
adder.idl:8-13, examples/README.md:24-26).

Output-line note pinned in 08's header (08:26-28) and asserted by the smoke
test (smoke:41): a host-fn ErrorPayload's **code** survives the wasm
round-trip; the free-text message does not — decoded errors carry
`"runtime error code N"`.

## 2. Doc-vs-code discrepancies

1. **P1 — cel-cli-design.md describes a 3-verb CLI; code ships 4.** The doc
   (cel-cli-design.md:5-8, table :35-43) says "what ships today (`eval` /
   `check` / `compile` only)" and plans `cel celfn gen --lang go` first
   (:43, :118-127). Code shipped a fourth subcommand `cel generate`
   (cel.cc:544-545, run_generate.cc) with `--language cpp` only and go
   "planned" (run_generate.cc:97-99, cel.cc:92-94) — a different verb name,
   different flag, different first language than designed. Doc was never
   re-statused after the m26 generate work landed.
2. **P1 — wat_runner_test claims to run "every WAT file"; it runs ~half.**
   Header comment (wat_runner_test.cc:1-2: "Run every WAT file under
   doc/implementation-plan/rewrite/wat/") and BUILD comment
   (tools/wat_runner/BUILD.bazel:26-27 "Exercises every WAT shape") vs
   reality: the dir holds 63 `.wat` files; the test loads ~28 of them.
   Never loaded by any test (grep over the repo finds no other consumer):
   01-05, 40-41, 50-55, 60-67, m13_*, m16_*, m18_*. CLAUDE.md's stronger
   claim ("wat_runner_test.cc re-assembles and re-runs every .wat ... on
   every build") is doubly wrong: subset only, AND the target is
   `manual`-tagged so it does not run on every build/test sweep.
3. **P1 — bound-aggregate eval: smoke test vs README vs activation matrix
   disagree.** cel_smoke_test.sh:50-54 asserts (in a comment) that
   "celwasm's eval path doesn't yet support comprehensions over
   activation-bound lists or any activation-bound map" and deliberately
   routes those cases through `check`. But tools/cel/README.md:87-98 shows
   `cel eval "xs.exists(x, x > 5)" --var "xs:list<int>=[1, 3, 5, 7]"  # → true`
   and `cel eval 'm["us"]' --var 'm:map<string,int>=...'  # → 1` as working,
   and activation_matrix_test.cc (manual) asserts exactly these shapes green
   through the same public API (BoundListIntExists :113, BoundMapStringIntLookup
   :161). Most likely the smoke-test comment is stale and the smoke test is
   under-asserting; see validation item 1.
4. **P1 — examples/BUILD.bazel claims "Examples depend ONLY on the
   sanctioned public targets" (BUILD:7-10); example 09 depends on
   `//compiler/celfn:function_library`** (BUILD:173), whose package default
   visibility is `//:internal` (compiler/celfn/BUILD.bazel:4, target
   :24-36 has no override). The "examples double as a compile-time check
   that the public surface is sufficient" property is violated for 09: an
   external embedder copying it cannot build against the public surface.
   (Matches the known "AddComponent visibility" bug #32.)
5. **P1 — wat_runner.h's "What this does" is pre-Phase-C.** Header step 2
   says the harness "Initialises a per-run wasmtime store + 2-page memory,
   matching `celwasm::Engine::Plan`'s M1 wiring" and step 3 says it binds
   "`arena_reset` / `arena_alloc` exports" (wat_runner.h:17-20);
   `WatRunOutput.memory_after` is documented as "Size = 2 pages (128 KiB)"
   (h:142-146). Code: the harness creates NO memory — it adopts the
   runtime's own exported `(memory 4 1024 shared)` (cc:469-476, 754-778) —
   binds all 115 runtime exports (cc:820-825), and the snapshot is the full
   live shared-memory size. The `.cc` carries corrected "Phase C" comments;
   the public header doc does not.
6. **P2 — examples/README.md:3 says "Seven small programs"; the table lists
   nine** (01-09, README:14-22).
7. **P2 — value_format.h:11 names the API `FormatMessages(v, formats)`; the
   declared function is `FormatMessage`** (value_format.h:54).
8. **P2 — var_parser.h:25 advertises `\uXXXX` and surrogate-pair escapes in
   string literals; the implementation's escape switch has no `u` arm**
   (var_parser.cc:174-220 — `\u` hits the `default:` "unknown escape"
   error). tools/cel/README.md:364 repeats the "surrogate-pair escapes"
   claim for var_parser_test. See validation item 6.
9. **P2 — cel-cli-design.md references `doc/user-guide.md` §9 throughout**
   (:6-8, :200); that file no longer exists — the user guide became the
   `doc/user-guide/` directory.
10. **P2 — wat_runner.cc comments cite `api/engine.cc`** (cc:444, 487, 721,
    758, 781) — the engine lives at `eval/engine.cc`; there is no `api/`
    dir. Stale path from a pre-reorg layout.
11. **P2 — README exit-code taxonomy is approximate.** README.md:24 says
    "2 usage error", but Engine-build and Plan failures (infrastructure,
    not usage) also exit 2 (cel.cc:350-359).
12. **P2 — usage text under-advertises `--format` aliases** — `txtpb` and
    `pbtxt` are accepted (value_format.cc:299) but only
    `textproto|json|cel` is printed (cel.cc:509).

## 3. Validation items

1. **Does `cel eval` handle activation-bound lists/maps today?**
   Run: `bazel run //tools/cel:cel -- eval "xs.exists(x, x > 5)" --var "xs:list<int>=[1, 3, 5, 7]"`
   and `bazel run //tools/cel:cel -- eval 'm["us"]' --var 'm:map<string,int>={"us": 1, "ca": 2}'`.
   If both print `true`/`1`, the cel_smoke_test.sh comment is stale and the
   smoke cases should be upgraded from `check` to `eval`; if not, the
   README's §"Lists and maps — bound via --var" examples are wrong.
2. **Have the never-loaded WATs rotted?**
   `for f in doc/implementation-plan/rewrite/wat/{01,02,03,04,05,40,41,50,51,52,53,54,55,60,61,62,63,64,65,66,67}_*.wat doc/implementation-plan/rewrite/wat/m1[368]_*.wat; do wasm-as "$f" -o /dev/null || echo "BROKEN: $f"; done`
   — settles whether the "every WAT re-assembles" doctrine is restorable or
   the corpus needs pruning.
3. **Do the manual-tagged gates pass right now?**
   `bazel test //tools/wat_runner:wat_runner_test //tools/cel:activation_matrix_test`
   — and confirm both appear in the manual-target catalog in
   `doc/implementation-plan/per-component-test-coverage.md`.
4. **Is example 09 reachable from outside `//:internal`?**
   `bazel query "visible(//some_external_pkg, //compiler/celfn:function_library)"`
   (or attempt the dep from a package not in the `//:internal` group) —
   settles whether discrepancy 4 needs a visibility promotion or a README
   correction.
5. **Is the wasmtime C-API tail-call panic still live?** Temporarily delete
   the `GTEST_SKIP` in `WatRunnerMapTest.DispatcherWatAssemblesAndImportsResolve`
   (wat_runner_test.cc:515-518) and run the test; if wasmtime has been
   bumped past the bug, two dispatcher WATs come back under harness
   coverage.
6. **`\uXXXX` escape support:**
   `bazel run //tools/cel:cel -- eval "s" --var 's:string="A"'` —
   expected per var_parser.cc reading: `ERROR ... unknown escape \u`. If it
   errors, fix var_parser.h:25 + README.md:364; if it succeeds, re-read the
   parser (and add the missing test).
7. **`cel compile` stdout purity:**
   `bazel run //tools/cel:cel -- compile "1+1" > /tmp/x.wasm && head -c4 /tmp/x.wasm | od -An -tx1`
   must be `00 61 73 6d` with nothing prepended (the smoke test only checks
   the `--output` path, cel_smoke_test.sh:73-88).
8. **`cel generate` end-to-end:** no test invokes the subcommand anywhere
   (no `run_generate_test.cc`; cel_smoke_test.sh never calls `generate`).
   Probe: `bazel run //tools/cel:cel -- generate --idl examples/adder.idl --out_dir /tmp/gen` and diff the four outputs against the
   `cel_wasm_component` macro's generated files.

## 4. Test coverage observations

Pinned well:
- **var_parser_test (22 cases):** full scalar matrix positive+negative
  (bool/int incl. INT64_MAX/uint incl. `u` suffix and `-1` reject/double/
  string escapes/bytes incl. `@file`/duration/timestamp), containers,
  nested `map<string,list<int>>`, message in all three inline/file formats,
  unknown-type, missing-prefix, bad-extension, declaration-only,
  missing-colon, trailing-garbage.
- **value_format_test (17 cases):** every `Value::Kind` through
  FormatScalar; message textproto-default/json/cel/multi-format-labeled;
  ParseFormatName incl. `txtpb` alias and reject.
- **cel_smoke_test.sh:** binary-level argv extraction (comma-bearing values
  survive), exit-code discipline (unknown subcommand, type error), wasm
  magic of `--output`, portable `od` instead of `xxd` (deliberate,
  :79-83).
- **activation_matrix_test (24 cases, manual):** the aggregate-binding
  matrix the CLI surfaces — list<int>/list<string>/map<string,int>/
  map<int,string>/proto singleton/nested/map-field/repeated/mixed — via the
  public API (header :12-24 states the axes).
- **wat_runner_test (~37 cases):** decodes raw CelValue bytes at
  `$eval`'s return offset; pins the 4-arg get_field ABI verbatim-arg
  round-trip, unknown-write absorption, missing-stub instantiation failure,
  arena reset/alloc spacing semantics, kArena map/list/arith/compare/
  string-concat/aggregate/3VL/base64/optional/poison-set-field/native-inline
  layouts, and select→lookup host-chaining via paired stubs.

Gaps:
- **`run_generate.cc` has no `_test.cc` and no smoke coverage** — the only
  untested source file in the component (violates the repo's
  every-file-gets-a-test rule). The emitters it calls are tested in
  `compiler/celfn/celfnc_emit`, but flag validation, package-name
  defaulting (run_generate.cc:52-60), and file I/O are not.
- The `cel` binary's `--format` and `--proto`/`--descriptor_set` paths are
  never exercised at the binary level (cel_smoke_test.sh is deliberately
  schema-less, :2-4); README's multi-format and FDS walkthroughs rest on
  library tests only.
- `ExtractRepeated` lives in `cel.cc`'s anonymous namespace — no direct
  unit test; only indirect smoke coverage. Edge shapes (trailing bare
  `--var` at argv end, `--var` consuming a following flag-looking token)
  are unpinned.
- var_parser: no INT64_MIN case (max is covered), no uint-overflow case,
  no list-of-message / map-with-message-value case.
- value_format: the cel-literal `[...]` placeholder for repeated fields,
  `FormatMessage` on a non-message value, and the `pbtxt` alias are
  unasserted.
- ~35 WAT trace files have no executing consumer (see §2.2/§3.2).
- 2 reasoned skips in wat_runner_test (dispatcher WATs, c-api tail-call
  panic) — correct per skip-discipline, but they mean the kDynamic
  dispatch shape is only covered in e2e, not at the WAT layer.

## 5. Design decisions worth preserving

1. **`--var`/`--format` bypass absl flags entirely** (ExtractRepeated runs
   before `absl::ParseCommandLine`) because absl comma-splits repeatable
   string-vector flags and overwrites on repeat (cel.cc:54-58). Any new
   repeatable, comma-bearing CLI flag must follow this pattern.
2. **Type-directed value parsing, zero coercion** (var_parser.h:32-36):
   the declared CelType disambiguates every literal; `int=3.14` errors.
   Unknown type names are passed through as message FQNs and left to the
   checker (var_parser.cc:130-134) — the CLI does not second-guess.
3. **One type grammar:** the CLI re-serialises parsed CelTypes into the
   exact `name:TypeSpec` strings `parse_and_check.cc` consumes
   (cel.cc:248-288) rather than keeping a second declaration path.
4. **DynamicMessage lifetime contract:** `Value::OwnedMessage` backs
   message vars; the factory and the ParsedVar vector must outlive every
   Eval that observes them (var_parser.h:56-61, 76-80).
5. **wat_runner shares no code with Engine::Plan, by design**
   (wat_runner.h:33-37) — if the harness depended on Plan's shape it could
   not be used to prototype changes to that shape. Resist "deduplication".
6. **Append-only full runtime-export binding** (`kRuntimeExports`,
   wat_runner.cc:28-33): mirrors the "always link the runtime fully" rule;
   dropping a name silently breaks WATs that import it — which is the
   point (tripwire, not bug).
7. **Missing 4-arg stub ⇒ instantiation failure** is a deliberate contract
   (wat_runner_test.cc:316-326): a WAT cannot reach `$eval` with an
   unimplemented `cel_get_field`/`cel_has_field`; 3-arg surfaces get no-op
   fallbacks instead so kArena-only WATs that merely *link* the imports
   still instantiate (wat_runner.h:98-101).
8. **Wire-shape reuse with per-surface semantic re-labelling:** the 3-arg
   `(i32,i32,i32)->()` stub type is shared across map_lookup/list_at/
   wkt_unwrap_wrapper/set_field, with the third (or second) arg's meaning
   redefined per surface and documented at the field (wat_runner.h:105-134).
9. **pre_writes after instantiation, before `$eval`** — and the pinned
   regression that `arena_reset` rewinds only the BSS cursor, never
   workspace bytes (wat_runner_test.cc:138-151).
10. **Examples are the public-surface compile gate and the doc-snippet rot
    gate**: deps restricted to public targets (modulo §2.4), smoke test
    asserts the documented output lines verbatim. New doc snippets should
    be derived from an example the smoke test runs.
11. **`repl` was considered and dropped** (cel-cli-design.md:45-47);
    `cel run` + `inspect` remain the designed-but-unbuilt half, premised on
    `cel.abi` being self-describing (vars yes; host-import/foreign-alias
    enumeration is an open ABI question, cel-cli-design.md:184-188).
12. **`cel compile` keeps stdout byte-clean** (diagnostics → stderr,
    cel.cc:462) so `cel compile ... > x.wasm` composes; and under
    `bazel run` a relative `--output` resolves inside runfiles — README
    tells users to pass absolute paths (README.md:28-31).
