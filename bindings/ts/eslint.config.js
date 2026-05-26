// Flat ESLint config for the ts/ subtree.  Google TypeScript Style Guide
// is canonical (ts/CLAUDE.md); typescript-eslint type-checked is the
// floor.  Prettier owns formatting (eslint-config-prettier disables any
// stylistic rules that would fight it — must be last).
//
// Throwaway probes (ts/eval/probes/**) and the .mjs bridge scripts are
// NOT linted here — they are disposable and pre-date the typed library.
// Real library + runner code (.ts under src/test) is linted strictly.

import js from '@eslint/js';
import tseslint from 'typescript-eslint';
import prettier from 'eslint-config-prettier';

export default tseslint.config(
  {
    ignores: [
      '**/node_modules/**',
      '**/dist/**',
      '**/coverage/**',
      '**/gen/**', // protobuf-es generated code
      '**/probes/**',
      '**/*.mjs',
      'eslint.config.js',
      'vitest.config.ts',
      'vitest.integration.config.ts',
      'vitest.conformance.config.ts',
    ],
  },
  js.configs.recommended,
  // Production floor: the *strict* type-checked preset (not merely
  // `recommended`) — it turns on the type-aware correctness rules that
  // catch real bugs (floating promises, unsafe any flow, misused
  // promises, non-exhaustive unions). Plus the stylistic-type-checked
  // consistency rules.
  ...tseslint.configs.strictTypeChecked,
  ...tseslint.configs.stylisticTypeChecked,
  {
    languageOptions: {
      parserOptions: {
        projectService: true,
        tsconfigRootDir: import.meta.dirname,
      },
    },
    rules: {
      // ── Google TS Style Guide ──
      'no-restricted-syntax': [
        'error',
        {
          selector: 'ExportDefaultDeclaration',
          message: 'No default exports (Google TS Style Guide).',
        },
        {
          selector: "TSModuleDeclaration[kind='namespace']",
          message: 'No namespaces; use ES modules.',
        },
      ],
      'eqeqeq': ['error', 'always'],
      'no-var': 'error',
      'prefer-const': 'error',
      '@typescript-eslint/no-unused-vars': [
        'error',
        { argsIgnorePattern: '^_' },
      ],

      // ── Public-surface contracts ──
      '@typescript-eslint/explicit-module-boundary-types': 'error',
      '@typescript-eslint/explicit-function-return-type': [
        'error',
        { allowExpressions: true },
      ],

      // ── Type-safety hardening (production) ──
      '@typescript-eslint/no-explicit-any': 'error',
      '@typescript-eslint/no-unsafe-assignment': 'error',
      '@typescript-eslint/no-unsafe-call': 'error',
      '@typescript-eslint/no-unsafe-member-access': 'error',
      '@typescript-eslint/no-unsafe-return': 'error',
      '@typescript-eslint/no-unsafe-argument': 'error',
      '@typescript-eslint/no-floating-promises': 'error',
      '@typescript-eslint/no-misused-promises': 'error',
      '@typescript-eslint/await-thenable': 'error',
      // Fully checks switches WITHOUT a default (a forgotten enum case is
      // an error), but lets a deliberate `default` catch-all stand — the
      // idiom for `typeof` guards and wire-byte decoders.
      '@typescript-eslint/switch-exhaustiveness-check': [
        'error',
        { considerDefaultExhaustiveForUnions: true },
      ],
      '@typescript-eslint/no-non-null-assertion': 'error',
      // Conflicts with no-non-null-assertion: it wants `x!` to strip
      // null/undefined, which we forbid. `as T` is the sanctioned form.
      '@typescript-eslint/non-nullable-type-assertion-style': 'off',
      '@typescript-eslint/consistent-type-imports': 'error',
      '@typescript-eslint/no-unnecessary-condition': 'error',
      '@typescript-eslint/restrict-template-expressions': [
        'error',
        { allowNumber: true },
      ],
    },
  },
  // Tests may use non-null assertions + a looser unbound-method stance.
  {
    files: ['**/test/**/*.ts'],
    rules: {
      '@typescript-eslint/no-non-null-assertion': 'off',
    },
  },
  prettier,
);
