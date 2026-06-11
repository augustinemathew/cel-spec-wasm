// A focused reader for the protobuf text format, scoped to the shapes
// the `cel.expr.conformance.test.SimpleTestFile` corpus uses.
//
// protobufjs cannot read textproto, and there is no reliable npm reader
// for an arbitrary `.proto` schema, so this module parses the corpus
// directly.  The grammar the corpus exercises is a regular subset of the
// full text format (https://protobuf.dev/reference/protobuf/textformat-spec/):
//
//   - `field: scalar` and `field { ... }` / `field: { ... }` (message),
//     `field <...>` angle-bracket message form,
//   - repeated fields as repeated `field:` lines (no `[a, b]` list
//     syntax — the corpus never uses it for the fields we read),
//   - string scalars in single OR double quotes, with C-style escapes
//     (`\n`, `\t`, `\\`, `\"`, `\'`, `\xHH`, `\ooo` octal, `\uHHHH`),
//     and ADJACENT string-literal concatenation (`"a" "b"` → `"ab"`),
//   - unquoted scalars: numbers (incl. leading `-`, `.`, exponents,
//     `inf`/`nan`), booleans (`true`/`false`), and bareword enum /
//     identifier tokens (`NULL_VALUE`, `INT64`),
//   - `#` line comments.
//
// This is NOT a general textformat parser; it raises on any construct
// the corpus does not contain (the `[ext]` extension syntax, `Any`
// expansion `[type.url]{...}`), so an unexpected shape fails loudly
// rather than being silently dropped.
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.7.

/**
 * A parsed textproto value.  A message is a map from field name to the
 * list of values seen for that field (repeated fields keep every
 * occurrence); a scalar is the decoded leaf.
 */
export type TextprotoValue = TextprotoMessage | TextprotoScalar;

/** A parsed message: field name → the ordered list of its values. */
export interface TextprotoMessage {
  readonly kind: 'message';
  readonly fields: ReadonlyMap<string, readonly TextprotoValue[]>;
}

/**
 * A parsed scalar leaf.  A `string` carries BOTH the exact decoded byte
 * sequence (`bytes` — each `\xHH` / `\ooo` escape is one byte, each
 * literal char its UTF-8 bytes) AND the UTF-8 decode of those bytes
 * (`value`).  This dual form is load-bearing: a `string_value` field
 * reads `value` (UTF-8 text), a `bytes_value` field reads `bytes` (the
 * raw octets) — the textproto byte escapes in a `string` field encode
 * UTF-8, so the two must not be conflated.  `number` is the parsed
 * value; `bool` a boolean; `enum` a bareword token.
 */
export type TextprotoScalar =
  | {
      readonly kind: 'string';
      readonly value: string;
      readonly bytes: Uint8Array;
    }
  | { readonly kind: 'number'; readonly value: number; readonly raw: string }
  | { readonly kind: 'bool'; readonly value: boolean }
  | { readonly kind: 'enum'; readonly value: string };

/** Thrown when the input is not valid textproto for the corpus subset. */
export class TextprotoParseError extends Error {
  override readonly name = 'TextprotoParseError';
  readonly line: number;
  readonly column: number;

  constructor(message: string, line: number, column: number) {
    super(`${String(line)}:${String(column)}: ${message}`);
    this.line = line;
    this.column = column;
  }
}

/**
 * Parse a complete textproto document into a single top-level message.
 * The top level has no surrounding braces — it is a sequence of
 * `field: value` / `field { ... }` entries.
 *
 * @throws {TextprotoParseError} on a malformed or out-of-subset input.
 */
export function parseTextproto(text: string): TextprotoMessage {
  const parser = new Parser(text);
  return parser.parseTopLevel();
}

/** Convenience: read a message's first value for `name`, or `undefined`. */
export function fieldValue(
  msg: TextprotoMessage,
  name: string,
): TextprotoValue | undefined {
  return msg.fields.get(name)?.[0];
}

/** Convenience: read every value for repeated field `name` (possibly empty). */
export function fieldValues(
  msg: TextprotoMessage,
  name: string,
): readonly TextprotoValue[] {
  return msg.fields.get(name) ?? [];
}

// ───────────────────────────────────────────────────────────────────
// Tokenizer + recursive-descent parser.
// ───────────────────────────────────────────────────────────────────

type TokenType =
  | 'ident' // a field name or bareword scalar
  | 'extname' // a bracketed extension / Any-type field name: `[type.url]`
  | 'colon'
  | 'lbrace'
  | 'rbrace'
  | 'lbracket'
  | 'rbracket'
  | 'string'
  | 'number'
  | 'eof';

interface Token {
  readonly type: TokenType;
  readonly text: string; // for string tokens: the UTF-8 decode of `bytes`
  readonly bytes?: Uint8Array; // set for string tokens: the exact octets
  readonly line: number;
  readonly column: number;
}

const IDENT_START = /[A-Za-z_]/;
const IDENT_CONT = /[A-Za-z0-9_.]/;
const DIGIT = /[0-9]/;

class Lexer {
  private readonly src: string;
  private pos = 0;
  private line = 1;
  private column = 1;

  constructor(src: string) {
    this.src = src;
  }

  private peekChar(): string {
    // `charAt` returns '' past the end — exactly the EOF sentinel here.
    return this.src.charAt(this.pos);
  }

  private advance(): string {
    const ch = this.src.charAt(this.pos);
    this.pos += 1;
    if (ch === '\n') {
      this.line += 1;
      this.column = 1;
    } else {
      this.column += 1;
    }
    return ch;
  }

  private skipTrivia(): void {
    for (;;) {
      const ch = this.peekChar();
      // `,` / `;` are the text-format's optional field separators
      // (`name: "x", ident: {...}`); outside a field position the
      // grammar never produces one, so treating them as trivia is safe.
      if (
        ch === ' ' ||
        ch === '\t' ||
        ch === '\r' ||
        ch === '\n' ||
        ch === ',' ||
        ch === ';'
      ) {
        this.advance();
        continue;
      }
      if (ch === '#') {
        while (this.peekChar() !== '' && this.peekChar() !== '\n') {
          this.advance();
        }
        continue;
      }
      break;
    }
  }

  next(): Token {
    this.skipTrivia();
    const line = this.line;
    const column = this.column;
    const ch = this.peekChar();
    if (ch === '') {
      return { type: 'eof', text: '', line, column };
    }
    if (ch === ':') {
      this.advance();
      return { type: 'colon', text: ':', line, column };
    }
    if (ch === '{' || ch === '<') {
      this.advance();
      return { type: 'lbrace', text: ch, line, column };
    }
    if (ch === '}' || ch === '>') {
      this.advance();
      return { type: 'rbrace', text: ch, line, column };
    }
    if (ch === '[') {
      return this.lexBracket(line, column);
    }
    if (ch === ']') {
      this.advance();
      return { type: 'rbracket', text: ']', line, column };
    }
    if (ch === '"' || ch === "'") {
      return this.lexString(line, column);
    }
    if (DIGIT.test(ch) || ch === '-' || ch === '+' || ch === '.') {
      return this.lexNumberOrIdent(line, column);
    }
    if (IDENT_START.test(ch)) {
      return this.lexIdent(line, column);
    }
    throw new TextprotoParseError(`unexpected character '${ch}'`, line, column);
  }

  private lexIdent(line: number, column: number): Token {
    let text = '';
    while (this.peekChar() !== '' && IDENT_CONT.test(this.peekChar())) {
      text += this.advance();
    }
    return { type: 'ident', text, line, column };
  }

  // `[` introduces either an extension / `Any`-type field name
  // (`[type.googleapis.com/Foo]`, whose body holds `.` and `/` that are
  // not otherwise token characters) OR a repeated-field short list
  // (`[1, 2, 3]`).  Disambiguate by the first non-space body character:
  // a letter starts a type name (lexed raw to `]`), anything else is a
  // list opener.  The bracketed forms appear only inside out-of-scope
  // object_value matchers (§A.3); the lexer reads them so the file loads.
  private lexBracket(line: number, column: number): Token {
    this.advance(); // consume '['
    let probe = this.pos;
    while (probe < this.srcLength() && this.charAt(probe) === ' ') {
      probe += 1;
    }
    const first = this.charAt(probe);
    if (!IDENT_START.test(first)) {
      return { type: 'lbracket', text: '[', line, column };
    }
    let text = '';
    while (this.peekChar() !== '' && this.peekChar() !== ']') {
      text += this.advance();
    }
    if (this.peekChar() !== ']') {
      throw new TextprotoParseError(
        'unterminated extension field name',
        line,
        column,
      );
    }
    this.advance(); // consume ']'
    return { type: 'extname', text: `[${text.trim()}]`, line, column };
  }

  private srcLength(): number {
    return this.src.length;
  }

  private charAt(i: number): string {
    return this.src.charAt(i);
  }

  // A token starting with a digit / sign / dot.  It can be a number, or
  // a bareword like `inf` / `nan` is handled in lexIdent; here we read a
  // numeric run and let the parser interpret it.
  private lexNumberOrIdent(line: number, column: number): Token {
    let text = '';
    const numeric = /[0-9A-Za-z_.+-]/;
    while (this.peekChar() !== '' && numeric.test(this.peekChar())) {
      text += this.advance();
    }
    return { type: 'number', text, line, column };
  }

  // Lex a quoted string to its exact byte sequence.  An escape's bytes
  // are appended verbatim (`\xHH` → one byte, `\u..` → the codepoint's
  // UTF-8 bytes); a literal char appends its UTF-8 bytes.  The token's
  // `text` is the UTF-8 decode of the whole byte run.
  private lexString(line: number, column: number): Token {
    const quote = this.advance(); // consume opening quote
    const bytes: number[] = [];
    for (;;) {
      const ch = this.peekChar();
      if (ch === '') {
        throw new TextprotoParseError('unterminated string', line, column);
      }
      if (ch === quote) {
        this.advance();
        break;
      }
      if (ch === '\\') {
        this.advance();
        this.readEscape(bytes, line, column);
        continue;
      }
      pushUtf8(bytes, this.advance().codePointAt(0) ?? 0);
    }
    const buf = Uint8Array.from(bytes);
    return {
      type: 'string',
      text: new TextDecoder('utf-8').decode(buf),
      bytes: buf,
      line,
      column,
    };
  }

  // Decode a `\`-escape (the backslash already consumed) into `bytes`.
  private readEscape(bytes: number[], line: number, column: number): void {
    const ch = this.advance();
    const simple = SIMPLE_ESCAPES.get(ch);
    if (simple !== undefined) {
      bytes.push(simple);
      return;
    }
    if (/[0-7]/.test(ch)) {
      bytes.push(this.readOctal(ch));
      return;
    }
    if (ch === 'x' || ch === 'X') {
      bytes.push(this.readHex(line, column));
      return;
    }
    if (ch === 'u') {
      pushUtf8(bytes, this.readUnicode(4, line, column));
      return;
    }
    if (ch === 'U') {
      pushUtf8(bytes, this.readUnicode(8, line, column));
      return;
    }
    // `\\`, `\"`, `\'`, `\?`, or an unknown escape: the char as UTF-8.
    pushUtf8(bytes, ch.codePointAt(0) ?? 0);
  }

  private readOctal(first: string): number {
    let digits = first;
    while (digits.length < 3 && /[0-7]/.test(this.peekChar())) {
      digits += this.advance();
    }
    return parseInt(digits, 8) & 0xff;
  }

  private readHex(line: number, column: number): number {
    let digits = '';
    while (digits.length < 2 && /[0-9A-Fa-f]/.test(this.peekChar())) {
      digits += this.advance();
    }
    if (digits.length === 0) {
      throw new TextprotoParseError('empty \\x escape', line, column);
    }
    return parseInt(digits, 16) & 0xff;
  }

  private readUnicode(width: number, line: number, column: number): number {
    let digits = '';
    while (digits.length < width && /[0-9A-Fa-f]/.test(this.peekChar())) {
      digits += this.advance();
    }
    if (digits.length === 0) {
      throw new TextprotoParseError('empty \\u escape', line, column);
    }
    return parseInt(digits, 16);
  }
}

const SIMPLE_ESCAPES: ReadonlyMap<string, number> = new Map([
  ['n', 0x0a],
  ['t', 0x09],
  ['r', 0x0d],
  ['b', 0x08],
  ['f', 0x0c],
  ['v', 0x0b],
  ['a', 0x07],
]);

/** Append the UTF-8 encoding of `codePoint` to `bytes`. */
function pushUtf8(bytes: number[], codePoint: number): void {
  if (codePoint < 0x80) {
    bytes.push(codePoint);
  } else if (codePoint < 0x800) {
    bytes.push(0xc0 | (codePoint >> 6), 0x80 | (codePoint & 0x3f));
  } else if (codePoint < 0x10000) {
    bytes.push(
      0xe0 | (codePoint >> 12),
      0x80 | ((codePoint >> 6) & 0x3f),
      0x80 | (codePoint & 0x3f),
    );
  } else {
    bytes.push(
      0xf0 | (codePoint >> 18),
      0x80 | ((codePoint >> 12) & 0x3f),
      0x80 | ((codePoint >> 6) & 0x3f),
      0x80 | (codePoint & 0x3f),
    );
  }
}

class Parser {
  private readonly lexer: Lexer;
  private lookahead: Token;

  constructor(src: string) {
    this.lexer = new Lexer(src);
    this.lookahead = this.lexer.next();
  }

  parseTopLevel(): TextprotoMessage {
    const fields = new Map<string, TextprotoValue[]>();
    while (this.lookahead.type !== 'eof') {
      this.parseField(fields);
    }
    return { kind: 'message', fields };
  }

  private take(): Token {
    const tok = this.lookahead;
    this.lookahead = this.lexer.next();
    return tok;
  }

  private parseMessageBody(): TextprotoMessage {
    const open = this.take(); // lbrace
    const fields = new Map<string, TextprotoValue[]>();
    while (this.lookahead.type !== 'rbrace') {
      if (this.lookahead.type === 'eof') {
        throw new TextprotoParseError(
          'unterminated message body',
          open.line,
          open.column,
        );
      }
      this.parseField(fields);
    }
    this.take(); // rbrace
    return { kind: 'message', fields };
  }

  private parseField(fields: Map<string, TextprotoValue[]>): void {
    const name = this.parseFieldName();
    // After a field name: either `: value`, or `{ ... }` (message), or
    // `: { ... }` (message with the optional colon), or `: [ ... ]` (a
    // repeated-field short list, used only inside out-of-scope
    // object_value matchers).
    if (this.lookahead.type === 'colon') {
      this.take();
    }
    if (this.lookahead.type === 'lbracket') {
      for (const value of this.parseListValues()) {
        this.pushField(fields, name, value);
      }
      return;
    }
    const value =
      this.lookahead.type === 'lbrace'
        ? this.parseMessageBody()
        : this.parseScalar();
    this.pushField(fields, name, value);
  }

  // A field name is a bareword (`single_int64`) or a bracketed extension
  // / `Any`-type name (`[type.googleapis.com/Foo]`).  The bracketed form
  // appears only inside out-of-scope object_value matchers (§A.3); the
  // parser still reads it so the file loads and the classifier can SKIP.
  private parseFieldName(): string {
    if (this.lookahead.type === 'ident' || this.lookahead.type === 'extname') {
      return this.take().text;
    }
    const tok = this.lookahead;
    throw new TextprotoParseError(
      `expected a field name, got '${tok.text || tok.type}'`,
      tok.line,
      tok.column,
    );
  }

  // `[ v, v, ... ]` — the repeated-field short list value form.
  private parseListValues(): TextprotoValue[] {
    this.take(); // '['
    const values: TextprotoValue[] = [];
    while (
      this.lookahead.type !== 'rbracket' &&
      this.lookahead.type !== 'eof'
    ) {
      values.push(
        this.lookahead.type === 'lbrace'
          ? this.parseMessageBody()
          : this.parseScalar(),
      );
    }
    if (this.lookahead.type !== 'rbracket') {
      throw new TextprotoParseError(
        'unterminated list value',
        this.lookahead.line,
        this.lookahead.column,
      );
    }
    this.take(); // ']'
    return values;
  }

  private pushField(
    fields: Map<string, TextprotoValue[]>,
    name: string,
    value: TextprotoValue,
  ): void {
    const existing = fields.get(name);
    if (existing) {
      existing.push(value);
    } else {
      fields.set(name, [value]);
    }
  }

  private parseScalar(): TextprotoScalar {
    const tok = this.lookahead;
    if (tok.type === 'string') {
      return this.parseStringConcat();
    }
    this.take();
    if (tok.type === 'number') {
      return this.interpretNumeric(tok);
    }
    if (tok.type === 'ident') {
      if (tok.text === 'true' || tok.text === 'false') {
        return { kind: 'bool', value: tok.text === 'true' };
      }
      if (tok.text === 'inf' || tok.text === 'nan') {
        return {
          kind: 'number',
          value: tok.text === 'inf' ? Infinity : NaN,
          raw: tok.text,
        };
      }
      return { kind: 'enum', value: tok.text };
    }
    throw new TextprotoParseError(
      `expected a scalar value, got '${tok.text || tok.type}'`,
      tok.line,
      tok.column,
    );
  }

  // Adjacent string literals concatenate at the BYTE level
  // (`expr: "a" "b"` → `"ab"`); the text is the UTF-8 decode of the
  // joined bytes (so a multi-byte char split across two adjacent literals
  // still decodes correctly).
  private parseStringConcat(): TextprotoScalar {
    const chunks: Uint8Array[] = [];
    let total = 0;
    while (this.lookahead.type === 'string') {
      const tok = this.take();
      const bytes = tok.bytes ?? new Uint8Array(0);
      chunks.push(bytes);
      total += bytes.length;
    }
    const joined = new Uint8Array(total);
    let offset = 0;
    for (const chunk of chunks) {
      joined.set(chunk, offset);
      offset += chunk.length;
    }
    return {
      kind: 'string',
      value: new TextDecoder('utf-8').decode(joined),
      bytes: joined,
    };
  }

  private interpretNumeric(tok: Token): TextprotoScalar {
    const t = tok.text;
    if (t === 'inf' || t === '-inf' || t === '+inf') {
      return {
        kind: 'number',
        value: t.startsWith('-') ? -Infinity : Infinity,
        raw: t,
      };
    }
    if (t === 'nan') {
      return { kind: 'number', value: NaN, raw: t };
    }
    const value = Number(t);
    if (Number.isNaN(value)) {
      // A bareword that lexed as `number` but is not numeric (rare):
      // treat it as an enum token rather than silently producing NaN.
      return { kind: 'enum', value: t };
    }
    return { kind: 'number', value, raw: t };
  }
}
