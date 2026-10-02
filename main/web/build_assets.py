#!/usr/bin/env python3
"""Generate the firmware web asset without npm or network dependencies."""
from __future__ import annotations

import argparse
import gzip
import io
from pathlib import Path


def compress(source: Path, destination: Path) -> None:
    data = source.read_bytes()
    output = io.BytesIO()
    # Empty filename and a fixed timestamp make the gzip header reproducible.
    with gzip.GzipFile(filename="", mode="wb", fileobj=output, compresslevel=9, mtime=0) as stream:
        stream.write(data)
    compressed = output.getvalue()
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() and destination.read_bytes() == compressed:
        return
    temporary = destination.with_name(destination.name + ".tmp")
    temporary.write_bytes(compressed)
    temporary.replace(destination)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    compress(args.source, args.destination)


if __name__ == "__main__":
    main()
