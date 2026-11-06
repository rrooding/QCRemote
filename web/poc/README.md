# Web UI proof of concept (issue #84)

The proof of concept behind [ADR 0011](../../docs/adr/0011-web-ui-stack.md): the preset list
and a live-updated current preset, rendered with the recommended stack, driven by a mock board
on the Mac. It is **not** firmware, nothing under `firmware/` references it, and it has never
run on the board.

What it is meant to prove, and nothing more:

- the recommended stack (Lit 3, no build step, ES modules served as-is) can render the Quad
  Cortex's preset-list layout and update it live from a WebSocket;
- the snapshot/patch/resync protocol in ADR 0011 works end to end, including a dropped
  message and a dropped link;
- preset switching can be **confirmed** rather than optimistic (issue #78) and still feel
  immediate;
- what the whole thing costs in flash once gzipped.

## Run it

```sh
node web/poc/tools/mock-server.mjs          # http://localhost:8080/
```

Node 22 or newer, no `npm install`, no dependencies. Open the page, and the mock replays a
session: a scene change at 4 s, a preset change at 9 s, the Quad Cortex "unplugged" at 15 s
and back at 20 s. Tapping a preset sends `switchPreset` and the row only moves once the mock
reports the board's own new state ~450 ms later.

Useful flags:

| Flag | What it does |
| --- | --- |
| `--port N` | Listen somewhere else (default 8080). |
| `--fixture <file>` | Replay a different session (default `fixtures/session.json`). |
| `--drop-every N` | Silently skip every Nth patch while still burning its sequence number, to exercise the gap → resync path. |

To point the page at a **real board** instead of the mock while still serving the UI from the
Mac, load `http://localhost:8080/?board=192.168.1.42` — the page then opens its WebSocket
against that address. That is the iteration loop ADR 0011 recommends: edit a file, reload,
no reflash.

## Measure it

```sh
python3 web/poc/tools/embed-assets.py              # size report
python3 web/poc/tools/embed-assets.py --out x.hpp  # + the generated C++ header
```

Python 3 standard library only. The header it emits is the prototype of ADR 0011's asset
pipeline: each asset as a deterministic gzip stream in a `constexpr uint8_t[]`, with a table
of path / content type / payload for the HTTP server to serve with `Content-Encoding: gzip`.
Nothing in `firmware/` includes it yet.

Measured on 2026-11-06, gzip level 9:

| | files served | gzipped |
| --- | --- | --- |
| As it stands (no build step) | 10 | 20 221 B |
| of which vendored Lit 3.3.3 | 1 | 7 346 B |
| Same source bundled + minified with esbuild | 3 | 13 601 B |

## Layout

```
index.html                 page shell: viewport, meta, design tokens, module entry
manifest.webmanifest       standalone/home-screen metadata (works over plain HTTP)
src/store.js               the state model: snapshot/patch, seq gaps, reconnect, pending switch
src/store-controller.js    Lit ReactiveController that ties the store to component updates
src/qc-app.js              shell: header, link state, scroll container
src/qc-now-playing.js      current preset + scene row
src/qc-preset-list.js      folders and presets, keyed, large tap targets
src/qc-knob.js             Milestone 3 rendering probe: SVG dial on Pointer Events
vendor/lit.js              pinned Lit 3.3.3 bundle (see tools/vendor-lit.mjs)
tools/mock-server.mjs      dev-only mock board: static files + RFC 6455 WebSocket + replay
tools/embed-assets.py      gzip + C++ header generator, the asset-pipeline prototype
tools/vendor-lit.mjs       regenerates vendor/lit.js on a Lit upgrade (needs npm, rarely run)
fixtures/session.json      a 3-folder / 48-preset catalog in the shape the board reports
```

## Things that are deliberately not here

- **Any Neural DSP artwork, icon, typeface or colour.** The layout and interaction model are
  reproduced; everything drawn is ours, with the system font stack and a palette written from
  scratch (issue #84 question 8). The preset names in the fixture are invented.
- **An app icon.** iOS wants a PNG `apple-touch-icon`; the manifest here has no `icons` entry,
  so a home-screen shortcut gets a screenshot instead of an icon. Drawing one is an
  implementation task, not research.
- **The grid / signal chain, footswitches, real parameters.** Milestone 3. The knob is a
  rendering probe, wired to nothing.
- **Tests.** The protocol behaviour was verified by driving the page in headless Chrome
  (see ADR 0011's "What was actually verified"); none of that is committed as a test suite,
  because where those tests belong depends on decisions the owner hasn't made yet (#19).
- **Security.** The mock server trusts everything it is sent and implements only the subset
  of RFC 6455 a browser needs. Never run it outside localhost.
