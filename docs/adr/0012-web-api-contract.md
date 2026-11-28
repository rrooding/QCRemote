# ADR 0012: Web UI ↔ Board API contract

Date: 2026-11-23
Status: Accepted

## Context

[ADR 0011](0011-web-ui-stack.md), decision 6, sketched the wire protocol between the web page
and the Board: JSON in a versioned envelope `{"v":1,"t":"<type>","seq":N,...}`, a snapshot
followed by patches, `resync` on a `seq` gap, and a `tick` heartbeat. Issue #15 asked for it
to be pinned down as a versioned JSON schema.

By the time that happened, the two sides had drifted. `web/poc/src/store.js` expected a
snapshot of `{catalog, current, link}` and shallow-merged patches. The firmware
(`QcLiveUpdateEnvelope.hpp`, from #75) sent a snapshot of the catalog only, and patches whose
only content was `lastChange:{command,count}`, a diagnostic counter of USB command ids that no
UI can use. The firmware also served `GET /api/presets.json` (ADR 0010's smoke test), which
holds its own 16 KB copy of the catalog JSON. It allowed exactly one WebSocket client, where
ADR 0011 assumed two.

The constraints that shaped the contract:

- **RAM is the scarce resource** (ADR 0010, 0011). Flash is not.
- **The page ships inside the firmware image.** Page and firmware are always built together.
- **The page reconnects on its own.** Backoff, plus immediate retry on `online` and
  `visibilitychange` (#20). So a tab opened before a firmware update reconnects to the new
  firmware without reloading, still running the old JavaScript. The Board sent no caching
  headers, so even a manual reload could serve cached old JavaScript.
- **Zephyr v4.5.0 shares one WebSocket receive buffer per resource** (ADR 0010, #91), so a
  second live client needs a patch or a separate resource and more RAM.

## Decision

The contract lives in [`docs/api/`](../api/README.md): a prose README for the rules, JSON
Schemas (2020-12) for the shapes, and example messages that double as test fixtures for both
sides. It refines ADR 0011's decision 6 as follows.

1. **The schema is documentation and fixtures, never firmware code.** The Board's encoder stays
   hand-written. Its unit tests compare output byte for byte with the example files, and a
   host-side script validates the examples against the schemas. The firmware image gains
   nothing.
2. **No protocol version number.** The `v` field is dropped from every message. Instead,
   `snapshot` carries a `build` id, the firmware's git commit. The page has the same id baked
   in, and on a mismatch it reloads itself (at most once per build per tab). Static assets
   are served with `Cache-Control: no-cache`, so the reload actually fetches the new page.
3. **`seq` counts every Board → page message on a connection**, ticks and errors included.
   A snapshot is always 0. After a gap the page sends `resync` and discards everything until
   the next snapshot.
4. **A patch replaces whole top-level sections** (`current`, `link`), with no merging inside a
   section. A Catalog change is always a new snapshot, never a patch. `lastChange` is not part
   of the contract.
5. **Commands carry a page-chosen `id`. Rejections are explicit, success is the state
   change.** The Board answers a rejected command with
   `{"t":"error","id","code","message"}`, using one of the fixed codes `qcDisconnected`,
   `badIndex`, `staleCatalog`, `busy` or `badRequest`. A completed `switchPreset` always
   produces a `current` patch, even if nothing changed.
6. **Presets are addressed by their Catalog-wide index plus the Catalog generation.** The
   Board rejects a stale generation with `staleCatalog`, rather than loading whatever now sits
   at that index.
7. **One live page at a time, and the newest wins.** The Board closes the older socket with
   close code `4001`. That page stops reconnecting and offers to take over.
8. **The WebSocket is the only API.** `GET /api/presets.json` is removed.
9. **Inbound frames are capped at 256 bytes and parsed by a small hand-written extractor**, in
   the same style as the encoder, not by Zephyr's JSON library.
10. **Liveness is a promise:** at least one message every 5 s, and the page treats 12.5 s of
    silence as a dead Connection.

## Consequences

- **The whole contract is in one place and both sides test against the same files.** Drift
  like `lastChange` shows up as a failing test instead of a confused page.
- **Messages shrink.** Each message is 6 bytes shorter without `"v":1,`. Patches carry only
  real UI state. Removing `/api/presets.json` frees its 16 KB buffer, the largest single gain.
- **No negotiation, so no compatibility promise.** A page and a Board from different builds
  never try to talk. The page reloads instead. That is only safe because the Board serves the
  page. A client that isn't served by the Board (a native app, a second Board version, a page
  hosted elsewhere) would need a real version field, and a future ADR to bring one back.
- **`curl`-ing the catalog no longer works.** A WebSocket client (`websocat`, or the browser
  console) is needed to inspect the Board's state. The examples and the mock server cover most
  development needs.
- **Single client is a product limit, not just a resource limit.** A phone and a Mac can't
  both be live. Making that possible is #91, which now has to keep the `4001` behaviour for
  the client count it supports.
- **The firmware gains state.** To send `busy` and `staleCatalog`, and to always confirm a
  switch, it must track one in-flight switch with a timeout and know the current generation
  when a command arrives.
- **Extending is additive.** New message types and sections (#21–23) need no coordination,
  because both sides ignore what they don't know. Changing the meaning of an existing field
  is only safe because the page and firmware always ship together. Once that stops being
  true, see the previous bullet about bringing a version back.
- **Unspecified for now:** what the Catalog and `current` show while the Link is down. That is
  left to a later feature.
