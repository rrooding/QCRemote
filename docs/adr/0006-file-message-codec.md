# ADR 0006: File message decode extends the hand-rolled codec too - nesting and repetition were already covered

Date: 2026-10-31
Status: Accepted

## Context

Issue #59 picks up `FileMessage` (command 4, qc-mcp's directory read), the first Tier 2
message with genuine nested/repeated structure - exactly the case [ADR 0003](0003-handshake-protobuf-codec.md)
originally flagged as needing nanopb, and the case [ADR 0004](0004-tier2-protobuf-codec.md)
confirmed `ModelRepo`/`SetlistPosition` turned out *not* to be. Checked against qc-mcp's real
descriptor set (`descriptors/qc_descriptors-4.1.pb`):

```
FileMessage
   1  action      enum MessageAction.Enum
   2  request_id  uint64
   3  type        int32          (0=Presets, 1=IRs, 2=Captures - qc-mcp's directory.py)
   4  folder      FolderInfo     (nested message, optional)
   ...                           (fields 5-12, not needed here)

FolderInfo
   1  key          string
   2  parent_key   string
   3  name         string
   4  is_factory   bool
   5  is_downloads bool
   6  is_user_default bool
   7  files        ProductData   (nested message, REPEATED)
   8  is_cloud     bool
   9  is_favorites bool
   10 is_plugin    bool

ProductData
   1  key    string
   2  index  int32
   3  name   string
   ...                           (fields 4-21, not needed here)
```

`folder` is genuinely nested and `folder.files` is genuinely repeated - the shape ADR 0003
expected Tier 2 to need and ADR 0004 didn't find. But `ProtoFieldReader` (`QcProtobuf.hpp`)
already reads `LengthDelimited` generically, with no assumption about what's inside: a nested
message and a repeated message field are both just the same wire type appearing (once, or
more than once with the same field number) in the outer stream. Recursing a second
`ProtoFieldReader` over a `LengthDelimited` field's `bytes` span decodes a nested message with
zero changes to `QcProtobuf.hpp`; doing that once per occurrence of field 7 decodes a repeated
one. Nesting one level deeper than that (`ProductData`'s own fields) is the same trick again.

## Decision

Extend the hand-rolled codec a second time rather than standing up nanopb now:
`QcFileMessage.hpp`'s `decodeFile()` is built entirely on the existing `ProtoFieldReader`,
recursed per nesting level, still with no changes to `QcProtobuf.hpp` itself.

Two further decisions, both scoped to this message:

- **Callback-based, not a reply struct.** Every other decoder so far
  (`decodeVersionReply`, `decodeSetlistPosition`, `decodeModelRepo`) returns one filled-in
  struct. A folder's preset list can't be sized up front without either a heap allocation or a
  fixed cap guessed at compile time (CODING_STANDARDS.md's static-allocation rule rules out
  the former; the latter would silently drop presets past whatever cap was picked). `decodeFile`
  instead takes a callback and invokes it once per preset, each call carrying `std::string_view`
  fields that point directly into the caller's buffer - no copy, no cap, no allocation.
- **`type != 0` (Presets) is not an error.** qc-mcp's `directory.py::structure_directory()`
  multiplexes the same `File{action:UPDATE}` stream across presets/IRs/captures purely on
  this field; a `decodeFile` caller only wants presets, so IRs/captures folders decode
  successfully with zero callbacks rather than failing.

`ProductData.index` is read by qc-mcp as `_file_dict()`'s own comment explains: on a directory
READ the device leaves `index` at 0 and the file's position in the `files` array is the real,
authoritative slot. `decodeFile` follows the same rule - the `index` passed to each callback is
a running count of `files` entries seen so far, not `ProductData.index`'s wire value.

Folder metadata (`key`/`name`/`is_factory`) is read in a first pass over `folder`'s bytes,
separate from a second pass that walks `files` and fires the callback - guaranteeing
callbacks always carry correct metadata regardless of whether a real encoder happens to put
`files` before or after the scalar fields in its wire output (protobuf makes no ordering
guarantee here, unlike `QcProtobuf.hpp`'s own writers which always emit ascending field
numbers).

## Consequences

- Same conclusion as ADR 0004, now confirmed on a message that actually has the shape ADR
  0003 worried about: hand-rolled nesting/repetition via recursive `ProtoFieldReader` is
  cheaper than standing up nanopb for Tier 2's message set so far. This still isn't a
  blanket claim about every remaining Tier 2 message (`Grid`'s `chains[]` is deeper and
  bigger) - revisit per-message, as ADR 0004 already did once.
- `decodeFile`'s callback signature is the contract issue #60's dispatch work calls into;
  changing it later (e.g. to also carry `is_downloads` or the folder's own `is_plugin`) is a
  signature change at that call site too, not just here.
- No bound on preset name length is enforced by this decoder - callers get a `string_view`
  into the shared buffer and must not assume any particular maximum before logging/copying it
  elsewhere, matching the zero-copy intent (a downstream cap belongs wherever the data is
  finally stored, e.g. a terminal print buffer in issue #9, not here).
- **Not yet verified against real device traffic** - no hardware run was done for this issue;
  the Ztest suite exercises constructed bytes built from the verified field numbers above, not
  a capture from an actual Quad Cortex.
