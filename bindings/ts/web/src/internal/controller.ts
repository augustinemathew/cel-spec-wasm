// The demo's orchestration: wire the Monaco editor and the
// compile → download → run loop onto an already-built {@link DemoView}.
//
// Compile runs `compiler.wasm` fully client-side via {@link CompileClient}
// (lazy-fetched on first use); run is pure client-side eval via
// `@cel-wasm/eval`.  The whole app is static — no server hop at any step.
// Compile diagnostics are surfaced both inline in Monaco
// (`setModelMarkers`) and in a readable error panel.

import type { Activation, CelValue, Program } from '@cel-wasm/eval';
import * as monaco from 'monaco-editor';

import {
  CelCompileError,
  CompileClient,
  type CompileDiagnostic,
} from './compile-client.js';
import { downloadWasm } from './download.js';
import { EXAMPLES, defaultExample, type Example } from './examples.js';
import {
  CEL_LANGUAGE_ID,
  CEL_THEME_ID,
  registerCelLanguage,
} from './monaco-cel.js';
import { renderResult } from './render.js';
import { runProgram } from './run.js';
import {
  VariableParseError,
  parseVariablesForm,
  type ParsedVariable,
} from './variables.js';
import type { DemoView } from './view.js';

/** The current compiled artifact, or `undefined` before the first compile. */
interface CompiledState {
  readonly program: Program;
  readonly source: string;
}

/** Wire a built {@link DemoView} into a live demo.  Returns nothing. */
export function attachController(view: DemoView): void {
  new DemoController(view).start();
}

class DemoController {
  private readonly view: DemoView;
  private readonly editor: monaco.editor.IStandaloneCodeEditor;
  private readonly model: monaco.editor.ITextModel;
  private readonly compiler: CompileClient;
  private compiled: CompiledState | undefined;

  constructor(view: DemoView) {
    this.view = view;
    this.compiler = new CompileClient({
      onLoadStart: () => {
        this.setStatus('Loading compiler (~6 MB)… first compile only.', 'busy');
      },
    });
    registerCelLanguage();
    this.model = monaco.editor.createModel(
      defaultExample().source,
      CEL_LANGUAGE_ID,
    );
    this.editor = monaco.editor.create(view.editorHost, {
      model: this.model,
      theme: CEL_THEME_ID,
      fontSize: 15,
      lineNumbers: 'on',
      minimap: { enabled: false },
      scrollBeyondLastLine: false,
      automaticLayout: true,
      padding: { top: 12, bottom: 12 },
    });
  }

  start(): void {
    this.populateExamples();
    this.applyExample(defaultExample());
    this.editor.focus();
    this.view.compileButton.addEventListener('click', () => {
      void this.onCompile();
    });
    this.view.downloadButton.addEventListener('click', () => {
      this.onDownload();
    });
    this.view.runButton.addEventListener('click', () => {
      void this.onRun();
    });
    this.view.exampleSelect.addEventListener('change', () => {
      this.onExampleChange();
    });
    this.model.onDidChangeContent(() => {
      this.invalidateCompiled();
    });
  }

  private populateExamples(): void {
    EXAMPLES.forEach((example, index) => {
      const option = document.createElement('option');
      option.value = String(index);
      option.textContent = example.label;
      this.view.exampleSelect.append(option);
    });
  }

  private applyExample(example: Example): void {
    this.model.setValue(example.source);
    this.view.variablesInput.value = example.variables;
    this.view.exampleNote.textContent = example.note;
    this.invalidateCompiled();
    this.clearMarkers();
  }

  private onExampleChange(): void {
    const example = EXAMPLES[Number(this.view.exampleSelect.value)];
    if (example !== undefined) {
      this.applyExample(example);
    }
  }

  private invalidateCompiled(): void {
    this.compiled = undefined;
    this.view.downloadButton.disabled = true;
    this.view.runButton.disabled = true;
    this.hide(this.view.programPanel);
    this.hide(this.view.resultPanel);
  }

  // ── Compile ─────────────────────────────────────────────────────────

  private async onCompile(): Promise<void> {
    const source = this.model.getValue().trim();
    if (source.length === 0) {
      this.setStatus('Enter an expression to compile.', 'idle');
      return;
    }
    let variables: ParsedVariable[];
    try {
      variables = parseVariablesForm(this.view.variablesInput.value);
    } catch (err) {
      this.showVariablesError(err);
      return;
    }
    this.setBusy(
      this.compiler.ready ? 'Compiling…' : 'Loading compiler (~6 MB)…',
    );
    try {
      const program = await this.compiler.compile(
        source,
        variables.map((v) => v.decl),
      );
      this.onCompileSuccess(source, program);
    } catch (err) {
      this.onCompileError(err);
    } finally {
      this.view.compileButton.disabled = false;
    }
  }

  private onCompileSuccess(source: string, program: Program): void {
    this.compiled = { program, source };
    this.clearMarkers();
    this.hide(this.view.errorPanel);
    this.showProgram(program);
    this.view.downloadButton.disabled = false;
    this.view.runButton.disabled = false;
    this.setStatus(
      `Compiled — ${String(program.wasm.byteLength)} bytes of portable wasm.`,
      'ok',
    );
  }

  private onCompileError(err: unknown): void {
    if (err instanceof CelCompileError) {
      this.markDiagnostics(err.diagnostics);
      this.showError('Compile failed', err.diagnostics, err.message, true);
      this.setStatus('Compile failed — see diagnostics.', 'error');
      return;
    }
    const message = err instanceof Error ? err.message : String(err);
    this.showError('Compile failed', [], message);
    this.setStatus('Compile failed.', 'error');
  }

  // ── Download ────────────────────────────────────────────────────────

  private onDownload(): void {
    if (this.compiled === undefined) {
      return;
    }
    downloadWasm(this.compiled.program.wasm, 'program.wasm');
    this.setStatus('Downloaded program.wasm — the portable artifact.', 'ok');
  }

  // ── Run (client-side eval) ──────────────────────────────────────────

  private async onRun(): Promise<void> {
    if (this.compiled === undefined) {
      return;
    }
    let activation: Activation;
    try {
      activation = this.buildActivation();
    } catch (err) {
      this.showVariablesError(err);
      return;
    }
    this.view.runButton.disabled = true;
    this.setStatus('Running in the browser…', 'busy');
    try {
      const result = await runProgram(this.compiled.program, activation);
      this.showResult(result);
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err);
      this.showError('Run failed', [], message);
      this.setStatus('Run failed.', 'error');
    } finally {
      this.view.runButton.disabled = false;
    }
  }

  private buildActivation(): Activation {
    const variables = parseVariablesForm(this.view.variablesInput.value);
    const activation: Activation = {};
    for (const variable of variables) {
      activation[variable.decl.name] = variable.value;
    }
    return activation;
  }

  // ── Monaco markers ──────────────────────────────────────────────────

  private markDiagnostics(diagnostics: readonly CompileDiagnostic[]): void {
    const markers: monaco.editor.IMarkerData[] = diagnostics
      .filter((d) => d.line !== undefined && d.column !== undefined)
      .map((d) => this.toMarker(d));
    monaco.editor.setModelMarkers(this.model, CEL_LANGUAGE_ID, markers);
  }

  private toMarker(d: CompileDiagnostic): monaco.editor.IMarkerData {
    const line = d.line ?? 1;
    const column = d.column ?? 1;
    return {
      severity: monaco.MarkerSeverity.Error,
      message: d.message,
      startLineNumber: line,
      startColumn: column,
      endLineNumber: line,
      endColumn: column + 1,
    };
  }

  private clearMarkers(): void {
    monaco.editor.setModelMarkers(this.model, CEL_LANGUAGE_ID, []);
  }

  // ── Panels ──────────────────────────────────────────────────────────

  private showProgram(program: Program): void {
    const { abi } = program;
    const vars = abi.variables.map((v) => v.name).join(', ') || 'none';
    this.view.programPanel.replaceChildren(
      panelTitle('Compiled Program'),
      keyValue('Size', `${String(program.wasm.byteLength)} bytes`),
      keyValue('ABI version', String(abi.version)),
      keyValue('Variables', vars),
      keyValue('Link mode', Number(abi.linkMode) === 1 ? 'static' : 'dynamic'),
    );
    this.show(this.view.programPanel);
  }

  private showResult(result: CelValue): void {
    const rendered = renderResult(result);
    this.view.resultPanel.replaceChildren(
      panelTitle('Result'),
      keyValue('Type', rendered.typeName),
      resultValue(rendered.text, rendered.className),
    );
    this.show(this.view.resultPanel);
    this.setStatus(
      rendered.className === 'error'
        ? 'Evaluated — the expression produced an error value.'
        : 'Evaluated client-side. No server hop for eval.',
      rendered.className === 'error' ? 'idle' : 'ok',
    );
  }

  private showError(
    heading: string,
    diagnostics: readonly CompileDiagnostic[],
    message: string,
    fromCompiler = false,
  ): void {
    const children: Node[] = [panelTitle(heading)];
    if (diagnostics.length > 0) {
      for (const d of diagnostics) {
        children.push(diagnosticLine(d));
      }
    } else {
      children.push(diagnosticLine({ message }));
    }
    // The client-side wasm compiler cannot recover cel-cpp's line/column
    // diagnostic (no C++ exception runtime in the stock wasi-sdk build);
    // tell the user where precise diagnostics come from.
    const hasLocation = diagnostics.some(
      (d) => d.line !== undefined && d.column !== undefined,
    );
    if (fromCompiler && !hasLocation) {
      children.push(errorNote());
    }
    this.view.errorPanel.replaceChildren(...children);
    this.show(this.view.errorPanel);
    this.hide(this.view.resultPanel);
  }

  private showVariablesError(err: unknown): void {
    const message =
      err instanceof VariableParseError
        ? err.message
        : err instanceof Error
          ? err.message
          : String(err);
    this.showError('Variables error', [], message);
    this.setStatus('Fix the variables form.', 'error');
  }

  // ── Status + visibility helpers ─────────────────────────────────────

  private setBusy(message: string): void {
    this.view.compileButton.disabled = true;
    this.setStatus(message, 'busy');
  }

  private setStatus(
    message: string,
    kind: 'idle' | 'busy' | 'ok' | 'error',
  ): void {
    this.view.status.textContent = message;
    this.view.status.className = `status status-${kind}`;
  }

  private show(panel: HTMLElement): void {
    panel.classList.remove('hidden');
  }

  private hide(panel: HTMLElement): void {
    panel.classList.add('hidden');
  }
}

function panelTitle(text: string): HTMLElement {
  const title = document.createElement('h2');
  title.className = 'panel-title';
  title.textContent = text;
  return title;
}

function keyValue(key: string, value: string): HTMLElement {
  const row = document.createElement('div');
  row.className = 'kv-row';
  const k = document.createElement('span');
  k.className = 'kv-key';
  k.textContent = key;
  const v = document.createElement('span');
  v.className = 'kv-value';
  v.textContent = value;
  row.append(k, v);
  return row;
}

function resultValue(text: string, className: 'value' | 'error'): HTMLElement {
  const pre = document.createElement('pre');
  pre.className = `result-value result-${className}`;
  pre.textContent = text;
  return pre;
}

function errorNote(): HTMLElement {
  const note = document.createElement('p');
  note.className = 'error-note';
  note.textContent =
    'Note: the in-browser wasm compiler reports a generic message — ' +
    'precise line/column diagnostics need the native (or emscripten) backend.';
  return note;
}

function diagnosticLine(d: CompileDiagnostic): HTMLElement {
  const line = document.createElement('div');
  line.className = 'diagnostic';
  const location =
    d.line !== undefined && d.column !== undefined
      ? `line ${String(d.line)}, col ${String(d.column)}: `
      : '';
  line.textContent = `${location}${d.message}`;
  return line;
}
