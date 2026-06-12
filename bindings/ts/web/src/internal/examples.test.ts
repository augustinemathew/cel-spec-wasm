import { describe, expect, it } from 'vitest';

import { EXAMPLES, defaultExample } from './examples.js';

describe('EXAMPLES', () => {
  it('is non-empty', () => {
    expect(EXAMPLES.length).toBeGreaterThan(0);
  });

  it('every example has a label, source, and note', () => {
    for (const example of EXAMPLES) {
      expect(example.label.length).toBeGreaterThan(0);
      expect(example.source.length).toBeGreaterThan(0);
      expect(example.note.length).toBeGreaterThan(0);
    }
  });

  it('covers the four demonstrated slices', () => {
    const sources = EXAMPLES.map((e) => e.source);
    expect(sources).toContain('age >= 18 && country in ["US", "CA"]');
    expect(sources).toContain('[1, 2, 3].map(x, x * 2)');
    expect(sources).toContain('"hello".size()');
    expect(sources).toContain('1 / 0');
  });

  it('the access-check example declares its variables', () => {
    const access = EXAMPLES.find((e) => e.label === 'Access check');
    expect(access?.variables).toContain('age:int=');
    expect(access?.variables).toContain('country:string=');
  });

  it('defaultExample returns the first example', () => {
    expect(defaultExample()).toBe(EXAMPLES[0]);
  });
});
