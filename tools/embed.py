#!/usr/bin/env python3
"""Embed a file in a C header as a byte array.

    embed.py INPUT OUTPUT SYMBOL

writes ``static const unsigned char SYMBOL[]`` and ``SYMBOL_LEN``. Used by the
Makefile to compile the dashboard into lookoutd, so the QNX target needs no
filesystem assets and nothing to deploy but the binaries.
"""
import sys
from pathlib import Path


def main(argv: list) -> int:
    if len(argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    src, dst, symbol = Path(argv[1]), Path(argv[2]), argv[3]
    data = src.read_bytes()
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    out = [
        f"/* Generated from {src.name} by tools/embed.py. Do not edit. */",
        f"static const unsigned char {symbol}[] = {{",
        *lines,
        "    0x00",  # NUL terminator, not counted in the length
        "};",
        f"static const unsigned int {symbol}_LEN = {len(data)};",
        "",
    ]
    dst.write_text("\n".join(out), encoding="ascii", newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
