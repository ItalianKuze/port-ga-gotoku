#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""PortGaGotoku launcher for Windows: Fist of the North Star: Lost Paradise (CUSA12781).

The window is HTML/CSS (launcher/ui) shown by pywebview in Windows' own WebView2; this file is
the bridge to pgg_game.py, which does the work (game folder check, boot image, environment, the
game in a job object). The roles of the frozen executable, the job object and the translation
scheme come from Supermedo's Windows launcher (github.com/Supermedo/bloodborne_pc, v1.6,
GPL-2.0-or-later).

Roles of PortGaGotoku.exe (frozen with PyInstaller, packaging/windows/package.sh):
  (none)              the launcher window;
  --play              the game with the saved settings, without the window (Start Hokuto.exe);
  --script FILE ARGS  a preparation script (scripts/prepare.py, ...), so no Python is needed.
"""
import ctypes
import os
from pathlib import Path
import runpy
import sys
import threading

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pgg_game as game  # noqa: E402
import pgg_lang  # noqa: E402

_ = pgg_lang.tr
UI_DIR = Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parent)) / 'ui'
LOG_KEEP = 400_000  # characters of the running log the window keeps


# ---------------------------------------------------------------------------------------------
# Command line roles of the frozen executable (as in Supermedo's launcher).

def attach_stdio():
    """A windowed executable starts without sys.stdout; inherited pipes or a console still exist."""
    import msvcrt
    for name, std in (('stdout', -11), ('stderr', -12)):
        if getattr(sys, name) is not None:
            continue
        stream = None
        handle = ctypes.windll.kernel32.GetStdHandle(std)
        if handle and handle != ctypes.c_void_p(-1).value:
            try:
                stream = open(msvcrt.open_osfhandle(handle, os.O_WRONLY), 'w', encoding='utf-8',
                              errors='replace', buffering=1)
            except OSError:
                stream = None
        setattr(sys, name, stream or open(os.devnull, 'w'))


def run_script(argv):
    """--script FILE ARGS: runs a preparation script; returns its exit code."""
    attach_stdio()
    path, sys.argv = argv[1], argv[1:]
    sys.path.insert(0, str(Path(path).resolve().parent))
    try:
        runpy.run_path(path, run_name='__main__')
    except SystemExit as stop:
        return stop.code if isinstance(stop.code, int) else (0 if stop.code is None else 1)
    return 0


def message_box(text, error=False):
    ctypes.windll.user32.MessageBoxW(None, text, 'PortGaGotoku', 0x10 if error else 0x40)


def tail(path, lines=20):
    try:
        return ''.join(Path(path).read_text(encoding='utf-8', errors='replace').splitlines(True)[-lines:])
    except OSError:
        return ''


def play_without_window(settings):
    """--play: prepares the image if needed, runs the game and waits for it. A failure is shown
    in a message box with the end of the log."""
    if not game.prepared(settings['game_dir']):
        log_path = game.new_log_path('-prepare')
        with open(log_path, 'w', encoding='utf-8') as log:
            ok = game.prepare(settings['game_dir'], log.write, game.game_state(settings)[0] == 'experimental')
        if not ok:
            message_box(_('Preparing the game failed. Log: {}').format(log_path) + '\n\n' + tail(log_path), True)
            return 1
    log_path = game.new_log_path()
    process, job = game.start_game(settings, log_path)
    code = process.wait()
    job.close()
    if code != 0:
        message_box(_('The game ended with code {}. Log: {}').format(code, log_path) + '\n\n' + tail(log_path), True)
    return code


# ---------------------------------------------------------------------------------------------
# The bridge the page calls (window.pywebview.api.*)

class Api:
    def __init__(self):
        self._window = None
        self._settings = game.load_settings()
        self._lock = threading.Lock()
        self._devices = None
        self._phase = 'idle'          # idle, preparing, running, stopping
        self._process = self._job = None
        self._log_path = None
        self._log = ''                # the current run's log (preparation, then the game)
        self._log_start = 0           # characters dropped from the front of self._log
        self._last_exit = None
        self._stopped = False
        threading.Thread(target=self._detect_devices, daemon=True).start()

    # ---- what the page shows -------------------------------------------------------------------
    def texts(self):
        """The language in use and its table (English texts are the keys)."""
        return {'lang': pgg_lang.LANG, 'table': pgg_lang.TABLES.get(pgg_lang.LANG, {})}

    def state(self):
        settings = self._settings
        check, info = game.game_state(settings)
        with self._lock:
            devices = self._devices
            phase, last_exit, stopped = self._phase, self._last_exit, self._stopped
        gpus = [{'name': name, 'kind': kind} for _i, kind, name in devices[0]] if devices else []
        return {
            'settings': settings,
            'game': {'state': check, 'playable': game.playable(check), 'title_id': info.get('TITLE_ID', ''),
                     'version': info.get('APP_VER', ''), 'title': info.get('TITLE', ''),
                     'tested_version': game.TESTED_VERSION, 'expected': game.TITLE_ID,
                     'prepared': game.playable(check) and game.prepared(settings['game_dir'])},
            'devices': None if devices is None else {'gpus': gpus, 'gamepads': devices[1]},
            'saves': game.save_slots(settings['game_dir']),
            'phase': phase, 'last_exit': last_exit, 'stopped': stopped,
            'log_name': self._log_path.name if self._log_path else '',
            'build': game.build_id(),
            'data_dir': str(game.DATA_DIR),
            'present_modes': list(game.PRESENT_MODES),
        }

    def banner(self):
        return game.banner_data_url(self._settings['game_dir']) if self._settings['game_dir'] else ''

    def log_since(self, position):
        """The log text after `position` (a count of characters since the run started)."""
        with self._lock:
            start = max(position, self._log_start)
            return {'text': self._log[start - self._log_start:], 'position': self._log_start + len(self._log),
                    'reset': position < self._log_start}

    # ---- settings ------------------------------------------------------------------------------
    def set_setting(self, key, value):
        if key not in game.DEFAULTS or key in ('game_dir', 'experimental_title'):
            return False
        if isinstance(game.DEFAULTS[key], bool):
            value = bool(value)
        elif key == 'present_mode' and value not in game.PRESENT_MODES:
            return False
        elif key == 'ui_language':
            if value not in ('', 'it', 'en'):
                return False
            pgg_lang.LANG = value or pgg_lang.windows_language()
        self._settings[key] = value
        game.save_settings(self._settings)
        return True

    def browse_game(self):
        """The folder dialog; the chosen folder is kept only when it holds the game."""
        import webview
        current = self._settings['game_dir'] or str(Path.home())
        chosen = self._window.create_file_dialog(webview.FileDialog.FOLDER, directory=current)
        if not chosen:
            return {'chosen': ''}
        folder = game.find_game_folder(chosen[0] if isinstance(chosen, (list, tuple)) else chosen)
        check, info = game.check_game(folder, self._settings['experimental_title'])
        if game.playable(check):
            self._settings['game_dir'] = folder
            game.save_settings(self._settings)
        return {'chosen': folder, 'state': check, 'title_id': info.get('TITLE_ID', ''), 'title': info.get('TITLE', ''),
                'version': info.get('APP_VER', '')}

    def try_experimental(self, folder):
        """Another edition of the game (another TITLE_ID), after the page's explicit confirmation:
        kept as the game folder and started without prepare.py's game check."""
        check, info = game.check_game(folder)
        if check != 'other_title':
            return False
        self._settings['game_dir'] = folder
        self._settings['experimental_title'] = info['TITLE_ID']
        game.save_settings(self._settings)
        return True

    def open_folder(self, name):
        paths = {**game.folders(), 'game': Path(self._settings['game_dir'] or game.DATA_DIR)}
        path = paths.get(name)
        if path is None:
            return False
        path.mkdir(parents=True, exist_ok=True) if name != 'game' else None
        os.startfile(str(path))
        return True

    # ---- the game ------------------------------------------------------------------------------
    def play(self):
        with self._lock:
            if self._phase != 'idle':
                return {'ok': False, 'reason': 'busy'}
        settings = dict(self._settings)
        if not game.playable(game.game_state(settings)[0]):
            return {'ok': False, 'reason': 'game'}
        return {'ok': True, 'running': game.running_probes()}

    def start(self):
        """Called by the page after play() (and its confirmation, when another bb-probe runs)."""
        with self._lock:
            if self._phase != 'idle':
                return False
            self._phase, self._stopped, self._last_exit = 'preparing', False, None
            self._log, self._log_start, self._log_path = '', 0, None
        threading.Thread(target=self._run, args=(dict(self._settings),), daemon=True).start()
        return True

    def stop(self):
        with self._lock:
            job = self._job
            if not job or self._phase != 'running':
                return False
            self._phase, self._stopped = 'stopping', True
        threading.Thread(target=job.stop, daemon=True).start()
        return True

    # ---- helpers -------------------------------------------------------------------------------
    def _append(self, text):
        with self._lock:
            self._log += text
            if len(self._log) > LOG_KEEP:
                drop = len(self._log) - LOG_KEEP
                self._log = self._log[drop:]
                self._log_start += drop

    def _detect_devices(self):
        found = game.devices() or ([], [])
        with self._lock:
            self._devices = found

    def _run(self, settings):
        if not game.prepared(settings['game_dir']):
            log_path = game.new_log_path('-prepare')
            with self._lock:
                self._log_path = log_path
            with open(log_path, 'w', encoding='utf-8') as log:
                def output(text):
                    log.write(text)
                    self._append(text)
                ok = game.prepare(settings['game_dir'], output, game.game_state(settings)[0] == 'experimental')
            if not ok:
                self._finish(-1)
                return
        log_path = game.new_log_path()
        try:
            process, job = game.start_game(settings, log_path)
        except OSError as error:
            self._append(f'\n{error}\n')
            self._finish(-1)
            return
        with self._lock:
            self._process, self._job, self._log_path = process, job, log_path
            if self._phase == 'preparing':
                self._phase = 'running'
        position = 0
        while True:
            code = process.poll()
            try:
                with open(log_path, 'rb') as log:
                    log.seek(position)
                    chunk = log.read()
                    position += len(chunk)
            except OSError:
                chunk = b''
            if chunk:
                self._append(chunk.decode('utf-8', errors='replace'))
            if code is not None:
                break
            threading.Event().wait(0.25)
        job.close()
        self._finish(code)

    def _finish(self, code):
        with self._lock:
            self._phase, self._process, self._job, self._last_exit = 'idle', None, None, code

    def closing(self):
        """The window's X: ask first while the game runs (it keeps running)."""
        with self._lock:
            busy = self._phase != 'idle'
        if not busy:
            return True
        return self._window.create_confirmation_dialog(
            'PortGaGotoku', _('The game is still running. Close the launcher anyway?\n\nThe game stays open; its '
                              'log is still written to the logs folder.'))


def main():
    args = sys.argv[1:]
    if args and args[0] == '--script':
        sys.exit(run_script(args))
    settings = game.load_settings()
    pgg_lang.LANG = settings['ui_language'] or pgg_lang.windows_language()
    if '--play' in args and game.playable(game.game_state(settings)[0]):
        sys.exit(play_without_window(settings))
    # Without a usable game folder there is nothing to play yet: the launcher opens instead.
    try:
        import webview
    except ImportError as error:
        message_box(f'pywebview: {error}', True)
        return 1
    api = Api()
    window = webview.create_window('PortGaGotoku', str(UI_DIR / 'index.html'), js_api=api, width=1100, height=700,
                                   min_size=(900, 600), background_color='#100e0d')
    api._window = window
    window.events.closing += api.closing
    try:
        webview.start(private_mode=False, storage_path=str(game.DATA_DIR / 'launcher-webview'))
    except Exception as error:  # WebView2 missing or broken
        message_box(_('The launcher window needs Microsoft Edge WebView2 Runtime ({}).\n\n"Start Hokuto.exe" '
                      'starts the game with the saved settings without this window.').format(error), True)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
