// Public surface of the cel-wasm conformance harness.
//
// Loads the upstream `*.textproto` corpus, compiles + evaluates each row
// through the TS bindings, and compares the decoded CelValue to the
// expected value/error, with a monotonic baseline ratchet mirroring the
// C++ gate (§A.7).
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.7;
//       scripts/check_conformance_monotonic.sh; conformance/runner.cc.

/** Per-row outcome classification (§A.7). */
export type { Outcome as ConformanceOutcome } from './runner.js';

export { runRow, type RowResult } from './runner.js';
export {
  loadCorpus,
  runCorpus,
  summaryLine,
  skipBreakdown,
  type ConformanceReport,
  type RunCorpusOptions,
  type TaggedResult,
} from './harness.js';
export {
  checkBaseline,
  updateBaseline,
  readCount,
  type BaselineCheck,
  type BaselinePaths,
} from './baseline.js';
export {
  loadSimpleTestFile,
  type SimpleTest,
  type ExpectedValue,
  type ResultMatcher,
} from './corpus.js';
export {
  classifyScope,
  type SkipCategory,
  type ScopeDecision,
} from './classify.js';
export {
  parseTextproto,
  TextprotoParseError,
  type TextprotoMessage,
} from './textproto.js';
