// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from 'vitest';

import { downloadWasm } from './download.js';

describe('downloadWasm', () => {
  afterEach(() => {
    vi.restoreAllMocks();
  });

  it('creates a blob URL, clicks an anchor, and revokes the URL', () => {
    const created: Blob[] = [];
    const createObjectURL = vi.fn((blob: Blob): string => {
      created.push(blob);
      return 'blob:fake';
    });
    const revokeObjectURL = vi.fn();
    vi.stubGlobal('URL', {
      ...URL,
      createObjectURL,
      revokeObjectURL,
    });
    const clickSpy = vi
      .spyOn(HTMLAnchorElement.prototype, 'click')
      .mockImplementation(() => undefined);

    downloadWasm(new Uint8Array([0, 97, 115, 109]), 'program.wasm');

    expect(createObjectURL).toHaveBeenCalledOnce();
    expect(created[0]?.type).toBe('application/wasm');
    expect(clickSpy).toHaveBeenCalledOnce();
    expect(revokeObjectURL).toHaveBeenCalledWith('blob:fake');
    // The synthetic anchor is removed from the document after the click.
    expect(document.querySelector('a[download]')).toBeNull();
    vi.unstubAllGlobals();
  });

  it('defaults the filename to program.wasm', () => {
    let downloadName = '';
    vi.stubGlobal('URL', {
      ...URL,
      createObjectURL: (): string => 'blob:fake',
      revokeObjectURL: (): void => undefined,
    });
    vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(function (
      this: HTMLAnchorElement,
    ) {
      downloadName = this.download;
    });
    downloadWasm(new Uint8Array([1]));
    expect(downloadName).toBe('program.wasm');
    vi.unstubAllGlobals();
  });
});
