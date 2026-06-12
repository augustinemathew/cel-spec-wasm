// Derive the `cel_fn.*` import name (the overload id) from a `.celfn`
// host-function declaration.
//
// A compiled Program calls a `@host` function through a wasm import named
// `cel_fn.<overload_id>`, where the compiler synthesizes the overload id
// from the declared function name plus each parameter's "argkind"
// spelling (`SynthesiseOverloadId`, compiler/celfn/function_library.cc:180;
// argkinds per `CelfnType::Argkind`, function_library.cc:30).  E.g.
// `int @host.addOne(int x);` imports as `cel_fn.addOne_int`, and
// `int @host.boom();` (no params) as `cel_fn.boom`.
//
// `Engine.defineFunction` accepts the SAME `.celfn` declaration string the
// caller passes to the compiler's `fns` option, so this module
// re-implements only the id synthesis — the grammar subset for `@host`
// decls (compiler/celfn/Celfn.g4 `hostFnDecl`) — never the full IDL
// (bodies, `@native` / `@component` backends stay compiler-side and are
// out of binding scope by design).

/** A parsed `@host` declaration — the call-site name + the import name. */
export interface HostFnDecl {
  /** The call-site function name (`addOne` in `int @host.addOne(int x);`). */
  readonly name: string;
  /** The `cel_fn.*` wasm import name (`addOne_int`). */
  readonly overloadId: string;
  /**
   * The declared return type is `uint`.  A JS `bigint` return value
   * otherwise encodes as CEL_INT (the int/uint distinction does not exist
   * in JS), so the `cel_fn` trampoline re-stamps the slot kind to
   * CEL_UINT for a uint-declared return — mirroring the C++
   * `HostCallContext::ReturnUint`.  False for the bare-overload-id form
   * (no declaration to read the return type from).
   */
  readonly returnsUint: boolean;
}

/** Thrown by {@link parseHostFnDecl} on a malformed declaration. */
export class CelFnDeclError extends Error {
  override readonly name = 'CelFnDeclError';
}

const IDENTIFIER_RE = /^[A-Za-z_][A-Za-z0-9_]*$/;

/**
 * Resolve `decl` to the overload id a Program imports it under: a full
 * `.celfn` `@host` declaration is parsed and its id synthesized; a bare
 * identifier is taken verbatim as an already-synthesized overload id
 * (mirroring the C++ `Engine::AddFunction(overload_id, …)` form).
 */
export function hostFnDecl(decl: string): HostFnDecl {
  const trimmed = decl.trim();
  if (IDENTIFIER_RE.test(trimmed)) {
    return { name: trimmed, overloadId: trimmed, returnsUint: false };
  }
  return parseHostFnDecl(decl);
}

/**
 * Parse a `.celfn` `@host` declaration (`<type> @host.<name>(<params>);`,
 * Celfn.g4 `hostFnDecl`) and synthesize its overload id.  Throws
 * {@link CelFnDeclError} on a malformed declaration or a non-`@host`
 * backend (`@component` / `@native` are out of binding scope by design).
 */
export function parseHostFnDecl(decl: string): HostFnDecl {
  const scanner = new Scanner(decl);
  // The return type does not participate in the overload id, but parsing
  // it validates the declaration shape (and rejects e.g. a TS-style
  // `"my_fn(int): int"` string that is not a `.celfn` decl).  Its one
  // semantic contribution is the uint-return flag (see HostFnDecl).
  const returnArgkind = parseArgkind(scanner);
  scanner.expect('@');
  const backend = scanner.identifier('backend');
  if (backend !== 'host') {
    throw new CelFnDeclError(
      `defineFunction: '@${backend}' functions are not registrable host ` +
        `functions (only '@host'; '@component' / '@native' dispatch ` +
        `compiler-side): '${decl}'`,
    );
  }
  scanner.expect('.');
  const name = scanner.identifier('function name');
  scanner.expect('(');
  const argkinds: string[] = [];
  if (!scanner.tryExpect(')')) {
    do {
      argkinds.push(parseParamArgkind(scanner));
    } while (scanner.tryExpect(','));
    scanner.expect(')');
  }
  scanner.tryExpect(';');
  scanner.expectEnd();
  return {
    name,
    overloadId: [name, ...argkinds].join('_'),
    returnsUint: returnArgkind === 'uint',
  };
}

/** One parameter: `'this'? type Identifier` (Celfn.g4 `param`). */
function parseParamArgkind(scanner: Scanner): string {
  scanner.tryKeyword('this');
  const argkind = parseArgkind(scanner);
  scanner.identifier('parameter name');
  return argkind;
}

/**
 * Parse one `.celfn` type and return its argkind spelling — the exact
 * strings `CelfnType::Argkind` emits (function_library.cc:30-69):
 * scalars verbatim, `Duration`/`Timestamp` lowercased, `list<T>` →
 * `list_<T>`, `map<K, V>` → `map_<K>_<V>`, `proto(a.b.C)` → `message_a_b_C`.
 */
function parseArgkind(scanner: Scanner): string {
  const head = scanner.identifier('type');
  switch (head) {
    case 'bool':
    case 'int':
    case 'uint':
    case 'double':
    case 'string':
    case 'bytes':
    case 'null':
      return head;
    case 'Duration':
      return 'duration';
    case 'Timestamp':
      return 'timestamp';
    case 'list': {
      scanner.expect('<');
      const elem = parseArgkind(scanner);
      scanner.expect('>');
      return `list_${elem}`;
    }
    case 'map': {
      scanner.expect('<');
      const key = parseMapKeyArgkind(scanner);
      scanner.expect(',');
      const value = parseArgkind(scanner);
      scanner.expect('>');
      return `map_${key}_${value}`;
    }
    case 'proto': {
      scanner.expect('(');
      const fqn = scanner.qualifiedIdentifier();
      scanner.expect(')');
      return `message_${fqn.replaceAll('.', '_')}`;
    }
    default:
      throw new CelFnDeclError(
        `defineFunction: '${head}' is not a .celfn type (expected one of ` +
          `bool/int/uint/double/string/bytes/null/Duration/Timestamp/` +
          `list<T>/map<K, V>/proto(<fqn>))`,
      );
  }
}

/** Map keys are restricted to bool/int/uint/string (Celfn.g4 `mapKeyType`). */
function parseMapKeyArgkind(scanner: Scanner): string {
  const key = scanner.identifier('map key type');
  if (key !== 'bool' && key !== 'int' && key !== 'uint' && key !== 'string') {
    throw new CelFnDeclError(
      `defineFunction: '${key}' is not a valid map key type (expected ` +
        `bool/int/uint/string)`,
    );
  }
  return key;
}

/** A cursor over the declaration string, skipping whitespace per token. */
class Scanner {
  private pos = 0;

  constructor(private readonly src: string) {}

  private skipWs(): void {
    while (this.pos < this.src.length && /\s/.test(this.src[this.pos] ?? '')) {
      this.pos += 1;
    }
  }

  /** Consume `token` or throw. */
  expect(token: string): void {
    if (!this.tryExpect(token)) {
      this.fail(`expected '${token}'`);
    }
  }

  /** Consume `token` if present; return whether it was. */
  tryExpect(token: string): boolean {
    this.skipWs();
    if (this.src.startsWith(token, this.pos)) {
      this.pos += token.length;
      return true;
    }
    return false;
  }

  /** Consume the keyword `word` if present as a whole identifier. */
  tryKeyword(word: string): boolean {
    this.skipWs();
    const end = this.pos + word.length;
    if (
      this.src.startsWith(word, this.pos) &&
      !/[A-Za-z0-9_]/.test(this.src[end] ?? '')
    ) {
      this.pos = end;
      return true;
    }
    return false;
  }

  /** Consume one identifier or throw naming `what`. */
  identifier(what: string): string {
    this.skipWs();
    const match = /^[A-Za-z_][A-Za-z0-9_]*/.exec(this.src.slice(this.pos));
    if (match === null) {
      this.fail(`expected ${what}`);
    }
    this.pos += match[0].length;
    return match[0];
  }

  /** Consume a dotted identifier (`a.b.C`, Celfn.g4 `qualifiedIdentifier`). */
  qualifiedIdentifier(): string {
    const parts = [this.identifier('qualified identifier')];
    while (this.tryExpect('.')) {
      parts.push(this.identifier('qualified identifier segment'));
    }
    return parts.join('.');
  }

  /** Throw unless the input is exhausted. */
  expectEnd(): void {
    this.skipWs();
    if (this.pos < this.src.length) {
      this.fail('unexpected trailing input');
    }
  }

  private fail(detail: string): never {
    throw new CelFnDeclError(
      `defineFunction: malformed .celfn host declaration (${detail} at ` +
        `offset ${String(this.pos)}): '${this.src}'`,
    );
  }
}
