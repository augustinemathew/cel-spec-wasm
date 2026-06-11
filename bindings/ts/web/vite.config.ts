import type { IncomingMessage, ServerResponse } from 'node:http';

import { defineConfig, type Plugin, type Connect } from 'vite';

import {
  parseCompileRequest,
  runCompile,
} from './dev-server/compile-handler.js';

const COMPILE_ROUTE = '/api/compile';
const MAX_BODY_BYTES = 256 * 1024;

/**
 * Read a request body up to {@link MAX_BODY_BYTES}, rejecting if it grows
 * past the cap (a hostile / runaway client should not exhaust memory).
 */
async function readBody(req: IncomingMessage): Promise<string> {
  return new Promise<string>((resolve, reject) => {
    const chunks: Buffer[] = [];
    let size = 0;
    req.on('data', (chunk: Buffer) => {
      size += chunk.length;
      if (size > MAX_BODY_BYTES) {
        reject(new Error('request body too large'));
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => {
      resolve(Buffer.concat(chunks).toString('utf8'));
    });
    req.on('error', reject);
  });
}

function sendJson(res: ServerResponse, status: number, body: unknown): void {
  const payload = JSON.stringify(body);
  res.statusCode = status;
  res.setHeader('Content-Type', 'application/json');
  res.end(payload);
}

/**
 * Handle one `POST /api/compile`.  Parses the body, runs the native
 * compiler in-process, and returns the discriminated compile response.
 * A malformed request is a 400; a missing CLI (or any non-compile
 * failure) is a 500 carrying a readable message.
 */
async function handleCompile(
  req: IncomingMessage,
  res: ServerResponse,
): Promise<void> {
  let request;
  try {
    const raw = await readBody(req);
    request = parseCompileRequest(JSON.parse(raw));
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err);
    sendJson(res, 400, { ok: false, error: message, diagnostics: [] });
    return;
  }
  try {
    const response = await runCompile(request);
    sendJson(res, 200, response);
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err);
    sendJson(res, 500, {
      ok: false,
      error: `compile failed in the dev server: ${message}`,
      diagnostics: [{ message }],
    });
  }
}

/**
 * A Vite plugin that mounts the `POST /api/compile` middleware on the dev
 * server.  This is the demo's documented Node-side compile fallback: the
 * browser can't subprocess the native `cel` CLI, so compile runs here and
 * eval runs client-side.
 */
function compileEndpoint(): Plugin {
  return {
    name: 'cel-wasm-compile-endpoint',
    configureServer(server) {
      const middleware: Connect.NextHandleFunction = (req, res, next) => {
        if (req.method !== 'POST' || req.url !== COMPILE_ROUTE) {
          next();
          return;
        }
        void handleCompile(req, res).catch((err: unknown) => {
          const message = err instanceof Error ? err.message : String(err);
          sendJson(res, 500, {
            ok: false,
            error: `compile endpoint crashed: ${message}`,
            diagnostics: [{ message }],
          });
        });
      };
      server.middlewares.use(middleware);
    },
  };
}

export default defineConfig({
  plugins: [compileEndpoint()],
  resolve: {
    alias: [
      {
        // Code imports the `monaco-editor` package root (so TypeScript
        // resolves its bundled types under nodenext), but the bundle only
        // needs the editor core — not every bundled language contribution.
        // An exact-match alias to `editor.api` keeps the build slim while
        // leaving deeper subpaths (e.g. the `editor.worker`) untouched.
        find: /^monaco-editor$/,
        replacement: 'monaco-editor/esm/vs/editor/editor.api',
      },
    ],
  },
  build: {
    target: 'es2022',
    // Under `dist/` so it shares the package's ignored build-output dir
    // (`dist/web/`) — the `tsc --build` tsbuildinfo lives at `dist/`, and
    // Vite only empties this subdir.
    outDir: 'dist/web',
    // Monaco's editor core is a single large chunk by design; the demo is
    // a showcase, not a size-budgeted production app, so raise the warning
    // ceiling above it rather than code-split the editor.
    chunkSizeWarningLimit: 3000,
  },
  // Monaco ships large worker bundles; this keeps Vite from warning on
  // them during the demo build.
  worker: {
    format: 'es',
  },
});
