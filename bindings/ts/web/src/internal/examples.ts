// The seed expressions the demo offers — each pairs a CEL `source` with a
// pre-filled variables form, chosen to exercise a different slice of the
// static subset: boolean logic over bound vars, a comprehension, a string
// builtin, and a spec-error value (divide-by-zero).

/** One seeded example — a label, a CEL expression, and its variables form. */
export interface Example {
  readonly label: string;
  readonly source: string;
  /** The variables form, one `name:type=value` per line (may be empty). */
  readonly variables: string;
  /** A one-line note on what the example demonstrates. */
  readonly note: string;
}

/** The ordered list of examples shown in the demo's example picker. */
export const EXAMPLES: readonly Example[] = [
  {
    label: 'Access check',
    source: 'age >= 18 && country in ["US", "CA"]',
    variables: 'age:int=25\ncountry:string=US',
    note: 'Boolean logic over bound variables — returns true.',
  },
  {
    label: 'List comprehension',
    source: '[1, 2, 3].map(x, x * 2)',
    variables: '',
    note: 'map() over a list literal — returns [2, 4, 6].',
  },
  {
    label: 'String builtin',
    source: '"hello".size()',
    variables: '',
    note: 'A string method — returns 5.',
  },
  {
    label: 'Divide by zero',
    source: '1 / 0',
    variables: '',
    note: 'A CEL spec error surfaces as an error value, not a crash.',
  },
];

/** The example shown when the demo first loads. */
export function defaultExample(): Example {
  const first = EXAMPLES[0];
  if (first === undefined) {
    throw new Error('EXAMPLES must not be empty');
  }
  return first;
}
