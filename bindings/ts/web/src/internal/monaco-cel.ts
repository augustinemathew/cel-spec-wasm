// A lightweight CEL language for Monaco — a Monarch tokenizer plus a calm
// theme.  This is syntax *highlighting*, not a grammar: it colours
// keywords, builtins, operators, strings, and numbers well enough to read
// an expression.  The authoritative parse lives in the C++ compiler the
// compile endpoint drives; the editor never needs the full grammar.

import * as monaco from 'monaco-editor';

/** The Monaco language id the CEL model is registered under. */
export const CEL_LANGUAGE_ID = 'cel';

/** The Monaco theme id the demo selects. */
export const CEL_THEME_ID = 'cel-calm';

const KEYWORDS = ['true', 'false', 'null', 'in'];

const BUILTINS = [
  'has',
  'size',
  'map',
  'filter',
  'exists',
  'exists_one',
  'all',
  'int',
  'uint',
  'double',
  'string',
  'bool',
  'bytes',
  'type',
  'timestamp',
  'duration',
  'dyn',
  'matches',
  'startsWith',
  'endsWith',
  'contains',
];

const MONARCH: monaco.languages.IMonarchLanguage = {
  keywords: KEYWORDS,
  builtins: BUILTINS,
  operators: [
    '&&',
    '||',
    '!',
    '==',
    '!=',
    '<',
    '<=',
    '>',
    '>=',
    '+',
    '-',
    '*',
    '/',
    '%',
    '?',
    ':',
    '.',
  ],
  tokenizer: {
    root: [
      [
        /[a-zA-Z_]\w*/,
        {
          cases: {
            '@keywords': 'keyword',
            '@builtins': 'type.identifier',
            '@default': 'identifier',
          },
        },
      ],
      [/0[xX][0-9a-fA-F]+[uU]?/, 'number.hex'],
      [/\d+\.\d+([eE][+-]?\d+)?/, 'number.float'],
      [/\d+[uU]?/, 'number'],
      [/"([^"\\]|\\.)*"/, 'string'],
      [/'([^'\\]|\\.)*'/, 'string'],
      [/[bB]"([^"\\]|\\.)*"/, 'string.escape'],
      [/[{}()[\]]/, '@brackets'],
      [/[<>!=]=?|&&|\|\||[+\-*/%]/, 'operator'],
      [/[.,;:?]/, 'delimiter'],
      [/\/\/.*$/, 'comment'],
      [/\s+/, 'white'],
    ],
  },
};

const THEME: monaco.editor.IStandaloneThemeData = {
  base: 'vs',
  inherit: true,
  rules: [
    { token: 'keyword', foreground: '7c3aed', fontStyle: 'bold' },
    { token: 'type.identifier', foreground: '0e7490' },
    { token: 'string', foreground: '15803d' },
    { token: 'string.escape', foreground: '15803d' },
    { token: 'number', foreground: 'b45309' },
    { token: 'number.float', foreground: 'b45309' },
    { token: 'number.hex', foreground: 'b45309' },
    { token: 'operator', foreground: '475569' },
    { token: 'comment', foreground: '94a3b8', fontStyle: 'italic' },
  ],
  colors: {
    'editor.background': '#ffffff',
    'editorLineNumber.foreground': '#cbd5e1',
    'editor.lineHighlightBackground': '#f8fafc',
  },
};

let registered = false;

/**
 * Register the CEL language + theme with Monaco (idempotent).  Call once
 * before creating an editor model so `language: CEL_LANGUAGE_ID` resolves.
 */
export function registerCelLanguage(): void {
  if (registered) {
    return;
  }
  monaco.languages.register({ id: CEL_LANGUAGE_ID });
  monaco.languages.setMonarchTokensProvider(CEL_LANGUAGE_ID, MONARCH);
  monaco.languages.setLanguageConfiguration(CEL_LANGUAGE_ID, {
    brackets: [
      ['{', '}'],
      ['[', ']'],
      ['(', ')'],
    ],
    autoClosingPairs: [
      { open: '{', close: '}' },
      { open: '[', close: ']' },
      { open: '(', close: ')' },
      { open: '"', close: '"' },
      { open: "'", close: "'" },
    ],
  });
  monaco.editor.defineTheme(CEL_THEME_ID, THEME);
  registered = true;
}
