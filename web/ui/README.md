# Real web UI (issue #88, ADR 0011)

The web UI the board actually serves from flash, superseding the inline page that used to
live in `QcWifiWebTransport.hpp`. Lit 3, no build step, plain ES modules - see
[ADR 0011](../../docs/adr/0011-web-ui-stack.md) for why.

This tree is **separate from [`web/poc/`](../poc/README.md)**, which stays as a frozen
research artifact: the PoC's protocol (a `current` preset, `link.qcConnected`) was never
what the board ships. [Issue #75](https://github.com/rrooding/QCRemote/issues/75)'s real
envelope (`QcLiveUpdateEnvelope.hpp`, `firmware/README.md`'s "Live WebSocket updates"
section) is leaner - no preset identity on the wire yet, just a catalog and a
command/count tally - and this tree is built against that, not the PoC's assumption. See
`src/store.js`'s own header comment for the exact shapes.

## Run it

```sh
node web/ui/tools/mock-server.mjs          # http://localhost:8080/
```

Node 18 or newer, no `npm install`, no dependencies. Open the page: the mock replays
`fixtures/session.json` - a 3-folder / 48-preset catalog, then two preset-change events a
few seconds apart (the issue #75 fix: a *repeat* change must update just as fast as the
first one, not wait up to a minute) and a scene-change tally bump.

Useful flags:

| Flag | What it does |
| --- | --- |
| `--port N` | Listen somewhere else (default 8080). |
| `--fixture <file>` | Replay a different session. |
| `--drop-every N` | Silently skip every Nth patch while still burning its sequence number, to exercise the gap → resync path. |
| `--max-ws-clients N` | Refuse any concurrent WebSocket upgrade past N, mirroring the real board's `CONFIG_WEBSOCKET_MAX_CONTEXTS=1` one-client policy. Set to `1` and open a second tab to check its fallback-to-HTTP behaviour without touching hardware. |

To point the page at a **real board** instead of the mock while still serving the UI from
the Mac, load `http://localhost:8080/?board=192.168.1.42` - the page then opens its
WebSocket (and its one-shot `GET /api/presets.json`) against that address. That is the
iteration loop ADR 0011 recommends: edit a file, reload, no reflash.

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
src/store.js               the state model: snapshot/patch, seq gaps, reconnect, HTTP fallback
src/store-controller.js    Lit ReactiveController that ties the store to component updates
src/qc-app.js              shell: header with link state, scroll container
src/qc-status.js           catalog summary + the last preset-change event (command/count)
src/qc-preset-list.js      folders and presets, keyed, read-only
vendor/lit.js               pinned Lit 3.3.3 bundle (see tools/vendor-lit.mjs) - this
                           tree's own copy, not shared with web/poc/vendor/lit.js
tools/mock-server.mjs      dev-only mock board: static files + RFC 6455 WebSocket + replay
tools/vendor-lit.mjs       regenerates vendor/lit.js on a Lit upgrade (needs npm, rarely run)
fixtures/session.json      a 3-folder / 48-preset catalog plus a few preset-change events
```

`firmware/app/tools/embed_assets.py` (not duplicated here) is the asset pipeline itself -
see that script and `firmware/README.md`'s web UI section for how editing a file here
reaches the board.

## What's deliberately not here

- **Preset switching, scenes, the grid, parameters, footswitches.** Issue #78 and
  Milestone 3. The board has nothing to switch to or report back yet on this wire.
- **Any Neural DSP artwork, icon, typeface or colour.** Everything drawn is ours - the
  system font stack and a palette written from scratch (ADR 0011 decision 8). The preset
  names in the fixture are invented.
- **An app icon.** iOS wants a PNG `apple-touch-icon`; the manifest here has no `icons`
  entry, so a home-screen shortcut gets a screenshot instead of an icon - drawing one is a
  later, separate task.
- **Caching headers, offline support.** ADR 0011 decision 7/consequences: left off until a
  page load measurably drags; there is no secure context for a service worker anyway.
