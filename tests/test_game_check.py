from paths import ROOT
import os
import struct
import tempfile
import unittest
from pathlib import Path

import game_check
from test_prepare import fixture


def param_sfo(entries):
    """A minimal param.sfo with UTF-8 string entries."""
    keys = b''.join(k.encode() + b'\0' for k in entries)
    values = [v.encode() + b'\0' for v in entries.values()]
    count = len(entries)
    key_table = 20 + 16 * count
    data_table = key_table + len(keys)
    data = bytearray(struct.pack('<4sIIII', b'\0PSF', 0x101, key_table, data_table, count))
    key_off = data_off = 0
    for key, value in zip(entries, values):
        data += struct.pack('<HHIII', key_off, 0x204, len(value), len(value), data_off)
        key_off += len(key) + 1
        data_off += len(value)
    return bytes(data + keys + b''.join(values))


class GameCheckTests(unittest.TestCase):
    def game(self, title, version, eboot=None):
        path = Path(self.tmp.name)
        (path / 'sce_sys').mkdir(exist_ok=True)
        (path / 'sce_sys/param.sfo').write_bytes(param_sfo({'APP_VER': version, 'TITLE_ID': title}))
        (path / 'eboot.bin').write_bytes(fixture() if eboot is None else eboot)
        return path

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        os.environ.pop('BB_SKIP_GAME_CHECK', None)
        os.environ.pop('PGG_SKIP_GAME_CHECK', None)

    def tearDown(self):
        os.environ.pop('BB_SKIP_GAME_CHECK', None)
        os.environ.pop('PGG_SKIP_GAME_CHECK', None)
        self.tmp.cleanup()

    def test_hokuto_passes(self):
        self.assertIsNone(game_check.problem(self.game('CUSA12781', '01.00')))
        self.assertIsNone(game_check.warning(self.game('CUSA12781', '01.00')))

    def test_other_version_only_warns(self):
        game = self.game('CUSA12781', '01.02')
        self.assertIsNone(game_check.problem(game))
        self.assertIn('01.02', game_check.warning(game))

    def test_other_title(self):
        self.assertEqual(game_check.problem(self.game('CUSA03173', '01.09')),
                         ('other_title', 'CUSA03173', '01.09'))

    def test_no_param_sfo(self):
        path = Path(self.tmp.name)
        (path / 'eboot.bin').write_bytes(b'x')
        self.assertEqual(game_check.problem(path)[0], 'unreadable')

    def test_skip_switch(self):
        os.environ['PGG_SKIP_GAME_CHECK'] = '1'
        self.assertIsNone(game_check.problem(self.game('CUSA03173', '01.00')))

    def test_skip_switch_old_name(self):
        os.environ['BB_SKIP_GAME_CHECK'] = '1'
        self.assertIsNone(game_check.problem(self.game('CUSA03173', '01.00')))

    def test_every_problem_is_explained(self):
        for kind in ('other_title', 'unreadable'):
            self.assertIn('CUSA12781', game_check.explain(kind, 'CUSA03173', '01.00'))


if __name__ == '__main__':
    unittest.main()
