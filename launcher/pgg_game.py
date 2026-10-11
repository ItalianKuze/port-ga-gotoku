# SPDX-License-Identifier: GPL-2.0-or-later
"""PortGaGotoku: what the Windows launcher does besides its window (no Tk here).

Hokuto ga Gotoku / Fist of the North Star: Lost Paradise (CUSA12781). Written for this game from
scripts/run-hokuto-win.sh; the launcher structure follows Supermedo's Windows launcher
(github.com/Supermedo/bloodborne_pc, launcher/bbport_launcher_win.py, GPL-2.0-or-later).

Folders:
  PORT_DIR   the package folder (PortGaGotoku.exe) or, from the sources, the repository;
  RUN_DIR    bb-probe.exe and its DLLs: <package>/bin, or out-win/run from the sources;
  DATA_DIR   PortGaGotoku-data next to PORT_DIR (BB_DATA_DIR overrides): settings
             (launcher.json), the prepared boot image (out/), saves (user/), save backups
             (save-backups/), logs (logs/), shader cache (cache/), GPU library folder (gpu/).
"""
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

TITLE_ID = 'CUSA12781'
# The title id the game saves under (its save data title, not the disc's).
SAVE_TITLE_ID = 'CUSA12776'
# The dump version the port was tested with (param.sfo APP_VER).
TESTED_VERSION = '01.00'
PRESENT_MODES = ('Mailbox', 'Fifo', 'Immediate')

FROZEN = getattr(sys, 'frozen', False)
PORT_DIR = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent.parent
RUN_DIR = PORT_DIR / 'bin' if FROZEN else PORT_DIR / 'out-win' / 'run'
SCRIPTS_DIR = PORT_DIR / 'scripts'
DATA_DIR = Path(os.environ.get('BB_DATA_DIR') or PORT_DIR.parent / 'PortGaGotoku-data')
SETTINGS_FILE = DATA_DIR / 'launcher.json'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)

DEFAULTS = {
    'game_dir': '',
    'present_mode': 'Mailbox',
    'fullscreen': False,
    'gpu': '',            # '' = automatic (discrete GPU preferred), else the Vulkan device name
    'frame_stats': True,  # BB_FRAME_STATS: frame statistics in the log
    'ui_language': '',    # '' = Windows' language (Italian or English)
}


def folders():
    """The data folders by name, as the launcher shows them."""
    return {
        'data': DATA_DIR, 'saves': DATA_DIR / 'user' / 'savedata', 'backups': DATA_DIR / 'save-backups',
        'logs': DATA_DIR / 'logs', 'cache': DATA_DIR / 'cache',
    }


def load_settings():
    try:
        stored = json.loads(SETTINGS_FILE.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        stored = {}
    settings = {**DEFAULTS, **{k: v for k, v in stored.items() if k in DEFAULTS}}
    if settings['present_mode'] not in PRESENT_MODES:
        settings['present_mode'] = DEFAULTS['present_mode']
    return settings


def save_settings(settings):
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    temporary = SETTINGS_FILE.with_suffix('.json.tmp')
    temporary.write_text(json.dumps(settings, indent=2, ensure_ascii=False), encoding='utf-8')
    os.replace(temporary, SETTINGS_FILE)


def build_id():
    """The build's version and commit (scripts/build-win.sh writes build-info.txt)."""
    try:
        return (RUN_DIR / 'build-info.txt').read_text(encoding='utf-8').strip()
    except OSError:
        return ''


# ---------------------------------------------------------------------------------------------
# The game folder

def read_sfo(path):
    """param.sfo as a dict (prepare.py's reader)."""
    sys.path.insert(0, str(SCRIPTS_DIR))
    from prepare import sfo
    return sfo(Path(path).read_bytes())


def check_game(folder):
    """(state, details) for a game folder. state: 'ok', 'untested_version', 'empty' (no folder
    chosen), 'missing' (the folder does not exist), 'no_eboot', 'no_sfo' or 'other_title'.
    details: TITLE_ID, APP_VER and TITLE when param.sfo could be read."""
    if not folder:
        return 'empty', {}
    game = Path(folder)
    if not game.is_dir():
        return 'missing', {}
    try:
        info = read_sfo(game / 'sce_sys' / 'param.sfo')
    except (OSError, ValueError, IndexError):
        info = None
    if not (game / 'eboot.bin').is_file():
        return 'no_eboot', info or {}
    if info is None:
        return 'no_sfo', {}
    if info.get('TITLE_ID') != TITLE_ID:
        return 'other_title', info
    if info.get('APP_VER') != TESTED_VERSION:
        return 'untested_version', info
    return 'ok', info


def playable(state):
    return state in ('ok', 'untested_version')


def find_game_folder(chosen):
    """The folder the user meant: the chosen one, or its CUSA12781 subfolder when it holds the game."""
    folder = Path(chosen)
    if not (folder / 'eboot.bin').is_file() and (folder / TITLE_ID / 'eboot.bin').is_file():
        folder = folder / TITLE_ID
    return str(folder)


def save_slots():
    """The game's save slots (dirName folders under user/savedata/1/CUSA12776), newest last."""
    saves = DATA_DIR / 'user' / 'savedata' / '1' / SAVE_TITLE_ID
    try:
        slots = [p for p in saves.iterdir() if p.is_dir() and not p.name.endswith('.sce_sys')]
    except OSError:
        return []
    return [p.name for p in sorted(slots, key=lambda p: p.stat().st_mtime)]


def banner_data_url(folder):
    """The game's key art from the user's dump (sce_sys/pic1.png, else pic0.png) as a data: URL;
    never stored by the launcher. '' when there is none."""
    import base64
    for name in ('pic1.png', 'pic0.png'):
        try:
            data = (Path(folder) / 'sce_sys' / name).read_bytes()
        except OSError:
            continue
        if data.startswith(b'\x89PNG'):
            return 'data:image/png;base64,' + base64.b64encode(data).decode('ascii')
    return ''


# ---------------------------------------------------------------------------------------------
# The prepared boot image (scripts/prepare.py, link_modules.py, content_profile.py)

def preparation_stamp(game):
    """What the prepared image was made from: the dump's eboot.bin and the preparation scripts."""
    eboot = Path(game) / 'eboot.bin'
    stat = eboot.stat()
    scripts = {}
    for name in ('prepare.py', 'link_modules.py', 'link_libc.py', 'content_profile.py', 'game_check.py'):
        try:
            scripts[name] = (SCRIPTS_DIR / name).stat().st_size
        except OSError:
            scripts[name] = None
    return {'game': str(Path(game).resolve()), 'eboot_size': stat.st_size, 'eboot_mtime': int(stat.st_mtime),
            'scripts': scripts}


def prepared(game):
    """True when DATA_DIR/out holds an image prepared from this game folder by these scripts."""
    out = DATA_DIR / 'out'
    if not ((out / 'boot-linked.bin').is_file() and (out / 'content.bin').is_file()):
        return False
    try:
        stamp = json.loads((out / 'prepared.json').read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return False
    return stamp == preparation_stamp(game)


def script_command(name, *args):
    """A preparation script: run by PortGaGotoku.exe itself when frozen, else by this Python."""
    path = str(SCRIPTS_DIR / name)
    return ([sys.executable, '--script', path, *args] if FROZEN else [sys.executable, path, *args])


def prepare(game, output):
    """Prepares the boot image into DATA_DIR/out (a new folder, swapped in when complete).
    output(text) receives the scripts' lines. Returns True when it succeeded."""
    out = DATA_DIR / 'out'
    new = DATA_DIR / 'out.new'
    if new.exists():
        shutil.rmtree(new)
    new.mkdir(parents=True)
    steps = (('prepare.py', game, '--experimental-elf', '--out', str(new)),
             ('link_modules.py', game, '--out', str(new)),
             ('content_profile.py', game, '--out', str(new), '--sku', 'full'))
    for name, *args in steps:
        output(f'== {name} ==\n')
        process = subprocess.Popen(script_command(name, *args), cwd=str(SCRIPTS_DIR), stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        for raw in iter(process.stdout.readline, b''):
            output(raw.decode('utf-8', errors='replace').replace('\r\n', '\n'))
        if process.wait() != 0:
            output(f'== {name}: exit code {process.returncode} ==\n')
            return False
    (new / 'prepared.json').write_text(json.dumps(preparation_stamp(game), indent=2), encoding='utf-8')
    if out.exists():
        old = DATA_DIR / 'out.old'
        if old.exists():
            shutil.rmtree(old)
        os.replace(out, old)
        os.replace(new, out)
        shutil.rmtree(old, ignore_errors=True)
    else:
        os.replace(new, out)
    return True


# ---------------------------------------------------------------------------------------------
# Devices (bb-probe --devices)

def devices():
    """(vulkan, gamepads): vulkan is a list of (index, type, name); gamepads a list of names.
    None when bb-probe could not be run."""
    try:
        result = subprocess.run([str(RUN_DIR / 'bb-probe.exe'), '--devices'], cwd=str(RUN_DIR), env=game_path_env(),
                                capture_output=True, timeout=20, creationflags=NO_WINDOW)
    except (OSError, subprocess.TimeoutExpired):
        return None
    vulkan, gamepads = [], []
    for line in result.stdout.decode('utf-8', errors='replace').splitlines():
        parts = line.split(' ', 2)
        if len(parts) < 3 or parts[0] != 'Device:':
            continue
        if parts[1] == 'vulkan' and parts[2] != 'none':
            index, kind, name = (parts[2].split(' ', 2) + ['', ''])[:3]
            vulkan.append((int(index), kind, name))
        elif parts[1] == 'gamepad' and not parts[2].startswith('none'):
            gamepads.append(parts[2])
    return vulkan, gamepads


# ---------------------------------------------------------------------------------------------
# Starting the game

def game_path_env():
    """The environment without other programs' DLLs: bb-probe's folder and Windows only, and no
    BB_* variables from the shell that started the launcher."""
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith('BB_')}
    root = os.environ.get('SystemRoot', r'C:\Windows')
    env['PATH'] = os.pathsep.join((str(RUN_DIR), rf'{root}\System32', root))
    return env


def game_environment(settings):
    env = game_path_env()
    env.update({
        'BB_DATA_DIR': str(DATA_DIR),
        'XDG_DATA_HOME': str(DATA_DIR),
        # The GPU library's folders and the port's settings file in the data folder, never
        # %APPDATA%\shadPS4 (the real shadPS4's) or the program folder.
        'BB_GPU_USER_DIR': str(DATA_DIR / 'gpu'),
        'BB_GPU_CACHE_DIR': str(DATA_DIR / 'cache'),
        'BB_CONFIG': str(DATA_DIR / 'bbport.ini'),
        'BB_TIMEOUT': '0',
        'BB_PRESENT_MODE': settings['present_mode'],
        # The in-game menu's language until one is chosen in it (bbport.ini): the launcher's.
        'BB_MENU_LANGUAGE': settings['ui_language'] or windows_language(),
    })
    if settings['fullscreen']:
        env['BB_FULLSCREEN'] = '1'
    if settings['gpu']:
        env['BB_GPU_DEVICE'] = settings['gpu']
    if settings['frame_stats']:
        env['BB_FRAME_STATS'] = '1'
    return env


def windows_language():
    import pgg_lang
    return pgg_lang.windows_language()


def game_command(settings):
    out = DATA_DIR / 'out'
    return [str(RUN_DIR / 'bb-probe.exe'), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
            '--app0', settings['game_dir'], '--user', str(DATA_DIR / 'user'), '--timeout', '0']


def new_log_path(suffix=''):
    logs = DATA_DIR / 'logs'
    logs.mkdir(parents=True, exist_ok=True)
    return logs / time.strftime(f'hokuto-%Y%m%d-%H%M%S{suffix}.log')


def log_header(settings):
    """The first lines of every log written by the launcher."""
    lines = [f'== PortGaGotoku launcher, build {build_id() or "unknown"} ==',
             f'== game {settings["game_dir"]} ==',
             f'== present mode {settings["present_mode"]}, fullscreen {"on" if settings["fullscreen"] else "off"}, '
             f'GPU {settings["gpu"] or "automatic"}, frame statistics {"on" if settings["frame_stats"] else "off"} ==',
             f'== data {DATA_DIR} ==']
    return '\n'.join(lines) + '\n'


def start_game(settings, log_path, extra_env=None):
    """Starts bb-probe with its output in log_path (the log survives the launcher). Returns
    (process, job). extra_env: more variables (the benchmark's scripted input)."""
    (DATA_DIR / 'user').mkdir(parents=True, exist_ok=True)
    with open(log_path, 'w', encoding='utf-8', newline='\n') as log:
        log.write(log_header(settings))
    log = open(log_path, 'ab')
    try:
        process = subprocess.Popen(game_command(settings), cwd=str(RUN_DIR),
                                   env={**game_environment(settings), **(extra_env or {})},
                                   stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                   creationflags=NO_WINDOW)
    finally:
        log.close()
    return process, GameJob(process)


def running_probes():
    """PIDs of bb-probe.exe processes already running (any copy)."""
    try:
        result = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq bb-probe.exe', '/FO', 'CSV', '/NH'],
                                capture_output=True, timeout=10, creationflags=NO_WINDOW)
    except (OSError, subprocess.TimeoutExpired):
        return []
    pids = []
    for line in result.stdout.decode('utf-8', errors='replace').splitlines():
        fields = [f.strip('"') for f in line.split('","')]
        if len(fields) > 1 and fields[0].lower().startswith('bb-probe') and fields[1].isdigit():
            pids.append(int(fields[1]))
    return pids


class GameJob:
    """A Windows job object holding the game (from Supermedo's launcher): Stop ends it and every
    process it started. Stop first asks the game window to close, as its X does, so a save in
    progress is finished (the runtime waits for it); the job is terminated only if the game is
    still there after `grace` seconds."""

    def __init__(self, process):
        self.process = process
        self.handle = None
        if os.name != 'nt':
            return
        kernel32 = ctypes.windll.kernel32
        kernel32.CreateJobObjectW.restype = ctypes.c_void_p
        handle = kernel32.CreateJobObjectW(None, None)
        if handle and kernel32.AssignProcessToJobObject(ctypes.c_void_p(handle), ctypes.c_void_p(int(process._handle))):
            self.handle = handle
        elif handle:
            kernel32.CloseHandle(ctypes.c_void_p(handle))

    def close_window(self):
        """Posts WM_CLOSE to the game's windows. True when there was one."""
        if os.name != 'nt':
            return False
        from ctypes import wintypes
        user32 = ctypes.windll.user32
        found = []
        callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

        def visit(hwnd, _param):
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == self.process.pid and user32.IsWindowVisible(hwnd):
                found.append(hwnd)
            return True
        user32.EnumWindows(callback_type(visit), 0)
        for hwnd in found:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
        return bool(found)

    def terminate(self):
        if self.handle:
            ctypes.windll.kernel32.TerminateJobObject(ctypes.c_void_p(self.handle), 1)
        elif self.process.poll() is None:
            self.process.kill()

    def stop(self, grace=10.0):
        """Asks the game to close, then terminates it after `grace` seconds. Returns how it ended:
        'closed' or 'terminated'."""
        if self.close_window():
            deadline = time.monotonic() + grace
            while time.monotonic() < deadline:
                if self.process.poll() is not None:
                    return 'closed'
                time.sleep(0.1)
        self.terminate()
        return 'terminated'

    def close(self):
        if self.handle:
            ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(self.handle))
            self.handle = None
