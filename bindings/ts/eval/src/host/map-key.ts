/**
 * Map-key normalization shared by the object + proto map backings. CEL
 * map-key equality is cross-type for numbers (int/uint/integral compare
 * equal), structural for string/bool — so keys reduce to a comparison
 * `tag` (`i:` for any integer, `s:` string, `b:` bool).
 */
import { CelKind, type CelValue } from '../celvalue.js';
import { HostBackingError } from './backing.js';

/** Comparison tag for a CEL map key. `int`/`uint` share the `i:` tag
 *  (cross-type numeric equality). Throws on a non-key kind. */
export function celKeyTag(key: CelValue): string {
  switch (key.kind) {
    case CelKind.String:
      return `s:${key.value}`;
    case CelKind.Int:
      return `i:${key.int}`;
    case CelKind.Uint:
      return `i:${key.uint}`;
    case CelKind.Bool:
      return `b:${key.bool ? 1 : 0}`;
    default:
      throw new HostBackingError(`invalid map key kind ${key.kind}`);
  }
}
