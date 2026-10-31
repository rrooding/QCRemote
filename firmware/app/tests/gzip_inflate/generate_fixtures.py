#!/usr/bin/env python3
"""Regenerates the gzip fixtures baked into src/main.cpp as C++ byte arrays.

Not run as part of the build - Ztest has no Python available at test time,
and these fixtures don't change unless the test scenarios do. Re-run this
and paste the output back into src/main.cpp if a fixture needs to change.

    python3 generate_fixtures.py
"""

import gzip

PLAINTEXT = b"QcRemote gzip test payload - repeated repeated repeated repeated data"


def cpp_array(name: str, data: bytes) -> None:
    hexed = ", ".join(f"0x{b:02x}" for b in data)
    print(f"// {name}: {len(data)} bytes")
    print(f"const std::vector<uint8_t> {name} = {{{hexed}}};")
    print()


def main() -> None:
    # mtime=0 for reproducible output - the MTIME field isn't checked by
    # gzip::inflate() (it's skipped as part of the fixed 10-byte header),
    # so its value doesn't matter beyond making this script's output
    # deterministic across runs.
    valid = gzip.compress(PLAINTEXT, compresslevel=6, mtime=0)
    cpp_array("kValidGzip", valid)
    cpp_array("kPlaintext", PLAINTEXT)

    # Cut well before the final deflate block - tinfl should report
    # TINFL_STATUS_NEEDS_MORE_INPUT, mapped to InflateError::Truncated.
    cpp_array("kTruncatedGzip", valid[:15])

    # Flip a bit inside the CRC32 trailer field (the first of the 8
    # trailer bytes) - the deflate stream itself still decodes cleanly and
    # the length still matches, only the CRC32 comparison should fail.
    bad_crc = bytearray(valid)
    bad_crc[-8] ^= 0xFF
    cpp_array("kBadCrcGzip", bytes(bad_crc))

    # Magic prefix present, but the flags byte has a reserved bit set
    # (0xE0 mask) - hasMagic() must return true (qc-mcp's own
    # decode_message() would also treat this as "compressed" from the
    # prefix alone) while inflate() fails cleanly on the header itself,
    # without ever reaching tinfl.
    cpp_array("kFalsePositiveMagic", bytes([0x1F, 0x8B, 0x08, 0xE0, 0, 0, 0, 0, 0, 0]))


if __name__ == "__main__":
    main()
