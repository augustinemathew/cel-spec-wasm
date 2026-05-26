// check-proto-gen.mjs — guarantee the committed protobuf-es bindings
// (eval/src/gen/cel_abi_pb.ts) are in sync with compiler_v2/abi/cel_abi.proto.
//
// The TS bindings are NOT yet regenerated automatically by the C++/bazel
// build (that lands with the aspect_rules_ts migration). Until then this
// guard makes staleness impossible to miss: it regenerates into a temp
// dir and diffs against the committed file, failing the `check` gate if
// they differ. If a `cel_abi.proto` change lands without `npm run
// proto:gen`, this turns red and names the fix.
//
// Requires `protoc` on PATH (CI has it). Run via `npm run proto:check`.

import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const GENERATED = 'eval/src/gen/cel_abi_pb.ts';
const PROTO_ROOT = '../../abi';
const PROTO = 'cel_abi.proto';

function fail(msg) {
  console.error(`check-proto-gen: ${msg}`);
  process.exit(1);
}

const tmp = mkdtempSync(join(tmpdir(), 'celabi-gen-'));
try {
  try {
    execFileSync(
      'protoc',
      [
        '--plugin=protoc-gen-es=./node_modules/.bin/protoc-gen-es',
        `--es_out=${tmp}`,
        '--es_opt=target=ts',
        '-I',
        PROTO_ROOT,
        PROTO,
      ],
      { stdio: ['ignore', 'ignore', 'inherit'] },
    );
  } catch {
    fail('protoc failed (is `protoc` on PATH?). Cannot verify generated bindings.');
  }

  const fresh = readFileSync(join(tmp, 'cel_abi_pb.ts'), 'utf8');
  const committed = readFileSync(GENERATED, 'utf8');
  if (fresh !== committed) {
    fail(
      `${GENERATED} is STALE vs ${PROTO_ROOT}/${PROTO}. ` +
        'Run `npm run proto:gen` and commit the result.',
    );
  }
  console.log('check-proto-gen: cel_abi_pb.ts is up to date.');
} finally {
  rmSync(tmp, { recursive: true, force: true });
}
