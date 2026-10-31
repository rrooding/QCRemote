# ADR 0004: Tier 2 messages are flat too - extend the hand-rolled codec, still defer nanopb

Date: 2026-10-31
Status: Accepted

## Context

[ADR 0003](0003-handshake-protobuf-codec.md) hand-rolled a small protobuf wire-format codec
(`QcProtobuf.hpp`) for Tier 1's four flat handshake messages, and explicitly deferred nanopb -
but assumed, without having verified it yet, that issue #5's Tier 2 (`ModelRepo`,
`SetlistPosition`) would need real codegen: *"Grid and ModelRepo have nested/repeated fields
(chains[], models[], param_values[], catalog entries) where hand-written encode/decode would
become real, error-prone work."*

Picking up Tier 2 now and checking that assumption against qc-mcp's actual descriptor set
(`descriptors/qc_descriptors-4.1.pb`, parsed with `descriptor_pb2.FileDescriptorSet`) shows it
was wrong for both messages in scope here:

```
SetlistPositionMessage        (all fields optional, proto3-style explicit presence)
   1  action                   enum MessageAction.Enum
   2  request_id               uint64
   3  folder_key                string
   4  position                 uint32
   5  is_factory               bool
   6  is_downloads             bool
   7  key_in_downloads         string
   8  is_plugin                bool
   9  key_in_plugin_folder     string

ModelRepoMessage
   1  action                   enum MessageAction.Enum
   2  request_id               uint64
   3  model_repo_payload       bytes
```

Both are flat and scalar-only - no nested message, no repeated field, no packed encoding.
Same shape as Tier 1's four messages, just with a `bytes` field in `ModelRepoMessage`'s case
instead of more scalars.

The "device catalog" (633 devices on CorOS 4.1, each with a full parameter schema) that made
`ModelRepo` sound like it would need real nested-message decoding does exist, but it's not
protobuf at all: `model_repo_payload` is a gzip+tar of `ModelRepo.xml`, opaque bytes from this
message's point of view (confirmed against qc-mcp's `catalog.py`/`PROTOCOL.md` section 10).
Issue #6 (gzip/inflate) and a separate XML parse are what turn that blob into the catalog -
this message's own protobuf structure never contains it directly.

## Decision

Extend the existing hand-rolled codec rather than standing up nanopb for Tier 2: two new
header-only decode functions (`decodeModelRepo` in `QcModelRepoMessage.hpp`,
`decodeSetlistPosition` in `QcSetlistPositionMessage.hpp`), built on the same
`ProtoFieldReader` ADR 0003 already introduced, with no changes to `QcProtobuf.hpp` itself -
its existing varint/string/bool/enum primitives and forward-skipping reader already cover
every field type both messages use, bytes included (a `bytes` field is wire-identical to a
`string` field; it decodes to the same `LengthDelimited` span, just handled as an opaque byte
range instead of text).

`ModelRepoMessage.model_repo_payload` decodes to a `std::span<const uint8_t>` pointing into
the caller's already-reassembled buffer, not a copy - the payload itself is still gzip'd/tar'd
and this decoder does not touch its contents (issue #6's job). Avoiding a copy matters
specifically here per issue #33's static-memory concern: a full catalog payload is far larger
than anything Tier 1 ever decoded, so duplicating it into a second static buffer on top of the
HID reassembly buffer that already holds it would roughly double the worst-case RAM cost for
no benefit.

## Consequences

- No new build-system dependency or codegen step added for Tier 2, same as ADR 0003's
  rationale for Tier 1 - `west build` still doesn't need a `.proto`/nanopb pipeline.
- This closes out the "revisit this ADR" note in ADR 0003's consequences: having now checked
  both Tier 2 messages against the real descriptor set, neither needs nanopb's nested/repeated
  field support, so there is no pending decision left to revisit on *this specific point*.
  Nanopb (or an equivalent) may still become worth it if a future message genuinely has nested
  or repeated fields - e.g. `Grid` (`chains[]`), out of scope for issue #5 - but that is a
  fresh decision for whichever issue picks up `Grid`, not implied by this one.
- `ModelRepoMessage`'s outer envelope (action/request_id/model_repo_payload) is plain,
  uncompressed protobuf and parses correctly today, independent of issue #6 - only the bytes
  *inside* `model_repo_payload` need inflating before they're anything but opaque. Issue #5's
  original framing ("your decoder takes an already-inflated protobuf blob") assumed the whole
  message might arrive compressed; verified against the real schema, that's not the shape of
  the wire format, so no inflate step is actually a prerequisite for `decodeModelRepo` itself.
  What issue #33's buffer-sizing exercise still needs to account for is that `model_repo_payload`
  can be large (hundreds of models' worth of compressed XML) - the HID reassembly buffer has to
  be big enough to hold the *whole* message, compressed payload included, before `decodeModelRepo`
  ever sees it, independent of whether that payload is later inflated on-device or off-device.
- `SetlistPositionMessage`'s `folder_key`/`key_in_downloads`/`key_in_plugin_folder` string
  fields are capped at a provisional 96-byte `etl::string` - no device-side max length is
  documented anywhere in qc-mcp; this is generous-but-bounded against the longest form observed
  in qc-mcp's docs (`/media/p4/Presets/<user folder name>`), not a verified hardware limit.
