# ADR 0011: Web UI stack - Lit, no build step, JSON over WebSockets

Date: 2026-11-06
Status: Proposed

## Context

[ADR 0010](0010-wifi-web-transport.md) settled that the board serves a web UI over Wi-Fi, and
proved it with a page that is a string literal inside `QcWifiWebTransport.hpp`: a `fetch()` of
`/api/presets.json`, a `<ul>` per folder, and an `innerHTML` rebuild on every message. That is
the right amount of UI for answering "is Wi-Fi viable", and the wrong amount for everything
the owner has asked for next.

The direction ([issue #84](https://github.com/rrooding/QCRemote/issues/84), 2026-11-06): the
UI should **feel native** on a phone and a Mac, and should **reproduce the Quad Cortex's own
UI elements** - preset list and folders, the grid / signal chain, scenes, footswitches and,
in Milestone 3, block parameters - with changes **streamed live** over WebSockets rather than
polled or reloaded.

This ADR picks the technology for that, before the page grows past the point where changing
course is cheap. It is a research decision with a proof of concept, not an implementation:
nothing under `firmware/` changes here, and [PR #74](https://github.com/rrooding/QCRemote/pull/74)
is untouched.

### The constraints that actually bind

- **Assets live in flash and are served by Zephyr's HTTP server.** Flash is 64 MB external on
  `frdm_rw612`; ADR 0010's whole Wi-Fi build uses 1.11% of it. **Asset size is therefore not a
  real constraint**, which turns out to be the single most important fact in this decision -
  see the measurements below, where the entire framework comparison spans 20 KB.
- **RAM is the scarce resource**, at 75.7% after ADR 0010. Anything the UI decision adds to
  RAM matters; anything it adds to flash does not.
- **Plain HTTP on the LAN.** Not a secure context: no service workers, so no offline caching
  and no Web Push. Everything the page needs must be served by the board.
- **The MCU cannot compress at runtime.** miniz is built decompress-only
  (`MINIZ_NO_DEFLATE_APIS`, `firmware/app/CMakeLists.txt:58`, per
  [ADR 0005](0005-gzip-inflate-miniz.md)), and Zephyr's WebSocket has no `permessage-deflate`
  (nothing in `subsys/net/lib/websocket/` mentions it). So **static assets must be gzipped at
  build time** and **dynamic JSON and WebSocket frames go out uncompressed**.
- **Firmware CI builds the image** and has no Node today.

### What Zephyr v4.5.0's HTTP server will and won't do

Checked against the pinned tree (`~/zephyrproject/zephyr`), not assumed:

- A **static** resource's `content_encoding` is emitted unconditionally and the server never
  reads the request's `Accept-Encoding` for it (`subsys/net/lib/http/http_server_http1.c:246-253`).
  Serving a pre-gzipped blob with `Content-Encoding: gzip` therefore works, and is technically
  a lie to a client that didn't ask for gzip - which no browser since the 1990s is.
- A static resource gets **no `Cache-Control`, no `ETag`, no `Last-Modified`** - the response
  template is status line, `Content-Type`, `Content-Length`, optionally `Content-Encoding`, and
  nothing else (same function). Every page load re-downloads every asset.
- A **dynamic** resource may set arbitrary response headers through
  `http_response_ctx::headers`; the only one the server refuses is `Transfer-Encoding`
  (`http_server_http1.c:417-490`). So `Cache-Control`, `ETag` and a 304 are available to a
  dynamic handler, and `CONFIG_HTTP_SERVER_CAPTURE_HEADERS` can hand it the request's
  `If-None-Match`.
- `HTTP_RESOURCE_TYPE_STATIC_FS` does negotiate compression properly (`.br`, `.gz`, `.zst`,
  `.lzw`, `.zz`, in that order - `http_server_core.c:1779`, gated on
  `CONFIG_HTTP_SERVER_COMPRESSION`, `subsys/net/lib/http/Kconfig:304`), but it reads from a
  filesystem. There is no filesystem on this board today, and adding one to serve ~20 KB of
  immutable assets is a lot of moving parts for no gain.
- The server never sends `Connection: close`; connections persist until
  `CONFIG_HTTP_SERVER_CLIENT_INACTIVITY_TIMEOUT` (default 10 s, `Kconfig:240`). With
  `CONFIG_HTTP_SERVER_MAX_CLIENTS=4`, a browser that opens its usual six parallel connections
  simply has two of them wait in the TCP backlog - slower, not broken.
  **Corrected by hardware testing (PR #92, issue #88): this was wrong on both counts, and
  the sentence above it - that a WebSocket does not occupy a client slot - is the only part
  that survived.**
  First, the effective parallel limit was never 4: Zephyr's own `CONFIG_ZVFS_POLL_MAX`
  default of 3 left the server's single `zsock_poll()` call room for the eventfd, the
  listener and exactly **one** client, and a `poll()` failure makes `http_server_run()`
  close every socket it owns, listener included.
  Second, waiting in the backlog is not "slower" when the connections ahead are keep-alive
  ones nothing closes: the extra connections wait out the inactivity timeout, and once the
  listening socket's backlog fills too, Zephyr drops further SYNs outright
  (`tcp_backlog_is_full()`), so the client falls back to ~1 s SYN retransmits. And the
  relevant count is not "six per page load" but how many connections are still *parked*
  when the next page load starts - two browsers open at once, or one reloading, is twelve.
  Third, and the one that only appears on a real link: `CONFIG_NET_TCP_MAX_SEND_WINDOW_SIZE`
  defaults to 0, which makes the per-connection TCP window **one third of the entire
  net_buf pool** (`subsys/net/ip/tcp.c`), so more than three concurrently-sending
  connections oversubscribe that pool by construction - at any pool size. On this board
  that was a 1 536-byte window against a 4 608-byte pool, and over a 25-150 ms Wi-Fi RTT
  the pool ran dry and responses stalled and then reset mid-body. Raising
  `HTTP_SERVER_MAX_CLIENTS` made it strictly worse, which is why it ended at 8 rather than
  the 13 an earlier round picked.
  See firmware/README.md's "Parallel connections" section for the measured chain and the
  fix (pinned 2 048-byte windows with the buffer pools sized to back them,
  `HTTP_SERVER_MAX_CLIENTS=8`, `ZVFS_POLL_MAX=10`, `CLIENT_INACTIVITY_TIMEOUT` 10 → 5, plus
  the `NET_MAX_CONTEXTS`/`NET_MAX_CONN` pools a multi-browser load also exhausted), which
  costs ~44 KB of RAM.
- **A WebSocket does not occupy an HTTP client slot.** `handle_http1_to_websocket_upgrade()`
  calls `websocket_register()` and then `http_server_release_client()`
  (`subsys/net/lib/http/http_server_ws.c:35-140`), so the upgraded socket is handed to the
  application and the inactivity timer no longer applies to it. Two open browsers do not
  starve the page out.

One bug found while reading this, **not fixed here** because PR #74 owns that file:
`websocket_register()` stores the caller's buffer pointer in the per-connection context
(`subsys/net/lib/websocket/websocket.c:1330`), and `QcWifiWebTransport` passes the same
`g_qcWsDataBuffer[256]` for every upgrade. With `CONFIG_WEBSOCKET_MAX_CONTEXTS=2`, two
connected browsers share one receive buffer. Listed under "Proposed changes to other issues"
below.

### Question 1: framework, or none

Every candidate was built into the *same* app - a folder/preset list rendered from the
`QcCatalogJson` shape, keyed, with a reactive "current preset" updated from a WebSocket - and
bundled identically (esbuild 0.28.2, `--minify`, ESM, es2022, production conditions), so the
differences are framework cost rather than how much app got written. Measured 2026-11-06:

| Stack | Version | Minified | **gzip -9** | brotli -11 | Build step |
| --- | --- | --- | --- | --- | --- |
| Vanilla DOM, no framework | - | 912 B | **505 B** | 417 B | none |
| SolidJS | 1.9.18 | 14 706 B | **6 024 B** | 5 489 B | required (JSX compiler) |
| Preact + hooks (JSX) | 11.2.0 | 14 267 B | **6 058 B** | 5 504 B | required (JSX) |
| **Lit** | **3.3.3** | **18 575 B** | **7 328 B** | 6 653 B | **optional** |
| Preact + Signals (JSX) | 11.2.0 / 2.11.4 | 21 845 B | **8 661 B** | 7 907 B | required (JSX) |
| Preact + Signals + htm | + htm 3.1.1 | 22 714 B | **9 151 B** | 8 351 B | none |
| Alpine (CDN build, app in HTML) | 3.17.5 | 55 935 B | **19 921 B** | 18 029 B | none |
| Svelte | 5.57.3 | 56 983 B | **21 403 B** | 19 469 B | required (compiler) |

Reading that table honestly: **the entire spread is 20 KB on a 64 MB flash, or 0.03%.** Size
cannot decide this. Svelte 5's number is its runtime baseline on a deliberately tiny app and
would amortise over a large one; it is listed as measured, not as a verdict on Svelte.
petite-vue was dropped from the comparison rather than measured: its last release is from
2022 and recommending an unmaintained library for a UI meant to grow into Milestone 3 would be
indefensible regardless of its size.

What does decide it, in order:

1. **Whether the firmware build needs Node.** `west build` is self-contained today. Lit is the
   only component framework here that can be used with no build step at all - its templates
   are tagged template literals, not a compiled syntax. Lit's own requirements page states
   that "the only transformation required to use Lit on modern browsers is to convert bare
   module specifiers to browser-compatible URLs"; resolving that once into a pinned, committed
   bundle removes the build step from the day-to-day loop entirely. Solid and Svelte cannot
   work this way - their reactivity *is* the compiler. Preact can, via `htm`, at a higher cost
   than Lit and with template parsing moved to runtime.
2. **Fit for a widget set.** "Reproduce the QC's UI elements" is a request for a component
   library: `qc-preset-list`, `qc-scene-bar`, `qc-footswitch`, `qc-grid`, `qc-knob`. Lit
   produces real custom elements with encapsulated styles, which can be developed and
   previewed in isolation in a plain HTML file.
3. **Update granularity under a stream.** lit-html patches only the bindings that changed; it
   has no virtual DOM to diff. For a parameter stream at 60 fps that is the right shape, and
   `repeat()` keeps keyed lists from being rebuilt. Solid and Preact Signals are at least as
   good here; vanilla and Alpine are not.
4. **Longevity.** Lit 3 is stable, Google-maintained, and standards-shaped - custom elements
   and Shadow DOM are the platform, so a future rewrite can keep the components.

### Question 2: DOM, SVG or Canvas

- **DOM + CSS** for everything that is a list, a button or a panel: preset list, scenes,
  footswitches. Accessibility, hit testing, text layout and scrolling all come free, and these
  are exactly the controls that must have large, reliable tap targets.
- **SVG** for the grid / signal chain and for knobs and meters. The grid is a few dozen
  shapes with real identity (a block is a thing you tap, drag and style), which is what SVG is
  for: each node is an element, so hit testing and event delegation are the DOM's, and lit-html
  binds to its attributes exactly like HTML. A knob is one arc with a `stroke-dasharray` plus a
  rotated line.
- **Canvas** only for something genuinely continuous and high-rate with no per-element
  identity - an output meter at 30-60 Hz is the single plausible case, and even that is one
  element with a CSS `transform` until proven otherwise. Canvas means owning a render loop,
  re-implementing hit testing, and losing accessibility; none of that is worth paying before
  there is a measurement that demands it.
- **Existing knob components** (`input-knob`, `x-knob`, `input-knobs.js`, the Svelte knob
  libraries) were looked at and are not recommended: the ones that fit are tens of lines of
  pointer maths wrapped in someone else's styling assumptions, and the QC look has to be ours
  anyway. `web/poc/src/qc-knob.js` is 164 lines including its comments.
- The rules that actually keep it at 60 fps on a phone matter more than the technology:
  animate only `transform` and `opacity`, use Pointer Events with `setPointerCapture` for one
  code path across touch/mouse/pencil, and set `touch-action: none` on a draggable control -
  without it iOS starts a scroll on the compositor thread and no amount of `preventDefault()`
  reliably takes it back.

### Question 3: client state and the WebSocket layer

One store, fed by the socket, with components subscribing - not per-component fetches, which
would re-derive the same state on every reconnect and let two tabs drift.

The protocol shape that makes this work past Milestone 2 is **snapshot plus patches with a
sequence number**:

- on connect, the board sends a full `snapshot`;
- every change after that is a `patch` carrying only the section that changed, with
  `seq = previous + 1`;
- a client that sees a gap knows it has missed a change and asks for a `resync` rather than
  rendering a guess. This is the part that scales to Milestone 3: a parameter change must not
  re-send the 48-preset catalog, and once it doesn't, "did I miss one" stops being answerable
  by eye.
- a `tick` at a known cadence makes silence meaningful. The board only speaks when something
  changes, so without a heartbeat an idle rig and a dead TCP connection look identical - and a
  Wi-Fi drop produces exactly that half-open socket. The browser's WebSocket API cannot send
  pings itself, so this has to be an application message.
- reconnect with capped exponential backoff plus jitter, and an immediate retry on `online`
  and on `visibilitychange` - unlocking a phone is the most common way this link dies.

**Preset switching is confirmed, not optimistic** (issue #78): the tap marks the row pending
and the selection only moves when the board reports its own new state. Optimism here would mean
inventing a device state we have no evidence for, and on a 2.4 GHz link that lie is visible.
Optimistic updates stay available later for changes that are cheap, idempotent and locally
reversible (a block bypass toggle), where the latency is worth more than the certainty.

### Question 4: JSON or a binary format

Measured on the real 3-folder / 48-preset catalog and on one preset-change delta:

| Payload | JSON | CBOR | MessagePack |
| --- | --- | --- | --- |
| Full catalog (48 presets) | 2 015 B | 1 681 B | 1 558 B |
| One preset-change delta | 62 B | 47 B | 45 B |

| Client-side decoder, bundled | Minified | gzip -9 |
| --- | --- | --- |
| `JSON.parse` | 0 B (built in) | 0 B |
| `cbor-x` (decode only) | 30 719 B | 11 436 B |
| `@msgpack/msgpack` (decode only) | 14 321 B | 4 437 B |

A binary format saves **334-457 B on the catalog** - a payload sent once per listing - and
**15-17 B per delta**, and costs **4.4-11.4 KB of decoder** that the board has to serve from
flash on every page load, plus an encoder on the MCU, plus frames that can no longer be read in
the browser's network panel or in a log. nanopb being already in the tree doesn't change the
arithmetic: it would still need a `.proto`, a generated decoder on the client, and a schema
evolution story for every message.

The MCU-side encoding cost argues the same way. `QcCatalogJson.hpp` is a hand-rolled,
heap-free, single-pass appender over a fixed `etl::vector`; it already runs on `qc_wifi_web`
rather than `qc_heartbeat` precisely so that its cost can't touch the KeepAlive path (ADR 0010).
Producing 2 KB of text that way is cheaper than linking a CBOR library to produce 1.7 KB.

The one case that would genuinely want binary is a future high-rate numeric stream (output
meters, a parameter sweep at 60 Hz). That case wants a purpose-built fixed-layout frame on a
WebSocket binary opcode - a `Float32Array` read directly on the client - not a
self-describing format, and it needs no library on either side. Keeping JSON now does not
close that door.

### Question 5: asset pipeline

The whole UI, gzipped at level 9 (`web/poc/tools/embed-assets.py`, 2026-11-06):

| | Files served | Total gzipped |
| --- | --- | --- |
| As served, no build step | 10 | **20 221 B** |
| of which vendored Lit 3.3.3 | 1 | 7 346 B |
| Same source, bundled + minified by esbuild | 3 | **13 601 B** |

(The vendored bundle is Lit alone, exporting the eight symbols this UI uses. Question 1's Lit
row is that same library *plus* the comparison app, which is why the two figures land within
20 bytes of each other.)

So a bundler buys 6.6 KB of flash (0.01% of the board's flash) and 7 fewer requests. That is
the entire case for adding Node to this repo, and it is not enough on its own.

Serving: the embed step emits each asset as a deterministic gzip stream (mtime 0, no stored
filename, so unchanged input produces byte-identical output and CI can diff it) in a
`constexpr uint8_t[]`, served as a static resource with `Content-Encoding: gzip`. A static
resource costs **no RAM**: `handle_http1_static_resource()` sends straight from the flash
pointer.

The cost of Zephyr emitting no caching headers for static resources is that every page load
re-downloads 20 KB. On a LAN that is tens of milliseconds, and the fix if it ever isn't -
hashed filenames plus a dynamic handler setting `Cache-Control: public, max-age=31536000,
immutable` and `ETag`, answering `If-None-Match` with a 304 - is available without changing
anything else, because dynamic resources can set those headers.

### Question 6: native feel

Verified against current sources rather than habit:

- **Home screen and standalone mode work over plain HTTP.** iOS's "Add to Home Screen" is
  offered for `http:` as well as `https:` documents, and a page whose manifest declares
  `display: standalone` (or which carries the legacy `apple-mobile-web-app-capable` meta)
  opens as a Home Screen web app with its own storage. On iOS 26 every site added to the Home
  Screen opens as a web app by default. What is *not* available without HTTPS is the
  secure-context set: service workers (so no offline cache, which is moot - the board is the
  only source of data), Web Push and the Badging API.
- **A practical trap**: the shortcut records the URL it was added with. If the board's DHCP
  lease changes address, the saved icon breaks. Zephyr has an mDNS responder
  (`CONFIG_MDNS_RESPONDER`, with `CONFIG_NET_HOSTNAME_ENABLE`), so a stable
  `http://qcremote.local/` is a few Kconfig lines, and AP mode's address is fixed anyway.
- **Viewport**: `width=device-width, initial-scale=1, viewport-fit=cover`, then
  `env(safe-area-inset-*)` on the shell and `100dvh` rather than `vh` so a collapsing Safari
  toolbar doesn't size the layout wrongly.
- **Zoom and bounce**: `touch-action: manipulation` on the body removes the double-tap zoom
  delay without disabling pinch zoom; `overscroll-behavior` stops the rubber-band at the page
  level and inside the list. `user-scalable=no` is deliberately not used - iOS Safari ignores
  it for accessibility, so relying on it produces a layout that is wrong on exactly the
  devices that need it most.
- **Haptics**: Safari has never implemented the Vibration API, and as of October 2026 caniuse
  still lists no iOS version that supports it. The working trick is clicking a hidden
  `<input type="checkbox" switch>` (Safari 17.4+), which the system answers with one Taptic
  tick; iOS 26.5 plays one tick per tap regardless of the pattern. Treat haptics as a
  feature-detected garnish (`navigator.vibrate` on Android, the switch trick on iOS), never as
  feedback the UI depends on.
- **Dark**: `color-scheme: dark` plus our own tokens. The system font stack
  (`-apple-system`/`system-ui`) costs zero bytes, is the single biggest reason a page reads as
  native on both platforms, and sidesteps the typeface question in question 8 entirely.

### Question 7: dev loop

A mock board on the Mac: a dependency-free Node script that serves the UI directory over HTTP
and speaks the same WebSocket protocol, replaying a fixture (a catalog in the shape the board
really reports, then timed scene/preset/connection events). Editing a file and reloading is
the whole loop; no reflash, no board, no Quad Cortex.

Two things make it more than a toy. `?board=<ip>` points the page's WebSocket at a real board
while the UI is still served from the Mac, which is how a UI change gets tested against real
device behaviour without a flash cycle. And `--drop-every N` deliberately drops patches while
still burning their sequence numbers, because "the UI quietly shows stale state after a lost
message" is otherwise a bug that only appears on a bad Wi-Fi day.

### Question 8: visual assets

Everything drawn is ours: a palette written from scratch, the system font stack, CSS and SVG
shapes. The Quad Cortex's *layout and interaction model* is what gets reproduced - a folder
list of large preset rows, a current-preset header, a row of scene buttons - not its artwork,
icons or typeface, none of which are copied from the device or from Neural DSP's app. The
fixture's preset names are invented for the same reason.

## Decision

1. **Lit 3 (currently 3.3.3), with no build step.** The UI is authored as plain ES modules
   using Lit's `html`/`css` tagged templates and served as-is. Lit's one requirement - bare
   module specifiers - is met by committing a single pinned bundle (`web/poc/vendor/lit.js`,
   18 956 B / 7 346 B gzipped), regenerated only on a Lit upgrade by
   `web/poc/tools/vendor-lit.mjs`. `west build` keeps needing nothing but Python.
2. **Add a bundler when, and only when, one of these is true**: the module count passes ~10
   served files, the message schema (#15) grows big enough to want TypeScript types, or a page
   load measurably drags. The migration is contained: the embed step's contract is "a directory
   of files in, pre-gzipped C arrays out", so swapping hand-written modules for a bundler's
   output changes one CMake line and nothing else. If that day comes, the bundler runs in its
   own CI job and the firmware build consumes its committed, CI-diffed output - firmware CI
   does not grow a Node dependency.
3. **DOM + CSS for lists and buttons, SVG for the grid, knobs and meters, Canvas for nothing
   yet.** Pointer Events with `setPointerCapture` and `touch-action: none` for every draggable
   control; animate only `transform` and `opacity`.
4. **One client store fed by the socket**, with components subscribing through a Lit
   `ReactiveController`. Snapshot on connect, patches keyed by a monotonic `seq`, `resync` on a
   gap, a `tick` heartbeat, capped backoff with jitter plus immediate retry on `online` and
   `visibilitychange`.
5. **Preset switching is confirmed, not optimistic.** The row shows a pending state until the
   board reports its own new state, with a timeout that surfaces the failure rather than
   silently reverting.
6. **JSON on the wire**, in a versioned envelope `{"v":1,"t":"<type>","seq":N,...}`, with
   unknown message types ignored by the client so the board can add messages without breaking
   a page served from older flash. Binary is reserved for a future high-rate numeric stream,
   as a purpose-built fixed-layout frame on a WebSocket binary opcode - not CBOR, MessagePack
   or protobuf.
7. **Assets are pre-gzipped at build time into a generated C++ header** and served as static
   resources with `Content-Encoding: gzip`, costing flash and no RAM. Caching headers are left
   off until a page load measurably drags; the upgrade path is hashed filenames plus a dynamic
   handler with `Cache-Control: immutable`, `ETag` and 304.
8. **Our own drawing, always.** No Neural DSP artwork, icons, typeface or colour values. The
   system font stack, our own palette, CSS and SVG.

The proof of concept lives in **`web/poc/`** (see its README). It is not firmware, nothing in
`firmware/` references it, and it has never run on the board.

## Consequences

- **The firmware build stays Python-only.** That is the main thing this decision buys and the
  main thing it spends: no minification of our own source, no TypeScript, no tree-shaking, and
  ten served files instead of three. Measured, that costs 6.6 KB of flash (0.01%) and seven
  extra requests on a first load. The claim that followed here - that with
  `CONFIG_HTTP_SERVER_MAX_CLIENTS=4` the two over-limit connections would just wait in the
  TCP backlog, slower but not broken - **did not survive hardware testing**; see the
  correction under "What Zephyr v4.5.0's HTTP server will and won't do" above. The real
  limits are now set explicitly in `firmware/app/prj.conf` and cost ~44 KB of RAM;
  decision 2's bundler trigger is unaffected, since bundling would not have fixed any of
  those limits (one bundle plus the page is still two parallel requests, two parked
  keep-alive connections hold their slots exactly as long as six, and a single large bundle
  against an unpinned window stalls the same way) - though it would cut the number of
  concurrent connections a page load needs, which is a real second-order lever.
- **Flash cost is 20 221 B today** for the whole UI, against 64 MB. RAM cost of serving it is
  zero: static resources send straight from flash. The existing three 16 KB JSON buffers in
  `QcWifiWebTransport` are ADR 0010's cost, not this one's - though decision 6's patches should
  eventually make two of them much smaller, since a delta is tens of bytes, not 16 KB.
- **Shadow DOM means theming goes through CSS custom properties.** A global stylesheet cannot
  reach into a component. That is the idiomatic answer and arguably the better one for a design
  system, but it is a real constraint on anyone expecting to restyle the app from one file: the
  tokens in `index.html` are the seam, and components must consume them rather than hard-code
  colours.
- **Lit is a dependency vendored as a binary-ish blob.** 19 KB of minified JavaScript is
  committed to this repo and cannot be read in review. The mitigations are that it is pinned,
  reproducible from one recorded command, and carries its licence and provenance in a banner -
  the same bargain [ADR 0005](0005-gzip-inflate-miniz.md) already struck with miniz, except
  that this one is checked in rather than fetched.
- **JSON frames and the catalog go out uncompressed**, because the MCU has no compressor. At
  2 015 B for the full catalog that is fine; it is also the reason decision 6's patches matter
  more than they would on a gzipping server.
- **No offline anything.** Without a secure context there are no service workers, so a Home
  Screen shortcut opened out of Wi-Fi range shows a network error rather than a cached shell.
  Given that the board is the only source of data, the cached shell would have nothing to show
  anyway - but the error page is Safari's, not ours, and it will look broken.
- **Nothing here is verified on hardware.** Every measurement is from a Mac against a mock
  board. The page has never been served from flash, never been loaded by an iPhone, and never
  run against Zephyr's own WebSocket implementation - which is a different implementation from
  the mock, with its own framing and its own 256-byte receive buffer.
- **A later move to HTTPS would break `ws://`.** A page served over HTTPS may not open an
  insecure WebSocket. That is not a reason to avoid HTTPS, but it is a coupled change, and
  self-signed certificates on a LAN device are their own project.

## What was actually verified

All on a Mac (macOS 26.6, headless Chrome 157, Node 26.3.1), against `web/poc/tools/mock-server.mjs`:

- The page renders the 3-folder / 48-preset catalog and shows the current preset, at a 390 ×
  844 iPhone viewport with no horizontal overflow.
- A tap sends `switchPreset`, leaves the selection where it was with the row marked pending,
  and moves it only when the mock reports the board's own state ~450 ms later
  (`pending=5 current=2` → `pending=null current=5`).
- Killing the mock mid-session drops the page to `offline` while keeping the catalog on
  screen; restarting it brings the page back to `online` through the backoff with no reload.
- With `--drop-every 1`, a dropped patch leaves the UI stale and pending; the next message's
  sequence gap triggers a `resync`, and the snapshot restores the true state.
- Sizes: `web/poc/tools/embed-assets.py` for the asset totals, esbuild 0.28.2 for every
  framework and decoder figure in this ADR.

Not verified: anything on the board, anything on iOS or Safari, the standalone/home-screen
behaviour, haptics, and 60 fps under a real parameter stream.

## Proposed changes to other issues

For the owner to approve - none of these were changed by this work:

- **#15 (message schema)**: adopt the versioned envelope and the snapshot/patch/`seq`/`resync`/
  `tick` shape from decision 6 rather than re-sending the whole catalog per change, and state
  explicitly that unknown message types are ignored.
- **#75 (live updates)**: fix the shared WebSocket receive buffer first -
  `g_qcWsDataBuffer[256]` is handed to every `websocket_register()` call while
  `CONFIG_WEBSOCKET_MAX_CONTEXTS=2`, so two connected browsers share one buffer. Add the `tick`
  heartbeat, since without it the board is silent when idle and a half-open socket is
  undetectable.
- **#78 (preset switching)**: specify it as confirmed rather than optimistic, including the
  pending state and the timeout, and define the client → board `switchPreset` message.
- **#20 (reconnection)**: capped backoff with jitter plus immediate retry on `online` and
  `visibilitychange`; treat a missed `tick` window as a dead link and close the socket so the
  reconnect path runs; re-request a snapshot on reconnect rather than assuming cached state.
- **New, small**: `CODING_STANDARDS.md` has an "App (JUCE, macOS/iOS)" section and no web
  section at all, which no longer matches what MVP2 is. It needs a short Web UI section
  (formatting, no-build rule, the vendoring rule, the "our own drawing" rule).
- **New, small**: `CONFIG_MDNS_RESPONDER` + `CONFIG_NET_HOSTNAME_ENABLE` for a stable
  `qcremote.local`, so a Home Screen shortcut survives a DHCP lease change.

## Sources

- Zephyr v4.5.0 source, pinned tree: `subsys/net/lib/http/http_server_http1.c`,
  `http_server_core.c`, `http_server_ws.c`, `subsys/net/lib/http/Kconfig`,
  `subsys/net/lib/websocket/websocket.c`, `subsys/net/lib/websocket/Kconfig`,
  `subsys/net/lib/dns/Kconfig`, `subsys/net/Kconfig.hostname`.
- Sizes: measured locally with esbuild 0.28.2 and `python3 -m gzip` level 9 on 2026-11-06;
  package versions as listed in the tables.
- [Lit requirements](https://lit.dev/docs/tools/requirements/) - browser support and the bare
  module specifier statement.
- [Preact 11 release notes](https://preactjs.com/blog/preact-11/) - ESM-only distribution,
  released 2026-09-29.
- [PWAs on iOS and Safari](https://openpwa.net/reference/platforms/ios-safari/) and
  [iOS Add to Home Screen](https://openpwa.net/reference/installation/ios-add-to-home-screen/) -
  `http:` documents are eligible, manifest `display` handling, iOS 16.4/18.4/26 milestones.
- [What's new in web apps (WWDC23)](https://developer.apple.com/videos/play/wwdc2023/10120/) -
  Home Screen web apps, standalone display mode, separate storage.
- [vibration (haptics for the mobile web)](https://github.com/ilyassesalama/vibration) and
  [Haptics on the Web: What's Possible and What's Fake in 2026](https://creativealive.com/haptics-web-whats-possible-whats-fake-2026/) -
  no Vibration API in any iOS Safari, the `<input type="checkbox" switch>` tick, iOS 26.5
  behaviour.
- Knob components surveyed: [GoogleChromeLabs/input-knob](https://www.webcomponents.org/element/input-knob),
  [stagas/x-knob](https://github.com/stagas/x-knob),
  [g200kg/input-knobs](https://g200kg.github.io/input-knobs/),
  [HowdyMoto/Knobs](https://github.com/HowdyMoto/Knobs).
