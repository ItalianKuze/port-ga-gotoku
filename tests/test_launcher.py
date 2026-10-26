"""PortGaGotoku launcher: translations (pgg_lang.py against pgg_main.py and launcher/ui) and the\nlogic without a window (launcher/pgg_game.py)."""
import ast
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'launcher'))


def write_sfo(path, fields):
    """A minimal param.sfo with string fields."""
    keys, values, entries = b'', b'', b''
    for key, value in fields.items():
        data = value.encode() + b'\0'
        entries += struct.pack('<HHIII', len(keys), 0x0204, len(data), len(data), len(values))
        keys += key.encode() + b'\0'
        values += data
    start = 20 + len(entries)
    path.write_bytes(struct.pack('<4sIIII', b'\0PSF', 0x101, start, start + len(keys), len(fields)) +
                     entries + keys + values)


def launcher_texts():
    """Every English text the launcher shows: _('...') in pgg_main.py, t('...') in ui/app.js,
    data-t / data-t-label in ui/index.html."""
    import re
    tree = ast.parse((ROOT / 'launcher' / 'pgg_main.py').read_text(encoding='utf-8'))
    texts = {node.args[0].value for node in ast.walk(tree)
             if isinstance(node, ast.Call) and getattr(node.func, 'id', None) == '_' and node.args
             and isinstance(node.args[0], ast.Constant)}
    script = (ROOT / 'launcher' / 'ui' / 'app.js').read_text(encoding='utf-8')
    for literal in re.findall(r"\bt\('((?:[^'\\]|\\.)*)'\)", script):
        texts.add(re.sub(r'\\(.)', lambda m: {'n': '\n'}.get(m.group(1), m.group(1)), literal))
    page = (ROOT / 'launcher' / 'ui' / 'index.html').read_text(encoding='utf-8')
    texts.update(re.findall(r'data-t(?:-label)?="([^"]*)"', page))
    return texts


class Translations(unittest.TestCase):
    def test_every_text_has_italian(self):
        import pgg_lang
        texts = launcher_texts()
        self.assertGreater(len(texts), 60)
        self.assertEqual(sorted(t for t in texts if t not in pgg_lang.IT), [])
        self.assertEqual(sorted(t for t in pgg_lang.IT if t not in texts), [])

    def test_placeholders_match(self):
        import pgg_lang
        for english, italian in pgg_lang.IT.items():
            self.assertEqual(english.count('{}'), italian.count('{}'), english)


class GameFolder(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        os.environ['BB_DATA_DIR'] = str(self.root / 'data')
        import importlib
        import pgg_game
        self.game = importlib.reload(pgg_game)

    def tearDown(self):
        os.environ.pop('BB_DATA_DIR', None)
        self.dir.cleanup()

    def folder(self, title='CUSA12781', version='01.00', eboot=True, sfo=True):
        game = self.root / 'game'
        (game / 'sce_sys').mkdir(parents=True, exist_ok=True)
        if eboot:
            (game / 'eboot.bin').write_bytes(b'\0' * 16)
        if sfo:
            write_sfo(game / 'sce_sys' / 'param.sfo', {'TITLE_ID': title, 'APP_VER': version, 'TITLE': 'Test'})
        return str(game)

    def test_states(self):
        check = self.game.check_game
        self.assertEqual(check('')[0], 'empty')
        self.assertEqual(check(str(self.root / 'nowhere'))[0], 'missing')
        self.assertEqual(check(self.folder())[0], 'ok')
        self.assertEqual(check(self.folder(version='01.02'))[0], 'untested_version')
        self.assertEqual(check(self.folder(title='CUSA12776'))[0], 'other_title')
        # Another edition only after the user chose to try it, and only that TITLE_ID.
        self.assertEqual(check(self.folder(title='CUSA10053'), 'CUSA10053')[0], 'experimental')
        self.assertEqual(check(self.folder(title='CUSA10053'), 'CUSA12776')[0], 'other_title')
        self.assertEqual(check(self.folder(), 'CUSA10053')[0], 'ok')
        self.assertTrue(self.game.playable('experimental'))
        self.assertFalse(self.game.playable('other_title'))

    def test_no_eboot_and_no_sfo(self):
        self.assertEqual(self.game.check_game(self.folder(eboot=False))[0], 'no_eboot')
        game = self.root / 'other'
        game.mkdir()
        (game / 'eboot.bin').write_bytes(b'')
        self.assertEqual(self.game.check_game(str(game))[0], 'no_sfo')

    def test_settings_round_trip_and_defaults(self):
        settings = self.game.load_settings()
        self.assertEqual(settings, self.game.DEFAULTS)
        settings.update(present_mode='Fifo', fullscreen=True)
        self.game.save_settings(settings)
        self.assertEqual(self.game.load_settings()['present_mode'], 'Fifo')
        stored = json.loads(self.game.SETTINGS_FILE.read_text(encoding='utf-8'))
        stored.update(present_mode='Bogus', unknown=1)
        self.game.SETTINGS_FILE.write_text(json.dumps(stored), encoding='utf-8')
        loaded = self.game.load_settings()
        self.assertEqual(loaded['present_mode'], 'Mailbox')
        self.assertNotIn('unknown', loaded)

    def test_environment(self):
        os.environ['BB_FILE_TRACE'] = '1'
        try:
            settings = {**self.game.DEFAULTS, 'game_dir': self.folder(), 'gpu': 'RTX 5070', 'fullscreen': True}
            env = self.game.game_environment(settings)
        finally:
            os.environ.pop('BB_FILE_TRACE')
        data = Path(os.environ['BB_DATA_DIR'])
        self.assertNotIn('BB_FILE_TRACE', env)
        self.assertEqual(env['BB_GPU_USER_DIR'], str(data / 'gpu'))
        self.assertEqual(env['BB_GPU_CACHE_DIR'], str(data / 'cache'))
        self.assertEqual(env['BB_CONFIG'], str(data / 'bbport.ini'))
        self.assertEqual((env['BB_PRESENT_MODE'], env['BB_FULLSCREEN'], env['BB_GPU_DEVICE']),
                         ('Mailbox', '1', 'RTX 5070'))
        self.assertIn(env['BB_MENU_LANGUAGE'], ('it', 'en'))
        self.assertEqual(self.game.game_environment({**settings, 'ui_language': 'en'})['BB_MENU_LANGUAGE'], 'en')
        self.assertNotIn('APPDATA', ''.join(v for k, v in env.items() if k.startswith('BB_')))
        command = self.game.game_command(settings)
        self.assertEqual(command[command.index('--timeout') + 1], '0')
        self.assertEqual(command[command.index('--app0') + 1], settings['game_dir'])

    def test_log_header_names_the_version(self):
        import hashlib
        game = self.folder(title='CUSA10053', version='01.02')
        header = self.game.log_header({**self.game.DEFAULTS, 'game_dir': game})
        self.assertIn(f'== game CUSA10053 version 01.02, eboot.bin sha256 {hashlib.sha256(bytes(16)).hexdigest()} ==',
                      header)
        self.assertIn('== game ? version ?, eboot.bin sha256 ? ==',
                      self.game.log_header({**self.game.DEFAULTS, 'game_dir': str(self.root / 'nowhere')}))

    def prepared_folder(self, image_format=2):
        """A game, a copy of the preparation scripts and an image in DATA_DIR/out; bb-probe answers
        `image_format`."""
        game = self.folder()
        scripts = self.root / 'scripts'
        scripts.mkdir()
        for name in self.game.PREPARATION_SCRIPTS:
            (scripts / name).write_bytes((ROOT / 'scripts' / name).read_bytes())
        self.game.SCRIPTS_DIR = scripts
        self.game._image_format.update(value=image_format, problem='')
        out = Path(os.environ['BB_DATA_DIR']) / 'out'
        out.mkdir(parents=True)
        (out / 'boot-linked.bin').write_bytes(b'x')
        (out / 'content.bin').write_bytes(b'x')
        return game, scripts, out

    def test_prepared_needs_a_matching_stamp(self):
        game, _scripts, out = self.prepared_folder()
        self.assertFalse(self.game.prepared(game))
        (out / 'prepared.json').write_text(json.dumps(self.game.preparation_stamp(game)), encoding='utf-8')
        self.assertTrue(self.game.prepared(game))
        (Path(game) / 'eboot.bin').write_bytes(b'\0' * 32)
        self.assertFalse(self.game.prepared(game))

    def test_a_script_changed_at_the_same_size_prepares_again(self):
        game, scripts, out = self.prepared_folder()
        (out / 'prepared.json').write_text(json.dumps(self.game.preparation_stamp(game)), encoding='utf-8')
        source = (scripts / 'link_modules.py').read_bytes()
        (scripts / 'link_modules.py').write_bytes(source[:-2] + b'#\n')
        self.assertEqual(len((scripts / 'link_modules.py').read_bytes()), len(source))
        self.assertFalse(self.game.prepared(game))

    def test_another_image_format_prepares_again(self):
        game, _scripts, out = self.prepared_folder()
        (out / 'prepared.json').write_text(json.dumps(self.game.preparation_stamp(game)), encoding='utf-8')
        self.game._image_format.update(value=3)
        self.assertFalse(self.game.prepared(game))

    def test_size_stamp_is_a_format_2_image(self):
        game, scripts, out = self.prepared_folder()
        (out / 'prepared.json').write_text(json.dumps(self.game.size_stamp(game)), encoding='utf-8')
        self.assertTrue(self.game.prepared(game))
        self.assertEqual(json.loads((out / 'prepared.json').read_text(encoding='utf-8')),
                         self.game.preparation_stamp(game))
        # Rewritten: a later change at the same size is seen.
        source = (scripts / 'prepare.py').read_bytes()
        (scripts / 'prepare.py').write_bytes(source[:-2] + b'#\n')
        self.assertFalse(self.game.prepared(game))

    def test_size_stamp_of_another_size_or_format_prepares_again(self):
        game, scripts, out = self.prepared_folder(image_format=3)
        (out / 'prepared.json').write_text(json.dumps(self.game.size_stamp(game)), encoding='utf-8')
        self.assertFalse(self.game.prepared(game))
        self.game._image_format.update(value=2)
        stamp = self.game.size_stamp(game)
        stamp['scripts']['link_modules.py'] += 8  # 0.1.2's link_modules.py
        (out / 'prepared.json').write_text(json.dumps(stamp), encoding='utf-8')
        self.assertFalse(self.game.prepared(game))


    def test_image_format_failures_are_asked_once_and_explained(self):
        import subprocess
        self.game.RUN_DIR = self.root / 'no-run-dir'
        self.assertIsNone(self.game.image_format())
        self.assertIn('could not be started', self.game.image_format_problem())
        calls = []

        def hanging(command, **kwargs):
            calls.append(kwargs['timeout'])
            raise subprocess.TimeoutExpired(command, kwargs['timeout'])
        run, self.game.subprocess.run = self.game.subprocess.run, hanging
        try:
            self.game._image_format.clear()
            self.assertIsNone(self.game.image_format())
            self.assertIsNone(self.game.image_format())
            self.assertIn(f'did not answer within {self.game.IMAGE_FORMAT_TIMEOUT} s', self.game.image_format_problem())
        finally:
            self.game.subprocess.run = run
        self.assertEqual(calls, [self.game.IMAGE_FORMAT_TIMEOUT])


class Helpers(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        os.environ['BB_DATA_DIR'] = str(self.root / 'data')
        import importlib
        import pgg_game
        self.game = importlib.reload(pgg_game)

    def tearDown(self):
        os.environ.pop('BB_DATA_DIR', None)
        self.dir.cleanup()

    def test_parent_folder_is_accepted(self):
        (self.root / 'dump' / 'CUSA12781').mkdir(parents=True)
        (self.root / 'dump' / 'CUSA12781' / 'eboot.bin').write_bytes(b'')
        self.assertEqual(self.game.find_game_folder(str(self.root / 'dump')), str(self.root / 'dump' / 'CUSA12781'))
        self.assertEqual(self.game.find_game_folder(str(self.root)), str(self.root))

    def game_folder(self, fields):
        game = self.root / 'game'
        (game / 'sce_sys').mkdir(parents=True, exist_ok=True)
        write_sfo(game / 'sce_sys' / 'param.sfo', fields)
        return str(game)

    def test_save_slots_skip_metadata(self):
        saves = self.root / 'data' / 'user' / 'savedata' / '1' / 'CUSA12776'
        for name in ('01', '01.sce_sys', '02', '02.sce_sys'):
            (saves / name).mkdir(parents=True)
        game = self.game_folder({'TITLE_ID': 'CUSA12781', 'INSTALL_DIR_SAVEDATA': 'CUSA12776'})
        self.assertEqual(sorted(self.game.save_slots(game)), ['01', '02'])

    def test_save_title_id_from_param_sfo(self):
        title = self.game.save_title_id
        self.assertEqual(title(str(self.root / 'nowhere')), 'CUSA12776')
        self.assertEqual(title(self.game_folder({'TITLE_ID': 'CUSA10053', 'INSTALL_DIR_SAVEDATA': 'CUSA10050'})),
                         'CUSA10050')
        self.assertEqual(title(self.game_folder({'TITLE_ID': 'CUSA10053'})), 'CUSA10053')

    def test_banner_only_png(self):
        game = self.root / 'game' / 'sce_sys'
        game.mkdir(parents=True)
        self.assertEqual(self.game.banner_data_url(str(self.root / 'game')), '')
        (game / 'pic1.png').write_bytes(b'\x89PNG\r\n\x1a\nrest')
        self.assertTrue(self.game.banner_data_url(str(self.root / 'game')).startswith('data:image/png;base64,'))


if __name__ == '__main__':
    unittest.main()
