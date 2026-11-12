// Regenerates web/ui/vendor/lit.js. Run this only when upgrading Lit - it
// is not part of the firmware build, not part of CI, and not needed to run
// the UI or to serve it from the board.
//
//   cd <repo root>
//   npm install --no-save lit@3.3.3 esbuild@0.28.2
//   node web/ui/tools/vendor-lit.mjs
//
// A copy of web/poc/tools/vendor-lit.mjs pointed at this tree instead - see
// that script's own header comment for why a vendored bundle exists at all
// (ADR 0011: Lit's one requirement is resolving bare module specifiers,
// done once into one pinned, committed file). web/ui vendors its own copy
// rather than sharing the PoC's, per "separate from web/poc/" (issue #88) -
// the two trees must be able to diverge (a future Lit upgrade here doesn't
// have to touch the frozen PoC, and vice versa).
//
// Everything is resolved against the current working directory rather than
// against this script, so the throwaway node_modules/ lives at the repo
// root and nothing under web/ ever has to become an npm package.

import { createRequire } from 'node:module';
import { gzipSync } from 'node:zlib';
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const cwd = process.cwd();
const requireFromCwd = createRequire(join(cwd, 'noop.js'));

let esbuild;
try {
  esbuild = await import(pathToFileURL(requireFromCwd.resolve('esbuild')).href);
} catch {
  console.error('esbuild not found. Run this from the repo root after:\n' +
                '  npm install --no-save lit@3.3.3 esbuild@0.28.2');
  process.exit(1);
}

const version = (pkg) =>
  JSON.parse(readFileSync(join(cwd, 'node_modules', pkg, 'package.json'), 'utf8')).version;
const litVersion = version('lit');
const esbuildVersion = version('esbuild');

const uiRoot = dirname(dirname(fileURLToPath(import.meta.url)));
const outfile = join(uiRoot, 'vendor', 'lit.js');

// Exactly what this UI imports, nothing else - the bundle is only as big as
// the parts actually used.
const entry = [
  "export { LitElement, html, css } from 'lit';",
  "export { repeat } from 'lit/directives/repeat.js';",
  "export { classMap } from 'lit/directives/class-map.js';",
].join('\n');

await esbuild.build({
  stdin: { contents: entry, resolveDir: cwd, sourcefile: 'qc-lit-entry.js', loader: 'js' },
  outfile,
  bundle: true,
  minify: true,
  format: 'esm',
  target: 'es2022',
  legalComments: 'none',
  conditions: ['production', 'browser', 'module', 'import'],
  define: { 'process.env.NODE_ENV': '"production"' },
  banner: {
    js: [
      '/*',
      ` * Lit ${litVersion} - BSD-3-Clause - https://lit.dev`,
      ' * Vendored bundle (LitElement, html, css, repeat, classMap), produced by',
      ` * web/ui/tools/vendor-lit.mjs with esbuild ${esbuildVersion}.`,
      ' * Do not edit by hand.',
      ' */',
    ].join('\n'),
  },
});

const bytes = readFileSync(outfile);
console.log(
  `lit ${litVersion} via esbuild ${esbuildVersion}: ` +
    `${bytes.length} B minified, ${gzipSync(bytes, { level: 9 }).length} B gzip -9`
);
