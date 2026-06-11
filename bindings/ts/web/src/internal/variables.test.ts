import { describe, expect, it } from 'vitest';

import {
  VariableParseError,
  parseVariableRow,
  parseVariablesForm,
} from './variables.js';

describe('parseVariableRow', () => {
  it('parses an int to a bigint', () => {
    const parsed = parseVariableRow('age:int=25');
    expect(parsed.decl).toEqual({ name: 'age', type: 'int' });
    expect(parsed.value).toBe(25n);
  });

  it('parses a negative int', () => {
    expect(parseVariableRow('n:int=-7').value).toBe(-7n);
  });

  it('parses INT64 boundary values', () => {
    expect(parseVariableRow('lo:int=-9223372036854775808').value).toBe(
      -9223372036854775808n,
    );
    expect(parseVariableRow('hi:int=9223372036854775807').value).toBe(
      9223372036854775807n,
    );
  });

  it('parses a uint to a non-negative bigint', () => {
    expect(parseVariableRow('u:uint=42').value).toBe(42n);
  });

  it('rejects a negative uint', () => {
    expect(() => parseVariableRow('u:uint=-1')).toThrow(VariableParseError);
  });

  it('parses UINT64_MAX', () => {
    expect(parseVariableRow('u:uint=18446744073709551615').value).toBe(
      18446744073709551615n,
    );
  });

  it('parses a double to a number', () => {
    expect(parseVariableRow('x:double=3.14').value).toBe(3.14);
  });

  it('parses an integral double as a number', () => {
    expect(parseVariableRow('x:double=2').value).toBe(2);
  });

  it('parses a bool', () => {
    expect(parseVariableRow('b:bool=true').value).toBe(true);
    expect(parseVariableRow('b:bool=false').value).toBe(false);
  });

  it('rejects a non-bool bool value', () => {
    expect(() => parseVariableRow('b:bool=yes')).toThrow(VariableParseError);
  });

  it('parses a string verbatim, preserving spaces', () => {
    expect(parseVariableRow('s:string= hi ').value).toBe(' hi ');
  });

  it('parses a string containing = and :', () => {
    expect(parseVariableRow('s:string=a=b:c').value).toBe('a=b:c');
  });

  it('parses an empty string', () => {
    expect(parseVariableRow('s:string=').value).toBe('');
  });

  it('parses bytes as UTF-8 of the value', () => {
    const parsed = parseVariableRow('data:bytes=hi');
    expect(parsed.value).toBeInstanceOf(Uint8Array);
    expect(parsed.value).toEqual(new TextEncoder().encode('hi'));
  });

  it('parses multi-byte UTF-8 bytes', () => {
    const parsed = parseVariableRow('data:bytes=café');
    expect(parsed.value).toEqual(new TextEncoder().encode('café'));
  });

  it('trims whitespace around the name and type', () => {
    const parsed = parseVariableRow('  age :  int = 25');
    expect(parsed.decl).toEqual({ name: 'age', type: 'int' });
    expect(parsed.value).toBe(25n);
  });

  it('rejects a row with no =', () => {
    expect(() => parseVariableRow('age:int')).toThrow(VariableParseError);
  });

  it('rejects a declaration with no :', () => {
    expect(() => parseVariableRow('age=25')).toThrow(VariableParseError);
  });

  it('rejects an empty name', () => {
    expect(() => parseVariableRow(':int=25')).toThrow(VariableParseError);
  });

  it('rejects an unknown type', () => {
    expect(() => parseVariableRow('x:money=1')).toThrow(VariableParseError);
  });

  it('rejects a non-integer int value', () => {
    expect(() => parseVariableRow('age:int=1.5')).toThrow(VariableParseError);
    expect(() => parseVariableRow('age:int=abc')).toThrow(VariableParseError);
  });

  it('rejects a non-numeric double', () => {
    expect(() => parseVariableRow('x:double=abc')).toThrow(VariableParseError);
  });
});

describe('parseVariablesForm', () => {
  it('parses multiple rows', () => {
    const parsed = parseVariablesForm('age:int=25\ncountry:string=US');
    expect(parsed).toHaveLength(2);
    expect(parsed[0]?.decl.name).toBe('age');
    expect(parsed[1]?.value).toBe('US');
  });

  it('ignores blank lines', () => {
    const parsed = parseVariablesForm('\nage:int=1\n\n\ncountry:string=US\n');
    expect(parsed).toHaveLength(2);
  });

  it('returns an empty list for an empty form', () => {
    expect(parseVariablesForm('')).toEqual([]);
    expect(parseVariablesForm('   \n  \n')).toEqual([]);
  });

  it('rejects a duplicate variable name', () => {
    expect(() => parseVariablesForm('age:int=1\nage:int=2')).toThrow(
      /duplicate/,
    );
  });

  it('propagates a malformed-row error', () => {
    expect(() => parseVariablesForm('age:int=1\nbad')).toThrow(
      VariableParseError,
    );
  });
});
