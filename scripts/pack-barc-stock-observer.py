#!/usr/bin/env python3
"""Build the BARC stock observer as a deterministic, archive-only game overlay."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import tempfile
import zipfile


ALLOWED = (
    "CONTENT-METADATA.json",
    "LuaRules/Gadgets/barc_stock_queue_reader.lua",
    "modinfo.lua",
)
FIXED_TIME = (1980, 1, 1, 0, 0, 0)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=Path("data/barc-stock-observer"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    if args.output.suffix != ".sdz":
        parser.error("--output must use the .sdz archive extension")
    discovered = tuple(sorted(str(path.relative_to(source)).replace(os.sep, "/") for path in source.rglob("*") if path.is_file()))
    if discovered != ALLOWED:
        raise SystemExit(f"source inventory differs: {discovered!r}")
    rows = []
    payloads: dict[str, bytes] = {}
    for relative in ALLOWED:
        path = source / relative
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
            raise SystemExit(f"refusing non-regular source: {relative}")
        payloads[relative] = path.read_bytes()
        rows.append({"path": relative, "bytes": len(payloads[relative]), "sha256": sha256(payloads[relative])})

    args.output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{args.output.name}.", dir=args.output.parent)
    os.close(fd)
    try:
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for relative in ALLOWED:
                entry = zipfile.ZipInfo(relative, FIXED_TIME)
                entry.create_system = 3
                entry.external_attr = (0o100644 << 16)
                entry.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(entry, payloads[relative], compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
        archive_bytes = Path(temporary).read_bytes()
        os.chmod(temporary, 0o644)
        os.replace(temporary, args.output)
    finally:
        if os.path.exists(temporary): os.unlink(temporary)

    inventory = {
        "schema": "barc.stock-observer-package-inventory/1",
        "archive": {"name": args.output.name, "bytes": len(archive_bytes), "sha256": sha256(archive_bytes)},
        "files": rows,
    }
    args.inventory.parent.mkdir(parents=True, exist_ok=True)
    args.inventory.write_text(json.dumps(inventory, indent=2) + "\n", encoding="ascii")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
