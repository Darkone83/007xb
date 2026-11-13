"""Asset Studio v0.6: single-file archive integration tests (no ROM bundled)."""
import hashlib
import json
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

BASE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(BASE / 'tools'))
import xb_studio_pipeline as studio


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, str): path.write_text(textwrap.dedent(data), 'utf-8')
    else: path.write_bytes(data)


class PipelineTests(unittest.TestCase):
    def test_full_gui_pipeline_backend_and_incremental_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root / 'upstream-custom'
            rom = bytearray(b'\x80\x37\x12\x40' + b'\x00' * 92)
            rom[0x20:0x29] = b'GOLDENEYE'
            rom[0x3e] = ord('E')
            raw = b'XXXYYYZZZ'
            rom.extend(raw)
            rom = bytes(rom)
            rom_path = root / 'inputs' / 'GoldenEye (U).z64'
            write(rom_path, rom)
            sha = hashlib.sha1(rom).hexdigest()

            write(src / 'scripts/filelist.u.csv', '96,3,assets/first.bin,0,1\n99,3,assets/no.bin,0,0\n')
            write(src / 'assets/images.def', 'IMAGE(TEST,3,0,0,0,0)\n')
            write(src / 'imagelist.u.csv', '102,3,assets/images/split/image0.bin,0,1\n')
            write(src / 'assets/obseg/file_resource_table.inc.c', '{0,"PtestZ"},\n')
            write(src / 'assets/obseg/prop/Fake/ModelFileHeader.inc.c', 'MODELFILEHEADER(Fake)\n')
            write(src / 'tools_pc/d88_propdefs.py', 'def convert_stream(x): return x\n')
            write(src / 'tools_pc/d43_emit.py', '''
                from pathlib import Path
                p=Path('data/pcmodels-ntsc-final')
                p.mkdir(parents=True, exist_ok=True)
                (p/'pcmodels.bin').write_bytes(bytes.fromhex('11720102'))
                (p/'manifest.csv').write_text('name,offset,size\\nPtestZ,0,4\\n')
                Path('data/converter-times.txt').write_text('1')
            ''')
            write(src / 'tools_pc/d69_emit.py', '''
                from pathlib import Path
                p=Path('data/pccg-ntsc-final')
                p.mkdir(parents=True, exist_ok=True)
                (p/'pccg.bin').write_bytes(bytes(16)+bytes.fromhex('11720102'))
                (p/'manifest.csv').write_text('name,offset,size\\nbg/test.seg,0,16\\nTbg_testZ,16,4\\n')
            ''')
            write(src / 'tools_pc/d88_emit.py', '''
                from pathlib import Path
                p=Path('data/pccg-ntsc-final')
                b=bytearray((p/'pccg.bin').read_bytes())
                csv=(p/'manifest.csv').read_text()
                for i in range(34):
                    pos=(len(b)+15)//16*16
                    b += bytes(pos-len(b))+bytes.fromhex('11720102')
                    csv+=f'Usetup{i}Z,{pos},4\\n'
                (p/'pccg.bin').write_bytes(b)
                (p/'manifest.csv').write_text(csv)
            ''')
            logo = root / 'branding' / 'darkone83-wireframe.obj'
            write(logo, 'v 0 0 0\nv 1 0 0\nl 1 2\n')
            events = []
            old_root = studio.xb.PROJ_ROOT
            old_sha = studio.xb.US_SHA1
            try:
                studio.xb.PROJ_ROOT = root
                studio.xb.US_SHA1 = sha
                settings = studio.Settings(rom=rom_path, source=src, logo=logo, project=root)
                self.assertTrue(studio.Pipeline(settings, lambda *a: events.append(a)).run())
                report = json.loads((root/'tools/.cache/007xb-asset-studio/asset_build_report.json').read_text())
                self.assertEqual(report['sidecars']['pccg']['stage_setups'], 34)
                self.assertEqual(report['archive']['entries'], 6)
                self.assertFalse((root/'data/extracted').exists())
                self.assertEqual(sorted(p.name for p in (root/'data').iterdir()), ['007xb_assets.dat'])
                self.assertEqual(len(studio.xb.inspect_archive(root/'data/007xb_assets.dat')), 6)
                counter = root/'tools/.cache/007xb-asset-studio/007xb-studio-source/data/converter-times.txt'
                counter.write_text('dont rerun')
                events.clear()
                self.assertTrue(studio.Pipeline(settings, lambda *a: events.append(a)).run())
                self.assertEqual(counter.read_text(), 'dont rerun')
                self.assertTrue(any('Conversion cache valid' in e[1] for e in events if e[0] == 'log'))
                # A cached converted sidecar is not trusted when corrupted.
                sidecar = root/'tools/.cache/007xb-asset-studio/007xb-studio-source/data/pcmodels-ntsc-final/pcmodels.bin'
                sidecar.write_bytes(b'corrupt')
                events.clear()
                self.assertTrue(studio.Pipeline(settings, lambda *a: events.append(a)).run())
                self.assertTrue(any('Conversion cache invalid' in e[1] for e in events if e[0]=='log'))
                self.assertEqual(sidecar.read_bytes(), bytes.fromhex('11720102'))
                self.assertEqual(sorted(p.name for p in (root/'data').iterdir()), ['007xb_assets.dat'])
            finally:
                studio.xb.PROJ_ROOT = old_root
                studio.xb.US_SHA1 = old_sha

    def test_staging_does_not_delete_unknown_directories(self):
        with tempfile.TemporaryDirectory() as tmp:
            t=Path(tmp); src=t/'source'; src.mkdir()
            dst=t/'tools/.cache/007xb-asset-studio/007xb-studio-source'; dst.mkdir(parents=True)
            write(dst/'private-work.txt', 'Do not delete me')
            with self.assertRaisesRegex(studio.StudioError, 'unrecognized'):
                studio._safe_restage(src,dst,'123',lambda s:None)
            self.assertEqual((dst/'private-work.txt').read_text(), 'Do not delete me')

    def test_auto_detect(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            write(root/'GoldenEye 007 (U).z64', b'rom')
            found=studio.discover_inputs(root)
            self.assertEqual(found['rom'].name, 'GoldenEye 007 (U).z64')
            self.assertEqual(set(found), {'rom'})

    def test_minimal_converter_bundle_has_no_game_data(self):
        import zipfile
        ref = studio.BUNDLED_REFERENCE
        self.assertTrue(ref.is_file())
        with zipfile.ZipFile(ref) as z:
            names = z.namelist()
            self.assertTrue({'LICENSE','NOTICE','tools_pc/d43_emit.py',
                'tools_pc/d69_emit.py','tools_pc/d88_emit.py',
                'tools_pc/d88_propdefs.py','scripts/filelist.u.csv'}.issubset(names))
            self.assertFalse(any(n.lower().endswith(('.z64','.v64','.n64','.dat','.bin','.mp3','.wav','.png'))
                for n in names))
            self.assertGreater(len([n for n in names if n.lower().endswith('modelfileheader.inc.c')]),500)

    def test_settings_only_require_rom(self):
        s=studio.Settings(rom=Path('GoldenEye.z64'))
        self.assertIsNone(s.source)
        self.assertIsNone(s.logo)
        self.assertFalse(hasattr(s, 'extract_all'))

    def test_tool_layout_and_frozen_paths(self):
        from unittest.mock import patch
        tools = BASE / 'tools'
        self.assertTrue((tools/'007xb Asset Studio.pyw').is_file())
        self.assertTrue((tools/'Build Windows EXE.cmd').is_file())
        self.assertTrue((tools/'Start 007xb.cmd').is_file())
        self.assertEqual(studio.DEFAULT_PROJECT, BASE)
        with patch.object(studio.sys, 'frozen', True, create=True):
            with patch.object(studio.sys, 'executable', str(BASE/'tools/dist/007xb Asset Studio.exe')):
                self.assertEqual(studio._project_root(), BASE)
            with patch.object(studio.sys, 'executable', str(BASE/'tools/007xb Asset Studio.exe')):
                self.assertEqual(studio._project_root(), BASE)

    def test_archive_is_only_public_deployment_artifact(self):
        # Make sure paths intended for Xbox are only archive, not source workspace.
        self.assertEqual(len(studio.STAGES), 5)
        self.assertNotIn('extract', [k for k,_ in studio.STAGES])
        self.assertEqual(studio.BUNDLED_REFERENCE.parent, BASE/'tools')


if __name__ == '__main__':
    unittest.main()
