#!/usr/bin/env python3
"""Build the pinned ReShade, REST, qUINT, and fast-GI runtime package for WotLK."""

import argparse
import hashlib
import json
import zipfile
from pathlib import Path

RESHAPE_VERSION = "6.8.0"
RESHAPE_SETUP_SHA256 = "afe4c8f13048306307983b8b3d41d5bf00a86820440b0e57dea10950e1176445"
D3DCOMPILER_SHA256 = "efbdbbcd0d954f8fdc53467de5d89ad525e4e4a9cfff8a15d07c6fdb350c407f"
QUINT_COMMIT = "98fed77b26669202027f575a6d8f590426c21ebd"
QUINT_ARCHIVE_SHA256 = "2f6ff2f5dd39ff400c07ecbbfd1156604459f44d9028d07fa6d98b84d4cfbfa9"
GLAMAYRE_COMMIT = "9dd9b826fa2cbea818ef1bc487e5f2e7f427c750"
GLAMAYRE_ARCHIVE_SHA256 = "8843a74f899585cd1b9b1ec8193b7cc08558e95f292955f649ad0ec05194cef9"
RESHADERS_COMMIT = "6db142b4b1a05c764222e5b0bd9a644b7ccfe1dc"
RESHADERS_ARCHIVE_SHA256 = "12d082c8ab1dbcb5e221e1b6116a0343f3182ee517f09bb966b117acc7635312"
REST_VERSION = "1.3.23.633"
REST_ARCHIVE_SHA256 = "79aaf38002e103034527eeb09553cbc422b44989d22258e905652131904afa6d"
QUINT_FILES = (
    "qUINT_bloom.fx",
    "qUINT_common.fxh",
    "qUINT_deband.fx",
    "qUINT_lightroom.fx",
    "qUINT_mxao.fx",
    "qUINT_sharp.fx",
)
RESHADERS_FILES = (
    "ReShade.fxh",
    "ReShadeUI.fxh",
)
REST_SHADER_FILES = (
    "REST_FLIP.fx",
    "REST_NOOP.fx",
    "REST_TONEMAP.fx",
)


def fail(message):
    raise SystemExit(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def checked_archive(path, expected_sha256, name):
    raw = path.read_bytes()
    actual = sha256(raw)
    if actual != expected_sha256:
        fail(f"{name} checksum mismatch: expected {expected_sha256}, got {actual}")
    return zipfile.ZipFile(path)

def write_member(package, name, data):
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o100644 << 16
    package.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def build(reshade_setup, d3dcompiler, quint_archive, glamayre_archive, reshader_archive,
          rest_archive, rest_config, output):
    with checked_archive(reshade_setup, RESHAPE_SETUP_SHA256, "ReShade setup") as reshade:
        try:
            runtime = reshade.read("ReShade32.dll")
        except KeyError:
            fail("ReShade setup does not contain ReShade32.dll")

    compiler = d3dcompiler.read_bytes()
    actual_compiler_sha256 = sha256(compiler)
    if actual_compiler_sha256 != D3DCOMPILER_SHA256:
        fail(
            f"D3DCompiler checksum mismatch: expected {D3DCOMPILER_SHA256}, "
            f"got {actual_compiler_sha256}")

    quint_root = f"qUINT-{QUINT_COMMIT}/Shaders"
    files = {
        "ReShade32.dll": runtime,
        "d3dcompiler_47.dll": compiler,
    }
    with checked_archive(quint_archive, QUINT_ARCHIVE_SHA256, "qUINT archive") as quint:
        for filename in QUINT_FILES:
            member = f"{quint_root}/{filename}"
            try:
                files[f"Shaders/{filename}"] = quint.read(member)
            except KeyError:
                fail(f"qUINT archive does not contain {member}")

    glamayre_root = f"Glamarye_Fast_Effects_for_ReShade-{GLAMAYRE_COMMIT}/Shaders"
    with checked_archive(
            glamayre_archive, GLAMAYRE_ARCHIVE_SHA256, "Glamarye archive") as glamayre:
        member = f"{glamayre_root}/Glamayre_Fast_Effects.fx"
        try:
            files["Shaders/Glamayre_Fast_Effects.fx"] = glamayre.read(member)
        except KeyError:
            fail(f"Glamarye archive does not contain {member}")

    reshader_root = f"reshade-shaders-{RESHADERS_COMMIT}/Shaders"
    with checked_archive(
            reshader_archive, RESHADERS_ARCHIVE_SHA256, "ReShade shaders archive") as reshaders:
        for filename in RESHADERS_FILES:
            member = f"{reshader_root}/{filename}"
            try:
                files[f"Shaders/{filename}"] = reshaders.read(member)
            except KeyError:
                fail(f"ReShade shaders archive does not contain {member}")

    with checked_archive(rest_archive, REST_ARCHIVE_SHA256, "REST archive") as rest:
        for source, destination in (
                ("ReshadeEffectShaderToggler.addon32",
                 "Addons/ReshadeEffectShaderToggler.addon32"),
                ("LICENSE", "Addons/ReshadeEffectShaderToggler-LICENSE.txt")):
            try:
                files[destination] = rest.read(source)
            except KeyError:
                fail(f"REST archive does not contain {source}")
        for filename in REST_SHADER_FILES:
            try:
                files[f"Shaders/{filename}"] = rest.read(filename)
            except KeyError:
                fail(f"REST archive does not contain {filename}")

    files["Config/ReshadeEffectShaderToggler.ini"] = rest_config.read_bytes()

    manifest = {
        "schema": 3,
        "reshade_version": RESHAPE_VERSION,
        "full_addon_support": True,
        "reshade_setup_sha256": RESHAPE_SETUP_SHA256,
        "d3dcompiler_sha256": D3DCOMPILER_SHA256,
        "quint_commit": QUINT_COMMIT,
        "quint_archive_sha256": QUINT_ARCHIVE_SHA256,
        "glamayre_commit": GLAMAYRE_COMMIT,
        "glamayre_archive_sha256": GLAMAYRE_ARCHIVE_SHA256,
        "reshade_shaders_commit": RESHADERS_COMMIT,
        "reshade_shaders_archive_sha256": RESHADERS_ARCHIVE_SHA256,
        "rest_version": REST_VERSION,
        "rest_archive_sha256": REST_ARCHIVE_SHA256,
        "rest_config_sha256": sha256(files["Config/ReshadeEffectShaderToggler.ini"]),
        "files": {name: sha256(data) for name, data in sorted(files.items())},
    }

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(f".{output.name}.tmp")
    with zipfile.ZipFile(temporary, "w") as package:
        write_member(package, "manifest.json", json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        for name, data in sorted(files.items()):
            write_member(package, name, data)
    temporary.replace(output)
    print(f"wrote {output} ({output.stat().st_size} bytes, sha256 {sha256(output.read_bytes())})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reshade-setup", required=True, type=Path)
    parser.add_argument("--d3dcompiler", required=True, type=Path)
    parser.add_argument("--quint-archive", required=True, type=Path)
    parser.add_argument("--glamayre-archive", required=True, type=Path)
    parser.add_argument("--reshade-shaders-archive", required=True, type=Path)
    parser.add_argument("--rest-archive", required=True, type=Path)
    parser.add_argument("--rest-config", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    build(
        args.reshade_setup,
        args.d3dcompiler,
        args.quint_archive,
        args.glamayre_archive,
        args.reshade_shaders_archive,
        args.rest_archive,
        args.rest_config,
        args.out,
    )


if __name__ == "__main__":
    main()
