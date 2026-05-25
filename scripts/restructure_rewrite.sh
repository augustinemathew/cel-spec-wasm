#!/usr/bin/env bash
#
# restructure_rewrite.sh — deterministic, idempotent rewriter for the repo
# restructure (Wave W3 runs it; W0 only authors it).
#
# It applies the FROZEN §1.2 (Bazel labels) + §1.3 (#include) + §1.4
# (strip_import_prefix) mappings from
# doc/implementation-plan/repo-restructure-execution.md across a WIDE glob of
# build/lint/hooks/source files. third_party/ and bazel-* are NEVER touched
# (the glob is sourced from `git ls-files`, so generated/symlinked trees are
# excluded by construction).
#
# Properties:
#   - Deterministic: pure function of the §1 mapping; no agent output needed.
#   - Idempotent: every rule rewrites an OLD token to a NEW token that no rule
#     re-matches, so a second run is a no-op. Safe to run twice.
#   - Longest-match-first: the api/ fan-out (the one non-prefix case) is encoded
#     explicitly, with the most specific paths (api/type, api/internal, the named
#     api leaf files) rewritten BEFORE any generic api prefix rule, so e.g.
#     compiler_v2/api/type lands at common/ not eval/.
#
# What it deliberately does NOT do (W3/W6 own these):
#   - `namespace celwasm::api` flattening (W6).
#   - collapsing `//compiler_v2:__subpackages__` to a new label (W3 per-package
#     visibility decision) — left verbatim.
#   - the `git mv` directory moves themselves (W3 step 1) — this script only
#     rewrites the textual references inside files.
#   - the LITERAL wildcard `//compiler_v2/...` (3-dot) in run_full_suite.sh,
#     build_lint_pch.sh, refresh_compile_db.sh. There is no single correct sed:
#     those scripts use it in TWO incompatible syntaxes — space-separated on
#     `bazel test/build` command lines vs. `+`-union inside aquery/query strings
#     (`//compiler_v2/... + //compiler_v2/...`). It must expand to the
#     PROJECT-PACKAGE SET ($PROJ, exec-doc §1.0) hand-converted per context:
#     command lines get the space-joined set, query strings get the `+`-joined
#     set. W3 does this by hand; the "no compiler_v2 in scripts" grep gate and
#     W5 (which RUNS these scripts) catch any miss. (questions-log Q3.)
#
set -euo pipefail

# ---------------------------------------------------------------------------
# File set — §1.5: build/lint/hooks infra hardcodes the moving paths too, so
# the glob widens beyond *.bazel/*.bzl/*.cc/*.h. Sourced from git so third_party
# and bazel-* symlink trees are excluded.
# ---------------------------------------------------------------------------
cd "$(git rev-parse --show-toplevel)"

mapfile -t FILES < <(git ls-files \
  '*.bazel' '*.bzl' '*.cc' '*.h' \
  'scripts/*.sh' 'scripts/*.py' scripts/lint_pch.h \
  .clang-tidy .clang-format-ignore .githooks/pre-push \
  | grep -v '^third_party/' || true)

if [[ ${#FILES[@]} -eq 0 ]]; then
  echo "restructure_rewrite: no files matched the glob; nothing to do." >&2
  exit 0
fi

# ---------------------------------------------------------------------------
# Rewrite rules. Each entry is a single `sed` substitution. ORDER MATTERS:
# longest / most-specific OLD token first so the api/ fan-out resolves before
# any generic api prefix. All NEW tokens are outside the OLD-token space, so
# re-running is a no-op (idempotent).
#
# Token forms covered (per §1.2 / §1.3):
#   Bazel labels    //compiler_v2/<pkg>(:target)
#   #include paths  "compiler_v2/<pkg>/...h"
#   strip_import_prefix  "/proto" -> "/spec/proto"
# A single regex per logical mapping handles BOTH the `//compiler_v2/api/...`
# label form and the `"compiler_v2/api/..."` include form, because they share
# the `compiler_v2/api/...` substring.
#
# Portability + boundary design: BSD sed (macOS) does NOT support `\b`, and the
# repo builds on both macOS and Linux, so the rules use NO word-boundary
# escape. Each rule is a pure PREFIX replacement: `//compiler_v2/api:engine`
# -> `//eval:engine` rewrites the prefix and leaves any suffix intact, so a
# suffixed target (`:engine_test`, `:instance_impl`) follows its base leaf to
# the same destination package. This is safe ONLY because every api leaf that
# shares a name-prefix routes to the SAME package (verified: engine/engine_test
# -> eval; compiler/compiler_test -> compiler; instance/instance_impl/
# instance_test -> eval; type/type_test -> common). No two prefix-sharing
# leaves diverge in destination, so prefix matching cannot mis-route. Include
# rules anchor on the full `.h"` so they need no boundary either.
# ---------------------------------------------------------------------------
SED_RULES=(
  # ---- §1.4 strip_import_prefix (proto_library under spec/proto/**) ----
  's#strip_import_prefix = "/proto"#strip_import_prefix = "/spec/proto"#g'

  # ===================================================================
  # api/ FAN-OUT — explicit, longest-match-first (the one non-prefix case).
  # api/type        -> common/
  # api/compiler|program -> compiler/
  # api/internal    -> eval/internal/
  # api/cel_pipeline_bench -> bench/
  # api/{engine,instance,activation,value,error,attribute,host_callback,
  #      cel_host, ...other leaves} -> eval/
  # ===================================================================

  # --- api/type -> common/type  (label `//compiler_v2/api:type` and include
  #     "compiler_v2/api/type.h"; must precede the generic api rule) ---
  's#//compiler_v2/api:type#//common:type#g'
  's#"compiler_v2/api/type\.h"#"common/type.h"#g'

  # --- api/compiler, api/program -> //compiler:compiler, //compiler:program ---
  's#//compiler_v2/api:compiler#//compiler:compiler#g'
  's#//compiler_v2/api:program#//compiler:program#g'
  's#"compiler_v2/api/compiler\.h"#"compiler/compiler.h"#g'
  's#"compiler_v2/api/program\.h"#"compiler/program.h"#g'

  # --- api/cel_pipeline_bench -> bench/ ---
  's#//compiler_v2/api:cel_pipeline_bench#//bench:cel_pipeline_bench#g'
  's#"compiler_v2/api/cel_pipeline_bench#"bench/cel_pipeline_bench#g'

  # --- api/internal/ -> eval/internal/ (more specific than api/<leaf>) ---
  's#//compiler_v2/api/internal#//eval/internal#g'
  's#"compiler_v2/api/internal/#"eval/internal/#g'

  # --- api/<eval leaves> -> eval/ : engine, instance, activation, value,
  #     error, attribute, host_callback, cel_host (label) ---
  #     Labels (named targets):
  's#//compiler_v2/api:engine#//eval:engine#g'
  's#//compiler_v2/api:instance#//eval:instance#g'
  's#//compiler_v2/api:activation#//eval:activation#g'
  's#//compiler_v2/api:value#//eval:value#g'
  's#//compiler_v2/api:error#//eval:error#g'
  's#//compiler_v2/api:attribute#//eval:attribute#g'
  's#//compiler_v2/api:host_callback#//eval:host_callback#g'
  's#//compiler_v2/api:cel_host#//eval:cel_host#g'
  #     Includes (header files) for the same eval leaves:
  's#"compiler_v2/api/engine\.h"#"eval/engine.h"#g'
  's#"compiler_v2/api/instance\.h"#"eval/instance.h"#g'
  's#"compiler_v2/api/activation\.h"#"eval/activation.h"#g'
  's#"compiler_v2/api/value\.h"#"eval/value.h"#g'
  's#"compiler_v2/api/error\.h"#"eval/error.h"#g'
  's#"compiler_v2/api/attribute\.h"#"eval/attribute.h"#g'
  's#"compiler_v2/api/host_callback\.h"#"eval/host_callback.h"#g'

  # ===================================================================
  # Prefix moves (straightforward — no fan-out).
  # ===================================================================

  # --- compiler_v2/host/ -> eval/host/ ---
  's#//compiler_v2/host#//eval/host#g'
  's#"compiler_v2/host/#"eval/host/#g'

  # --- compiler_v2/{frontend,ir,codegen,celfn}/ -> compiler/<same> ---
  's#//compiler_v2/frontend#//compiler/frontend#g'
  's#//compiler_v2/ir#//compiler/ir#g'
  's#//compiler_v2/codegen#//compiler/codegen#g'
  's#//compiler_v2/celfn#//compiler/celfn#g'
  's#"compiler_v2/frontend/#"compiler/frontend/#g'
  's#"compiler_v2/ir/#"compiler/ir/#g'
  's#"compiler_v2/codegen/#"compiler/codegen/#g'
  's#"compiler_v2/celfn/#"compiler/celfn/#g'

  # --- compiler_v2/compile.{h} + //compiler_v2:compile -> compiler/internal ---
  's#//compiler_v2:compile#//compiler/internal:compile#g'
  's#"compiler_v2/compile\.h"#"compiler/internal/compile.h"#g'

  # --- compiler_v2/{abi,runtime}/ -> strip compiler_v2/ ---
  's#//compiler_v2/abi#//abi#g'
  's#//compiler_v2/runtime#//runtime#g'
  's#"compiler_v2/abi/#"abi/#g'
  's#"compiler_v2/runtime/#"runtime/#g'

  # --- compiler_v2/{tools,conformance,e2e,bench,testdata}/ -> strip ---
  's#//compiler_v2/tools#//tools#g'
  's#//compiler_v2/conformance#//conformance#g'
  's#//compiler_v2/e2e#//e2e#g'
  's#//compiler_v2/bench#//bench#g'
  's#//compiler_v2/testdata#//testdata#g'
  's#"compiler_v2/tools/#"tools/#g'
  's#"compiler_v2/conformance/#"conformance/#g'
  's#"compiler_v2/e2e/#"e2e/#g'
  's#"compiler_v2/bench/#"bench/#g'
  's#"compiler_v2/testdata/#"testdata/#g'
  #     bare-path (non-label, non-include) forms used in scripts / configs:
  's#compiler_v2/conformance/#conformance/#g'
  's#compiler_v2/tools/#tools/#g'
  's#compiler_v2/e2e/#e2e/#g'
  's#compiler_v2/bench/#bench/#g'
  's#compiler_v2/testdata/#testdata/#g'

  # ===================================================================
  # Heritage proto/tests moves (§1.2 spec rows).
  # ===================================================================
  's#@cel-spec//proto/cel#//spec/proto/cel#g'
  's#//proto/cel#//spec/proto/cel#g'
  's#//tests/simple#//spec/tests/simple#g'
)

# ---------------------------------------------------------------------------
# Apply. Report a before/after count of the canary substring `compiler_v2`
# across the file set so the operator can see the sweep took effect.
# ---------------------------------------------------------------------------
count_canary() {
  # Count files still mentioning compiler_v2 (the primary thing the sweep clears).
  grep -lE 'compiler_v2' "${FILES[@]}" 2>/dev/null | wc -l | tr -d ' '
}

before_files=$(count_canary)
echo "restructure_rewrite: ${#FILES[@]} files in scope; ${before_files} mention 'compiler_v2' before."

SED_ARGS=()
for rule in "${SED_RULES[@]}"; do
  SED_ARGS+=(-e "$rule")
done

# BSD sed (macOS) and GNU sed both accept `-i ''`? No — GNU rejects the empty
# suffix arg. Detect and branch.
if sed --version >/dev/null 2>&1; then
  # GNU sed
  sed -i "${SED_ARGS[@]}" "${FILES[@]}"
else
  # BSD/macOS sed
  sed -i '' "${SED_ARGS[@]}" "${FILES[@]}"
fi

after_files=$(count_canary)
echo "restructure_rewrite: ${after_files} mention 'compiler_v2' after."
echo "restructure_rewrite: done (idempotent — re-running is a no-op)."
