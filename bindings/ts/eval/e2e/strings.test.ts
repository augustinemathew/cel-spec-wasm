// e2e string-extension behaviors — cel-cpp's `extensions/strings.cc`
// surface, lit up through compile → plan → eval.
//
// Ported from the C++ `e2e/m12_test.cc` (the `string_ext` suite), whose
// source expressions track conformance rows from
// `spec/tests/simple/testdata/string_ext.textproto`.  The functions
// covered: size, startsWith, endsWith, contains, matches, substring,
// replace, split, join, upperAscii, lowerAscii, trim, indexOf,
// lastIndexOf, charAt, format.
//
// `size('é') == 1` is the Unicode-codepoint count (langdef §"Strings":
// string length is in code points, not bytes).

import { describe, expect, it } from 'vitest';

import { evalCel } from './helpers.js';

describe('size — code-point count (langdef §Strings)', () => {
  it.each([
    ["'tacocat'.size()", 7n],
    ["size('tacocat')", 7n],
    ["''.size()", 0n],
    ["'é'.size()", 1n], // one code point, two UTF-8 bytes
    ["'日本語'.size()", 3n],
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
});

describe('predicates — startsWith / endsWith / contains / matches', () => {
  it.each([
    ["'foobar'.startsWith('foo')", true],
    ["'foobar'.startsWith('bar')", false],
    ["'foobar'.endsWith('bar')", true],
    ["'foobar'.endsWith('foo')", false],
    ["'foobar'.contains('oba')", true],
    ["'foobar'.contains('xyz')", false],
    ["'abc'.matches('a.c')", true], // RE2 regex
    ["'abc'.matches('^a')", true],
    ["'abc'.matches('z')", false],
    ["matches('abc', 'a.c')", true], // global form
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
});

describe('substring / replace', () => {
  it.each([
    ["'hello'.substring(1)", 'ello'],
    ["'hello'.substring(1, 3)", 'el'],
    ["'hello'.substring(0, 0)", ''],
    ["'hello'.replace('l', 'L')", 'heLLo'],
    ["'a-b-c'.replace('-', '_')", 'a_b_c'],
    ["'a-b-c'.replace('-', '_', 1)", 'a_b-c'], // limited replacement count
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
});

describe('split / join', () => {
  it('split on a delimiter → list<string>', async () => {
    expect(await evalCel("'a,b,c'.split(',')")).toEqual(['a', 'b', 'c']);
  });
  it('split on a space', async () => {
    expect(await evalCel("'foo bar'.split(' ')")).toEqual(['foo', 'bar']);
  });
  it('join with a separator', async () => {
    expect(await evalCel("['a', 'b', 'c'].join('-')")).toBe('a-b-c');
  });
  it('join with no separator', async () => {
    expect(await evalCel("['a', 'b'].join()")).toBe('ab');
  });
  it('split then join round-trips', async () => {
    expect(await evalCel("'x,y,z'.split(',').join('|')")).toBe('x|y|z');
  });
});

describe('case / trim', () => {
  it.each([
    ["'Foo'.upperAscii()", 'FOO'],
    ["'Foo'.lowerAscii()", 'foo'],
    ["'MixedCASE123'.upperAscii()", 'MIXEDCASE123'],
    ["'  pad  '.trim()", 'pad'],
    ["'\\ttab\\n'.trim()", 'tab'],
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
});

describe('indexOf / lastIndexOf / charAt', () => {
  it.each([
    ["'abcabc'.indexOf('b')", 1n],
    ["'abcabc'.indexOf('z')", -1n],
    ["'abcabc'.lastIndexOf('b')", 4n],
    ["'hello'.charAt(0)", 'h'],
    ["'hello'.charAt(4)", 'o'],
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
});

describe('format — printf-style substitution', () => {
  it.each([
    ["'%d'.format([42])", '42'],
    ["'%s and %d'.format(['x', 5])", 'x and 5'],
    ["'%.2f'.format([3.14159])", '3.14'],
  ])('%s == %s', async (src, want) => {
    expect(await evalCel(src)).toBe(want);
  });
});
