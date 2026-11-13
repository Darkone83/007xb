#!/usr/bin/env python3
"""007xb offline preparation tools (standard library only; no ROM included).

Commands:
    prepare --rom PATH      Validate a ROM and write normalized US .z64 copy.
    emit --pc-port PATH     Invoke the GoldenEye PC port's three existing converters.
    pack --pc-port PATH     Bundle verified US ROM and PC sidecars into an indexed archive.
    inspect --archive PATH Validate the archive's structure and per-file CRCs.
    logo --obj PATH        Convert an OBJ's line segments for future Xbox D3D8 rendering.
    stage --zip PATH        Stage the upstream converters/metadata from the PC-port ZIP.
    extract-upstream         Extract and RZ-decompress upstream-marked ROM files/images.
    check-sidecars           Verify the upstream converters' output before any packaging.
    extract-raw --filelist  Diagnostic-only extraction of every raw ROM filelist range.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
import zipfile
from pathlib import Path, PurePosixPath
from typing import Iterable

US_SHA1 = "abe01e4aeb033b6c0836819f549c791b26cfde83"
OTHER_SHA1 = {
    "167c3c433dec1f1eb921736f7d53fac8cb45ee31": "PAL (Europe)",
    "2a5dade32f7fad6c73c659d2026994632c1b3174": "NTSC-J (Japan)",
}
ROM_FILENAME = "ge007.ntsc-final.z64"
ROM_DATA_PATH = Path("data") / ROM_FILENAME
PROJ_ROOT = Path(__file__).resolve().parent.parent

MAGIC_Z64 = b"\x80\x37\x12\x40"
MAGIC_V64 = b"\x37\x80\x40\x12"
MAGIC_N64 = b"\x40\x12\x37\x80"

# Stable, provisional 007xb archive v1; little endian, 4096-byte aligned payloads.
# Header fields: magic, version, count, directory_offset, record_bytes,
# total_bytes, directory_crc32.
ARCHIVE_MAGIC = b"007XBDAT"
ARCHIVE_HEADER = struct.Struct("<8sIIIIII")  # 32 bytes
ARCHIVE_ENTRY = struct.Struct("<64sIIII")    # name, offset, size, crc32, flags
ARCHIVE_VERSION = 1
ALIGNMENT = 4096
MAX_U32 = 0xFFFFFFFF

LOGO_MAGIC = b"D83LINES"
LOGO_HEADER = struct.Struct("<8sIIII")  # magic, version, vertex_count, segment_count, reserved


class ToolError(RuntimeError):
    pass


def sha1_bytes(data: bytes) -> str:
    return hashlib.sha1(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def format_bytes(n: int) -> str:
    return f"{n:,} bytes ({n / 1048576:.2f} MiB)"


def align(n: int, block: int = ALIGNMENT) -> int:
    return (n + block - 1) // block * block


def normalized_rom(data: bytes) -> tuple[str, bytes]:
    if len(data) < 64:
        raise ToolError("File is too short to be an N64 ROM.")
    magic = data[:4]
    if magic == MAGIC_Z64:
        return "z64 (big endian)", data
    if magic == MAGIC_V64:
        if len(data) % 2:
            raise ToolError("Byte-swapped v64 ROM has an odd byte count.")
        b = bytearray(data)
        b[0::2], b[1::2] = data[1::2], data[0::2]
        return "v64 (byte-swapped)", bytes(b)
    if magic == MAGIC_N64:
        if len(data) % 4:
            raise ToolError("Little-endian n64 ROM byte count is not divisible by four.")
        b = bytearray(data)
        b[0::4], b[1::4], b[2::4], b[3::4] = (
            data[3::4], data[2::4], data[1::4], data[0::4]
        )
        return "n64 (little endian)", bytes(b)
    raise ToolError(f"Unrecognized N64 ROM header magic: {magic.hex(' ')}.")


def rom_identity(data: bytes) -> dict:
    input_order, normalized = normalized_rom(data)
    actual_sha1 = sha1_bytes(normalized)
    internal_name = normalized[0x20:0x34].decode("ascii", "replace").strip(" \0")
    return {
        "input_byte_order": input_order,
        "normalized_sha1": actual_sha1,
        "normalized_size": len(normalized),
        "internal_title": internal_name,
        "cartridge_id": normalized[0x3C:0x3E].decode("ascii", "replace"),
        "country_code_hex": f"0x{normalized[0x3E]:02X}",
        "revision": normalized[0x3F],
        "verified_us": actual_sha1 == US_SHA1,
    }


def require_us_rom(data: bytes) -> tuple[dict, bytes]:
    info = rom_identity(data)
    sha = info["normalized_sha1"]
    if sha != US_SHA1:
        label = OTHER_SHA1.get(sha)
        if label:
            raise ToolError(f"Identified {label} GoldenEye ROM, but 007xb phase 1 supports US only (SHA-1 {sha}).")
        raise ToolError(
            "ROM does not match the known unmodified GoldenEye NTSC-U image.\n"
            f"  Found SHA-1:    {sha}\n"
            f"  Expected SHA-1: {US_SHA1}\n"
            "No output created; check revision and bytes or provide a clean dump."
        )
    _, normalized = normalized_rom(data)
    return info, normalized


def save_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(mode="wb", dir=path.parent, prefix=".007xb-", delete=False) as fh:
            tmp_path = Path(fh.name)
            fh.write(data)
        os.replace(tmp_path, path)
    finally:
        if tmp_path is not None and tmp_path.exists():
            tmp_path.unlink()


def verify_rom_file(path: Path) -> tuple[dict, bytes]:
    if not path.is_file():
        raise ToolError(f"ROM file not found: {path}")
    return require_us_rom(path.read_bytes())


def command_prepare(args: argparse.Namespace) -> None:
    rom_path = Path(args.rom).expanduser().resolve()
    info, normalized = verify_rom_file(rom_path)
    dest = PROJ_ROOT / ROM_DATA_PATH
    save_atomic(dest, normalized)
    info["source_filename"] = rom_path.name
    info["target_file"] = ROM_DATA_PATH.as_posix()
    info["version"] = "GoldenEye 007 NTSC-U"
    manifest_path = PROJ_ROOT / "data" / "rom_info.json"
    save_atomic(manifest_path, (json.dumps(info, indent=2, sort_keys=True) + "\n").encode("utf-8"))
    print(f"[PASS] GoldenEye NTSC-U ROM ({info['input_byte_order']})")
    print(f"       SHA-1: {info['normalized_sha1']}")
    print(f"       Size:  {format_bytes(info['normalized_size'])}")
    print(f"       Title: {info['internal_title']}")
    print(f"[WRITE] {dest}")
    print(f"[WRITE] {manifest_path}")


def command_emit(args: argparse.Namespace) -> None:
    upstream = Path(args.pc_port).expanduser().resolve()
    check_upstream_tree(upstream)
    src = PROJ_ROOT / ROM_DATA_PATH
    info, _ = verify_rom_file(src)
    target = upstream / ROM_DATA_PATH
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_file():
        existing_sha = sha1_bytes(normalized_rom(target.read_bytes())[1])
        if existing_sha != US_SHA1:
            raise ToolError(f"Refusing to replace a different ROM already at {target}")
    elif src.resolve() != target.resolve():
        shutil.copyfile(src, target)
    print(f"[PASS] US ROM prepared: {info['normalized_sha1']}", flush=True)
    for cmd in [
        [sys.executable, "tools_pc/d43_emit.py", "ntsc-final"],
        [sys.executable, "tools_pc/d69_emit.py", "ntsc-final"],
        [sys.executable, "tools_pc/d88_emit.py", "ntsc-final", "--regen"],
    ]:
        print(f"[RUN] {upstream.name}: {' '.join(cmd)}", flush=True)
        try:
            subprocess.run(cmd, cwd=upstream, check=True)
        except subprocess.CalledProcessError as exc:
            raise ToolError(f"Upstream converter failed (exit {exc.returncode}): {' '.join(cmd)}") from exc
    summaries = verify_upstream_sidecars(upstream)
    for label, summary in summaries.items():
        print(f"[PASS] {label}: {summary['entries']} entries / {format_bytes(summary['bytes'])}")
    print("[PASS] PC x86_64 sidecars emitted and verified. These are NOT RXDK 32-bit layouts.")


def get_pack_sources(pc_port: Path, logo: Path | None = None, rom_path: Path | None = None) -> list[tuple[str, Path]]:
    # Include the normalized big-endian ROM because the Xbox runtime performs
    # absolute ROM-address reads directly inside this indexed archive.
    paths = [
        ("rom/ge007.ntsc-final.z64", rom_path if rom_path is not None else PROJ_ROOT / ROM_DATA_PATH),
        ("pcmodels/pcmodels.bin", pc_port / "data/pcmodels-ntsc-final/pcmodels.bin"),
        ("pcmodels/manifest.csv", pc_port / "data/pcmodels-ntsc-final/manifest.csv"),
        ("pccg/pccg.bin", pc_port / "data/pccg-ntsc-final/pccg.bin"),
        ("pccg/manifest.csv", pc_port / "data/pccg-ntsc-final/manifest.csv"),
    ]
    for name, src in paths:
        if not src.is_file():
            raise ToolError(f"Required converted resource is missing: {src}\nRun 'emit --pc-port ...' first.")
        if src.stat().st_size == 0:
            raise ToolError(f"Required resource is empty: {src}")
    verify_rom_file(paths[0][1])
    verify_upstream_sidecars(pc_port)
    if logo is None:
        candidate = PROJ_ROOT / "data/darkone83.lines"
        logo = candidate if candidate.is_file() else None
    if logo is not None:
        validate_logo(logo)
        paths.append(("branding/darkone83.lines", logo))
    return paths


def write_archive(output: Path, sources: Iterable[tuple[str, Path]]) -> list[dict]:
    srcs = list(sources)
    if not srcs or len(srcs) > 10000:
        raise ToolError("Unreasonable number of entries")
    names = [name for name, _ in srcs]
    if len(names) != len(set(names)):
        raise ToolError("Duplicate archive entry names")
    for name in names:
        try:
            encoded = name.encode("ascii")
        except UnicodeEncodeError as exc:
            raise ToolError(f"Non-ASCII archive entry: {name}") from exc
        if len(encoded) > 63 or name.startswith("/") or ".." in Path(name).parts:
            raise ToolError(f"Invalid archive entry name: {name}")
    srcs.sort(key=lambda item: item[0])
    dir_offset = ARCHIVE_HEADER.size
    dir_size = ARCHIVE_ENTRY.size * len(srcs)
    file_records: list[dict] = []
    output.parent.mkdir(parents=True, exist_ok=True)
    tmp_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(mode="w+b", dir=output.parent, prefix=".007xb-", delete=False) as dst:
            tmp_path = Path(dst.name)
            dst.write(b"\x00" * (ARCHIVE_HEADER.size + dir_size))
            for name, path in srcs:
                offset = align(dst.tell())
                if offset > MAX_U32:
                    raise ToolError("Archive exceeds 32-bit offset range")
                dst.write(b"\x00" * (offset - dst.tell()))
                crc = 0
                size = 0
                with path.open("rb") as fh:
                    for chunk in iter(lambda: fh.read(1024 * 1024), b""):
                        dst.write(chunk)
                        crc = zlib.crc32(chunk, crc)
                        size += len(chunk)
                if size > MAX_U32:
                    raise ToolError(f"Asset exceeds 32-bit size range: {name}")
                file_records.append({
                    "name": name, "offset": offset, "size": size,
                    "crc32": crc & MAX_U32, "flags": 0,
                    "sha256": sha256_file(path),
                })
            total_size = dst.tell()
            if total_size > MAX_U32:
                raise ToolError("Archive exceeds 4 GiB format limit")
            directory = b"".join(
                ARCHIVE_ENTRY.pack(
                    row["name"].encode("ascii").ljust(64, b"\0"),
                    row["offset"], row["size"], row["crc32"], row["flags"]
                ) for row in file_records
            )
            dst.seek(dir_offset)
            dst.write(directory)
            dst.seek(0)
            dst.write(ARCHIVE_HEADER.pack(
                ARCHIVE_MAGIC, ARCHIVE_VERSION, len(file_records),
                dir_offset, ARCHIVE_ENTRY.size, total_size,
                zlib.crc32(directory) & MAX_U32
            ))
            dst.flush()
            os.fsync(dst.fileno())
        os.replace(tmp_path, output)
        return file_records
    finally:
        if tmp_path is not None and tmp_path.exists():
            tmp_path.unlink()


def inspect_archive(path: Path, *, verify_payloads: bool = True) -> list[dict]:
    if not path.is_file():
        raise ToolError(f"Archive not found: {path}")
    real_size = path.stat().st_size
    with path.open("rb") as fh:
        data = fh.read(ARCHIVE_HEADER.size)
        if len(data) != ARCHIVE_HEADER.size:
            raise ToolError("Truncated archive header")
        magic, version, count, dir_offset, record_size, declared_size, directory_crc = ARCHIVE_HEADER.unpack(data)
        if magic != ARCHIVE_MAGIC or version != ARCHIVE_VERSION:
            raise ToolError("Wrong 007xb archive magic/version")
        if not (1 <= count <= 10000) or record_size != ARCHIVE_ENTRY.size:
            raise ToolError("Invalid archive record count or entry size")
        if declared_size != real_size or dir_offset != ARCHIVE_HEADER.size:
            raise ToolError("Archive header size/offset mismatch")
        dir_end = dir_offset + count * record_size
        if dir_end > real_size:
            raise ToolError("Directory extends beyond archive")
        fh.seek(dir_offset)
        directory = fh.read(count * record_size)
        if zlib.crc32(directory) & MAX_U32 != directory_crc:
            raise ToolError("Archive directory checksum mismatch")
        rows = []
        names = set()
        used = []
        for i in range(count):
            raw_name, offset, size, crc, flags = ARCHIVE_ENTRY.unpack_from(directory, i * record_size)
            try:
                name = raw_name.split(b"\0", 1)[0].decode("ascii")
            except UnicodeDecodeError as exc:
                raise ToolError("Non-ASCII entry name") from exc
            if not name or name in names or flags != 0:
                raise ToolError(f"Duplicate/invalid entry or flags: {name!r}")
            names.add(name)
            if offset % ALIGNMENT or offset < align(dir_end) or size <= 0 or offset + size > real_size:
                raise ToolError(f"Bad archive offset/size/alignment: {name}")
            used.append((offset, offset + size, name))
            if verify_payloads:
                fh.seek(offset)
                remaining = size
                calculated_crc = 0
                while remaining:
                    part = fh.read(min(remaining, 1024 * 1024))
                    if not part:
                        raise ToolError(f"Truncated entry: {name}")
                    calculated_crc = zlib.crc32(part, calculated_crc)
                    remaining -= len(part)
                if calculated_crc & MAX_U32 != crc:
                    raise ToolError(f"CRC mismatch: {name}")
            rows.append({"name": name, "offset": offset, "size": size, "crc32": f"{crc:08x}"})
        used.sort()
        for previous, current in zip(used, used[1:]):
            if previous[1] > current[0]:
                raise ToolError(f"Overlapping entries: {previous[2]} and {current[2]}")
        return rows


def command_pack(args: argparse.Namespace) -> None:
    port = Path(args.pc_port).expanduser().resolve()
    sources = get_pack_sources(port, Path(args.logo).expanduser().resolve() if args.logo else None)
    output = Path(args.output).expanduser().resolve() if args.output else PROJ_ROOT / "data/007xb_assets.dat"
    if any(output == path.resolve() for _, path in sources):
        raise ToolError("Refusing to overwrite a source file with the output archive")
    records = write_archive(output, sources)
    inspect_archive(output)
    manifest = {
        "format": "007xb indexed data archive v1 (provisional)",
        "content_layout": "PC x86_64 source ROM + converted reference sidecars; NOT RXDK 32-bit compatible yet",
        "endian": "little",
        "sector_alignment": ALIGNMENT,
        "rom_sha1": US_SHA1,
        "archive_sha256": sha256_file(output),
        "archive_bytes": output.stat().st_size,
        "assets": records,
        "note": "Offline pack verified for CRC/structure, NOT yet tested by an Xbox runtime loader.",
    }
    manifest_path = output.with_suffix(".manifest.json")
    save_atomic(manifest_path, (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8"))
    print(f"[PASS] {len(records)} verified archive entries, size {format_bytes(output.stat().st_size)}")
    print(f"[WRITE] {output}")
    print(f"[WRITE] {manifest_path}")
    print("[NOTE] PC converter sidecars use 64-bit pointer layouts; Xbox 32-bit conversion remains to be implemented.")


def command_inspect(args: argparse.Namespace) -> None:
    path = Path(args.archive).expanduser().resolve()
    rows = inspect_archive(path)
    print(f"[PASS] 007xb v1 archive: {path} ({len(rows)} verified entries)")
    for row in rows:
        print(f"  {row['name']:<36} {row['size']:>11,} bytes  offset {row['offset']:>9,}  crc32 {row['crc32']}")


def parse_obj_lines(path: Path) -> tuple[list[tuple[float, float, float]], list[tuple[int, int]]]:
    if not path.is_file():
        raise ToolError(f"OBJ not found: {path}")
    vertices = []
    segments = []
    with path.open("r", encoding="utf-8-sig", errors="replace") as fh:
        for line_no, line in enumerate(fh, 1):
            parts = line.split("#", 1)[0].strip().split()
            if not parts:
                continue
            if parts[0] == "v":
                if len(parts) < 4:
                    raise ToolError(f"Invalid OBJ vertex at line {line_no}")
                vertices.append(tuple(float(v) for v in parts[1:4]))
            elif parts[0] == "l":
                if len(parts) < 3:
                    raise ToolError(f"Invalid OBJ polyline at line {line_no}")
                indices = []
                for token in parts[1:]:
                    value = int(token.split("/", 1)[0])
                    index = len(vertices) + value if value < 0 else value - 1
                    if not (0 <= index < len(vertices)):
                        raise ToolError(f"OBJ line index out of range at {line_no}: {token}")
                    indices.append(index)
                segments.extend(zip(indices, indices[1:]))
    if not vertices or not segments:
        raise ToolError("OBJ must contain v coordinates and l line segments (not only f faces)")
    return vertices, segments


def validate_logo(path: Path) -> None:
    if not path.is_file() or path.stat().st_size < LOGO_HEADER.size:
        raise ToolError(f"Logo file missing or invalid: {path}")
    with path.open("rb") as source:
        magic, version, n_v, n_l, reserved = LOGO_HEADER.unpack(source.read(LOGO_HEADER.size))
    if (magic != LOGO_MAGIC or version != 1 or reserved != 0 or
            LOGO_HEADER.size + n_v * 12 + n_l * 8 != path.stat().st_size or
            n_v == 0 or n_l == 0):
        raise ToolError(f"Logo file failed format verification: {path}")


def command_logo(args: argparse.Namespace) -> None:
    src = Path(args.obj).expanduser().resolve()
    vertices, segments = parse_obj_lines(src)
    if max(len(vertices), len(segments)) > MAX_U32:
        raise ToolError("Logo index count exceeds format range")
    dest = Path(args.output).expanduser().resolve() if args.output else PROJ_ROOT / "data/darkone83.lines"
    data = bytearray(LOGO_HEADER.pack(LOGO_MAGIC, 1, len(vertices), len(segments), 0))
    for x, y, z in vertices:
        data.extend(struct.pack("<fff", x, y, z))
    for a, b in segments:
        data.extend(struct.pack("<II", a, b))
    save_atomic(dest, bytes(data))
    print(f"[PASS] Logo: {len(vertices):,} vertices, {len(segments):,} line segments")
    print(f"[WRITE] {dest} ({format_bytes(len(data))})")


def parse_filelist(path: Path, rom_length: int) -> tuple[list[dict], int]:
    """Validate upstream scripts/filelist.u.csv ranges; preserve raw bytes exactly.

    The upstream columns are offset,length,destination,compressed,extract.
    This diagnostic path extracts every nonzero ROM span *raw*, ignoring the
    extract flag, and does not decompress. Use extract-upstream for proper assets.
    Zero-length entries are intentional placeholders and are skipped.
    """
    if not path.is_file():
        raise ToolError(f"File list not found: {path}")
    rows = []
    zero_length = 0
    seen = set()
    with path.open("r", encoding="utf-8-sig", newline="") as source:
        for line_number, fields in enumerate(csv.reader(source), 1):
            if not fields or not any(x.strip() for x in fields):
                continue
            if fields[0].lstrip().startswith("#"):
                continue
            if len(fields) != 5:
                raise ToolError(f"Invalid filelist row {line_number}: expected 5 columns, found {len(fields)}")
            try:
                offset, length = int(fields[0].strip()), int(fields[1].strip())
                compressed, other_flag = int(fields[3].strip()), int(fields[4].strip())
            except ValueError as exc:
                raise ToolError(f"Invalid numeric field in filelist row {line_number}") from exc
            if compressed not in (0, 1) or other_flag not in (0, 1):
                raise ToolError(f"Invalid compressed/extract flags at row {line_number}")
            name = fields[2].strip().replace("\\", "/")
            target = PurePosixPath(name)
            if (not name or target.is_absolute() or not target.parts or
                    any(x in (".", "..") for x in target.parts) or
                    ":" in name or "\x00" in name):
                raise ToolError(f"Unsafe filelist filename in row {line_number}: {name!r}")
            # Catch collisions on case-insensitive Windows filesystems too.
            folded = name.casefold()
            if folded in seen:
                raise ToolError(f"Duplicate filelist filename (case-insensitive): {name}")
            seen.add(folded)
            if offset < 0 or length < 0 or offset > rom_length or offset + length > rom_length:
                raise ToolError(f"Filelist extent outside ROM at row {line_number}: {name} offset={offset}, size={length}")
            if length == 0:
                zero_length += 1
                continue
            rows.append({"name": name, "rom_offset": offset, "size": length,
                         "compressed_flag": compressed, "extract_flag": other_flag,
                         "other_flag": other_flag})
    if not rows:
        raise ToolError("The supplied filelist has no nonempty assets")
    return rows, zero_length


def command_extract_raw(args: argparse.Namespace) -> None:
    """Extract the ROM's original byte ranges; no image/audio/model decoding."""
    rom_path = PROJ_ROOT / ROM_DATA_PATH
    info, rom = verify_rom_file(rom_path)
    filelist = (Path(args.filelist).expanduser().resolve() if args.filelist else
                Path(args.pc_port).expanduser().resolve() / "scripts/filelist.u.csv")
    rows, zero_length = parse_filelist(filelist, len(rom))
    output = Path(args.output).expanduser().resolve() if args.output else PROJ_ROOT / "data/raw"
    manifest_path = output / "_007xb_manifest.json"
    dest_files = []
    for row in rows:
        dest = output.joinpath(*PurePosixPath(row["name"]).parts)
        if dest == manifest_path:
            raise ToolError("A filelist entry conflicts with the manifest filename")
        dest_files.append(dest)
    for row, dest in zip(rows, dest_files):
        content = rom[row["rom_offset"]:row["rom_offset"] + row["size"]]
        if len(content) != row["size"]:
            raise ToolError(f"Short ROM read for {row['name']}")
        save_atomic(dest, content)
        # Verify exactly what was written to disk.
        if dest.read_bytes() != content:
            raise ToolError(f"Extracted asset did not match ROM data: {dest}")
        row["crc32"] = f"{zlib.crc32(content) & MAX_U32:08x}"
        row["sha256"] = hashlib.sha256(content).hexdigest()
    manifest = {
        "kind": "007xb exact raw ROM slice extraction (not PC conversion)",
        "rom_sha1": info["normalized_sha1"],
        "rom_bytes": len(rom),
        "filelist_name": filelist.name,
        "filelist_sha256": sha256_file(filelist),
        "extracted_files": len(rows),
        "zero_size_entries_skipped": zero_length,
        "total_raw_bytes": sum(row["size"] for row in rows),
        "note": "ROM offsets may overlap; compressed resources remain compressed and in N64 byte layout.",
        "assets": rows,
    }
    save_atomic(manifest_path, (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8"))
    print(f"[PASS] Extracted {len(rows):,} raw ROM ranges ({format_bytes(manifest['total_raw_bytes'])})")
    print(f"       Skipped {zero_length:,} zero-length placeholders")
    print(f"[WRITE] {output}")
    print(f"[WRITE] {manifest_path}")
    print("[NOTE] Raw ranges are NOT converted to PC/Xbox-endian models, textures or audio.")


def command_pack_raw(args: argparse.Namespace) -> None:
    """Pack verified filelist slices + original ROM into the provisional v1 archive.

    This is a storage and integrity proof of concept, NOT a replacement for the
    PC port's endian/struct layout sidecars or an engine-ready resource pack.
    """
    manifest_path = Path(args.manifest).expanduser().resolve() if args.manifest else PROJ_ROOT / "data/raw/_007xb_manifest.json"
    if not manifest_path.is_file():
        raise ToolError(f"Raw extraction manifest not found: {manifest_path}")
    try:
        manifest = json.loads(manifest_path.read_text("utf-8"))
    except (json.JSONDecodeError, UnicodeError) as exc:
        raise ToolError(f"Unreadable raw extraction manifest: {manifest_path}") from exc
    if manifest.get("kind") != "007xb exact raw ROM slice extraction (not PC conversion)":
        raise ToolError("Manifest is not a 007xb raw-extraction manifest")
    if manifest.get("rom_sha1") != US_SHA1:
        raise ToolError("Manifest ROM checksum does not match the NTSC-U baseline")
    if manifest.get("extracted_files") != len(manifest.get("assets", [])):
        raise ToolError("Manifest extracted file count mismatch")
    rom_path = PROJ_ROOT / ROM_DATA_PATH
    verify_rom_file(rom_path)
    output = Path(args.output).expanduser().resolve() if args.output else PROJ_ROOT / "data/007xb_raw.dat"
    output.parent.mkdir(parents=True, exist_ok=True)
    sources = [("rom/ge007.ntsc-final.z64", rom_path)]
    names = []
    root = manifest_path.parent
    for index, row in enumerate(manifest["assets"]):
        name = row.get("name")
        if not isinstance(name, str) or not name or PurePosixPath(name).is_absolute() or ":" in name or ".." in PurePosixPath(name).parts:
            raise ToolError(f"Unsafe name in extraction manifest entry {index}")
        file = root.joinpath(*PurePosixPath(name).parts)
        if not file.is_file() or file.stat().st_size != row.get("size") or sha256_file(file) != row.get("sha256"):
            raise ToolError(f"Missing or altered raw extraction file: {file}")
        entry = f"raw/{index:04d}.bin"
        sources.append((entry, file))
        names.append({"archive_entry": entry, "source_name": name,
                      "rom_offset": row["rom_offset"], "size": row["size"]})
    if args.logo:
        logo = Path(args.logo).expanduser().resolve()
        if not logo.is_file() or logo.stat().st_size < LOGO_HEADER.size:
            raise ToolError(f"Logo file missing or invalid: {logo}")
        with logo.open("rb") as fd:
            header = fd.read(LOGO_HEADER.size)
        magic, version, vertices, segments, reserved = LOGO_HEADER.unpack(header)
        expected = LOGO_HEADER.size + vertices * 12 + segments * 8
        if magic != LOGO_MAGIC or version != 1 or reserved != 0 or expected != logo.stat().st_size:
            raise ToolError(f"Not a valid 007xb wireframe line file: {logo}")
        sources.append(("branding/darkone83.lines", logo))
    if any(output == path.resolve() for _, path in sources):
        raise ToolError("Refusing to replace an asset with the archive")
    records = write_archive(output, sources)
    inspect_archive(output)
    output_manifest = {
        "kind": "007xb provisional raw asset archive",
        "format": "007xb indexed data archive v1 (provisional)",
        "archive_sha256": sha256_file(output),
        "archive_bytes": output.stat().st_size,
        "rom_sha1": US_SHA1,
        "raw_asset_map": names,
        "assets": records,
        "note": "Verified archive of ROM and selected ROM slices, NOT complete PC/Xbox resource conversion.",
    }
    save_atomic(output.with_suffix(".manifest.json"), (json.dumps(output_manifest, indent=2, sort_keys=True) + "\n").encode("utf-8"))
    print(f"[PASS] Packed and CRC-verified {len(records)} entries ({format_bytes(output.stat().st_size)})")
    print(f"[WRITE] {output}")
    print(f"[WRITE] {output.with_suffix('.manifest.json')}")
    print("[NOTE] Provisional format; no RXDK runtime loader or converted stage sidecars yet.")


# ------------------------------------------------------------------ upstream ---
UPSTREAM_REQUIRED = (
    "tools_pc/d43_emit.py", "tools_pc/d69_emit.py", "tools_pc/d88_emit.py",
    "tools_pc/d88_propdefs.py", "scripts/filelist.u.csv",
    "assets/obseg/file_resource_table.inc.c",
)
UPSTREAM_EXTRA = ("assets/images.def", "imagelist.u.csv", "LICENSE", "NOTICE")
MAX_RZ_INFLATED = 32 * 1024 * 1024


def check_upstream_tree(upstream: Path) -> dict:
    if not upstream.is_dir():
        raise ToolError(f"PC port source directory missing: {upstream}\nUse 'stage --zip ...' first, or point to a checkout directory.")
    missing = [p for p in UPSTREAM_REQUIRED if not (upstream / p).is_file()]
    if missing:
        raise ToolError("PC port source is missing required converter inputs: " + ", ".join(missing))
    headers = [p for p in (upstream / "assets").rglob("*")
               if p.is_file() and p.name.lower().endswith("modelfileheader.inc.c")]
    if not headers:
        raise ToolError("No assets/**/ModelFileHeader.inc.c metadata found; source archive was staged incompletely")
    return {"model_headers": len(headers)}


def stage_from_zip(archive: Path, output: Path) -> dict:
    """Extract only Python converter prerequisites from the supplied PC-port ZIP.

    Does not extract/copy game data; the tool only needs the committed metadata.
    Avoids importing a full PC source tree and allows users to supply a ZIP.
    """
    if not archive.is_file():
        raise ToolError(f"Upstream ZIP not found: {archive}")
    if output.exists():
        raise ToolError(f"Refusing to overwrite existing upstream staging directory: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    tmp: Path | None = None
    try:
        with zipfile.ZipFile(archive, "r") as z:
            roots = set()
            for name in z.namelist():
                if name == "tools_pc/d43_emit.py":
                    roots.add("")
                elif name.endswith("/tools_pc/d43_emit.py"):
                    roots.add(name[:-len("tools_pc/d43_emit.py")])
            if len(roots) != 1:
                raise ToolError("ZIP must contain exactly one goldeneye-pc-port source tree with tools_pc/d43_emit.py")
            prefix = next(iter(roots))
            selected = []
            for entry in z.infolist():
                name = entry.filename
                if entry.is_dir() or not name.startswith(prefix):
                    continue
                rel = name[len(prefix):]
                rp = PurePosixPath(rel)
                if not rel or rp.is_absolute() or ".." in rp.parts or "\\" in rel or ":" in rel:
                    continue
                keep = (rel in UPSTREAM_REQUIRED or rel in UPSTREAM_EXTRA or
                        (rel.startswith("assets/") and rel.lower().endswith("modelfileheader.inc.c")))
                if not keep:
                    continue
                # Extract ordinary regular files only; no archive symlink support.
                unix_type = (entry.external_attr >> 16) & 0o170000
                if unix_type not in (0, 0o100000):
                    raise ToolError(f"Unsupported ZIP entry type: {rel}")
                if entry.file_size > 12 * 1024 * 1024:
                    raise ToolError(f"Unexpectedly large converter input in ZIP: {rel}")
                selected.append((entry, rp))
            tmp = Path(tempfile.mkdtemp(prefix=".007xb-stage-", dir=output.parent))
            for entry, rp in selected:
                dest = tmp.joinpath(*rp.parts)
                dest.parent.mkdir(parents=True, exist_ok=True)
                with z.open(entry, "r") as src, dest.open("wb") as dst:
                    shutil.copyfileobj(src, dst)
            stats = check_upstream_tree(tmp)
            manifest = {
                "kind": "007xb minimal upstream converter+metadata stage",
                "source_zip_sha256": sha256_file(archive),
                "selected_files": len(selected),
                "model_headers": stats["model_headers"],
                "source_zip_filename": archive.name,
                "note": "Original game ROM and extracted copyrighted game resources are excluded",
            }
            save_atomic(tmp / "_007xb_stage.json", (json.dumps(manifest, indent=2) + "\n").encode())
        tmp.rename(output)
        tmp = None
        return manifest
    except zipfile.BadZipFile as exc:
        raise ToolError(f"Invalid upstream ZIP: {exc}") from exc
    finally:
        if tmp is not None:
            shutil.rmtree(tmp, ignore_errors=True)


def command_stage(args: argparse.Namespace) -> None:
    archive = Path(args.zip).expanduser().resolve()
    output = Path(args.output).expanduser().resolve() if args.output else PROJ_ROOT / "external/goldeneye-pc-port"
    info = stage_from_zip(archive, output)
    print(f"[PASS] Staged {info['selected_files']} upstream converter/metadata files ({info['model_headers']} ModelFileHeader files)")
    print(f"[WRITE] {output}")
    print("[NOTE] This is a minimal converter workspace, not the complete PC source tree.")


def verify_sidecar(manifest: Path, data_file: Path, kind: str) -> dict:
    """Check exact row spans / alignment / magic expected by upstream PC loader."""
    if not manifest.is_file() or not data_file.is_file():
        raise ToolError(f"Missing {kind} PC sidecar or manifest: {manifest}, {data_file}")
    total = data_file.stat().st_size
    if total <= 0:
        raise ToolError(f"Empty PC sidecar: {data_file}")
    rows = []
    seen = set()
    with manifest.open("r", encoding="utf-8-sig", newline="") as source:
        reader = csv.reader(source)
        if next(reader, None) != ["name", "offset", "size"]:
            raise ToolError(f"Wrong manifest header in {manifest}")
        with data_file.open("rb") as blob:
            prev_end = 0
            for num, fields in enumerate(reader, 2):
                if len(fields) != 3:
                    raise ToolError(f"Wrong field count in {manifest} at line {num}")
                name = fields[0]
                if not name or name.casefold() in seen or "\x00" in name:
                    raise ToolError(f"Duplicate/invalid sidecar name in {manifest}: {name!r}")
                seen.add(name.casefold())
                try:
                    offset, size = int(fields[1]), int(fields[2])
                except ValueError as exc:
                    raise ToolError(f"Invalid sidecar numeric field at {manifest}:{num}") from exc
                if offset < prev_end or size <= 0 or offset % 16 or offset + size > total:
                    raise ToolError(f"Invalid sidecar extent at {manifest}:{num}: {name} ({offset}, {size})")
                if kind == "pcmodels" or (kind == "pccg" and not name.startswith("bg/")):
                    blob.seek(offset)
                    if blob.read(2) != b"\x11\x72":
                        raise ToolError(f"Missing RZ header in {kind} sidecar {name}")
                prev_end = offset + size
                rows.append((name, offset, size))
    if not rows:
        raise ToolError(f"No sidecar entries in {manifest}")
    return {"entries": len(rows), "bytes": total, "last_end": rows[-1][1] + rows[-1][2],
            "stage_setups": sum(n.startswith(("Usetup", "Ump_setup")) for n, _, _ in rows)}


def verify_upstream_sidecars(pc_port: Path) -> dict:
    result = {}
    for kind in ("pcmodels", "pccg"):
        path = pc_port / f"data/{kind}-ntsc-final"
        result[kind] = verify_sidecar(path / "manifest.csv", path / f"{kind}.bin", kind)
    if result["pccg"]["stage_setups"] < 34:
        raise ToolError("pccg stage-setup conversion is incomplete (expected 21 solo + 13 multiplayer); run d88_emit.py")
    return result


def command_check_sidecars(args: argparse.Namespace) -> None:
    port = Path(args.pc_port).expanduser().resolve()
    summaries = verify_upstream_sidecars(port)
    for name, summary in summaries.items():
        print(f"[PASS] {name}: {summary['entries']} entries, {format_bytes(summary['bytes'])}")
    print(f"[PASS] {summaries['pccg']['stage_setups']} stage setups represented")
    print("[NOTE] Verified PC x86_64 layout only; Xbox 32-bit structs need a separate conversion.")


def decompress_1172(payload: bytes, source: str = "resource") -> bytes:
    if len(payload) < 5 or payload[:2] != b"\x11\x72":
        raise ToolError(f"Bad 1172 compressed resource header: {source}")
    try:
        inflater = zlib.decompressobj(-15)
        out = inflater.decompress(payload[2:], MAX_RZ_INFLATED + 1)
        if len(out) > MAX_RZ_INFLATED or inflater.unconsumed_tail:
            raise ToolError(f"Decompressed resource exceeds safety cap: {source}")
        out += inflater.flush()
        if not inflater.eof or len(out) > MAX_RZ_INFLATED:
            raise ToolError(f"Truncated or oversized RZ resource: {source}")
        return out
    except zlib.error as exc:
        raise ToolError(f"RZ decode failed for {source}: {exc}") from exc


def parse_image_rows(upstream: Path, rom_size: int) -> tuple[list[dict], int]:
    """Follow the upstream sync_imagelist_with_def mapping without rewriting images.def."""
    images_def = upstream / "assets/images.def"
    image_list = upstream / "imagelist.u.csv"
    if not images_def.is_file() or not image_list.is_file():
        raise ToolError("Image extraction needs assets/images.def and imagelist.u.csv from the PC-port ZIP")
    names = []
    for line in images_def.read_text("utf-8-sig").splitlines():
        match = re.match(r"^\s*IMAGE\(\s*([^,\s]+)", line)
        if match:
            names.append(match.group(1))
    if not names:
        raise ToolError("No IMAGE(...) entries found in images.def")
    rows, zeros = parse_filelist(image_list, rom_size)
    if zeros:
        raise ToolError(f"Unexpected zero-length image list entries: {zeros}")
    for i, row in enumerate(rows):
        # Current upstream has 2,699 ROM images but 2,698 named IMAGE entries.
        # Preserve the unmatched final blob as image2698.bin instead of inventing a name.
        if i < len(names):
            name = names[i]
            if not re.fullmatch(r"[0-9A-Za-z_]+", name):
                raise ToolError(f"Unsafe IMAGE name from images.def: {name!r}")
            row["name"] = f"assets/images/split/{name}.bin"
        else:
            row["name"] = f"assets/images/split/image{i}.bin"
        row["compressed_flag"] = 0
        row["extract_flag"] = 1
        row["other_flag"] = 1
    if len(names) > len(rows):
        raise ToolError("images.def lists more names than image extents; cannot map them safely")
    all_names = [r["name"].casefold() for r in rows]
    if len(all_names) != len(set(all_names)):
        raise ToolError("Duplicate output image names after IMAGE(...) mapping")
    return rows, max(0, len(rows) - len(names))


def extract_upstream_assets(rom: bytes, upstream: Path, output: Path, include_images: bool, *, progress=None, cancel=None) -> dict:
    rows, zero = parse_filelist(upstream / "scripts/filelist.u.csv", len(rom))
    extraction_rows = [r for r in rows if r["extract_flag"] == 1]
    skipped = len(rows) - len(extraction_rows)
    count_images = 0
    unnamed = 0
    if include_images:
        image_rows, unnamed = parse_image_rows(upstream, len(rom))
        count_images = len(image_rows)
        extraction_rows.extend(image_rows)
    seen = set()
    for row in extraction_rows:
        name = row["name"]
        rp = PurePosixPath(name)
        if rp.is_absolute() or ".." in rp.parts or ":" in name or "\\" in name:
            raise ToolError(f"Unsafe upstream output file: {name}")
        if name.casefold() in seen:
            raise ToolError(f"Duplicate upstream output file: {name}")
        seen.add(name.casefold())
    # One directory structure for both regular files and images. Derived game
    # assets should remain in git-ignored data/, never in the distributed ZIP.
    manifest_rows = []
    total_bytes = 0
    count_rz = 0
    for index, row in enumerate(extraction_rows):
        if cancel is not None and cancel.is_set():
            raise ToolError("Asset extraction cancelled by user")
        src = rom[row["rom_offset"]:row["rom_offset"] + row["size"]]
        if len(src) != row["size"]:
            raise ToolError(f"Truncated ROM resource {row['name']}")
        content = decompress_1172(src, row["name"]) if row["compressed_flag"] else src
        if row["compressed_flag"]:
            count_rz += 1
        path = output.joinpath(*PurePosixPath(row["name"]).parts)
        save_atomic(path, content)
        if path.stat().st_size != len(content) or sha256_file(path) != hashlib.sha256(content).hexdigest():
            raise ToolError(f"Write verification failed: {path}")
        manifest_rows.append({"name": row["name"], "rom_offset": row["rom_offset"],
                              "rom_size": row["size"], "inflated": bool(row["compressed_flag"]),
                              "size": len(content), "sha256": hashlib.sha256(content).hexdigest()})
        total_bytes += len(content)
        if progress is not None and ((index + 1) % 25 == 0 or index + 1 == len(extraction_rows)):
            progress(index + 1, len(extraction_rows))
        elif progress is None and (index + 1) % 250 == 0:
            print(f"[EXTRACT] {index + 1}/{len(extraction_rows)}", flush=True)
    manifest = {"kind": "007xb upstream-compatible extracted assets",
                "rom_sha1": hashlib.sha1(rom).hexdigest(),
                "filelist_sha256": sha256_file(upstream / "scripts/filelist.u.csv"),
                "extracted_files": len(extraction_rows), "compressed_inflated": count_rz,
                "skipped_filelist_flag": skipped, "skipped_zero_length": zero,
                "image_files": count_images, "unnamed_image_entries": unnamed,
                "total_output_bytes": total_bytes, "assets": manifest_rows}
    save_atomic(output / "_007xb_upstream_manifest.json", (json.dumps(manifest, indent=2) + "\n").encode())
    return manifest


def command_extract_upstream(args: argparse.Namespace) -> None:
    upstream = Path(args.pc_port).expanduser().resolve()
    check_upstream_tree(upstream)
    info, rom = verify_rom_file(PROJ_ROOT / ROM_DATA_PATH)
    out = Path(args.output).expanduser().resolve() if args.output else PROJ_ROOT / "data/upstream-extracted"
    m = extract_upstream_assets(rom, upstream, out, include_images=not args.no_images)
    print(f"[PASS] {m['extracted_files']:,} upstream assets ({m['compressed_inflated']:,} RZ decoded; {m['image_files']:,} images)")
    print(f"[PASS] {format_bytes(m['total_output_bytes'])}; ROM SHA-1 {info['normalized_sha1']}")
    print(f"[WRITE] {out}")
    if m["unnamed_image_entries"]:
        print(f"[NOTE] {m['unnamed_image_entries']} image has no images.def name; kept with its raw numbered name")
    print("[NOTE] This covers filelist+image resource extraction; the upstream full source rebuild also extracts RSP microcode.")


def command_pipeline(args: argparse.Namespace) -> None:
    """One command for archive source -> converter stage -> PC reference pack."""
    if args.zip:
        upstream = PROJ_ROOT / "external/goldeneye-pc-port"
        archive = Path(args.zip).expanduser().resolve()
        if upstream.exists():
            stamp = upstream / "_007xb_stage.json"
            if not stamp.is_file():
                raise ToolError(f"Staging directory already exists without a 007xb stamp: {upstream}")
            info = json.loads(stamp.read_text("utf-8"))
            if info.get("source_zip_sha256") != sha256_file(archive):
                raise ToolError("Upstream staging ZIP changed. Choose another stage directory or manually remove external/goldeneye-pc-port")
            print(f"[PASS] Reusing staged upstream converters: {upstream}")
        else:
            stage_from_zip(archive, upstream)
            print(f"[PASS] Staged PC port converter inputs: {upstream}")
    else:
        upstream = Path(args.pc_port).expanduser().resolve()
    if args.rom:
        command_prepare(argparse.Namespace(rom=args.rom))
    else:
        verify_rom_file(PROJ_ROOT / ROM_DATA_PATH)
    command_emit(argparse.Namespace(pc_port=str(upstream)))
    obj = Path(args.obj).expanduser().resolve() if args.obj else PROJ_ROOT / "branding/darkone83-wireframe.obj"
    if obj.is_file():
        command_logo(argparse.Namespace(obj=str(obj), output=None))
    else:
        print("[NOTE] Darkone83 OBJ was not found; logo is not added to the pack")
    command_pack(argparse.Namespace(pc_port=str(upstream), output=args.output, logo=None))
    print("[PASS] PC reference asset pipeline complete (not yet an RXDK build)")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="007xb GoldenEye NTSC-U offline asset tools")
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("prepare", help="Validate, normalize and stage a US GoldenEye ROM")
    p.add_argument("--rom", required=True, help="User-owned GoldenEye .z64, .v64 or .n64 file")
    p.set_defaults(func=command_prepare)
    p = sub.add_parser("pipeline", help="One-command ROM -> upstream PC sidecars -> validated reference pack")
    origin = p.add_mutually_exclusive_group(required=True)
    origin.add_argument("--zip", help="Your PC-port source archive (staged automatically)")
    origin.add_argument("--pc-port", help="Existing unpacked or staged PC port directory")
    p.add_argument("--rom", help="GoldenEye ROM; optional if already prepared")
    p.add_argument("--obj", help="Optional wireframe OBJ (defaults to branding/darkone83-wireframe.obj)")
    p.add_argument("--output", help="Output reference .dat archive")
    p.set_defaults(func=command_pipeline)
    p = sub.add_parser("stage", help="Stage the actual PC-port ZIP's converter scripts + committed metadata")
    p.add_argument("--zip", required=True, help="Your goldeneye-pc-port-main.zip")
    p.add_argument("--output", help="Output directory (default: external/goldeneye-pc-port)")
    p.set_defaults(func=command_stage)
    p = sub.add_parser("extract-upstream", help="Honor filelist extract flag, inflate 1172 data, and extract named images")
    p.add_argument("--pc-port", required=True, help="Staged or unpacked PC port")
    p.add_argument("--output", help="Output root (default: data/upstream-extracted)")
    p.add_argument("--no-images", action="store_true", help="Skip imagelist/images.def image extraction")
    p.set_defaults(func=command_extract_upstream)
    p = sub.add_parser("check-sidecars", help="Check converter output manifests, limits, and RZ magic")
    p.add_argument("--pc-port", required=True)
    p.set_defaults(func=command_check_sidecars)
    p = sub.add_parser("extract-raw", help="Extract ROM filelist byte ranges without decompressing or converting")
    source = p.add_mutually_exclusive_group(required=True)
    source.add_argument("--filelist", help="Upstream scripts/filelist.u.csv (or sample probe list)")
    source.add_argument("--pc-port", help="GoldenEye PC port checkout; uses scripts/filelist.u.csv")
    p.add_argument("--output", help="Output directory (default: 007xb/data/raw)")
    p.set_defaults(func=command_extract_raw)
    p = sub.add_parser("emit", help="Run upstream PC port model/stage/setup converters")
    p.add_argument("--pc-port", required=True, help="Path to a goldeneye-pc-port checkout")
    p.set_defaults(func=command_emit)
    p = sub.add_parser("pack", help="Pack PC reference sidecars + ROM in provisional archive; NOT Xbox-ready")
    p.add_argument("--pc-port", required=True, help="Path to the staged or unpacked PC port")
    p.add_argument("--logo", help="Optional Darkone83 .lines asset (defaults to data/darkone83.lines when present)")
    p.add_argument("--output", help="Output .dat path (default: 007xb/data/007xb_assets.dat)")
    p.set_defaults(func=command_pack)
    p = sub.add_parser("pack-raw", help="Package validated ROM slices + original ROM into a test archive")
    p.add_argument("--manifest", help="Extraction manifest (default: data/raw/_007xb_manifest.json)")
    p.add_argument("--logo", help="Optional logo .lines file produced by 'logo' command")
    p.add_argument("--output", help="Output .dat path (default: 007xb/data/007xb_raw.dat)")
    p.set_defaults(func=command_pack_raw)
    p = sub.add_parser("inspect", help="Validate a packed 007xb .dat archive")
    p.add_argument("--archive", required=True)
    p.set_defaults(func=command_inspect)
    p = sub.add_parser("logo", help="Convert OBJ line geometry into future Direct3D wireframe data")
    p.add_argument("--obj", required=True)
    p.add_argument("--output", help="Output .lines path (default: 007xb/data/darkone83.lines)")
    p.set_defaults(func=command_logo)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        args.func(args)
        return 0
    except (ToolError, OSError, ValueError, struct.error) as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
