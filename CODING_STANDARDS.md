# Coding Standards

These are binding for all contributions unless a PR discussion explicitly revisits one.
Adapted from [QCPresetLeveler](https://github.com/rrooding/QCPresetLeveler/blob/main/CODING_STANDARDS.md)
for this project's current shape: an embedded **firmware** side (devboard, USB host bridge to
the Quad Cortex, a Wi-Fi web UI served from the board) and the **web UI** itself (plain ES
modules, no build step). There is no longer a separate native client app — the originally
planned JUCE/BLE macOS/iOS app (MVP2) was dropped in favor of a web UI served by the firmware
over Wi-Fi ([issue #71](https://github.com/rrooding/QCRemote/issues/71),
[ADR 0010](docs/adr/0010-wifi-web-transport.md),
[ADR 0011](docs/adr/0011-web-ui-stack.md)), and the BLE/JUCE issues were closed as not planned.
BLE stays possible later as an additional transport behind `QcPresetTransport` — see "Open
questions" below — but no BLE code exists today.

The general C++ rules below apply to all C++ in the repo; today that means the firmware only.
The **Firmware** section layers Zephyr-specific conventions (concurrency primitives,
devicetree, Kconfig, ISR safety) on top of it, following
[Zephyr's own coding guidelines](https://docs.zephyrproject.org/latest/contribute/coding_guidelines/index.html)
where they're genuinely about the OS rather than the language. The **Web UI** section covers
the embedded page's own conventions, which are a different language and a much shorter list.

---

## C++ (language & style)

This section is the project's general C++ policy. The **Firmware** section below refers back
to it rather than repeating it — if a firmware-specific rule conflicts with something here,
the firmware section's version wins for firmware code, but that split should be rare and is
called out explicitly where it exists (exceptions/RTTI, below).

### Language

- **C++23**, built via the toolchains the firmware actually uses (the Zephyr SDK's
  arm-zephyr-eabi GCC/Clang, and Xtensa GCC for the ESP32-S3 target) — see
  [firmware/README.md](firmware/README.md) for the pinned versions. If a toolchain's headers
  don't fully support C++23 cleanly, that's resolved by fixing the build, not by quietly
  downgrading the standard.
- Prefer standard-library facilities over hand-rolled equivalents when both exist and neither
  has a real advantage (`std::optional` over a sentinel value).
- Use `std::expected<T, E>` for fallible operations off any time-sensitive path — protocol
  parsing, device enumeration, session setup — reserving exceptions (where enabled at all; see
  the Firmware section) for truly exceptional, non-time-sensitive paths, caught close to the
  call site.

### Time-sensitive code (non-negotiable)

Some code in this project runs on a path where a stall has a real, user-visible consequence —
not a hard ISR deadline necessarily, but close enough to one that the same discipline applies.
Two concrete examples in the firmware today: the USB HID receive path
(`QcHidBridge::onReportIn()`, `firmware/app/src/QcHidBridge.hpp`) and the ~200ms KeepAlive
heartbeat (`QcSession`, [issue #8](https://github.com/rrooding/QCRemote/issues/8)) — a stalled
callback on either can desync a session or drop a report. Code on a path like this must not:

- allocate or free memory (no `new`, no container growth that can reallocate, no
  `std::string` construction)
- take a lock that can block for an unbounded time
- log, print, or otherwise do blocking I/O
- throw or catch exceptions

This isn't theoretical: `QcHidBridge::feedReport()`'s `Overflow` case deliberately avoids
`LOG_WRN` on the `usbh` thread specifically because `CONFIG_LOG_MODE_IMMEDIATE` formats
synchronously there, and that exact pattern overflowed its 1536-byte stack once already — it
increments an atomic counter instead, surfaced later by the heartbeat thread. See that file's
comments for the full story.

Communication between a time-sensitive path and the rest of the system goes through lock-free
or bounded-wait structures (`std::atomic`, a fixed-capacity queue/ring buffer, a semaphore used
only to signal) — never a mutex a time-sensitive path itself has to block to acquire. A PR
touching a path like this should call out in its description how that was preserved.

### Ownership & memory

- RAII everywhere. No raw owning pointers — `std::unique_ptr` for exclusive ownership.
- No `new`/`delete` directly; use `std::make_unique` or a project factory helper.
- Pass non-trivial types by `const&` unless ownership is actually transferred (then take by
  value and `std::move`).

### References and pointers

Preference order: **reference > smart pointer > raw pointer**. Reach for a raw pointer only
when neither of the first two fits — the deliberate exception, not the default.

A raw pointer is legitimate when:

- **Nullability is the actual point** — e.g. an accessor that can legitimately report "no
  Quad Cortex currently connected", where `std::optional<T&>` isn't available pre-standard.
- **A library interface mandates it.** Zephyr's C APIs (`k_*` calls, driver structs) and its
  code-generating macros are the concrete case here: `HTTP_SERVICE_DEFINE`/
  `HTTP_RESOURCE_DEFINE` (`QcWifiWebTransport.hpp`) generate file-scope static tables that the
  HTTP server discovers resources through, with plain C function-pointer callbacks — which is
  why that class routes them through a single static `instance_` pointer rather than a
  capturing closure. `UsbHostClass.hpp`'s trampoline statics follow the same pattern for
  Zephyr's USB host shim. Match the interface; don't wrap it in something else just to avoid
  the word "pointer".
- **You're indexing into a C-style buffer** where `std::span` doesn't fit the call site
  cleanly.

Where a function takes a `(pointer, count)` pair for a buffer *we* control the signature of,
prefer `std::span` over raw pointer + separate length —
`UsbHostClass::trampolineReportIn(const uint8_t* data, size_t len)` does exactly this,
converting the shim's raw callback arguments into `std::span<const uint8_t>` before calling
`onReportIn()`.

### Naming & structure

- Types/classes: `PascalCase`. Functions, methods, variables: `camelCase`. Private/protected
  member variables: trailing underscore (`presetName_`).
- One class per file where reasonable. Project namespace: `qcbridge`. Avoid globals and
  singletons except where a library interface leaves no choice (the static-instance-pointer
  pattern above); prefer passing dependencies in explicitly.
- Small functions, single responsibility, early returns over nested conditionals,
  const-correct by default.

### Header-only implementation

- Implementation code lives entirely in `.hpp` files — `Thing.hpp`, no paired `Thing.cpp`.
  Define methods in-class (implicitly `inline`) or out-of-class marked `inline` explicitly;
  free functions at namespace scope must be `inline` too, to avoid ODR violations.
- `#pragma once` on every header.
- `.cpp`/`.c` files exist only where the toolchain genuinely requires one: Zephyr's
  application entry point, Ztest test binaries, and the narrow C-shim exception the Firmware
  section documents below (`usbh_shim.c`).
- **Known tradeoff:** header-only means every translation unit including a changed header
  gets recompiled, slower than a `.h`/`.cpp` split as the codebase grows. If it becomes
  painful, the mitigation is precompiled headers or a unity build, not reverting to split
  files.

### Comments & documentation

- No comments that restate what the code does — names should already say that.
- A comment is warranted only for the non-obvious *why*: a hidden constraint, a protocol
  quirk (e.g. "session drops if KeepAlive lapses >200ms per qc-mcp's PROTOCOL.md"), a
  workaround for a specific bug.
- No mandatory Doxygen blocks on every function. Brief header-level summaries are fine on
  genuine module boundaries (the Wi-Fi/WebSocket transport, the USB HID bridge, the command
  protocol layer) where a reader can't infer intent from the file alone.

### Formatting

- A root `.clang-format` ([tracked in #30](https://github.com/rrooding/QCRemote/issues/30),
  not yet added): LLVM base style, 4-space indent, ~110 column limit, covering all C++ in the
  repo. Until it lands, match the style of the surrounding file.
- Intended to be enforced in CI once #30 lands — a PR that isn't clang-format-clean should
  fail the build.

### Static analysis

- `clang-tidy`, intended check groups `cppcoreguidelines-*`, `performance-*`, `bugprone-*`,
  and `modernize-*`. Not yet wired into CI — re-evaluate specific check-group exclusions
  (Zephyr macro-generated code tripping `cppcoreguidelines-*`, etc.) once it is.
- Suppressions require a `// NOLINT(check-name): reason` comment — a bare `NOLINT` doesn't
  pass review.

---

## Firmware (devboard, Zephyr)

The devboard side runs on Zephyr, a real-time OS with its own long-established conventions —
but unlike a typical Zephyr module, this firmware is **C++23**, not C. The RTOS-specific rules
below (concurrency primitives, devicetree, Kconfig, ISR safety) follow
[Zephyr's own coding guidelines](https://docs.zephyrproject.org/latest/contribute/coding_guidelines/index.html)
where they're genuinely about the OS rather than the language — but naming, formatting,
ownership, and error-handling style follow the **C++ (language & style)** section above, not
Zephyr's C/kernel conventions, so the codebase doesn't drift into two different dialects for
no reason.

### Language & style

- **C++23**, enabled via `CONFIG_CPP=y` + `CONFIG_STD_CPP23=y`. Confirmed present in Zephyr
  v4.5.0 (`lib/cpp/Kconfig`), the release [firmware/README.md](firmware/README.md) pins — see
  [firmware/app/prj.conf](firmware/app/prj.conf) for the actual config.
- `CONFIG_CPP_EXCEPTIONS=n` and `CONFIG_CPP_RTTI=n` — no exceptions, no RTTI, for the whole
  firmware image, stricter than the shared section's general "off the time-sensitive path"
  default, since there's no OS-level crash recovery on a microcontroller. Use
  `std::expected<T, E>` for fallible operations (protocol parsing, session setup, device
  enumeration) instead.
- Naming, ownership, and references/pointers follow the **C++ (language & style)** section
  verbatim. Zephyr's own C APIs (`k_*` calls, driver structs) are that section's "library
  interface mandates it" case — call them directly (Zephyr's headers already handle
  `extern "C"` linkage), and wrap the ones you take/release around a scope in a small RAII
  guard (e.g. a `ScopedKMutex` around `k_mutex_lock`/`k_mutex_unlock`) rather than pairing
  lock/unlock calls by hand.
- Formatting and the header-only `.hpp` convention follow the shared section above, unchanged.
- **Narrow exception: a plain-C shim file is allowed where a Zephyr header genuinely isn't
  C++-safe**, discovered bringing up issue #3 — `zephyr/usb/usbh.h` has a struct field
  literally named `class` (a C++ reserved word), and `zephyr/drivers/usb/uhc.h` relies on
  C's implicit `void*` conversions in several inline functions. Both are real bugs in
  Zephyr's public API, not something our code can work around from the C++ side. See
  [`usbh_shim.c`](firmware/app/src/usbh_shim.c)/[`usbh_shim.h`](firmware/app/src/usbh_shim.h)
  for the pattern: a `.c` file (compiled under C, where both issues don't apply) exposing a
  minimal, C++-safe API, with a small C++ base class
  ([`UsbHostClass.hpp`](firmware/app/src/UsbHostClass.hpp)) on top so the rest of the
  firmware never includes a Zephyr USB header directly. This is the same reasoning as the
  shared section's `.cpp`-file exceptions — a toolchain/interop necessity, not a style
  choice — scoped to wherever a header actually forces it, not a general excuse for `.c`
  files. Worth reporting upstream to Zephyr regardless of this workaround.

### Hardware description

- Board/pin/peripheral configuration belongs in **devicetree** overlays (`.overlay`,
  `.dts`), not hardcoded GPIO numbers or register addresses in C. Use `DT_` macros
  (`DT_NODELABEL`, `DT_PROP`, etc.) to pull configuration from the devicetree at compile time.
- Feature toggles and module selection go through **Kconfig** (`CONFIG_*` symbols with `.conf`
  files per board), not `#ifdef` soup with project-invented macros. New Kconfig options need
  help text explaining what they do and why the default is what it is.
- This matters concretely for the USB host bring-up
  ([#3](https://github.com/rrooding/QCRemote/issues/3)): interface/endpoint config should
  come from devicetree/Kconfig where the USB host stack supports it, not be hand-rolled.

### Concurrency & interrupt safety

- Use Zephyr kernel primitives for synchronization and hand-off: `k_mutex`, `k_sem`,
  `k_msgq`, `k_fifo`/`k_lifo`, and `k_work`/work queues for deferred processing. Don't roll
  custom spinlocks or busy-wait loops.
- **ISR context is minimal by construction.** USB host HID report callbacks that run in
  interrupt or ISR-adjacent context must not block — no `k_mutex_lock` with a non-zero
  timeout, no `k_sleep`, no blocking log backend calls. Hand off to a `k_work` item or push
  into a `k_msgq`/ring buffer and process on a thread instead. This is directly relevant to
  the ~200ms KeepAlive heartbeat ([#8](https://github.com/rrooding/QCRemote/issues/8)) — any
  lock on the USB send path should be a `k_mutex` taken from thread context, never from the
  callback that triggers it.
- Give threads explicit, documented priorities (the KeepAlive timer thread should not be
  starved by a lower-priority thread) rather than relying on Zephyr's defaults.

### Memory & containers

- Prefer static allocation: `k_mem_slab`, fixed-size `k_heap` regions, or plain
  static/const buffers sized for the known worst case (a single HID-framed message, one
  inflated protobuf payload) over unbounded dynamic allocation. Flash/RAM budgets on a
  microcontroller make an OOM at 2am on stage a real failure mode, not a theoretical one.
- Size the nanopb and inflate (miniz or similar) buffers statically per the message types
  actually needed rather than allocating generously "to be safe" — the protocol's own
  128-byte report cap gives a hard upper bound to design against.
- **Use [ETL](https://github.com/ETLCPP/etl) instead of the standard library for any
  container whose own storage would otherwise heap-allocate.** `etl::vector<T, N>` instead of
  `std::vector<T>`, `etl::string<N>` instead of `std::string`, `etl::map`/`etl::flat_map`
  instead of `std::map`/`std::unordered_map`, `etl::deque`/`etl::queue`/`etl::stack`/
  `etl::circular_buffer` for their std:: equivalents. ETL's containers take their capacity as
  a compile-time template parameter and hold their storage inline (static or on the stack),
  so they never call the heap — this is the concrete mechanism behind the static-allocation
  rule above, not a separate concern from it.
- `std::optional`, `std::expected`, `std::span`, `std::string_view`, and `std::variant` stay
  as `std::`, not ETL — they don't own heap-allocating storage themselves (a `std::span` over
  an `etl::vector`'s buffer is normal and expected), so there's nothing for ETL to improve
  there. The swap is specifically for the container types that would otherwise grow via the
  heap.
- No `new`/`delete`, `std::make_unique` over a dynamically-sized buffer, or STL container
  growth (`push_back` past a reserved capacity, etc.) in firmware code, enforced harder here
  than the shared section's general rule since there's no heap fragmentation budget to spend.

### Logging

- Use Zephyr's logging subsystem (`LOG_MODULE_REGISTER`, `LOG_INF`/`LOG_WRN`/`LOG_ERR`/
  `LOG_DBG`) instead of `printk`/`printf` for anything beyond a throwaway bring-up hack.
  Log level is per-module and controlled via Kconfig (`CONFIG_<MODULE>_LOG_LEVEL`).
- Never call a blocking log backend from ISR or ISR-adjacent context — Zephyr's deferred
  logging mode exists for exactly this. Where a thread runs with
  `CONFIG_LOG_MODE_IMMEDIATE` (e.g. `usbh`), treat *every* enabled log level on that thread as
  effectively synchronous and budget its stack accordingly, same as
  `QcHidBridge.hpp`'s `feedReport()` does for its `Overflow` case.

### Error handling

- Zephyr APIs return negative `errno` codes (or a subset per-API). Check every `k_*`/driver
  return value that can fail — don't discard them, even for calls that "shouldn't" fail on a
  known-good devkit. Wrap the boundary between Zephyr's `errno`-returning C APIs and the
  firmware's own C++ logic with `std::expected<T, int>` (or a small project-specific error
  enum), per the shared section's `std::expected` rule, rather than propagating raw `int`
  return codes through call sites that aren't talking to Zephyr directly.

### Testing

- Use Zephyr's **Ztest** framework, run via **Twister**, targeting `native_sim` (or QEMU)
  for anything that doesn't require real hardware — the HID framing/reassembly logic, the
  protobuf message decode, the session state machine, the Wi-Fi/WebSocket JSON encoding. This
  mirrors the shared section's principle of decoupling logic from hardware/OS glue so it's
  actually testable in CI.
- Ztest's macros (`ZTEST()`, `ZTEST_SUITE()`) are plain C macros and do expand fine in a C++
  translation unit, but the framework itself is written C-first — keep test bodies thin,
  calling into ordinary C++ functions/classes for the actual logic under test, rather than
  fighting the framework's assumptions with heavier C++ fixtures.
- Hardware-dependent paths (actual USB enumeration against a real Quad Cortex Mini, actual
  Wi-Fi radio behavior) stay as manual/hardware-in-the-loop validation steps
  ([#10](https://github.com/rrooding/QCRemote/issues/10)) rather than forced into Twister.

### Build

- `west build -b <board>`, warnings-as-errors via
  `CONFIG_COMPILER_WARNINGS_AS_ERRORS=y` (or equivalent `-Wall -Wextra -Werror` extra
  flags). CI builds run in a pinned container
  (`ghcr.io/rrooding/qcremote-firmware`, [#46](https://github.com/rrooding/QCRemote/issues/46))
  as parallel per-target jobs — see [firmware/README.md](firmware/README.md) for local setup.

---

## Web UI (Lit, no build step)

The board serves a web UI over Wi-Fi/HTTP instead of a native app
([ADR 0010](docs/adr/0010-wifi-web-transport.md)). The stack for that UI, and the reasoning
behind it, is [ADR 0011](docs/adr/0011-web-ui-stack.md) — this section states the conventions
that follow from it rather than repeating the research; read the ADR for the "why".

- **Lit 3, plain ES modules, no build step.** Templates are authored with Lit's `html`/`css`
  tagged literals and served as-is; the one real requirement (bare module specifiers) is met
  by committing a single pinned, vendored Lit bundle rather than adding a bundler. The
  firmware build stays Python-only — no Node dependency for `west build`.
- **DOM + CSS** for lists, buttons and panels (preset list, scenes, footswitches). **SVG** for
  the grid/signal chain, knobs and meters — elements with real identity that need hit testing
  and styling. **Canvas** for nothing yet; it's reserved for a genuinely continuous, high-rate
  display (an output meter) if one ever needs it, not a default choice.
- **Assets are pre-gzipped at build time into a generated C++ header**, served as static HTTP
  resources with `Content-Encoding: gzip` — costing flash, not RAM, per ADR 0011's own
  measurements.
- **JSON over WebSocket**, in a versioned envelope (`{"v":1,"t":"<type>","seq":N,...}`) with
  unknown message types ignored by the client, so the board can add messages without breaking
  a page served from older flash. The full message schema is tracked in
  [#15](https://github.com/rrooding/QCRemote/issues/15).
- **Our own drawing, always.** No Neural DSP artwork, icons, typeface or color values are
  copied from the device or its companion app — only the Quad Cortex's layout and interaction
  model (a folder list, a current-preset header, a row of scene buttons) is reproduced.
- **No JS/CSS formatting or lint tooling is enforced yet.** If the UI outgrows a handful of
  files, that's a question for [#30](https://github.com/rrooding/QCRemote/issues/30)
  (which already notes it as an open option, e.g. Prettier) rather than something to decide
  here first.

The planned location for this code is **`web/ui/`**; ADR 0011's own research prototype lives
in `web/poc/` and is frozen as a reference once the real UI replaces it. The implementation
([issue #88](https://github.com/rrooding/QCRemote/issues/88)) is in review as of this writing
and not yet merged — treat the exact source layout and tooling under `web/ui/` as the target
shape this section describes, not yet as shipped fact, until that lands.

---

## Workflow

`master` is a protected branch: no direct pushes, no force-pushes, no deletion, enforced even
for admins. New functionality — firmware or web UI, any change beyond a trivial doc/typo fix —
goes through a **pull request linked to the backlog issue it implements** (`Closes #N` or
`Refs #N` in the PR description, same convention as the commit-message footer below). No
required-approver count is set, so a solo PR can still be merged without waiting on a second
reviewer — the point is the paper trail (what issue a change addresses, reviewable diff
before it lands), not gatekeeping.

## Recording decisions

Consequential architecture/tooling decisions (devkit/RTOS choice, transport choice, language
standard version, similar one-way-door calls) get recorded as an ADR under
[`docs/adr/`](docs/adr/), using the template there — see
[ADR 0001](docs/adr/0001-record-architecture-decisions.md) for the format and the reasoning
behind using ADRs at all. Not every note belongs in an ADR: protocol findings, bring-up
quirks, and other non-decision write-ups stay as regular docs.

## Commit messages (both sides)

[Conventional Commits](https://www.conventionalcommits.org/), with linting planned in CI
([#28](https://github.com/rrooding/QCRemote/issues/28), not yet wired up) so a changelog can
eventually be generated rather than hand-written.

```
<type>[optional scope]: <description>

[optional body]

[optional footer(s)]
```

- **Type**: `feat`, `fix`, `docs`, `style`, `refactor`, `perf`, `test`, `build`, `ci`,
  `chore`, `revert`.
- **Scope** is optional but encouraged, matching the areas the roadmap now splits along:
  `firmware`, `usb`, `protocol`, `wifi`, `web`, `build`, `ci`, `docs`, `adr` — e.g.
  `feat(firmware): implement KeepAlive heartbeat` or `feat(web): add scene bar`. `ble` is
  **reserved**, not in active use, for a possible future BLE transport. This list matches
  [#28](https://github.com/rrooding/QCRemote/issues/28)'s proposal — if that issue's scope
  list changes before it lands, update this list to match rather than let the two drift.
- **Breaking changes**: mark with `!` after the type/scope (`feat(protocol)!: ...`) and/or a
  `BREAKING CHANGE:` footer explaining the impact.
- Reference the backlog issue a commit addresses in the footer where it applies (`Refs #8`,
  `Closes #8`).
- Description is imperative mood, no trailing period (`add`, not `Added`/`adds`). Don't
  capitalize the first word, but embedded acronyms/proper nouns (`USB`, `HID`, `BLE`) are
  fine — the subject just can't be Sentence case/Start Case/PascalCase/UPPERCASE as a whole.

## Open questions to flag rather than assume

- Devkit/RTOS choice: **resolved** — [ADR 0002](docs/adr/0002-devkit-selection.md), NXP
  FRDM-RW612 on Zephyr v4.5.0, `CONFIG_STD_CPP23` confirmed present. An ESP32-S3 second
  target exists per [ADR 0009](docs/adr/0009-esp32-s3-second-target.md); the C++23 language
  rules above (naming, RAII, `std::expected`, no exceptions/RTTI, shared `.clang-format`)
  carry over unchanged to it.
- Web UI stack: **resolved** — [ADR 0011](docs/adr/0011-web-ui-stack.md). A future BLE
  transport alongside or instead of Wi-Fi remains possible behind `QcPresetTransport`
  (ADR 0010) but isn't planned work today.
- Exact clang-format column width/brace style, and clang-tidy check exclusions: settle once
  [#30](https://github.com/rrooding/QCRemote/issues/30) lands and real tooling runs against
  the current codebase, rather than guessing upfront.
- Firmware coverage/test gating thresholds: no number set yet — pick one once there's a
  reason to gate on it.
