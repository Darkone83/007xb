"""007xb Asset Studio -- one orchestrated offline asset preparation workflow.

Only Python's standard library is required. The user supplies their own US
GoldenEye N64 ROM; this distribution bundles the minimum PC-port converter
scripts and source metadata. No ROM or extracted game assets are distributed.
The output is ONE 007xb_assets.dat file, containing the ROM, PC sidecars,
and bundled logo. All conversion/staging artifacts stay under tools/.cache/.
No loose extracted game-resource tree is written to the Xbox data folder.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import importlib.util
import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import uuid
from dataclasses import dataclass
from pathlib import Path

# PyInstaller keeps data files in _MEIPASS, but writes must go next to the EXE.
RESOURCE_ROOT = Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parent.parent))
ENGINE_PATH = RESOURCE_ROOT / 'tools' / '007xb_assets.py'
spec = importlib.util.spec_from_file_location('xb007_asset_engine', ENGINE_PATH)
if spec is None or spec.loader is None:
    raise RuntimeError('Cannot load 007xb asset engine')
xb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(xb)
def _project_root():
    if not getattr(sys, 'frozen', False):
        return Path(__file__).resolve().parent.parent
    # Builds initially live in tools/dist/; released EXEs live in tools/.
    folder = Path(sys.executable).resolve().parent
    if folder.name.casefold() == 'dist' and folder.parent.name.casefold() == 'tools':
        return folder.parent.parent
    return folder.parent

DEFAULT_PROJECT = _project_root()
if getattr(sys, 'frozen', False):
    xb.PROJ_ROOT = DEFAULT_PROJECT

STAGES = (
    ('validate', 'Verify ROM'),
    ('source', 'Prepare built-in tools'),
    ('convert', 'Build game sidecars'),
    ('logo', 'Prepare archive branding'),
    ('package', 'Package & verify'),
)
STUDIO_VERSION = '0.6'


# Bundle contains only converter scripts, source metadata and licenses.
# It must NEVER include a ROM, extracted graphics, audio, or other game bytes.
BUNDLED_REFERENCE = RESOURCE_ROOT / 'tools' / '007xb_converter_reference.zip'


@dataclass
class Settings:
    rom: Path
    source: Path | None = None  # Optional developer override; GUI never asks for it.
    logo: Path | None = None    # Bundled Darkone83 logo used by default.
    project: Path = DEFAULT_PROJECT


class StudioError(RuntimeError):
    pass


class Cancelled(StudioError):
    pass


def discover_inputs(root: Path) -> dict:
    """Look in the project's input locations and Downloads; never recurse disks."""
    places = [root, root / 'inputs', root / 'source', root.parent]
    downloads = Path.home() / 'Downloads'
    if downloads.is_dir():
        places.append(downloads)
    found: dict[str, Path | None] = {'rom': None}
    fallback_rom = None
    for folder in places:
        if not folder.is_dir():
            continue
        try:
            entries = list(folder.iterdir())
        except OSError:
            continue
        for p in entries:
            name = p.name.casefold()
            if p.is_file() and p.suffix.lower() in ('.z64', '.v64', '.n64'):
                if 'goldeneye' in name or 'ge007' in name:
                    if found['rom'] is None:
                        found['rom'] = p
                elif fallback_rom is None:
                    fallback_rom = p
    # A generic name is acceptable; ROM identity is validated by SHA-1 on Build.
    if found['rom'] is None:
        found['rom'] = fallback_rom
    return found


def _folder_files(folder: Path):
    paths = [folder / p for p in xb.UPSTREAM_REQUIRED + xb.UPSTREAM_EXTRA if (folder / p).is_file()]
    if (folder / 'assets').is_dir():
        paths += [p for p in (folder / 'assets').rglob('*') if p.is_file() and not p.is_symlink() and p.name.lower().endswith('modelfileheader.inc.c')]
    return sorted(set(paths))


def _source_fingerprint(src: Path) -> str:
    if src.is_file() and src.suffix.casefold() == '.zip':
        return xb.sha256_file(src)
    if src.is_dir():
        xb.check_upstream_tree(src)
        hasher = hashlib.sha256()
        for f in _folder_files(src):
            hasher.update(f.relative_to(src).as_posix().encode('utf-8'))
            hasher.update(xb.sha256_file(f).encode('ascii'))
        return hasher.hexdigest()
    raise StudioError('PC-port source must be a goldeneye-pc-port ZIP or its source directory.')


def _stage_from_folder(src: Path, dst: Path):
    xb.check_upstream_tree(src)
    if dst.exists():
        raise StudioError(f'Staging destination already exists: {dst}')
    dst.mkdir(parents=True, exist_ok=False)
    try:
        for f in _folder_files(src):
            relative = f.relative_to(src)
            target = dst / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, target)
        return xb.check_upstream_tree(dst)
    except Exception:
        shutil.rmtree(dst, ignore_errors=True)
        raise


def _safe_restage(source: Path, dest: Path, source_hash: str, log):
    """Swap only Studio's own staging directory, with backup/rollback."""
    marker = dest / '_007xb_studio_source.json'
    if marker.is_file():
        try:
            stamp = json.loads(marker.read_text('utf-8'))
            if stamp.get('source_hash') == source_hash:
                xb.check_upstream_tree(dest)
                log('Reusing previously verified source workspace')
                return dest
        except (OSError, ValueError, xb.ToolError):
            pass
    # Fail closed on folders not owned by Studio. Never delete user content.
    if dest.exists() and not marker.is_file():
        raise StudioError(f'An unrecognized folder blocks Studio staging: {dest}. Move it elsewhere first; it has not been modified.')
    dest.parent.mkdir(parents=True, exist_ok=True)
    temp = dest.parent / ('.007xb-source-new-' + uuid.uuid4().hex[:10])
    backup = dest.parent / ('.007xb-source-previous-' + uuid.uuid4().hex[:10])
    had_old = dest.exists()
    try:
        if source.is_file():
            info = xb.stage_from_zip(source, temp)
            n = info['selected_files']
        else:
            _stage_from_folder(source, temp)
            n = len(_folder_files(source))
        xb.save_atomic(temp / '_007xb_studio_source.json', json.dumps({
            'studio_version': STUDIO_VERSION, 'source_hash': source_hash,
            'source_kind': 'zip' if source.is_file() else 'folder',
            'selected_files': n,
        }, indent=2).encode('utf-8'))
        if had_old:
            dest.rename(backup)
        try:
            temp.rename(dest)
        except BaseException:
            if had_old and backup.exists():
                backup.rename(dest)
            raise
        log(f'Staged {n:,} required upstream source files')
    finally:
        if temp.exists():
            shutil.rmtree(temp, ignore_errors=True)
        if backup.exists() and dest.exists():
            shutil.rmtree(backup, ignore_errors=True)
    return dest


class Pipeline:
    def __init__(self, settings: Settings, emit=None, cancelled: threading.Event | None = None):
        self.s = settings
        self.emit = emit or (lambda *_: None)
        self.cancel = cancelled or threading.Event()
        self.staged: Path | None = None
        self.source_hash = ''
        self.rom_bytes: bytes | None = None
        self.rom_info = {}
        self._log_handle = None

    @property
    def project(self):
        return self.s.project.expanduser().resolve()

    def log(self, msg: str):
        text = str(msg).rstrip('\r\n')
        if not text:
            return
        if self._log_handle is not None:
            self._log_handle.write(text + '\n')
            self._log_handle.flush()
        self.emit('log', text)

    def check_cancel(self):
        if self.cancel.is_set():
            raise Cancelled('Build cancelled. Previously validated files were retained.')

    def stage(self, index, text=None):
        self.check_cancel()
        key, name = STAGES[index]
        self.emit('stage', {'index': index, 'key': key, 'label': name, 'detail': text or name})
        self.emit('progress', index / len(STAGES))
        self.log(f'\n[{index + 1}/{len(STAGES)}] {name}')

    def done(self, index, detail='Complete'):
        self.emit('stage_done', {'index': index, 'detail': detail})
        self.emit('progress', (index + 1) / len(STAGES))
        self.log(f'[OK] {detail}')

    def _run_converter_frozen(self, filename, args):
        """Run embedded Python converters through the frozen EXE's child mode.

        Windowed PyInstaller executables have no stdout console, so the child
        writes a local log file and we stream its contents into the GUI.
        """
        self.check_cancel()
        assert self.staged is not None
        log_path = self.staged / 'data' / ('.007xb-' + filename + '.log')
        log_path.parent.mkdir(parents=True, exist_ok=True)
        if log_path.exists():
            log_path.unlink()
        env = os.environ.copy()
        env['GE007_CONVERTER_LOG'] = str(log_path)
        cmd = [sys.executable, '--007xb-converter', filename, *args]
        self.log(f'Running embedded {filename}...')
        flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
        proc = subprocess.Popen(cmd, cwd=self.staged, stdin=subprocess.DEVNULL,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                env=env, creationflags=flags)
        position = 0
        pending = ''

        def flush_new():
            nonlocal position, pending
            if not log_path.is_file():
                return
            with log_path.open('r', encoding='utf-8', errors='replace') as stream:
                stream.seek(position)
                pending += stream.read()
                position = stream.tell()
            while '\n' in pending:
                line, pending = pending.split('\n', 1)
                if line.strip():
                    self.log(line.rstrip())

        try:
            while proc.poll() is None:
                if self.cancel.is_set():
                    proc.terminate()
                    try:
                        proc.wait(timeout=4)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                    raise Cancelled(f'Cancelled while running {filename}')
                flush_new()
                time.sleep(0.15)
            flush_new()
            if pending.strip():
                self.log(pending.strip())
            if proc.returncode:
                raise StudioError(f'Embedded {filename} failed (exit {proc.returncode}). See build.log for details.')
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()

    def _run_converter(self, filename, args):
        self.check_cancel()
        if getattr(sys, 'frozen', False):
            return self._run_converter_frozen(filename, args)
        interpreter = Path(sys.executable)
        # .pyw starts under pythonw.exe on Windows. The converters need a
        # normal Python interpreter so redirected stdout/stderr always work.
        if os.name == 'nt' and interpreter.name.casefold() == 'pythonw.exe':
            console_exe = interpreter.with_name('python.exe')
            if not console_exe.is_file():
                raise StudioError(f'Python console interpreter was not found: {console_exe}')
            interpreter = console_exe
        cmd = [str(interpreter), f'tools_pc/{filename}', *args]
        self.log(f'Running upstream {filename}...')
        flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
        # Pipes are drained in a reader thread so Stop works even between lines.
        proc = subprocess.Popen(cmd, cwd=self.staged, stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, encoding='utf-8', errors='replace',
                                bufsize=1, creationflags=flags)
        outq: queue.Queue = queue.Queue()

        def drain():
            try:
                for line in proc.stdout:
                    outq.put(line)
            finally:
                outq.put(None)
        read_thread = threading.Thread(target=drain, daemon=True)
        read_thread.start()
        ended = False
        try:
            while not ended:
                if self.cancel.is_set():
                    proc.terminate()
                    try: proc.wait(timeout=4)
                    except subprocess.TimeoutExpired: proc.kill()
                    raise Cancelled(f'Cancelled while running {filename}')
                try:
                    line = outq.get(timeout=0.15)
                except queue.Empty:
                    continue
                if line is None:
                    ended = True
                elif line.strip():
                    self.log(line.rstrip())
            return_code = proc.wait(timeout=60)
            if return_code:
                raise StudioError(f'{filename} failed (exit {return_code}). See build.log for the upstream error.')
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            if proc.stdout is not None:
                proc.stdout.close()
            read_thread.join(timeout=1)

    def run(self):
        project = self.project
        if xb.PROJ_ROOT != project:
            raise StudioError(f'Studio is installed in {xb.PROJ_ROOT}, but project was set to {project}. Move Studio into 007xb instead of running it from another folder.')
        (project / 'data').mkdir(parents=True, exist_ok=True)
        cache = project / 'tools' / '.cache' / '007xb-asset-studio'
        cache.mkdir(parents=True, exist_ok=True)
        logfile = cache / 'build.log'
        succeeded = False
        with logfile.open('w', encoding='utf-8') as loghandle:
            self._log_handle = loghandle
            self.log(f'007xb Asset Studio {STUDIO_VERSION} | local build | {time.strftime("%Y-%m-%d %H:%M:%S")}')
            self.log(f'Project: {project}')
            try:
                self._build()
                succeeded = True
                self.log('\nBUILD SUCCEEDED — copy only data/007xb_assets.dat to D:\\data\\007xb_assets.dat.')
                self.emit('finished', {'success': True, 'output': str(project / 'data' / '007xb_assets.dat'), 'log': str(logfile)})
                return True
            except (xb.ToolError, StudioError, OSError, ValueError, subprocess.SubprocessError) as exc:
                self.log(f'\nBUILD STOPPED: {exc}')
                self.emit('finished', {'success': False, 'error': str(exc), 'log': str(logfile)})
                return False
            except Exception as exc:
                self.log(f'\nUNEXPECTED FAILURE: {exc}')
                self.log(traceback.format_exc())
                self.emit('finished', {'success': False, 'error': str(exc), 'log': str(logfile)})
                return False
            finally:
                self._log_handle = None

    def _build(self):
        project = self.project
        cache = project / 'tools' / '.cache' / '007xb-asset-studio'
        cache.mkdir(parents=True, exist_ok=True)

        self.stage(0)
        rom_path = self.s.rom.expanduser().resolve()
        source = (self.s.source if self.s.source is not None else BUNDLED_REFERENCE).expanduser().resolve()
        logo = (self.s.logo if self.s.logo is not None else RESOURCE_ROOT / 'tools' / 'branding' / 'darkone83-wireframe.obj').expanduser().resolve()
        info, rom = xb.verify_rom_file(rom_path)
        self.rom_bytes, self.rom_info = rom, info
        self.log(f"Verified {info['internal_title']} ({info['input_byte_order']}); SHA-1 {info['normalized_sha1']}")
        if not (source.is_file() or source.is_dir()):
            raise StudioError(f'Bundled converter reference is missing or inaccessible: {source}. Re-extract the tools folder.')
        if not logo.is_file():
            raise StudioError(f'Bundled Darkone83 logo is missing: {logo}. Re-extract the tools folder.')
        verts, lines = xb.parse_obj_lines(logo)
        self.log(f'Darkone83 logo: {len(verts):,} vertices, {len(lines):,} lines')
        self.source_hash = _source_fingerprint(source)
        self.done(0, 'Clean US ROM verified; bundled converters and branding present')

        self.stage(1)
        staged = _safe_restage(source, cache / '007xb-studio-source', self.source_hash, self.log)
        self.staged = staged
        local_rom = staged / xb.ROM_DATA_PATH
        local_rom.parent.mkdir(parents=True, exist_ok=True)
        if not local_rom.is_file() or xb.sha1_bytes(local_rom.read_bytes()) != xb.US_SHA1:
            xb.save_atomic(local_rom, rom)
        self.done(1, 'Normalized ROM cached privately under tools/.cache/')

        self.stage(2)
        state_path = cache / 'asset_studio_state.json'
        state = {}
        if state_path.is_file():
            try: state = json.loads(state_path.read_text('utf-8'))
            except (OSError, ValueError): pass
        key = f"{xb.US_SHA1}|{self.source_hash}|studio-{STUDIO_VERSION}"
        summaries = None
        if state.get('conversion_key') == key:
            try:
                summaries = xb.verify_upstream_sidecars(staged)
                self.log('Conversion cache valid; reusing three verified converter outputs')
            except xb.ToolError:
                self.log('Conversion cache invalid; rebuilding sidecars')
        if summaries is None:
            for filename, args in (
                ('d43_emit.py', ['ntsc-final']),
                ('d69_emit.py', ['ntsc-final']),
                ('d88_emit.py', ['ntsc-final', '--regen']),
            ):
                self._run_converter(filename, args)
            summaries = xb.verify_upstream_sidecars(staged)
        self.log(f"Models: {summaries['pcmodels']['entries']} | Stage resources: {summaries['pccg']['entries']} | Setup files: {summaries['pccg']['stage_setups']}")
        state['conversion_key'] = key
        xb.save_atomic(state_path, json.dumps(state, indent=2).encode('utf-8'))
        self.done(2, 'Converted sidecars verified in private cache; no loose output created')

        self.stage(3)
        logo_path = cache / 'darkone83.lines'
        content = bytearray(xb.LOGO_HEADER.pack(xb.LOGO_MAGIC, 1, len(verts), len(lines), 0))
        import struct
        for x, y, z in verts: content.extend(struct.pack('<fff', x, y, z))
        for a, b in lines: content.extend(struct.pack('<II', a, b))
        xb.save_atomic(logo_path, bytes(content))
        xb.validate_logo(logo_path)
        self.done(3, f'Darkone83 logo ready for archive ({len(lines):,} lines)')

        self.stage(4)
        archive = project / 'data' / '007xb_assets.dat'
        temporary_archive = cache / '007xb_assets.dat.building'
        paths = xb.get_pack_sources(staged, logo_path, rom_path=local_rom)
        # Validate all six entries before replacing any known-good game pack.
        # The temp file and final file share a filesystem when tools/ is in 007xb.
        records = xb.write_archive(temporary_archive, paths)
        checked = xb.inspect_archive(temporary_archive)
        if len(checked) != len(records):
            raise StudioError('Archive verification did not return the expected file count')
        names = {row['name'] for row in checked}
        expected = {name for name, _ in paths}
        if names != expected or len(names) != 6:
            raise StudioError(f'Archive entries incomplete: {sorted(expected - names)}')
        self.check_cancel()
        os.replace(temporary_archive, archive)
        manifest = {
            'studio_version': STUDIO_VERSION,
            'rom_sha1': xb.US_SHA1,
            'archive_sha256': xb.sha256_file(archive),
            'archive_bytes': archive.stat().st_size,
            'contents': records,
            'single_file_deploy': 'data/007xb_assets.dat',
            'note': 'PC sidecars are reference data; the active Xbox engine loads the ROM entry.',
        }
        xb.save_atomic(cache / 'archive.manifest.json', json.dumps(manifest, indent=2, sort_keys=True).encode('utf-8'))
        report = {
            'studio_version': STUDIO_VERSION,
            'rom_sha1': xb.US_SHA1,
            'sidecars': summaries,
            'archive': {'path': 'data/007xb_assets.dat', 'size': archive.stat().st_size,
                        'entries': len(records), 'verified': True},
            'deployment': 'Copy only 007xb_assets.dat to D:\\data\\007xb_assets.dat.',
            'rom_is_user_supplied': True,
        }
        xb.save_atomic(cache / 'asset_build_report.json', json.dumps(report, indent=2).encode('utf-8'))
        self.done(4, f'{len(records)}-entry archive verified; only data/007xb_assets.dat is deployable')
