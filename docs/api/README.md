# Web UI ↔ Board API contract

The contract between the web page and the Board's embedded web server. The page is served
by the Board itself, from the same firmware image, so the two always ship together.
Terms (Board, Quad Cortex, Link, Connection, Catalog, Generation, Preset, Current preset) are
defined in [`GLOSSARY.md`](../../GLOSSARY.md). The reasoning behind the less obvious choices is
in [ADR 0012](../adr/0012-web-api-contract.md).

| File | What it is |
| --- | --- |
| [`schema/server-message.schema.json`](schema/server-message.schema.json) | Every Board → page message (JSON Schema 2020-12) |
| [`schema/client-message.schema.json`](schema/client-message.schema.json) | Every page → Board message |
| [`examples/`](examples/) | One message per file, byte for byte as the firmware emits it. `examples/invalid/` holds messages the schemas must reject |
| [`check_examples.py`](check_examples.py) | `uv run docs/api/check_examples.py` checks every example against the schemas |

The schemas and examples are documentation and test fixtures only. Nothing here is compiled
into the firmware: the Board's encoder stays hand-written, and its unit tests compare its
output against the example files.

## CI

The [`api-contract`](../../.github/workflows/api-contract.yml) workflow runs on every pull
request, and on every push to `master`, that touches `docs/api/**`, `web/ui/**` or the
workflow file. It has two jobs:

- **`check-examples`** runs `uv run docs/api/check_examples.py`. A change that breaks an
  example, or the schema it is checked against, fails it. The checker verifies that every
  example validates (and every `examples/invalid/` one is rejected), that examples are compact,
  and that page → Board frames are at most 256 bytes.
- **`web-ui-tests`** runs `node --test 'web/ui/test/*.test.js'` (Node 22): the page's Store and the dev
  mock Board, both tested against these examples.

Neither installs any of the firmware toolchain, so the workflow is independent of the
`firmware` one. The `firmware` workflow also runs on changes to `docs/api/**` and `web/ui/**`,
because its `native_sim` suites compare the Board's frames with these examples byte for byte
and embed `web/ui` into the image. Run the same commands locally before pushing.

This document covers the rules a schema can't express. When it and a schema disagree, fix
whichever is wrong in the same change.

## HTTP

| Path | What |
| --- | --- |
| `/`, `/src/*`, … | The page's static assets, pre-gzipped at build time, sent with `Content-Encoding: gzip` and `Cache-Control: no-cache` |
| `/ws` | The WebSocket. Everything else in this document happens here |

There is no JSON over plain HTTP. The page gets all of its state from the WebSocket.

`Cache-Control: no-cache` means the browser revalidates every asset, so a reload after a
firmware update never runs old JavaScript against new firmware. See [Build id](#build-id).

## WebSocket

Text frames, one JSON object per frame, UTF-8, no whitespace. Every message has a type `t`.

### Board → page

| `t` | When | Carries |
| --- | --- | --- |
| `snapshot` | On connect, after a `resync`, and whenever the Catalog changes | `build`, `catalog`, `current`, `link`: the complete state |
| `patch` | When `current` or `link` changes | Only the sections that changed |
| `tick` | When nothing else has been sent for the tick interval | Nothing |
| `error` | When the Board rejects a command | `id`, `code`, `message` |

```json
{"t":"snapshot","seq":0,"build":"85ebe14","catalog":{"generation":7,"presetCount":4,"incomplete":false,"folders":[{"name":"My Presets","presets":[{"index":0,"name":"Clean Chime"},{"index":1,"name":"Edge Crunch"}]},{"name":"Live Set","presets":[{"index":2,"name":"Opener"},{"index":3,"name":"Verse 1"}]}]},"current":{"index":2,"name":"Opener","scene":null},"link":{"qcConnected":true}}
{"t":"patch","seq":1,"current":{"index":3,"name":"Verse 1","scene":null}}
{"t":"tick","seq":2}
{"t":"error","seq":3,"id":"c7","code":"staleCatalog","message":"generation 6 != 7"}
```

### Page → Board

| `t` | When | Carries |
| --- | --- | --- |
| `resync` | After the page sees a `seq` gap | Nothing |
| `switchPreset` | The user picked a Preset | `id`, `index`, `generation` |

```json
{"t":"resync"}
{"t":"switchPreset","id":"c7","index":3,"generation":7}
```

A page → Board frame is at most **256 bytes**. `id` is 1–16 characters of `[0-9A-Za-z_-]`.

The page sends `t` and `id` exactly as written above, with no JSON escapes in them. The
Board reads both without unescaping, so a frame that spells either with an escape (for
example `{"t":"re\u0073ync"}`, which a general JSON parser reads as `resync`) is a
`badRequest`. Other string members, which the Board doesn't read, may contain escapes.

## Sequence numbers

`seq` counts every Board → page message on one WebSocket connection, ticks and errors
included.

- A `snapshot` always has `seq` 0. It resets the count.
- Every later message has exactly the previous `seq` + 1.
- Anything else is a **gap**: the Board dropped a message, so the page's state can no longer
  be trusted. The page sends `resync` and then **discards every message until the next
  `snapshot`**, because patches that were already on their way apply to a state it no longer
  has.

The page sends nothing on connect: the Board sends a `snapshot` as soon as the WebSocket opens.

## Patches

Each top-level section in a `patch` (`current`, `link`) **replaces** that section of the
page's state. There is no merging inside a section, so a field going back to `null` is just a
section with that field set to `null`.

The Catalog never changes by patch. When the Board re-reads the listing, it sends a new
`snapshot` with a new `generation`.

## Liveness

The Board sends at least one message every **5 s**
(`CONFIG_QC_WIFI_WEB_TICK_INTERVAL_MS`). If nothing else went out in that window, it sends
a `tick`.

The page treats **12.5 s** of silence (2.5 intervals) as a dead Connection. It closes the
socket and reconnects, rather than waiting for TCP to notice a half-open connection after a
Wi-Fi drop.

Change both numbers together, here and in the page.

## Switching a Preset

1. The page sends `switchPreset` with the Preset's Catalog-wide `index` and the `generation`
   of the Catalog it was showing. The page marks that Preset *pending*, not selected.
2. The Board either:
   - **rejects it** with an `error` carrying the same `id`, or
   - asks the Quad Cortex to load it. Once the Quad Cortex reports the load, the Board sends
     a `current` patch. It **always** sends this patch when a switch completes, even if
     `current` didn't change (the user tapped the Preset that was already loaded).
3. The page clears *pending* when `current.index` matches, and the selection moves.
4. If neither arrives within **4 s**, the page clears *pending* and shows that the Board didn't
   confirm the switch. The Board gives up on its side of an in-flight switch within the same
   4 s, so a lost switch never leaves it stuck at `busy`.

Success never gets a message of its own. The state change is the confirmation (ADR 0011,
decision 5: confirmed, not optimistic).

## Errors

| `code` | Meaning |
| --- | --- |
| `qcDisconnected` | The Link is down, so the Board can't reach the Quad Cortex |
| `badIndex` | No Preset with that `index` in the current Catalog |
| `staleCatalog` | `generation` isn't the Board's current one. The page is showing an old Catalog, and a fresh `snapshot` is already on its way or has arrived |
| `busy` | Another switch is still in flight. The Board doesn't queue or replace it |
| `badRequest` | The frame isn't JSON, is over 256 bytes, has an unknown `t`, or is missing or has malformed fields. `id` is present only if one could be read |

`message` is English detail for the console and logs. The page never shows it. It maps `code`
to its own wording.

## Build id

There is no protocol version number. The page and the firmware are built into the same image,
so they can only disagree when a page that was loaded *before* a firmware update reconnects
to the new firmware. The page's reconnect logic makes this likely, not rare.

- `snapshot.build` is the firmware's build id, the git commit it was built from, with
  `-dirty` appended when it was built from uncommitted changes (`unknown` outside a git
  checkout).
- The page has the same id baked in by the asset embed step at build time.
- When they differ, the page calls `location.reload()` and doesn't apply the snapshot.
  To guard against a reload loop (an asset cache that ignores `no-cache`), it reloads at most
  once per Board build id per tab.

## One page at a time

The Board serves **one** live WebSocket at a time. RAM is the constraint (ADR 0010, #91).

When a second page connects, the **newest one wins**. The Board closes the older socket with
close code **`4001` (`replaced`)**, and that page shows "Opened on another device" and
**stops reconnecting**. It offers a button to take over, which connects again and in turn
replaces the other page. Two pages never fight over the socket on their own.

| Close code | Meaning | Page reconnects? |
| --- | --- | --- |
| `4001` | Replaced by a newer page | No, only when the user takes over |
| anything else | Dropped | Yes, with backoff (#20) |

## Extending the contract

- Both sides **ignore message types and fields they don't know**. This is the extension point:
  scenes (#21), parameters (#22), footswitches (#23) and so on add new `t` values and new
  sections later, without breaking anything.
- The Board answers a page → Board `t` it doesn't know with `badRequest`.
- Add the message to the schema, add at least one example (and an invalid one if there's a
  rule worth pinning down), update this document, and run `check_examples.py`.

## Current limits

These come from the firmware's static buffers, not from the contract. The schema doesn't
enforce them, so the page must not depend on them.

- A `snapshot` is at most 16 KB plus a 256 B envelope (`QcWebApiSession::kCatalogJsonCapacity`,
  `kEnvelopeOverhead`). A Catalog that doesn't fit is cut at a Folder or Preset boundary and
  sent with `incomplete:true`.
- At most 8 Folders of 64 Presets (`QcPresetCatalog`, ADR 0008). Names are truncated to 32
  characters.

## Not specified yet

- **What the Catalog and `current` show while the Link is down**, and how they recover when it
  comes back. Today the contract only has `link.qcConnected` and the `qcDisconnected` error.
  Tracked separately as a post-MVP2 feature.
