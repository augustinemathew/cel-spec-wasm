// Flat ESLint config (ESLint 9 default).  Strict-type-checked
// @typescript-eslint + eslint-plugin-import, mirroring the §A.6
// guidelines: no `any`, explicit return types on exports, named
// exports only.  Prettier owns formatting; ESLint owns correctness.
import eslint from '@eslint/js';
import importPlugin from 'eslint-plugin-import';
import tseslint from 'typescript-eslint';

export default tseslint.config(
  {
    ignores: ['**/dist/**', '**/coverage/**', '**/node_modules/**'],
  },
  eslint.configs.recommended,
  ...tseslint.configs.strictTypeChecked,
  ...tseslint.configs.stylisticTypeChecked,
  {
    languageOptions: {
      parserOptions: {
        projectService: {
          // Loose files — the `*.test.ts` excluded from the build
          // tsconfigs and the root config files — are type-checked under
          // this dedicated project so they still get type-aware rules.
          allowDefaultProject: ['*.config.mjs', '*.config.ts'],
          defaultProject: './tsconfig.eslint.json',
        },
        tsconfigRootDir: import.meta.dirname,
      },
    },
    plugins: {
      import: importPlugin,
    },
    settings: {
      'import/resolver': {
        typescript: {
          project: [
            './eval/tsconfig.json',
            './compiler/tsconfig.json',
            './conformance/tsconfig.json',
            './web/tsconfig.json',
          ],
        },
      },
    },
    rules: {
      '@typescript-eslint/no-explicit-any': 'error',
      '@typescript-eslint/no-unused-vars': [
        'error',
        { argsIgnorePattern: '^_', varsIgnorePattern: '^_' },
      ],
      '@typescript-eslint/explicit-module-boundary-types': 'error',
      '@typescript-eslint/explicit-function-return-type': [
        'error',
        { allowExpressions: true },
      ],
      'no-restricted-syntax': [
        'error',
        {
          selector: 'ExportDefaultDeclaration',
          message: 'Use named exports only (Google TS style / §A.6).',
        },
      ],
      'import/no-default-export': 'error',
      'import/order': [
        'error',
        { 'newlines-between': 'always', alphabetize: { order: 'asc' } },
      ],
    },
  },
  {
    // Config and test files may relax a few project-only rules.
    files: ['**/*.test.ts', '*.config.{ts,mts,mjs}', '**/vitest.config.ts'],
    rules: {
      'import/no-default-export': 'off',
      'no-restricted-syntax': 'off',
    },
  },
);
