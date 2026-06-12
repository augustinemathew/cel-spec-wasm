// @vitest-environment jsdom
import { describe, expect, it } from 'vitest';

import { buildView } from './view.js';

describe('buildView', () => {
  function mount(): HTMLElement {
    const root = document.createElement('div');
    document.body.append(root);
    return root;
  }

  it('builds every wired element handle', () => {
    const view = buildView(mount());
    expect(view.editorHost).toBeInstanceOf(HTMLElement);
    expect(view.variablesInput).toBeInstanceOf(HTMLTextAreaElement);
    expect(view.compileButton).toBeInstanceOf(HTMLButtonElement);
    expect(view.downloadButton).toBeInstanceOf(HTMLButtonElement);
    expect(view.runButton).toBeInstanceOf(HTMLButtonElement);
    expect(view.exampleSelect).toBeInstanceOf(HTMLSelectElement);
    expect(view.status).toBeInstanceOf(HTMLElement);
    expect(view.errorPanel).toBeInstanceOf(HTMLElement);
    expect(view.programPanel).toBeInstanceOf(HTMLElement);
    expect(view.resultPanel).toBeInstanceOf(HTMLElement);
    expect(view.exampleNote).toBeInstanceOf(HTMLElement);
  });

  it('starts download and run disabled (nothing compiled yet)', () => {
    const view = buildView(mount());
    expect(view.downloadButton.disabled).toBe(true);
    expect(view.runButton.disabled).toBe(true);
    expect(view.compileButton.disabled).toBe(false);
  });

  it('hides the program / result / error panels initially', () => {
    const view = buildView(mount());
    expect(view.programPanel.classList.contains('hidden')).toBe(true);
    expect(view.resultPanel.classList.contains('hidden')).toBe(true);
    expect(view.errorPanel.classList.contains('hidden')).toBe(true);
  });

  it('replaces prior content on a re-build', () => {
    const root = mount();
    root.textContent = 'stale';
    buildView(root);
    expect(root.textContent).not.toBe('stale');
    expect(root.querySelector('.app-header')).not.toBeNull();
  });
});
