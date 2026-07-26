#!/usr/bin/env python3
"""Raise the supported WoW 3.3.5a smooth-spline speed limit to 1000 yd/s."""

import argparse
import hashlib
import os
import shutil
import tempfile
from pathlib import Path

STOCK_SHA256 = "aa63a5750d60ef16746c686b3d5e26876d98953eab08b1c026cd0faf78e88cb8"
PATCHED_SHA256 = "11442686766adfc93485c7cf7e854af0783669ac5e53acfdf3e386c101f72b97"
PATCH_OFFSET = 0x33C3D3
STOCK_OPERAND = bytes.fromhex("ec 22 9f 00")  # Address of the client's 50.0f constant.
PATCHED_OPERAND = bytes.fromhex("80 a0 9e 00")  # Address of an existing 1000.0f constant.


def fail(message):
    raise SystemExit(message)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def patch_client(path, dry_run=False):
    if not path.is_file():
        fail(f"client executable not found: {path}")

    current_sha = sha256(path)
    if current_sha == PATCHED_SHA256:
        print(f"{path} already supports 1000 yd/s smooth splines")
        return
    if current_sha != STOCK_SHA256:
        fail(
            f"unsupported Wow.exe (sha256 {current_sha}); expected unmodified "
            "WoW 3.3.5a build 12340 executable"
        )

    raw = path.read_bytes()
    found = raw[PATCH_OFFSET:PATCH_OFFSET + len(STOCK_OPERAND)]
    if found != STOCK_OPERAND:
        fail(f"{path}: unexpected bytes at offset 0x{PATCH_OFFSET:X}: {found.hex(' ')}")

    if dry_run:
        print(f"{path} can be patched for 1000 yd/s smooth splines")
        return

    backup = path.with_name(f"{path.name}.stock-12340")
    if backup.exists():
        if sha256(backup) != STOCK_SHA256:
            fail(f"refusing to replace unexpected backup: {backup}")
    else:
        shutil.copy2(path, backup)

    patched = bytearray(raw)
    patched[PATCH_OFFSET:PATCH_OFFSET + len(PATCHED_OPERAND)] = PATCHED_OPERAND

    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=f".{path.name}.", delete=False) as output:
            temporary = Path(output.name)
            output.write(patched)
            output.flush()
            os.fsync(output.fileno())
        shutil.copystat(path, temporary)
        if sha256(temporary) != PATCHED_SHA256:
            fail("patched executable checksum did not match the supported result")
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)

    print(f"patched {path}; original preserved as {backup.name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wow_exe", type=Path, help="path to the WoW 3.3.5a build 12340 executable")
    parser.add_argument("--dry-run", action="store_true", help="validate without changing the executable")
    args = parser.parse_args()
    patch_client(args.wow_exe, args.dry_run)


if __name__ == "__main__":
    main()
