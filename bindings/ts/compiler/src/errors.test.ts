import { describe, expect, it } from 'vitest';

import { CelCompileError, parseDiagnostics } from './errors.js';

// The stderr samples below are copied verbatim from the `cel` CLI for
// the three failure shapes the binding must parse: a syntax error (named
// `<cli>` file token), a type-check error (empty file token, preceded by
// a header line), and an undeclared-reference error.
describe('parseDiagnostics', () => {
  it('parses a syntax error with file/line/column', () => {
    const stderr =
      "ERROR: ERROR: <cli>:1:5: Syntax error: mismatched input ')' " +
      "expecting {'[', '{', '('}\n | (1 +)\n | ....^\n";
    const diags = parseDiagnostics(stderr);
    expect(diags).toHaveLength(1);
    expect(diags[0]).toEqual({
      line: 1,
      column: 5,
      message: "Syntax error: mismatched input ')' expecting {'[', '{', '('}",
    });
  });

  it('parses a type-check error and drops the header line', () => {
    const stderr =
      'ERROR: type check failed:\n' +
      "ERROR: :1:4: found no matching overload for '_+_' applied to " +
      "'(int, string)'\n";
    const diags = parseDiagnostics(stderr);
    expect(diags).toHaveLength(1);
    expect(diags[0]).toEqual({
      line: 1,
      column: 4,
      message:
        "found no matching overload for '_+_' applied to '(int, string)'",
    });
  });

  it('parses an undeclared-reference error', () => {
    const stderr =
      'ERROR: type check failed:\n' +
      "ERROR: :1:2: undeclared reference to 'z' (in container '')\n";
    const diags = parseDiagnostics(stderr);
    expect(diags).toHaveLength(1);
    expect(diags[0]?.line).toBe(1);
    expect(diags[0]?.column).toBe(2);
    expect(diags[0]?.message).toContain("undeclared reference to 'z'");
  });

  it('collects multiple located diagnostics', () => {
    const stderr = 'ERROR: :1:2: first problem\nERROR: :3:7: second problem\n';
    const diags = parseDiagnostics(stderr);
    expect(diags).toHaveLength(2);
    expect(diags[0]).toEqual({ line: 1, column: 2, message: 'first problem' });
    expect(diags[1]).toEqual({ line: 3, column: 7, message: 'second problem' });
  });

  it('keeps a bare ERROR line without a location as a location-less diagnostic', () => {
    const diags = parseDiagnostics('ERROR: something went wrong\n');
    expect(diags).toHaveLength(1);
    expect(diags[0]).toEqual({ message: 'something went wrong' });
  });

  it('falls back to the whole stderr when no ERROR line is present', () => {
    const diags = parseDiagnostics('unexpected crash\nsecond line\n');
    expect(diags).toHaveLength(1);
    expect(diags[0]?.line).toBeUndefined();
    expect(diags[0]?.column).toBeUndefined();
    expect(diags[0]?.message).toBe('unexpected crash\nsecond line');
  });

  it('never returns an empty list for empty stderr', () => {
    const diags = parseDiagnostics('');
    expect(diags).toHaveLength(1);
    expect(diags[0]?.message).toBe('compilation failed');
  });
});

describe('CelCompileError', () => {
  it('carries diagnostics and summarizes the first located one', () => {
    const err = new CelCompileError([
      { line: 1, column: 5, message: 'bad token' },
      { message: 'header' },
    ]);
    expect(err).toBeInstanceOf(Error);
    expect(err.name).toBe('CelCompileError');
    expect(err.diagnostics).toHaveLength(2);
    expect(err.message).toBe('1:5: bad token');
  });

  it('summarizes a location-less first diagnostic by message alone', () => {
    const err = new CelCompileError([{ message: 'something broke' }]);
    expect(err.message).toBe('something broke');
  });

  it('uses an explicit message when provided', () => {
    const err = new CelCompileError([{ message: 'x' }], 'overridden');
    expect(err.message).toBe('overridden');
  });

  it('summarizes to a default when given no diagnostics', () => {
    const err = new CelCompileError([]);
    expect(err.message).toBe('compilation failed');
  });
});
