// Build the demo's static DOM and hand back typed element references.
//
// Keeping the markup construction here (out of the orchestration in
// `index.ts`) means the wiring code deals only with already-resolved
// element handles, never `getElementById` lookups that might be null.

/** The element handles the orchestration wires behaviour onto. */
export interface DemoView {
  readonly editorHost: HTMLElement;
  readonly variablesInput: HTMLTextAreaElement;
  readonly compileButton: HTMLButtonElement;
  readonly downloadButton: HTMLButtonElement;
  readonly runButton: HTMLButtonElement;
  readonly exampleSelect: HTMLSelectElement;
  readonly status: HTMLElement;
  readonly errorPanel: HTMLElement;
  readonly programPanel: HTMLElement;
  readonly resultPanel: HTMLElement;
  readonly exampleNote: HTMLElement;
}

function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  className?: string,
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (className !== undefined) {
    node.className = className;
  }
  return node;
}

function header(): HTMLElement {
  const head = el('header', 'app-header');
  const title = el('h1');
  title.textContent = 'cel-wasm';
  const tagline = el('p', 'tagline');
  tagline.textContent =
    'Compile a CEL expression to a portable .wasm Program, download it, ' +
    'and run it right here in your browser — no server at eval time.';
  head.append(title, tagline);
  return head;
}

function buildEditorPane(view: Mutable): HTMLElement {
  const pane = el('section', 'pane editor-pane');
  const label = el('div', 'pane-label');
  label.textContent = 'CEL expression';
  const picker = el('div', 'example-picker');
  const pickerLabel = el('label', 'field-label');
  pickerLabel.textContent = 'Example';
  view.exampleSelect = el('select', 'example-select');
  pickerLabel.htmlFor = 'example-select';
  view.exampleSelect.id = 'example-select';
  picker.append(pickerLabel, view.exampleSelect);
  view.editorHost = el('div', 'editor-host');
  view.exampleNote = el('p', 'example-note');
  pane.append(label, picker, view.editorHost, view.exampleNote);
  return pane;
}

function buildVariablesPane(view: Mutable): HTMLElement {
  const pane = el('section', 'pane variables-pane');
  const label = el('div', 'pane-label');
  label.textContent = 'Variables';
  const hint = el('p', 'pane-hint');
  hint.textContent =
    'One per line, as name:type=value (int, uint, double, bool, string, bytes).';
  view.variablesInput = el('textarea', 'variables-input');
  view.variablesInput.spellcheck = false;
  view.variablesInput.rows = 4;
  pane.append(label, hint, view.variablesInput);
  return pane;
}

function buildActions(view: Mutable): HTMLElement {
  const actions = el('div', 'actions');
  view.compileButton = el('button', 'btn btn-primary');
  view.compileButton.type = 'button';
  view.compileButton.textContent = 'Compile';
  view.downloadButton = el('button', 'btn');
  view.downloadButton.type = 'button';
  view.downloadButton.textContent = 'Download .wasm';
  view.downloadButton.disabled = true;
  view.runButton = el('button', 'btn btn-accent');
  view.runButton.type = 'button';
  view.runButton.textContent = 'Run';
  view.runButton.disabled = true;
  actions.append(view.compileButton, view.downloadButton, view.runButton);
  return actions;
}

function buildOutputPane(view: Mutable): HTMLElement {
  const pane = el('section', 'pane output-pane');
  view.status = el('div', 'status status-idle');
  view.status.textContent = 'Type an expression and press Compile.';
  view.errorPanel = el('div', 'panel error-panel hidden');
  view.programPanel = el('div', 'panel program-panel hidden');
  view.resultPanel = el('div', 'panel result-panel hidden');
  pane.append(
    view.status,
    view.errorPanel,
    view.programPanel,
    view.resultPanel,
  );
  return pane;
}

type Mutable = {
  -readonly [K in keyof DemoView]: DemoView[K];
};

/**
 * Construct the demo into `root` and return the wired element handles.
 * The orchestration in `index.ts` attaches Monaco + event listeners onto
 * these; this function owns only the static structure and styling hooks.
 */
export function buildView(root: HTMLElement): DemoView {
  root.replaceChildren();
  const partial = {} as Mutable;
  const main = el('main', 'app-main');
  const left = el('div', 'column column-left');
  left.append(
    buildEditorPane(partial),
    buildVariablesPane(partial),
    buildActions(partial),
  );
  const right = el('div', 'column column-right');
  right.append(buildOutputPane(partial));
  main.append(left, right);
  root.append(header(), main);
  return partial;
}
