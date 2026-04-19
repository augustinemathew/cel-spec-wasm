#!/usr/bin/env bash
# Clones google/cel-cpp into third_party/cel-cpp at the pinned revision.
#
# Usage (from the repo root):
#   third_party/fetch_cel_cpp.sh
#
# The build is wired to this path via `local_path_override` in MODULE.bazel,
# so this must run before `bazel build //compiler/...` succeeds on a fresh
# checkout.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="${script_dir}/cel-cpp"
sha_file="${script_dir}/cel-cpp.sha"
upstream="https://github.com/google/cel-cpp.git"

if [[ ! -f "${sha_file}" ]]; then
  echo "error: missing ${sha_file}" >&2
  exit 1
fi
pinned_sha="$(tr -d '[:space:]' < "${sha_file}")"

if [[ -d "${repo_dir}/.git" ]]; then
  echo "cel-cpp already present; fetching and checking out ${pinned_sha}"
  git -C "${repo_dir}" fetch --depth 1 origin "${pinned_sha}"
  git -C "${repo_dir}" checkout -q "${pinned_sha}"
else
  echo "cloning cel-cpp at ${pinned_sha}"
  rm -rf "${repo_dir}"
  git clone --filter=blob:none --no-checkout "${upstream}" "${repo_dir}"
  git -C "${repo_dir}" fetch --depth 1 origin "${pinned_sha}"
  git -C "${repo_dir}" checkout -q "${pinned_sha}"
fi

echo "cel-cpp ready at ${repo_dir}"
