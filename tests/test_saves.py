from paths import ROOT
import os
import struct
import tempfile
import unittest
from pathlib import Path

import pgg_saves

PARAM = {'title': 'Fist of the North Star: Lost Paradise', 'subtitle': '08 Ch. 6: Reunion',
         'detail': 'Total Play Time: 3:30:57/Level: 16', 'user_param': 0}


def shadps4_slot(folder, slot='08', files=None, param=PARAM):
    """A slot folder as shadPS4 and Apollo write it."""
    folder = Path(folder)
    (folder / 'sce_sys').mkdir(parents=True)
    for name, data in (files or {'data.saveps4': b'save' * 100, 'printclupak0': b'\1' * 64}).items():
        (folder / name).write_bytes(data)
    (folder / 'sce_sys/param.sfo').write_bytes(pgg_saves.slot_sfo(param, slot, 'CUSA12776'))
    (folder / 'sce_sys/icon0.png').write_bytes(b'\x89PNG icon')
    os.utime(folder / 'sce_sys/param.sfo', (1700000000, 1700000000))
    return folder


class SfoTests(unittest.TestCase):
    def test_layout_as_shadps4(self):
        # Offsets and sizes of a param.sfo shadPS4 wrote for Hokuto (PortGaGotoku-test-saves-vp, slot 08).
        raw = pgg_saves.slot_sfo(PARAM, '08', 'CUSA12776')
        self.assertEqual(len(raw), 1596)
        magic, version, keys, data, count = struct.unpack_from('<4sIIII', raw)
        self.assertEqual((magic, version, keys, data, count), (b'\0PSF', 0x101, 148, 249, 8))
        offsets = [struct.unpack_from('<HHIII', raw, 20 + 16 * i)[4] for i in range(count)]
        self.assertEqual(offsets, [3, 11, 139, 267, 1291, 1323, 1327, 1339])

    def test_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'param.sfo'
            path.write_bytes(pgg_saves.slot_sfo(PARAM, '08', 'CUSA12776', blocks=320))
            sfo = pgg_saves.read_sfo(path)
        self.assertEqual(sfo['MAINTITLE'], PARAM['title'])
        self.assertEqual(sfo['SUBTITLE'], PARAM['subtitle'])
        self.assertEqual(sfo['DETAIL'], PARAM['detail'])
        self.assertEqual(sfo['SAVEDATA_DIRECTORY'], '08')
        self.assertEqual(sfo['SAVEDATA_LIST_PARAM'], 0)
        self.assertEqual(sfo['TITLE_ID'], 'CUSA12776')
        self.assertEqual(sfo['ACCOUNT_ID'], bytes(8))
        self.assertEqual(sfo['SAVEDATA_BLOCKS'], struct.pack('<Q', 320))

    def test_not_an_sfo(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'param.sfo'
            path.write_bytes(b'nope' * 10)
            with self.assertRaises(pgg_saves.SaveError):
                pgg_saves.read_sfo(path)


class ImportExportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.data = self.root / 'data'
        self.data.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def slot(self, slot='08'):
        return [Path(p) for p in pgg_saves.slot_dirs(str(self.data), '1', 'CUSA12776', slot)]

    def test_dry_run_writes_nothing(self):
        source = shadps4_slot(self.root / 'src/08')
        self.assertEqual(pgg_saves.main(['import', str(source), '--data', str(self.data), '--title', 'CUSA12776']), 0)
        self.assertEqual(list(self.data.iterdir()), [])

    def test_import(self):
        source = shadps4_slot(self.root / 'src/08')
        plan = pgg_saves.plan_import(str(source), str(self.data), title='CUSA12776')
        self.assertEqual((plan.slot, plan.backup, plan.warnings), ('08', '', []))
        self.assertEqual(pgg_saves.apply_import(plan), [])
        host, meta = self.slot()
        self.assertEqual(sorted(os.listdir(host)), ['data.saveps4', 'printclupak0'])
        self.assertEqual((host / 'data.saveps4').read_bytes(), (source / 'data.saveps4').read_bytes())
        self.assertEqual((meta / 'icon0.png').read_bytes(), b'\x89PNG icon')
        self.assertEqual(os.path.getsize(meta / 'param.bin'), 1328)
        param = pgg_saves.read_param(meta / 'param.bin')
        self.assertEqual({k: param[k] for k in PARAM}, PARAM)
        # Dated like the source's param.sfo (the runtime reads param.bin's mtime).
        self.assertEqual(param['mtime'], 1700000000)
        self.assertEqual(int(os.path.getmtime(meta / 'param.bin')), 1700000000)

    def test_import_backs_up_the_existing_slot(self):
        host, meta = self.slot()
        host.mkdir(parents=True)
        meta.mkdir()
        (host / 'data.saveps4').write_bytes(b'old')
        (host / 'extra').write_bytes(b'kept')
        plan = pgg_saves.plan_import(str(shadps4_slot(self.root / 'src/08')), str(self.data), title='CUSA12776')
        self.assertTrue(plan.backup)
        self.assertEqual(pgg_saves.apply_import(plan), ['extra'])
        self.assertEqual((Path(plan.backup) / '08/data.saveps4').read_bytes(), b'old')
        self.assertEqual((host / 'extra').read_bytes(), b'kept')

    def test_import_needs_a_slot_name(self):
        source = self.root / 'src/plain'
        source.mkdir(parents=True)
        (source / 'data.saveps4').write_bytes(b'x')
        with self.assertRaises(pgg_saves.SaveError):
            pgg_saves.plan_import(str(source), str(self.data), title='CUSA12776')
        plan = pgg_saves.plan_import(str(source), str(self.data), slot='05', title='CUSA12776')
        self.assertEqual(plan.slot, '05')
        self.assertTrue(plan.warnings)

    def test_import_then_export_gives_the_same_slot(self):
        source = shadps4_slot(self.root / 'src/08')
        pgg_saves.apply_import(pgg_saves.plan_import(str(source), str(self.data), title='CUSA12776'))
        for shadps4, target in ((False, self.root / 'ps4/08'), (True, self.root / 'shad/1/CUSA12776/08')):
            dest = self.root / ('shad' if shadps4 else 'ps4')
            plan = pgg_saves.plan_export(str(self.data), '08', str(dest), shadps4, title='CUSA12776')
            self.assertEqual(Path(plan.target), target)
            pgg_saves.apply_export(plan)
            for name in ('data.saveps4', 'printclupak0', 'sce_sys/param.sfo', 'sce_sys/icon0.png'):
                self.assertEqual((target / name).read_bytes(), (source / name).read_bytes(), name)

    def test_export_refuses_an_existing_target(self):
        pgg_saves.apply_import(pgg_saves.plan_import(str(shadps4_slot(self.root / 'src/08')), str(self.data),
                                                     title='CUSA12776'))
        (self.root / 'out/08').mkdir(parents=True)
        with self.assertRaises(pgg_saves.SaveError):
            pgg_saves.plan_export(str(self.data), '08', str(self.root / 'out'), title='CUSA12776')

    def test_export_of_a_missing_slot(self):
        with self.assertRaises(pgg_saves.SaveError):
            pgg_saves.plan_export(str(self.data), '03', str(self.root / 'out'), title='CUSA12776')


class BackupTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.data = self.root / 'data'
        pgg_saves.apply_import(pgg_saves.plan_import(str(shadps4_slot(self.root / 'src/08')), str(self.data),
                                                     title='CUSA12776'))
        self.host, self.meta = [Path(p) for p in pgg_saves.slot_dirs(str(self.data), '1', 'CUSA12776', '08')]

    def tearDown(self):
        self.tmp.cleanup()

    def runtime_backup(self, name, content):
        """A backup as the runtime makes it before the game writes a slot."""
        folder = self.data / 'save-backups' / name
        (folder / '08').mkdir(parents=True)
        (folder / '08/data.saveps4').write_bytes(content)
        (folder / '08.sce_sys').mkdir()
        (folder / '08.sce_sys/param.bin').write_bytes(pgg_saves.pack_param('T', 'old subtitle'))
        return folder

    def test_lists(self):
        slots = pgg_saves.list_slots(str(self.data), 'CUSA12776')
        self.assertEqual([(s['slot'], s['subtitle'], s['files']) for s in slots], [('08', PARAM['subtitle'], 2)])
        self.runtime_backup('08-20261020-101010', b'a')
        self.runtime_backup('08-import-20261021-101010', b'b')
        (self.data / 'save-backups/not-a-backup').mkdir()
        backups = pgg_saves.list_backups(str(self.data))
        self.assertEqual([(b['name'], b['kind'], b['slot']) for b in backups],
                         [('08-import-20261021-101010', 'import', '08'), ('08-20261020-101010', 'game', '08')])
        self.assertEqual(backups[1]['subtitle'], 'old subtitle')

    def test_restore_keeps_the_current_slot_first(self):
        self.runtime_backup('08-20261020-101010', b'old')
        before = {n: (self.host / n).read_bytes() for n in os.listdir(self.host)}
        plan = pgg_saves.plan_restore(str(self.data), '08-20261020-101010', 'CUSA12776')
        self.assertIn('08-restore-', plan.safety)
        self.assertEqual((self.host / 'data.saveps4').read_bytes(), before['data.saveps4'])  # the plan writes nothing
        pgg_saves.apply_restore(plan)
        self.assertEqual(os.listdir(self.host), ['data.saveps4'])
        self.assertEqual((self.host / 'data.saveps4').read_bytes(), b'old')
        self.assertEqual(pgg_saves.read_param(self.meta / 'param.bin')['subtitle'], 'old subtitle')
        kept = Path(plan.safety)
        self.assertEqual({n: (kept / '08' / n).read_bytes() for n in os.listdir(kept / '08')}, before)
        self.assertTrue((kept / '08.sce_sys/param.bin').is_file())
        # The backup restored stays where it was.
        self.assertTrue((self.data / 'save-backups/08-20261020-101010/08/data.saveps4').is_file())
        self.assertIn('restore', [b['kind'] for b in pgg_saves.list_backups(str(self.data))])

    def test_failed_restore_puts_the_slot_back(self):
        self.runtime_backup('08-20261020-101010', b'old')
        plan = pgg_saves.plan_restore(str(self.data), '08-20261020-101010', 'CUSA12776')
        original = pgg_saves.sha
        pgg_saves.sha = lambda path: 'different' if 'save-backups' in str(path) else original(path)
        try:
            with self.assertRaises(pgg_saves.SaveError):
                pgg_saves.apply_restore(plan)
        finally:
            pgg_saves.sha = original
        self.assertEqual(sorted(os.listdir(self.host)), ['data.saveps4', 'printclupak0'])
        self.assertEqual(pgg_saves.read_param(self.meta / 'param.bin')['subtitle'], PARAM['subtitle'])

    def test_restore_refuses_bad_names(self):
        for name in ('', '../x', '08-missing', 'x/y'):
            with self.assertRaises(pgg_saves.SaveError):
                pgg_saves.plan_restore(str(self.data), name, 'CUSA12776')


if __name__ == '__main__':
    unittest.main()
