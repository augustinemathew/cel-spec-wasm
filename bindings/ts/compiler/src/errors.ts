// Compile-error surface for the cel-wasm TypeScript compiler binding.
//
// The C++ `cel` CLI reports parse / type-check failures on stderr as
// `ERROR:` lines, the diagnostic ones carrying a `<file>:<line>:<col>:
// <message>` location prefix.  This module turns that text into a
// structured {@link CelCompileError} so callers see line/col/message
// instead of a raw blob.

/**
 * One diagnostic parsed from the compiler's stderr.  `line` / `column`
 * are 1-based when the compiler reported a location; they are `undefined`
 * for diagnostics without one (e.g. the bare `type check failed:`
 * header), in which case only `message` is meaningful.
 */
export interface Diagnostic {
  readonly message: string;
  readonly line?: number;
  readonly column?: number;
}

/**
 * Thrown by {@link compile} when the compiler exits non-zero.  Carries
 * the parsed {@link Diagnostic} list; `.message` is a one-line summary.
 */
export class CelCompileError extends Error {
  override readonly name = 'CelCompileError';
  readonly diagnostics: readonly Diagnostic[];

  constructor(diagnostics: readonly Diagnostic[], message?: string) {
    super(message ?? summarize(diagnostics));
    this.diagnostics = diagnostics;
  }
}

// The compiler prints diagnostics on lines beginning with `ERROR:`; a
// syntax error can carry a doubled prefix (`ERROR: ERROR: …`).  A located
// diagnostic looks like:
//
//   ERROR: ERROR: <cli>:1:5: Syntax error: mismatched input ')' ...
//   ERROR: :1:4: found no matching overload for '_+_' ...
//
// After stripping the leading `ERROR:` run, the remainder is an optional
// file token (anything up to the first colon-number), then
// `:<line>:<col>:`, then the human message.  Non-located lines (e.g.
// `type check failed:`) carry no location.
const ERROR_PREFIX = /^(?:ERROR:\s*)+/;
const LOCATED_DIAGNOSTIC =
  /^(?<file>[^:\n]*):(?<line>\d+):(?<column>\d+):\s*(?<message>.*)$/;

/**
 * Parse the compiler's stderr into a list of {@link Diagnostic}s.  Every
 * `ERROR:`-prefixed line becomes one diagnostic; located lines keep their
 * 1-based line/column.  If no `ERROR:` line is present (an unexpected
 * failure mode), the whole trimmed stderr is returned as a single
 * location-less diagnostic so the error is never empty.
 */
export function parseDiagnostics(stderr: string): Diagnostic[] {
  const diagnostics: Diagnostic[] = [];
  for (const rawLine of stderr.split('\n')) {
    const line = rawLine.trim();
    if (line.length === 0 || !ERROR_PREFIX.test(line)) {
      continue;
    }
    const body = line.replace(ERROR_PREFIX, '');
    const located = LOCATED_DIAGNOSTIC.exec(body);
    if (located?.groups) {
      diagnostics.push({
        message: located.groups.message?.trim() ?? '',
        line: Number(located.groups.line),
        column: Number(located.groups.column),
      });
      continue;
    }
    // Drop the `type check failed:` / `parse failed:` header lines —
    // they carry no actionable location and only repeat the section the
    // located lines below already convey.
    const message = body.trim();
    if (message.length > 0 && !message.endsWith(':')) {
      diagnostics.push({ message });
    }
  }
  if (diagnostics.length === 0) {
    const fallback = stderr.trim();
    diagnostics.push({
      message: fallback.length > 0 ? fallback : 'compilation failed',
    });
  }
  return diagnostics;
}

/** Render a one-line summary of the first diagnostic for `Error.message`. */
function summarize(diagnostics: readonly Diagnostic[]): string {
  const first = diagnostics[0];
  if (first === undefined) {
    return 'compilation failed';
  }
  if (first.line !== undefined && first.column !== undefined) {
    return `${String(first.line)}:${String(first.column)}: ${first.message}`;
  }
  return first.message;
}
