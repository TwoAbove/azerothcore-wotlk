#!/usr/bin/env python3
"""Install the managed ReShade, REST, qUINT, and fast-GI runtime into WoW 3.3.5a."""

import argparse
import configparser
import hashlib
import io
import json
import os
import shutil
import struct
import tempfile
import zipfile
from pathlib import Path

REQUIRED_MEMBERS = {
    "ReShade32.dll",
    "Addons/ReshadeEffectShaderToggler.addon32",
    "Addons/ReshadeEffectShaderToggler-LICENSE.txt",
    "Config/ReshadeEffectShaderToggler.ini",
    "d3dcompiler_47.dll",
    "Shaders/Glamayre_Fast_Effects.fx",
    "Shaders/ReShade.fxh",
    "Shaders/ReShadeUI.fxh",
    "Shaders/REST_FLIP.fx",
    "Shaders/REST_NOOP.fx",
    "Shaders/REST_TONEMAP.fx",
    "Shaders/qUINT_bloom.fx",
    "Shaders/qUINT_common.fxh",
    "Shaders/qUINT_deband.fx",
    "Shaders/qUINT_lightroom.fx",
    "Shaders/qUINT_mxao.fx",
    "Shaders/qUINT_sharp.fx",
}


def fail(message):
    raise SystemExit(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def classify_dll(path):
    raw = path.read_bytes()
    if len(raw) < 0x40 or raw[:2] != b"MZ":
        return "unknown"
    pe_offset = struct.unpack_from("<I", raw, 0x3C)[0]
    if pe_offset + 26 > len(raw) or raw[pe_offset:pe_offset + 4] != b"PE\0\0":
        return "unknown"
    machine = struct.unpack_from("<H", raw, pe_offset + 4)[0]
    optional_magic = struct.unpack_from("<H", raw, pe_offset + 24)[0]
    if machine != 0x14C or optional_magic != 0x10B:
        return "wrong-architecture"
    lowered = raw.lower()
    if b"reshade" in lowered:
        return "reshade"
    if b"dxvk" in lowered:
        return "dxvk"
    return "unknown"


def atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=f".{path.name}.", delete=False) as output:
            temporary = Path(output.name)
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        temporary.chmod(0o644)
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def ini_value(text, section, key):
    active = False
    section_name = section.casefold()
    key_name = key.casefold()
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            active = stripped[1:-1].strip().casefold() == section_name
            continue
        if active and not stripped.startswith(("#", ";")) and "=" in line:
            candidate, value = line.split("=", 1)
            if candidate.strip().casefold() == key_name:
                return value.strip()
    return None


def merge_rest_config(path, default_data):
    def load(data):
        parser = configparser.ConfigParser(interpolation=None, strict=False)
        parser.optionxform = str
        parser.read_string(data.decode("utf-8-sig"))
        return parser

    if not path.exists():
        atomic_write(path, default_data)
        return

    raw = path.read_bytes()
    try:
        config = load(raw)
        defaults = load(default_data)
        amount = config.getint("General", "AmountGroups", fallback=0)
    except (configparser.Error, UnicodeDecodeError, ValueError) as error:
        fail(f"invalid REST configuration {path}: {error}")
    if amount < 0:
        fail(f"invalid REST configuration {path}: AmountGroups cannot be negative")

    target = None
    for index in range(amount):
        section = f"Group{index}"
        if config.get(section, "Name", fallback="").casefold() == "world before ui":
            target = section
            break

    if target is None:
        for index in range(amount):
            section = f"Group{index}"
            if config.get(section, "Name", fallback="").casefold() != "default":
                continue
            if all(config.getint(
                    f"{section}_{kind}", "AmountHashes", fallback=0) == 0
                    for kind in ("VertexShaders", "PixelShaders", "ComputeShaders")):
                target = section
                break

    new_group = target is None
    if new_group:
        target = f"Group{amount}"
        amount += 1
        if not config.has_section("General"):
            config.add_section("General")
        config.set("General", "AmountGroups", str(amount))
        config.add_section(target)

    managed_keys = ("Name", "Techniques", "AllowAllTechniques", "TechniqueExceptions")
    if new_group:
        managed_keys = tuple(defaults["Group0"])
    for key in managed_keys:
        config.set(target, key, defaults.get("Group0", key))

    for suffix, count_key in (
            ("VertexShaders", "AmountHashes"),
            ("PixelShaders", "AmountHashes"),
            ("ComputeShaders", "AmountHashes"),
            ("Constants", "AmountConstants")):
        section = f"{target}_{suffix}"
        if not config.has_section(section):
            config.add_section(section)
            config.set(section, count_key, "0")

    output = io.StringIO()
    config.write(output, space_around_delimiters=False)
    updated = output.getvalue().encode("utf-8")
    if updated != raw:
        atomic_write(path, updated)



def set_ini_values(text, section, values):
    lines = text.splitlines()
    section_name = section.casefold()
    start = None
    end = len(lines)
    for index, line in enumerate(lines):
        stripped = line.strip()
        if not (stripped.startswith("[") and stripped.endswith("]")):
            continue
        if start is None and stripped[1:-1].strip().casefold() == section_name:
            start = index
        elif start is not None:
            end = index
            break

    if start is None:
        if lines and lines[-1].strip():
            lines.append("")
        lines.append(f"[{section}]")
        start = len(lines) - 1
        end = len(lines)

    wanted = {key.casefold(): (key, value) for key, value in values.items()}
    seen = set()
    replacement = []
    for line in lines[start + 1:end]:
        stripped = line.strip()
        key_name = None
        if stripped and not stripped.startswith(("#", ";")) and "=" in line:
            key_name = line.split("=", 1)[0].strip().casefold()
        if key_name in wanted:
            if key_name not in seen:
                key, value = wanted[key_name]
                replacement.append(f"{key}={value}")
                seen.add(key_name)
        else:
            replacement.append(line)
    for key_name, (key, value) in wanted.items():
        if key_name not in seen:
            replacement.append(f"{key}={value}")

    lines[start + 1:end] = replacement
    return "\n".join(lines).rstrip() + "\n"


def merge_search_path(current, required):
    paths = [item.strip() for item in (current or "").split(",") if item.strip()]
    if required.casefold() not in {item.casefold() for item in paths}:
        paths.append(required)
    return ",".join(paths)

def dll_entries(path):
    if not path.is_file():
        return set()
    return {
        line.strip().casefold()
        for line in path.read_text(encoding="utf-8-sig").splitlines()
        if line.strip()
    }


def is_wowsilicon_client(wow_dir):
    return (
        (wow_dir / "mods" / "winerosetta.dll").is_file()
        and (wow_dir / "libDllLdr.dll").is_file()
        and "mods/winerosetta.dll" in dll_entries(wow_dir / "dlls.txt")
    )


def remove_dll_entry(path, entry):
    if not path.is_file():
        return
    lines = path.read_text(encoding="utf-8-sig").splitlines()
    filtered = [line for line in lines if line.strip().casefold() != entry.casefold()]
    if filtered == lines:
        return
    data = ("\n".join(filtered).rstrip() + "\n").encode("utf-8")
    atomic_write(path, data)


def install_wowsilicon_launcher(wow_dir):
    launcher = """#!/bin/zsh
set -e

GAME_DIR=${0:A:h}
LOG_DIR="$GAME_DIR/Logs"
LOG_PATH="$LOG_DIR/Fabled-WoW.log"
LOG_LIMIT_BYTES=4194304
LOG_BACKUPS=2
cd "$GAME_DIR"

if [[ "${1:-}" == "--run-detached" ]]; then
    set +e
    {
        echo "===== Fabled WoW started $(date) ====="
        ROSETTA_X87_PATH="$GAME_DIR/rosettax87/rosettax87" \\
        WINEDLLOVERRIDES="d3d9=n,b;d3dcompiler_47=n,b" \\
        MTL_HUD_ENABLED=0 \\
        MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=1 \\
        DXVK_ASYNC=1 \\
        "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/CrossOver-Hosted Application/wineloader2" "$GAME_DIR/Wow.exe"
        status=$?
        echo "===== Fabled WoW exited with status $status $(date) ====="
    } 2>&1 | /usr/bin/env python3 -c '
import os
import sys
from pathlib import Path

path = Path(sys.argv[1])
limit = int(sys.argv[2])
backups = int(sys.argv[3])
path.parent.mkdir(parents=True, exist_ok=True)

def rotate():
    for index in range(backups, 0, -1):
        source = path if index == 1 else Path(f"{path}.{index - 1}")
        target = Path(f"{path}.{index}")
        if source.exists():
            os.replace(source, target)

if path.exists() and path.stat().st_size >= limit:
    rotate()

output = path.open("ab", buffering=0)
for chunk in iter(lambda: sys.stdin.buffer.read(65536), b""):
    offset = 0
    while offset < len(chunk):
        remaining = limit - output.tell()
        if remaining == 0:
            output.close()
            rotate()
            output = path.open("wb", buffering=0)
            remaining = limit
        piece = chunk[offset:offset + remaining]
        output.write(piece)
        offset += len(piece)
output.close()
' "$LOG_PATH" "$LOG_LIMIT_BYTES" "$LOG_BACKUPS"
    exit
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required for bounded WoW logging" >&2
    exit 1
fi

mkdir -p "$LOG_DIR"
/usr/bin/nohup "$0" --run-detached </dev/null >/dev/null 2>&1 &
pid=$!
echo "WoW started in the background (PID $pid)"
echo "Log: $LOG_PATH (4 MiB, with two rotated backups)"
"""
    path = wow_dir / "Launch-WoW-Fabled.command"
    atomic_write(path, launcher.encode("utf-8"))
    path.chmod(0o755)


def load_package(path, expected_sha256):
    raw = path.read_bytes()
    actual_sha256 = sha256(raw)
    if actual_sha256 != expected_sha256:
        fail(f"runtime package checksum mismatch: expected {expected_sha256}, got {actual_sha256}")

    with zipfile.ZipFile(path) as package:
        try:
            manifest = json.loads(package.read("manifest.json"))
        except (KeyError, json.JSONDecodeError) as error:
            fail(f"invalid runtime package manifest: {error}")
        if (manifest.get("schema") != 3
                or manifest.get("full_addon_support") is not True
                or set(manifest.get("files", {})) != REQUIRED_MEMBERS):
            fail("runtime package manifest has an unsupported file set")
        files = {}
        for name, expected in manifest["files"].items():
            try:
                data = package.read(name)
            except KeyError:
                fail(f"runtime package is missing {name}")
            actual = sha256(data)
            if actual != expected:
                fail(f"runtime package member {name} checksum mismatch")
            files[name] = data
    return manifest, files


def install(wow_dir, package_path, package_sha256, installer_sha256):
    if not (wow_dir / "Data").is_dir() or not (wow_dir / "Wow.exe").is_file():
        fail(f"not a WoW client directory: {wow_dir}")

    manifest, files = load_package(package_path, package_sha256)
    runtime = files["ReShade32.dll"]
    compiler = files["d3dcompiler_47.dll"]
    wowsilicon = is_wowsilicon_client(wow_dir)
    root_d3d9_path = wow_dir / "d3d9.dll"
    proxy_path = wow_dir / "d3d9_dxvk.dll"
    migrated_dxvk = False

    if wowsilicon:
        root_kind = classify_dll(root_d3d9_path) if root_d3d9_path.exists() else "missing"
        proxy_kind = classify_dll(proxy_path) if proxy_path.exists() else "missing"
        if root_kind == "reshade":
            if proxy_kind != "dxvk":
                fail("WoWSilicon D9VK is missing; apply its game patch once, then rerun this updater")
        elif root_kind == "dxvk":
            if proxy_kind not in {"missing", "dxvk"}:
                fail(f"refusing to replace unsupported {proxy_path.name} ({proxy_kind})")
            os.replace(root_d3d9_path, proxy_path)
            migrated_dxvk = True
        else:
            fail("WoWSilicon D9VK is missing or unsupported; apply its game patch once, then rerun this updater")
        runtime_path = root_d3d9_path
        backup_path = wow_dir / "d3d9.dll.pre-fabled-visuals"
    else:
        runtime_path = root_d3d9_path
        backup_path = wow_dir / "d3d9.dll.pre-fabled-visuals"

    allowed_runtime_kinds = {"missing", "reshade", "dxvk"}
    runtime_kind = classify_dll(runtime_path) if runtime_path.exists() else "missing"
    if runtime_kind not in allowed_runtime_kinds:
        fail(f"refusing to replace unsupported {runtime_path} ({runtime_kind})")

    if runtime_kind == "dxvk":
        proxy_kind = classify_dll(proxy_path) if proxy_path.exists() else "missing"
        if proxy_kind not in {"missing", "dxvk"}:
            fail(f"refusing to use unsupported {proxy_path.name} ({proxy_kind})")
        if proxy_path.exists() and sha256(runtime_path.read_bytes()) != sha256(proxy_path.read_bytes()):
            fail("both d3d9.dll and d3d9_dxvk.dll contain different DXVK builds")
        os.replace(runtime_path, proxy_path)
        migrated_dxvk = True
    elif runtime_kind == "reshade" and runtime_path.read_bytes() != runtime and not backup_path.exists():
        shutil.copy2(runtime_path, backup_path)

    try:
        if not runtime_path.exists() or runtime_path.read_bytes() != runtime:
            atomic_write(runtime_path, runtime)
    except BaseException:
        if migrated_dxvk and not runtime_path.exists() and proxy_path.exists():
            os.replace(proxy_path, runtime_path)
        raise

    compiler_path = wow_dir / "d3dcompiler_47.dll"
    compiler_backup = wow_dir / "d3dcompiler_47.dll.pre-fabled-visuals"
    if (compiler_path.exists()
            and compiler_path.read_bytes() != compiler
            and not compiler_backup.exists()):
        shutil.copy2(compiler_path, compiler_backup)
    if not compiler_path.exists() or compiler_path.read_bytes() != compiler:
        atomic_write(compiler_path, compiler)


    rest_name = "ReshadeEffectShaderToggler.addon32"
    rest_active_path = wow_dir / rest_name
    rest_disabled_path = wow_dir / f"{rest_name}.disabled"
    rest_data = files[f"Addons/{rest_name}"]
    rest_path = rest_disabled_path if rest_disabled_path.exists() and not rest_active_path.exists() else rest_active_path
    if not rest_path.exists() or rest_path.read_bytes() != rest_data:
        atomic_write(rest_path, rest_data)
    rest_license_path = wow_dir / "ReshadeEffectShaderToggler-LICENSE.txt"
    rest_license = files["Addons/ReshadeEffectShaderToggler-LICENSE.txt"]
    if not rest_license_path.exists() or rest_license_path.read_bytes() != rest_license:
        atomic_write(rest_license_path, rest_license)
    rest_config_path = wow_dir / "ReshadeEffectShaderToggler.ini"
    merge_rest_config(rest_config_path, files["Config/ReshadeEffectShaderToggler.ini"])

    proxy_enabled = proxy_path.exists()
    if wowsilicon:
        remove_dll_entry(wow_dir / "dlls.txt", "mods/ReShade32.dll")
        install_wowsilicon_launcher(wow_dir)

    shader_dir = wow_dir / "reshade-shaders" / "Shaders"
    for member, data in files.items():
        if not member.startswith("Shaders/"):
            continue
        destination = shader_dir / Path(member).name
        if not destination.exists() or destination.read_bytes() != data:
            atomic_write(destination, data)

    (wow_dir / "reshade-shaders" / "Textures").mkdir(parents=True, exist_ok=True)
    (wow_dir / "reshade-shaders" / "Cache").mkdir(parents=True, exist_ok=True)

    config_path = wow_dir / "ReShade.ini"
    config = config_path.read_text(encoding="utf-8-sig") if config_path.exists() else ""
    shader_search = merge_search_path(
        ini_value(config, "GENERAL", "EffectSearchPaths"),
        r".\reshade-shaders\Shaders\**",
    )
    texture_search = merge_search_path(
        ini_value(config, "GENERAL", "TextureSearchPaths"),
        r".\reshade-shaders\Textures\**",
    )
    config = set_ini_values(config, "GENERAL", {
        "EffectSearchPaths": shader_search,
        "IntermediateCachePath": r".\reshade-shaders\Cache",
        "PerformanceMode": "1",
        "PresetPath": r".\Fabled-Visuals.ini",
        "StartupPresetPath": r".\Fabled-Visuals.ini",
        "TextureSearchPaths": texture_search,
    })
    if proxy_enabled:
        config = set_ini_values(config, "PROXY", {
            "EnableProxyLibrary": "1",
            "ProxyLibrary": "d3d9_dxvk.dll",
        })
    atomic_write(config_path, config.encode("utf-8"))
    marker = f"{package_sha256}\n{installer_sha256}\n".encode("ascii")
    atomic_write(wow_dir / ".fabled-visuals-runtime.sha256", marker)

    if wowsilicon:
        backend = "ReShade -> WoWSilicon D9VK (Launch-WoW-Fabled.command)"
    elif proxy_enabled:
        backend = "ReShade -> DXVK"
    else:
        backend = "ReShade -> system D3D9/Wine"
    rest_state = "enabled" if rest_active_path.exists() else "disabled"
    print(
        f"installed ReShade {manifest['reshade_version']} full add-on build, "
        f"REST {manifest['rest_version']} ({rest_state}), qUINT {manifest['quint_commit']}, "
        f"and Glamarye {manifest['glamayre_commit']} ({backend})"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wow-dir", required=True, type=Path)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--package-sha256", required=True)
    parser.add_argument("--installer-sha256", required=True)
    args = parser.parse_args()
    for option, digest in (
            ("--package-sha256", args.package_sha256),
            ("--installer-sha256", args.installer_sha256)):
        if len(digest) != 64 or any(character not in "0123456789abcdefABCDEF" for character in digest):
            fail(f"{option} must be a 64-character hexadecimal digest")
    install(
        args.wow_dir.resolve(),
        args.package,
        args.package_sha256.lower(),
        args.installer_sha256.lower(),
    )


if __name__ == "__main__":
    main()
