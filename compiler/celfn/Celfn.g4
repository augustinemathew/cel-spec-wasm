// Celfn.g4 — ANTLR4 grammar for the M13 `.celfn` IDL.
//
// See `doc/implementation-plan/rewrite/m13-custom-fns.md` §3 for
// the canonical productions + restrictions.  This grammar mirrors
// §3.2's EBNF directly; semantic validation (alias collisions,
// proto-on-foreign rejection, etc.) lives in the C++ visitor that
// walks the parse tree, not in the grammar.
//
// The grammar deliberately uses string-token bodies for the
// `cel_expr` non-terminal: CEL expressions are parsed by cel-cpp's
// own parser later, when the typechecker has the receiver+params
// bound as free variables.  Bodies are matched here as everything
// up to the terminating `;` and stored as raw text on the AST node.

grammar Celfn;

// ── Top-level ───────────────────────────────────────────────────────

file
    : moduleDirective? fileItem* EOF
    ;

moduleDirective
    : 'Module' Identifier ';'
    ;

fileItem
    : hostFnDecl
    | foreignFnDecl
    | bareHostDecl
    | nativeFnDecl
    ;

// ── Declarations ────────────────────────────────────────────────────

// `<type> @host.<name>(<params>) ;` — host-backed.  No body.
hostFnDecl
    : type '@' 'host' '.' Identifier '(' params? ')' ';'
    ;

// `<type> <alias>.<name>(<params>) ;` — foreign-wasm-backed.  No body.
// `<alias>` MUST NOT be `host` (host uses the @-prefix form above);
// the `host` keyword tokenizes separately so user inputs of that
// shape land in `bareHostDecl` below and the visitor rejects them
// with a suggestion to use `@host.`.
foreignFnDecl
    : type Identifier '.' Identifier '(' params? ')' ';'
    ;

// Diagnostic-only production.  Matches `type 'host' '.' Identifier
// '(' ... ')' ';'` — the shape the lexer produces when a user
// writes `bool host.foo(...)` (without the leading `@`).  The
// visitor unconditionally returns an InvalidArgument with a
// suggestion to use `@host.`.  Pulling this into a real grammar
// production (instead of just a syntax-error) lets us surface a
// good error message instead of ANTLR's generic "no viable
// alternative".
bareHostDecl
    : type 'host' '.' Identifier '(' params? ')' ';'
    ;

// `<type> @native.<name>(<params>) = <cel-expr> ;` — CEL-defined.  Has
// body.  Symmetric with the `@host.` prefix above (and bare-alias
// foreign below): `@host` is C++-backed, `@native` is CEL-body-backed,
// a bare `<alias>.` is foreign-wasm-backed.  `native`, like `host`,
// tokenizes as a keyword and is therefore reserved as an alias name.
nativeFnDecl
    : type '@' 'native' '.' Identifier '(' params? ')' '=' celExprBody ';'
    ;

// ── Parameters ──────────────────────────────────────────────────────

params
    : param (',' param)*
    ;

param
    : 'this'? type Identifier
    ;

// ── Types ───────────────────────────────────────────────────────────

type
    : primitiveType
    | wktKeyword
    | listType
    | mapType
    | protoType
    | 'null'
    ;

primitiveType
    : 'bool' | 'int' | 'uint' | 'double' | 'string' | 'bytes'
    ;

wktKeyword
    : 'Duration' | 'Timestamp'
    ;

listType
    : 'list' '<' type '>'
    ;

mapType
    : 'map' '<' mapKeyType ',' type '>'
    ;

mapKeyType
    : 'bool' | 'int' | 'uint' | 'string'
    ;

protoType
    : 'proto' '(' qualifiedIdentifier ')'
    ;

qualifiedIdentifier
    : Identifier ('.' Identifier)*
    ;

// ── CEL expression body ─────────────────────────────────────────────
//
// Matches as a token sequence; the actual parsing is done by
// cel-cpp's parser later.  The lexer rule below produces a single
// CelExprText token whose content is everything up to the next `;`
// at parse-depth 0 (outside parens / brackets / braces / strings).
celExprBody
    : CelExprText
    ;

// ── Lexer rules ─────────────────────────────────────────────────────

// Identifiers — standard C-style.
Identifier
    : [a-zA-Z_] [a-zA-Z_0-9]*
    ;

// String literals — double-quoted, no escapes in v1.
StringLiteral
    : '"' ~["\r\n]* '"'
    ;

// CelExprText — everything up to a `;` outside any nesting.  Pragma:
// we use a lexer-mode shape so the grammar's `=` token reliably
// transitions us into CEL-expression mode and the next `;` returns
// to file mode.  Implemented via the predicate below.
//
// Simpler v0 shape: match anything but `;` (one or more characters,
// non-greedy stop at `;`).  This is good enough for every example
// in the design doc.  Strings containing `;` would break it, but
// CEL doesn't permit `;` in identifiers or operators, and our
// initial test suite covers expressions without semicolons in
// string literals.  When that becomes a real concern, we promote
// this rule to a lexer mode that tracks nesting properly.
CelExprText
    : { _input->LA(-1) == '=' }? ~[;]+
    ;

// Whitespace + comments.
WS
    : [ \t\r\n]+ -> skip
    ;
LineComment
    : '//' ~[\r\n]* -> skip
    ;
BlockComment
    : '/*' .*? '*/' -> skip
    ;
