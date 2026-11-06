// Regenerates web/poc/vendor/lit.js. Run this only when upgrading Lit - it
// is not part of the firmware build, not part of CI, and not needed to run
// the PoC or to serve the UI.
//
//   cd <repo root>
//   npm install --no-save lit@3.3.3 esbuild@0.28.2
//   node web/poc/tools/vendor-lit.mjs
//
// Why a vendored bundle at all: Lit's published source imports its own
// packages by bare specifier ("lit-html", "@lit/reactive-element"), and a
// browser can only load URLs. Lit's own requirements page is explicit that
// "the only transformation required to use Lit on modern browsers is to
// convert bare module specifiers to browser-compatible URLs". Doing that
// once, into one pinned file committed to the repo, is what buys a UI that
// otherwise needs no build step at all - see ADR 0011.
//
// Everything is resolved against the current working directory rather than
// against this script, so the throwaway node_modules/ lives at the repo root
// and nothing under web/ ever has to become an npm package.

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

const pocRoot = dirname(dirname(fileURLToPath(import.meta.url)));
const outfile = join(pocRoot, 'vendor', 'lit.js');

// Exactly what this UI imports, nothing else - the bundle is only as big as
// the parts actually used.
const entry = [
  "export { LitElement, html, svg, css, nothing } from 'lit';",
  "export { repeat } from 'lit/directives/repeat.js';",
  "export { classMap } from 'lit/directives/class-map.js';",
  "export { styleMap } from 'lit/directives/style-map.js';",
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
      ' * Vendored bundle (LitElement, html, svg, css, nothing, repeat, classMap,',
      ` * styleMap), produced by web/poc/tools/vendor-lit.mjs with esbuild ${esbuildVersion}.`,
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
