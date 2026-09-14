# Coding Standards

These are binding for all contributions unless a PR discussion explicitly revisits one.
Adapted from [QCPresetLeveler](https://github.com/rrooding/QCPresetLeveler/blob/main/CODING_STANDARDS.md)
for this project's split shape: an embedded **firmware** side (devboard, protocol bridge to
the Quad Cortex) and an **app** side (JUCE, macOS/iOS client). Rules below are scoped to
whichever side they apply to — most of the C++/JUCE section carries over unchanged; the
firmware section is new and Zephyr-specific, since the devkit/RTOS choice
([#1](https://github.com/rrooding/QCRemote/issues/1)) is expected to land on Zephyr.

---

## App (JUCE, macOS/iOS)

### Language

- **C++23**, no exceptions across the audio/UI boundary described below. All CI platforms
  must build with a C++23-conformant compiler (GCC 13+, Clang 16+, MSVC 19.36+). If JUCE's
  own headers don't fully support C++23 cleanly, that's resolved by fixing the build, not by
  quietly downgrading the standard.
- Prefer standard-library facilities over JUCE equivalents when both exist and neither has a
  real advantage (`std::optional` over a sentinel value), but use JUCE's types when
  interoperating with JUCE APIs (`juce::String` at UI boundaries) rather than converting back
  and forth needlessly.
- Use `std::expected<T, E>` for fallible operations off the real-time path — BLE/MIDI parsing,
  device enumeration, session/preset-cache I/O. Reserve exceptions for truly exceptional,
  non-real-time paths (a malformed cache file), caught close to the call site — never let one
  cross into a real-time callback.

### Real-time thread safety (non-negotiable)

If this app ends up doing anything on an audio callback (`processBlock` or equivalent) —
today it's a control-only client, but don't assume that stays true — that code must not:

- allocate or free memory (no `new`, no container growth that can reallocate, no
  `std::string`/`juce::String` construction)
- take a lock or mutex
- log, print, or otherwise do blocking I/O
- throw or catch exceptions

The same discipline applies to the BLE/MIDI receive callback: it's not the audio thread, but
it *is* time-sensitive (the devboard's ~200ms KeepAlive cadence means a stalled callback can
desync a session), so treat it as a soft real-time boundary. Communication between any
time-sensitive callback and the UI thread goes through lock-free structures (`std::atomic`,
`juce::AbstractFifo`, or a single-producer/single-consumer ring buffer) — never a
mutex-guarded shared object. A PR touching this path should call out in its description how
thread-safety was preserved.

### Ownership & memory

- RAII everywhere. No raw owning pointers — `std::unique_ptr` for exclusive ownership,
  `juce::ReferenceCountedObjectPtr` only where a JUCE API demands reference counting.
- No `new`/`delete` directly; use `std::make_unique` or JUCE's equivalent factory helpers.
- Pass non-trivial types by `const&` unless ownership is actually transferred (then take by
  value and `std::move`).

### References and pointers

Preference order: **reference > smart pointer > raw pointer**. Reach for a raw pointer only
when neither of the first two fits — the deliberate exception, not the default.

A raw pointer is legitimate when:

- **Nullability is the actual point** — e.g. a "no BLE peripheral connected" or "no MIDI
  output selected" accessor returning `juce::MidiOutput*`, where `std::optional<T&>` isn't
  available pre-standard.
- **A library interface mandates it** — JUCE's own APIs are pointer-based in places we don't
  control (`AudioIODeviceCallback`'s `const float* const*` buffers,
  `audioDeviceAboutToStart(juce::AudioIODevice* device)`). Match the interface; don't wrap it
  in something else just to avoid the word "pointer". Same applies to the native
  CoreBluetooth bridge if the custom-GATT path from
  [#14](https://github.com/rrooding/QCRemote/issues/14) is chosen — Objective-C++ bridging
  code will be pointer-heavy by necessity.
- **You're indexing into a C-style buffer** where `std::span` doesn't fit the call site
  cleanly.

Where a function takes a `(pointer, count)` pair for a buffer *we* control the signature of,
prefer `std::span` over raw pointer + separate length.

### Naming & structure

- Types/classes: `PascalCase`. Functions, methods, variables: `camelCase`. Private/protected
  member variables: trailing underscore (`presetName_`). *(Departs from JUCE's own no-prefix
  convention — flag it if you'd rather match JUCE's house style instead.)*
- One class per file where reasonable. Project namespace: `qcbridge`. Avoid globals and
  singletons except where a JUCE API leaves no choice (`JUCEApplication`); prefer passing
  dependencies in explicitly.
- Small functions, single responsibility, early returns over nested conditionals,
  const-correct by default.

### Header-only implementation

- Implementation code lives entirely in `.hpp` files — `Thing.hpp`, no paired `Thing.cpp`.
  Define methods in-class (implicitly `inline`) or out-of-class marked `inline` explicitly;
  free functions at namespace scope must be `inline` too, to avoid ODR violations.
- `#pragma once` on every header.
- `.cpp` files exist only where the toolchain genuinely requires one: the JUCE application
  entry point (`Main.cpp`, `START_JUCE_APPLICATION`) and Catch2 test binaries.
- **Known tradeoff:** header-only means every translation unit including a changed header
  gets recompiled, slower than a `.h`/`.cpp` split as the codebase grows. If it becomes
  painful, the mitigation is precompiled headers or a unity build, not reverting to split
  files.
- **Known gotcha:** a class using `JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR` and then used
  as a plain value member elsewhere can have its implicit default constructor silently
  deleted (self-referential `LeakedObjectDetector<OwnerClass>`, evaluated while the class is
  incomplete). Declare `ClassName() = default;` explicitly rather than debugging the
  resulting "field has no default constructor" error.

### Comments & documentation

- No comments that restate what the code does — names should already say that.
- A comment is warranted only for the non-obvious *why*: a hidden constraint, a protocol
  quirk (e.g. "session drops if KeepAlive lapses >200ms per qc-mcp's PROTOCOL.md"), a
  workaround for a specific bug.
- No mandatory Doxygen blocks on every function. Brief header-level summaries are fine on
  genuine module boundaries (the BLE transport layer, the command protocol layer) where a
  reader can't infer intent from the file alone.

### Formatting

- `.clang-format`, LLVM base style, 4-space indent, ~110 column limit.
- Enforced in CI — a PR that isn't clang-format-clean fails the build.

### Static analysis

- `clang-tidy` in CI with `cppcoreguidelines-*`, `performance-*`, `bugprone-*`, and
  `modernize-*` check groups. Warnings are errors.
- Suppressions require a `// NOLINT(check-name): reason` comment — a bare `NOLINT` doesn't
  pass review.
- Re-evaluate check-group exclusions (magic numbers in UI layout code, JUCE
  leak-detector/ownership macros tripping `cppcoreguidelines-*`, etc.) against real code once
  it exists, same as QCPresetLeveler did, rather than guessing upfront.
- On macOS, Homebrew/LLVM `clang-tidy` doesn't resolve the SDK sysroot the way AppleClang
  does implicitly — pass `-isysroot` explicitly via `xcrun` in the lint script.

### Testing

- **Catch2** via CMake `FetchContent`, run through CTest.
- Test files mirror the source tree (`src/Foo/Bar.hpp` → `tests/Foo/BarTests.cpp`).
- Anything with real logic — command/response protocol encoding, preset-list parsing,
  reconnection state machine — gets unit tests in the same PR that introduces it.
- Decouple protocol/state logic from JUCE's UI and BLE plumbing: pure functions/classes that
  take bytes/messages in and produce results out, exercised directly in tests, with the
  actual `CBCentralManager`/`MidiInput` callback reduced to a thin adapter around them.
- Coverage gated in CI (target: 85%, scoped to `src/` excluding `src/ui/` and the native BLE
  bridge glue) — settle the exact number once the test suite exists.
- Sanitizers (ASan + UBSan) run in CI on at least one platform's test suite.

### Build

- Warnings-as-errors on all platforms: `-Wall -Wextra -Wpedantic -Werror` (GCC/Clang),
  `/W4 /WX` (MSVC).

---

## Firmware (devboard, Zephyr)

The devboard side runs on Zephyr, a real-time OS with its own long-established conventions —
but unlike a typical Zephyr module, this firmware is **C++23, the same standard as the app
side**, not C. The RTOS-specific rules below (concurrency primitives, devicetree, Kconfig,
ISR safety) still follow
[Zephyr's own coding guidelines](https://docs.zephyrproject.org/latest/contribute/coding_guidelines/index.html)
where they're genuinely about the OS rather than the language — but naming, formatting,
ownership, and error-handling style follow the **App** section above, not Zephyr's C/kernel
conventions, so the two codebases don't drift into two different dialects for no reason.

### Language & style

- **C++23**, matching the app side, enabled via `CONFIG_CPP=y` + `CONFIG_STD_CPP23=y`.
  Confirmed present in Zephyr v4.4.2 (`lib/cpp/Kconfig`), the release
  [firmware/README.md](firmware/README.md) pins — see
  [firmware/app/prj.conf](firmware/app/prj.conf) for the actual config.
- `CONFIG_CPP_EXCEPTIONS=n` and `CONFIG_CPP_RTTI=n` — no exceptions, no RTTI, for the whole
  firmware image. This is the app's "no exceptions on a real-time path" rule applied
  everywhere here rather than to a subset, since there's no OS-level crash recovery on a
  microcontroller. Use `std::expected<T, E>` for fallible operations (protocol parsing,
  session setup, device enumeration), same as the app.
- Naming, ownership, and references/pointers follow the **App** section verbatim
  (`PascalCase` types, `camelCase` functions/variables, trailing-underscore private members;
  RAII and `std::unique_ptr` over raw owning pointers; reference > smart pointer > raw
  pointer). Zephyr's own C APIs (`k_*` calls, driver structs) are the embedded equivalent of
  JUCE's pointer-based interfaces — call them directly (Zephyr's headers already handle
  `extern "C"` linkage), and wrap the ones you take/release around a scope in a small RAII
  guard (e.g. a `ScopedKMutex` around `k_mutex_lock`/`k_mutex_unlock`) rather than pairing
  lock/unlock calls by hand.
- Formatting: the same `.clang-format` as the app (LLVM base style, 4-space indent,
  ~110 columns), not Zephyr's own kernel-style config — this is C++23 application code
  running on Zephyr, not a patch to Zephyr's own C kernel tree. If a contribution ever does
  patch Zephyr's own upstream sources (rather than code in this repo), that patch follows
  Zephyr's kernel style and `checkpatch.pl` instead, since it lives in a different codebase
  with its own rules.
- The header-only `.hpp` convention from the App section applies here too, for consistency.
  Flag it in review if firmware build times on the target toolchain make this genuinely
  painful — that's a build-performance tradeoff to revisit with real data, not a reason to
  diverge upfront.

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
- **ISR context is minimal by construction.** USB host HID report callbacks and BLE stack
  callbacks that run in interrupt or ISR-adjacent context must not block — no `k_mutex_lock`
  with a non-zero timeout, no `k_sleep`, no blocking log backend calls. Hand off to a
  `k_work` item or push into a `k_msgq`/ring buffer and process on a thread instead. This is
  directly relevant to the ~200ms KeepAlive heartbeat
  ([#8](https://github.com/rrooding/QCRemote/issues/8)) and the "lock around the USB send
  path" note in that issue — the lock in question should be a `k_mutex` taken from thread
  context, never from the callback that triggers it.
- Give threads explicit, documented priorities (the KeepAlive timer thread should not be
  starved by, say, a lower-priority BLE notification thread) rather than relying on Zephyr's
  defaults.

### Memory & containers

- Prefer static allocation: `k_mem_slab`, fixed-size `k_heap` regions, or plain
  static/const buffers sized for the known worst case (a single HID-framed message, one
  inflated protobuf payload) over unbounded dynamic allocation. Flash/RAM budgets on a
  microcontroller make an OOM at 2am on stage a real failure mode, not a theoretical one.
- Size the nanopb and inflate (miniz or similar) buffers statically per the message types
  actually needed for MVP1/MVP2 rather than allocating generously "to be safe" —
  the protocol's own 128-byte report cap gives a hard upper bound to design against.
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
  growth (`push_back` past a reserved capacity, etc.) in firmware code, mirroring the app
  side's ownership rules but enforced harder here since there's no heap fragmentation
  budget to spend.

### Logging

- Use Zephyr's logging subsystem (`LOG_MODULE_REGISTER`, `LOG_INF`/`LOG_WRN`/`LOG_ERR`/
  `LOG_DBG`) instead of `printk`/`printf` for anything beyond a throwaway bring-up hack.
  Log level is per-module and controlled via Kconfig (`CONFIG_<MODULE>_LOG_LEVEL`), which is
  what makes MVP1's "print preset names to terminal" ([#9](https://github.com/rrooding/QCRemote/issues/9))
  distinguishable from ordinary debug noise once other modules start logging too.
- Never call a blocking log backend from ISR context — Zephyr's deferred logging mode exists
  for exactly this; make sure it's enabled if any log calls happen near interrupt-adjacent
  code.

### Error handling

- Zephyr APIs return negative `errno` codes (or a subset per-API). Check every `k_*`/driver
  return value that can fail — don't discard them, even for calls that "shouldn't" fail on a
  known-good devkit. Wrap the boundary between Zephyr's `errno`-returning C APIs and the
  firmware's own C++ logic with `std::expected<T, int>` (or a small project-specific error
  enum), same as the app wraps its own fallible operations, rather than propagating raw
  `int` return codes through call sites that aren't talking to Zephyr directly.

### Testing

- Use Zephyr's **Ztest** framework, run via **Twister**, targeting `native_sim` (or QEMU)
  for anything that doesn't require real hardware — the HID framing/reassembly logic, the
  protobuf message decode, the session state machine. This mirrors the app side's principle
  of decoupling logic from hardware/OS glue so it's actually testable in CI.
- Ztest's macros (`ZTEST()`, `ZTEST_SUITE()`) are plain C macros and do expand fine in a C++
  translation unit, but the framework itself is written C-first — keep test bodies thin,
  calling into ordinary C++ functions/classes for the actual logic under test, rather than
  fighting the framework's assumptions with heavier C++ fixtures. Flag any real friction
  found here once tests actually exist, rather than assuming it either works perfectly or
  doesn't.
- Hardware-dependent paths (actual USB enumeration against a real Quad Cortex Mini, actual
  BLE radio behavior) stay as manual/hardware-in-the-loop validation steps
  ([#10](https://github.com/rrooding/QCRemote/issues/10)) rather than forced into Twister.

### Build

- `west build -b <board>`, warnings-as-errors via
  `CONFIG_COMPILER_WARNINGS_AS_ERRORS=y` (or equivalent `-Wall -Wextra -Werror` extra
  flags) so firmware CI has the same bar as the app side.

---

## Recording decisions

Consequential architecture/tooling decisions (devkit/RTOS choice, BLE transport choice,
language standard version, similar one-way-door calls) get recorded as an ADR under
[`docs/adr/`](docs/adr/), using the template there — see
[ADR 0001](docs/adr/0001-record-architecture-decisions.md) for the format and the reasoning
behind using ADRs at all. Not every note belongs in an ADR: protocol findings, bring-up
quirks, and other non-decision write-ups stay as regular docs.

## Commit messages (both sides)

[Conventional Commits](https://www.conventionalcommits.org/), enforced in CI so a changelog
can eventually be generated rather than hand-written.

```
<type>[optional scope]: <description>

[optional body]

[optional footer(s)]
```

- **Type**: `feat`, `fix`, `docs`, `style`, `refactor`, `perf`, `test`, `build`, `ci`,
  `chore`, `revert`.
- **Scope** is optional but encouraged, matching the areas the roadmap already splits along:
  `firmware`, `usb`, `ble`, `protocol`, `app`, `ui`, `midi`, `build`, `ci`, `docs` — e.g.
  `feat(firmware): implement KeepAlive heartbeat` or `feat(app): show connection status`.
- **Breaking changes**: mark with `!` after the type/scope (`feat(protocol)!: ...`) and/or a
  `BREAKING CHANGE:` footer explaining the impact.
- Reference the backlog issue a commit addresses in the footer where it applies (`Refs #8`,
  `Closes #8`).
- Description is imperative mood, no trailing period (`add`, not `Added`/`adds`). Don't
  capitalize the first word, but embedded acronyms/proper nouns (`BLE`, `HID`, `JUCE`) are
  fine — the subject just can't be Sentence case/Start Case/PascalCase/UPPERCASE as a whole.

## Open questions to flag rather than assume

- Devkit/RTOS choice: **resolved** — [ADR 0002](docs/adr/0002-devkit-selection.md), NXP
  FRDM-RW612 on Zephyr v4.4.2, `CONFIG_STD_CPP23` confirmed present. A later switch to a
  smaller board (ESP32-C3) is expected; if that ever became ESP-IDF instead of Zephyr, the
  Firmware section's OS-specific parts (`k_*`, `west`, devicetree+Kconfig) would need an
  ESP-IDF pass, but the C++23 language rules (naming, RAII, `std::expected`, no
  exceptions/RTTI, shared `.clang-format`) would carry over unchanged either way.
- GATT-vs-BLE-MIDI transport choice ([#14](https://github.com/rrooding/QCRemote/issues/14))
  affects how much native platform-bridge code (Objective-C++/CoreBluetooth) the app side
  needs — revisit the "raw pointer" and testing guidance above once that's decided.
- Exact clang-format column width/brace style, and clang-tidy check exclusions: settle once
  real app-side code exists to run the tooling against, same approach QCPresetLeveler took.
- Firmware coverage/test gating thresholds: no number set yet — pick one once the Ztest
  suite for MVP1's protocol logic exists to measure against.
