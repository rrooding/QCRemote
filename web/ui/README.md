# Real web UI (issue #88, ADR 0011)

The web UI the board actually serves from flash, superseding the inline page that used to
live in `QcWifiWebTransport.hpp`. Lit 3, no build step, plain ES modules - see
[ADR 0011](../../docs/adr/0011-web-ui-stack.md) for why.

This tree is **separate from [`web/poc/`](../poc/README.md)**, which stays as a frozen
research artifact. It speaks the Board's WebSocket contract in [`docs/api/`](../../docs/api/README.md)
([ADR 0012](../../docs/adr/0012-web-api-contract.md)): a snapshot, section-replacing patches,
ticks and errors, `resync` after a `seq` gap, a build id instead of a protocol version, and
close code 4001 when another page takes over. `src/store.js` is where the page applies it.

## Run it

```sh
node web/ui/tools/mock-server.mjs          # http://localhost:8080/
```

Node 18 or newer, no `npm install`, no dependencies. Open the page: the mock replays
`fixtures/session.json` - a 3-folder / 48-preset Catalog, two preset changes at the unit, the
Link dropping and coming back, an error, a Catalog change (a new snapshot), and after 40 s one
replace with close code 4001, so the "Opened on another device" banner and its take-over button
can be tried. Tapping a Preset switches to it (#78): the mock answers `switchPreset` with a
`current` patch or a `qcDisconnected` / `staleCatalog` / `badIndex` error.

The mock serves one live page at a time like the Board: opening a second tab closes the first
with 4001. It sends the page's own build id (`src/build.js`), so the page never reloads
against it.

Useful flags:

| Flag | What it does |
| --- | --- |
| `--port N` | Listen somewhere else (default 8080; 0 picks a free port). |
| `--fixture <file>` | Replay a different session. |
| `--drop-every N` | Silently skip every Nth message after a snapshot while still burning its `seq`, to exercise the gap → `resync` path. |

To point the page at a **real Board** instead of the mock while still serving the UI from
the Mac, load `http://localhost:8080/?board=192.168.1.42` - the page then opens its
WebSocket against that address. That is the iteration loop ADR 0011 recommends: edit a file,
reload, no reflash. The page served from the Mac has build id `dev`, so against a Board it
reloads once and then shows that it doesn't match the Board's firmware; that's expected.

## Test it

```sh
npm test --prefix web/ui
```

This is the one place the test command lives (`web/ui/package.json`); CI runs the same line.
Node's built-in test runner, no `npm install`; the Node version in `.node-version` (22) or
newer, for its built-in WebSocket client. Run it on that version before pushing: Node 22's
`node --test` is stricter about its arguments than newer releases. `test/store.test.js` drives the `Store`
through its public interface with a fake WebSocket, `location` and `sessionStorage`, feeding
it the Board → page messages in `docs/api/examples/server/`. `test/mock-server.test.js` runs
the mock on a free port and checks what it sends against the same examples, and every error
code it can send against the schema. `test/store-controller.test.js` covers the Lit glue.
CI runs the same command (the `web-ui-tests` job in `.github/workflows/api-contract.yml`,
see `docs/api/README.md`'s CI section) on every change under `web/ui/` or `docs/api/`.

## Build id

`src/build.js` is checked in as `export const BUILD = 'dev';`. The firmware's asset embed
step rewrites that value to the firmware's build id when it embeds the page, so the page
and the Board's `snapshot.build` match. When they don't, the page reloads once per Board
build per tab (`sessionStorage`) without applying the snapshot.

## Measure it

```sh
python3 firmware/app/tools/embed_assets.py --ui-dir web/ui --out /tmp/x.hpp
```

Python 3 standard library only - the same script `firmware/app/CMakeLists.txt` runs on
every build (see `firmware/app/cmake/EmbedWebAssets.cmake`). It prints a per-asset and
total gzip size report and, with `--out`, the generated header: each asset as a
deterministic gzip stream in a `constexpr uint8_t[]`, plus the
`http_resource_detail_static`/`HTTP_RESOURCE_DEFINE` pair that serves it from the board
with `Content-Encoding: gzip`.

## Layout

```
index.html                 page shell: viewport, meta, design tokens, module entry
manifest.webmanifest       standalone/home-screen metadata (works over plain HTTP)
src/store.js               the state model: the docs/api contract, reconnect, take-over
src/build.js               the page's build id ("dev"; rewritten by the firmware embed step)
src/store-controller.js    Lit ReactiveController that ties the store to component updates
src/qc-app.js              shell: header with link state, scroll container
src/qc-status.js           the Current preset, the Link and a Catalog summary
src/qc-preset-list.js      folders and presets, keyed; tap to switch, pending and Current marked
vendor/lit.js               pinned Lit 3.3.3 bundle (see tools/vendor-lit.mjs) - this
                           tree's own copy, not shared with web/poc/vendor/lit.js
tools/mock-server.mjs      dev-only mock board: static files + RFC 6455 WebSocket + replay
tools/vendor-lit.mjs       regenerates vendor/lit.js on a Lit upgrade (needs npm, rarely run)
fixtures/session.json      the mock's scripted session (Catalog, patches, an error, a 4001)
test/                      node --test suites for the store, its controller and the mock
```

`firmware/app/tools/embed_assets.py` (not duplicated here) is the asset pipeline itself -
see that script and `firmware/README.md`'s web UI section for how editing a file here
reaches the board.

## What's deliberately not here

- **Scenes, the grid, parameters and footswitches.** Milestone 3.
- **Any Neural DSP artwork, icon, typeface or colour.** Everything drawn is ours - the
  system font stack and a palette written from scratch (ADR 0011 decision 8). The preset
  names in the fixture are invented.
- **An app icon.** iOS wants a PNG `apple-touch-icon`; the manifest here has no `icons`
  entry, so a home-screen shortcut gets a screenshot instead of an icon - drawing one is a
  later, separate task.
- **Caching headers, offline support.** ADR 0011 decision 7/consequences: left off until a
  page load measurably drags; there is no secure context for a service worker anyway.
