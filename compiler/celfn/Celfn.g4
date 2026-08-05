// Celfn.g4 — ANTLR4 grammar for the `.celfn` IDL.
//
// See `doc/implementation-plan/rewrite/m13-custom-fns.md` §3 for
// the canonical productions + restrictions.  This grammar mirrors
// §3.2's EBNF directly; semantic validation (alias collisions,
// proto-on-foreign rejection, etc.) lives in the C++ visitor that
// walks the parse tree, not in the grammar.  The only declaration
// backend is `@host.` — a C++ callback the embedder binds at Plan
// time.

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
    | bareHostDecl
    ;

// ── Declarations ────────────────────────────────────────────────────

// `<type> @host.<name>(<params>) ;` — host-backed.  No body.
hostFnDecl
    : type '@' 'host' '.' Identifier '(' params? ')' ';'
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

// ── Lexer rules ─────────────────────────────────────────────────────

// Identifiers — standard C-style.
Identifier
    : [a-zA-Z_] [a-zA-Z_0-9]*
    ;

// String literals — double-quoted, no escapes in v1.
StringLiteral
    : '"' ~["\r\n]* '"'
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
