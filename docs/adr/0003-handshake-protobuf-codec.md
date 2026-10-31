# ADR 0003: Hand-rolled protobuf codec for Tier 1 session messages, defer nanopb

Date: 2026-09-15
Status: Accepted

## Context

Issue #5 ("Port/implement protobuf decoding for required message types") originally assumed
nanopb as the protobuf implementation for the whole MVP1 message set. Picking up issue #7
(session handshake: `ResetCommsBuffers`, `Version`, `Connection`) only needs a narrow slice of
that set — four flat messages (`ResetCommsBuffers`, `Version`, `Connection`, `KeepAlive`),
verified field-by-field against qc-mcp's checked-in `descriptors/qc_descriptors-4.1.pb`. None
of the four has a nested message, a repeated field, or packed encoding; they're a handful of
scalar fields (string/uint64/bool/enum) each.

qc-mcp itself has no static `.proto` source for these messages — it recovers descriptors
dynamically from the installed Cortex Control app binary at runtime. Bringing in nanopb now
would mean standing up a full pipeline (protoc + nanopb_generator wired into CMake, a hand-
written `.proto` for just these four messages since none is checked in anywhere) to encode/
decode payloads simple enough to hand-write directly against the verified field numbers.

Full MVP1 scope (issue #5 Tier 2) does include messages that would genuinely benefit from
codegen — `Grid` and `ModelRepo` have nested/repeated fields (`chains[]`, `models[]`,
`param_values[]`, catalog entries) where hand-written encode/decode would become real,
error-prone work. That's a different cost/benefit than the four flat handshake messages.

## Decision

Implement a small, purpose-built protobuf wire-format codec (`QcProtobuf.hpp`: varint/tag/
string/uint64/bool/enum writers, a forward-only field-skipping reader) scoped to Tier 1's four
flat messages, rather than integrating nanopb now. Message-specific encode/decode
(`QcHandshakeMessages.hpp`) is built directly on top, against the field numbers verified from
qc-mcp's descriptor set.

## Consequences

- No new build-system dependency or codegen step for #7/#8 (KeepAlive) — `west build` stays
  as simple as it is today.
- The hand-rolled reader is intentionally forward-only and skips unknown fields (matching
  proto2 wire-format compatibility rules), so it tolerates `VersionMessage`'s other 17 fields
  we don't read without needing to know their types.
- This does **not** replace the need for nanopb (or an equivalent) once #5 Tier 2 (`Grid`,
  `ModelRepo`, `SetlistPosition`) is picked up — those messages have real nested/repeated
  structure where hand-written encode/decode stops being the cheaper option. Revisit this ADR
  at that point rather than extending the hand-rolled codec to cover nesting/repetition it
  wasn't designed for.
- Two protobuf-adjacent code paths may briefly coexist once Tier 2 lands (the hand-rolled
  Tier 1 codec + nanopb-generated Tier 2 code) unless Tier 1 is also migrated to nanopb at
  that time — an explicit follow-up decision, not a foregone one, since Tier 1's messages stay
  simple regardless of what Tier 2 needs.
