#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""PortGaGotoku launcher for Windows (Tkinter): Fist of the North Star: Lost Paradise (CUSA12781).

The structure comes from Supermedo's Windows launcher (github.com/Supermedo/bloodborne_pc,
launcher/bbport_launcher_win.py, v1.6, GPL-2.0-or-later): settings pages in a side bar, folder
rows with "Browse…", the game in a Windows job object, the log page, texts written in English and
translated by table (pgg_lang.py), and one frozen executable with several roles. Everything else
is written for Hokuto, from scripts/run-hokuto-win.sh; only options verified on Hokuto are here.

Roles of PortGaGotoku.exe (frozen with PyInstaller, packaging/windows/package.sh):
  (none)              the launcher window;
  --play              the game with the saved settings, without the window (Gioca Hokuto.exe);
  --script FILE ARGS  a preparation script (scripts/prepare.py, ...), so no Python is needed.
"""
import ctypes
import os
from pathlib import Path
import queue
import runpy
import sys
import threading

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pgg_game as game  # noqa: E402
import pgg_lang  # noqa: E402

_ = pgg_lang.tr
MAX_LOG_LINES = 6000

# Colours: dark panels, the red and gold of the game's menus.
BG, PANEL, CARD, LINE = '#121012', '#1b181b', '#262126', '#3a3238'
TEXT, MUTED, ACCENT, ACCENT_HI, GOLD = '#ece4dc', '#a2968e', '#a3262a', '#c23238', '#d6b25e'
OK, WARN, BAD = '#9fcf8a', '#e3b25a', '#e07a6c'


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
    log_path = game.new_log_path()
    if not game.prepared(settings['game_dir']):
        with open(log_path, 'w', encoding='utf-8') as log:
            ok = game.prepare(settings['game_dir'], log.write)
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
# The window

class Launcher:
    def __init__(self, root, tk, ttk, filedialog, messagebox):
        self.tk, self.ttk, self.filedialog, self.messagebox = tk, ttk, filedialog, messagebox
        self.root = root
        self.settings = game.load_settings()
        self.vars = {}
        self.process = self.job = None
        self.log_path = None
        self.busy = False            # preparing or running
        self.stopping = False
        self.last_exit = None
        self.devices = None          # (vulkan, gamepads) once bb-probe --devices answered
        self.ui_calls = queue.Queue()  # work for the Tk thread from helper threads
        root.title('PortGaGotoku — Fist of the North Star: Lost Paradise')
        root.configure(bg=BG)
        self.dpi = root.winfo_fpixels('1i') / 96.0
        root.geometry(f'{self.px(1080)}x{self.px(700)}')
        root.minsize(self.px(940), self.px(620))
        self.set_icon()
        self.style()
        self.build()
        self.show('play')
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.pump)
        threading.Thread(target=self.detect_devices, daemon=True).start()
        if not self.settings['game_dir']:
            # First start: the folder picker opens by itself.
            root.after(400, self.browse_game)

    def px(self, size):
        return int(size * self.dpi)

    def later(self, function):
        """Runs function on the Tk thread (from a helper thread)."""
        self.ui_calls.put(function)

    def set_icon(self):
        icon = Path(self.settings['game_dir'] or '.') / 'sce_sys' / 'icon0.png'
        try:
            self.icon = self.tk.PhotoImage(file=str(icon))
            if self.icon.width() > 128:
                self.icon = self.icon.subsample(self.icon.width() // 64)
            self.root.iconphoto(True, self.icon)
        except (self.tk.TclError, OSError):
            pass

    # ---- look --------------------------------------------------------------------------------
    def style(self):
        s = self.ttk.Style(self.root)
        s.theme_use('clam')
        base = ('Segoe UI', 10)
        self.root.option_add('*TCombobox*Listbox.background', CARD)
        self.root.option_add('*TCombobox*Listbox.foreground', TEXT)
        self.root.option_add('*TCombobox*Listbox.selectBackground', ACCENT)
        self.root.option_add('*TCombobox*Listbox.font', base)
        s.configure('.', background=PANEL, foreground=TEXT, fieldbackground=CARD, bordercolor=LINE,
                    lightcolor=LINE, darkcolor=LINE, troughcolor=CARD, focuscolor=GOLD, font=base)
        s.configure('TFrame', background=PANEL)
        s.configure('TLabel', background=PANEL, foreground=TEXT)
        s.configure('Muted.TLabel', background=PANEL, foreground=MUTED, font=('Segoe UI', 9))
        s.configure('Section.TLabel', background=PANEL, foreground=GOLD, font=('Segoe UI Semibold', 13))
        s.configure('Title.TLabel', background=PANEL, foreground=TEXT, font=('Segoe UI Light', 20))
        s.configure('TCheckbutton', background=PANEL, foreground=TEXT, padding=(0, 3))
        s.map('TCheckbutton', background=[('active', PANEL)], foreground=[('disabled', MUTED)])
        s.configure('TCombobox', arrowcolor=GOLD, foreground=TEXT, padding=4)
        s.map('TCombobox', fieldbackground=[('readonly', CARD)], foreground=[('readonly', TEXT)],
              selectbackground=[('readonly', CARD)], selectforeground=[('readonly', TEXT)])
        s.configure('TButton', background=CARD, foreground=TEXT, padding=(12, 5), borderwidth=1)
        s.map('TButton', background=[('active', LINE), ('disabled', PANEL)], foreground=[('disabled', MUTED)])
        s.configure('Play.TButton', background=ACCENT, foreground='#f6eee6', font=('Segoe UI Semibold', 15),
                    padding=(36, 9), borderwidth=0)
        s.map('Play.TButton', background=[('active', ACCENT_HI), ('disabled', '#3a2224')],
              foreground=[('disabled', '#8a7a74')])
        s.configure('Vertical.TScrollbar', background=CARD, arrowcolor=GOLD, troughcolor=PANEL, bordercolor=PANEL)

    # ---- widget helpers ----------------------------------------------------------------------
    def var(self, key):
        """The Tk variable of a setting (one per setting)."""
        if key not in self.vars:
            value = self.settings[key]
            v = self.tk.BooleanVar(value=value) if isinstance(value, bool) else self.tk.StringVar(value=value)
            v.trace_add('write', lambda *_a: self.refresh())
            self.vars[key] = v
        return self.vars[key]

    def choice(self, parent, key, options, width=40):
        """A read-only combobox over (value, shown text) pairs, kept in sync with its variable."""
        var = self.var(key)
        box = self.ttk.Combobox(parent, state='readonly', width=width)

        def fill(options):
            box.options = list(options)
            box.configure(values=[text for _v, text in box.options])
            show()

        def show(*_args):
            values = [v for v, _t in box.options]
            box.current(values.index(var.get()) if var.get() in values else 0)
        box.bind('<<ComboboxSelected>>', lambda _e: var.set(box.options[box.current()][0]))
        var.trace_add('write', show)
        box.fill = fill
        fill(options)
        return box

    def page(self, name, title, subtitle):
        """A settings page: a heading and a grid below it."""
        outer = self.ttk.Frame(self.content, padding=(28, 22, 28, 18))
        self.ttk.Label(outer, text=title, style='Title.TLabel').pack(anchor='w')
        self.ttk.Label(outer, text=subtitle, style='Muted.TLabel', wraplength=self.px(700),
                       justify='left').pack(anchor='w', pady=(0, 10))
        inner = self.ttk.Frame(outer)
        inner.pack(fill='both', expand=True)
        inner.columnconfigure(1, weight=1)
        self.pages[name] = outer
        return inner

    def row(self, parent, title, widget, hint=None):
        r = parent.grid_size()[1]
        self.ttk.Label(parent, text=title).grid(row=r, column=0, sticky='nw', padx=(0, 18), pady=(9, 0))
        widget.grid(row=r, column=1, sticky='w', pady=(6, 0))
        if hint:
            self.ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(600), justify='left').grid(
                row=r + 1, column=1, sticky='w', pady=(2, 2))
        return widget

    def note(self, parent, text, top=14):
        self.ttk.Label(parent, text=text, style='Muted.TLabel', wraplength=self.px(720), justify='left').grid(
            row=parent.grid_size()[1], column=0, columnspan=2, sticky='w', pady=(top, 0))

    def section(self, parent, title, top=16):
        self.ttk.Label(parent, text=title, style='Section.TLabel').grid(
            row=parent.grid_size()[1], column=0, columnspan=2, sticky='w', pady=(top, 2))

    # ---- layout ------------------------------------------------------------------------------
    def build(self):
        tk, ttk = self.tk, self.ttk
        side = tk.Frame(self.root, bg=BG, width=self.px(230))
        side.pack(side='left', fill='y')
        side.pack_propagate(False)
        tk.Label(side, text='北斗が如く', bg=BG, fg=GOLD, font=('Yu Gothic UI', 17)).pack(anchor='w', padx=22, pady=(24, 0))
        tk.Label(side, text='Fist of the North Star:\nLost Paradise', bg=BG, fg=TEXT, justify='left',
                 font=('Segoe UI', 10)).pack(anchor='w', padx=22)
        tk.Label(side, text='PortGaGotoku · Windows', bg=BG, fg=MUTED, font=('Segoe UI', 9)).pack(
            anchor='w', padx=22, pady=(4, 20))
        self.nav, self.current = {}, None
        for name, title in (('play', _('Play')), ('display', _('Display')), ('stats', _('Statistics')),
                            ('folders', _('Folders')), ('log', _('Log'))):
            item = tk.Label(side, text='    ' + title, bg=BG, fg=TEXT, anchor='w', font=('Segoe UI', 11),
                            pady=10, cursor='hand2')
            item.pack(fill='x')
            item.bind('<Button-1>', lambda _e, n=name: self.show(n))
            self.nav[name] = item
        tk.Frame(side, bg=BG).pack(fill='both', expand=True)
        lang = tk.Frame(side, bg=BG)
        lang.pack(anchor='w', padx=22, pady=(0, 6))
        tk.Label(lang, text=_('Launcher language'), bg=BG, fg=MUTED, font=('Segoe UI', 9)).pack(anchor='w')
        self.choice(lang, 'ui_language', (('', _('Windows language')), ('it', 'Italiano'), ('en', 'English')),
                    width=18).pack(anchor='w')
        self.lang_note = tk.Label(side, text='', bg=BG, fg=MUTED, font=('Segoe UI', 8), justify='left',
                                  wraplength=self.px(200))
        self.lang_note.pack(anchor='w', padx=22)
        tk.Label(side, text=_('Build {}').format(game.build_id() or '?'), bg=BG, fg=MUTED,
                 font=('Segoe UI', 8)).pack(anchor='w', padx=22, pady=(6, 16))

        right = tk.Frame(self.root, bg=PANEL)
        right.pack(side='left', fill='both', expand=True)
        bar = tk.Frame(right, bg=BG, height=self.px(68))
        bar.pack(fill='x', side='bottom')
        bar.pack_propagate(False)
        self.status = tk.Label(bar, text='', bg=BG, fg=MUTED, font=('Segoe UI', 10), anchor='w', justify='left',
                               wraplength=self.px(560))
        self.status.pack(side='left', padx=24)
        self.play_button = ttk.Button(bar, text=_('PLAY'), style='Play.TButton', command=self.play)
        self.play_button.pack(side='right', padx=(10, 24), pady=10)
        self.stop_button = ttk.Button(bar, text=_('Stop'), command=self.stop, state='disabled')
        self.stop_button.pack(side='right', pady=10)
        self.content = tk.Frame(right, bg=PANEL)
        self.content.pack(fill='both', expand=True)

        self.pages = {}
        self.build_play()
        self.build_display()
        self.build_stats()
        self.build_folders()
        self.build_log()
        self.refresh()

    def show(self, name):
        for page in self.pages.values():
            page.pack_forget()
        self.pages[name].pack(fill='both', expand=True)
        self.current = name
        for n, item in self.nav.items():
            item.configure(bg=PANEL if n == name else BG, fg=GOLD if n == name else TEXT)
        self.refresh()

    def build_play(self):
        tk, ttk = self.tk, self.ttk
        page = tk.Frame(self.content, bg=PANEL)
        self.pages['play'] = page
        self.banner = tk.Label(page, bg=BG, height=self.px(230) // 16)
        self.banner.pack(fill='x')
        self.banner_image = None
        body = ttk.Frame(page, padding=(28, 14, 28, 8))
        body.pack(fill='both', expand=True)
        body.columnconfigure(1, weight=1)
        ttk.Label(body, text=_('Ready check'), style='Section.TLabel').grid(row=0, column=0, columnspan=3,
                                                                            sticky='w', pady=(0, 6))
        self.checks = {}
        for r, key in enumerate(('game', 'prepared', 'gpu', 'gamepad', 'saves', 'exit'), start=1):
            label = ttk.Label(body, text='', justify='left', wraplength=self.px(620))
            label.grid(row=r, column=0, columnspan=2, sticky='w', pady=3)
            self.checks[key] = label
        # "Browse…" next to the game folder message itself.
        self.game_browse = ttk.Button(body, text=_('Browse…'), command=self.browse_game)
        self.game_browse.grid(row=1, column=2, sticky='e', padx=(12, 0))
        ttk.Label(body, text=_('In the game: Insert or L3+R3 opens the port menu. The window closes with its X '
                               '(Alt+F4 in fullscreen): a save in progress is finished first.'),
                  style='Muted.TLabel', wraplength=self.px(700), justify='left').grid(
            row=8, column=0, columnspan=3, sticky='w', pady=(14, 0))

    def build_display(self):
        page = self.page('display', _('Display'), _('Read when the game starts.'))
        self.row(page, _('Present mode'), self.choice(page, 'present_mode', (
            ('Mailbox', _('Mailbox (recommended)')), ('Fifo', _('Fifo (classic vsync)')),
            ('Immediate', _('Immediate (no vsync, may tear)')))),
            _('All three were tried on Hokuto: same frame times (60 FPS standing, the drops while walking '
              'come from the frame work, not from the presentation).'))
        fullscreen = self.ttk.Checkbutton(page, text=_('Fullscreen'), variable=self.var('fullscreen'))
        self.row(page, '', fullscreen, _('Alt+F4 closes the game (there is no X in fullscreen). Alt+Tab has '
                                         'not been tried yet.'))
        self.gpu_box = self.choice(page, 'gpu', (('', _('Automatic (discrete GPU preferred)')),))
        self.row(page, _('Graphics card'), self.gpu_box, _('The list comes from Vulkan when the launcher starts.'))
        self.note(page, _('Output resolution and the graphics options of the port menu are not here: on Hokuto '
                          'they have no effect yet (they will come with a graphics profile for Hokuto).'))

    def build_stats(self):
        page = self.page('stats', _('Statistics'), _('Measurements for the performance work.'))
        self.row(page, '', self.ttk.Checkbutton(page, text=_('Frame statistics in the log'),
                                                variable=self.var('frame_stats')),
                 _('Every few seconds: FPS, worst frame, shader compiles (lines "Frame stats:" in the log).'))
        self.note(page, _('On-screen statistics (HUD) will come in the next phase.'))

    def build_folders(self):
        ttk = self.ttk
        page = self.page('folders', _('Folders'), _('Everything the port writes stays in the data folder.'))
        holder = ttk.Frame(page)
        self.game_entry = ttk.Entry(holder, textvariable=self.var('game_dir'), width=58)
        self.game_entry.pack(side='left')
        ttk.Button(holder, text=_('Browse…'), command=self.browse_game).pack(side='left', padx=(6, 0))
        ttk.Button(holder, text=_('Open'), command=lambda: self.open_path(self.var('game_dir').get())).pack(
            side='left', padx=(6, 0))
        self.row(page, _('Game folder ({})').format(game.TITLE_ID), holder,
                 _('The folder of the dump with eboot.bin and sce_sys. It is only read.'))
        names = (('data', _('Data folder')), ('saves', _('Saves')), ('backups', _('Save backups')),
                 ('logs', _('Logs')), ('cache', _('Shader cache')))
        for key, title in names:
            path = game.folders()[key]
            line = ttk.Frame(page)
            ttk.Label(line, text=str(path), width=62).pack(side='left')
            ttk.Button(line, text=_('Open'), command=lambda p=path: self.open_path(p)).pack(side='left', padx=(6, 0))
            self.row(page, title, line)
        self.note(page, _('Settings: {}. The data folder is PortGaGotoku-data next to the program folder '
                          '(BB_DATA_DIR changes it).').format(game.SETTINGS_FILE))

    def build_log(self):
        tk, ttk = self.tk, self.ttk
        page = ttk.Frame(self.content, padding=(20, 14, 20, 8))
        self.pages['log'] = page
        top = ttk.Frame(page)
        top.pack(fill='x', pady=(0, 8))
        ttk.Label(top, text=_('Log'), style='Title.TLabel').pack(side='left')
        ttk.Button(top, text=_('Copy'), command=self.copy_log).pack(side='right')
        ttk.Button(top, text=_('Open the logs folder'), command=lambda: self.open_path(game.folders()['logs'])).pack(
            side='right', padx=6)
        self.log_name = ttk.Label(page, text='', style='Muted.TLabel')
        self.log_name.pack(anchor='w')
        self.log = tk.Text(page, wrap='none', bg='#0b0a0b', fg='#d2c9be', insertbackground=TEXT, relief='flat',
                           font=('Consolas', 9), padx=8, pady=6, state='disabled', highlightthickness=0)
        bar = ttk.Scrollbar(page, command=self.log.yview)
        self.log.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y')
        self.log.pack(fill='both', expand=True)

    # ---- state -------------------------------------------------------------------------------
    def collect(self):
        for key, var in self.vars.items():
            self.settings[key] = var.get()
        try:
            game.save_settings(self.settings)
        except OSError as error:
            self.status.configure(text=_('Could not save the settings: {}').format(error), fg=BAD)

    def game_message(self, state, info):
        title, version = info.get('TITLE_ID', '?'), info.get('APP_VER', '?')
        return {
            'ok': (OK, _('✓ Game found: {} (CUSA12781), version {}').format(info.get('TITLE', ''), version)),
            'untested_version': (WARN, _('⚠ CUSA12781 version {}: only version {} has been tried').format(
                version, game.TESTED_VERSION)),
            'empty': (BAD, _('✗ Game folder not chosen: choose the CUSA12781 folder with eboot.bin')),
            'missing': (BAD, _('✗ The game folder does not exist any more: choose it again')),
            'no_eboot': (BAD, _('✗ No eboot.bin in this folder: it is not the game folder (CUSA12781)')),
            'no_sfo': (BAD, _('✗ No readable sce_sys\\param.sfo in this folder: it is not a complete PS4 dump')),
            'other_title': (BAD, _('✗ This folder holds {} ("{}"), not CUSA12781: choose the Fist of the North Star: '
                                   'Lost Paradise folder').format(title, info.get('TITLE', '?'))),
        }[state]

    def refresh(self):
        if not hasattr(self, 'checks'):
            return
        settings = {**self.settings, **{k: v.get() for k, v in self.vars.items()}}
        state, info = game.check_game(settings['game_dir'])
        colour, text = self.game_message(state, info)
        self.checks['game'].configure(text=text, foreground=colour)
        if game.playable(state):
            ready = game.prepared(settings['game_dir'])
            self.checks['prepared'].configure(
                text=_('✓ Boot image ready') if ready else _('• The boot image is prepared at the next start '
                                                             '(a few seconds; the game files are only read)'),
                foreground=OK if ready else MUTED)
        else:
            self.checks['prepared'].configure(text='', foreground=MUTED)
        if self.devices is None:
            self.checks['gpu'].configure(text=_('• Graphics card: checking…'), foreground=MUTED)
            self.checks['gamepad'].configure(text=_('• Controller: checking…'), foreground=MUTED)
        else:
            vulkan, pads = self.devices
            chosen = settings['gpu']
            if not vulkan:
                gpu = (BAD, _('✗ No Vulkan graphics card found (update the driver)'))
            elif chosen and not any(chosen == name for _i, _k, name in vulkan):
                gpu = (WARN, _('⚠ Graphics card "{}" not found: the game chooses automatically').format(chosen))
            else:
                name = chosen or next((n for _i, k, n in vulkan if k == 'discrete'), vulkan[0][2])
                gpu = (OK, _('✓ Graphics card: {}{}').format(name, '' if chosen else _(' (automatic)')))
            self.checks['gpu'].configure(text=gpu[1], foreground=gpu[0])
            self.checks['gamepad'].configure(
                text=_('✓ Controller: {}').format(', '.join(pads)) if pads else
                _('• No controller found when the launcher started (keyboard works; a controller connected '
                  'later is taken by the game)'), foreground=OK if pads else MUTED)
        saves = game.folders()['saves'] / '1' / 'CUSA12776'
        slots = sorted(p.name for p in saves.iterdir() if p.is_dir() and not p.name.endswith('.sce_sys')) \
            if saves.is_dir() else []
        self.checks['saves'].configure(
            text=_('✓ Saves: slots {}').format(', '.join(slots)) if slots else
            _('• No saves yet: the game creates them in {}').format(saves), foreground=OK if slots else MUTED)
        if self.last_exit is None:
            self.checks['exit'].configure(text='', foreground=MUTED)
        elif self.last_exit == 0:
            self.checks['exit'].configure(text=_('✓ Last game: closed normally'), foreground=OK)
        else:
            self.checks['exit'].configure(text=_('✗ Last game: ended with code {} (see the Log page)').format(
                self.last_exit), foreground=BAD)
        ui = settings['ui_language']
        self.lang_note.configure(text=_('Applies when the launcher is opened again') if
                                 (ui or pgg_lang.windows_language()) != pgg_lang.LANG else '')
        if self.devices:
            self.gpu_box.fill([('', _('Automatic (discrete GPU preferred)'))] +
                              [(name, f'{name} ({kind})') for _i, kind, name in self.devices[0]])
        if self.banner_image is None:
            self.load_banner(settings['game_dir'])
        if not self.busy:
            self.play_button.configure(state='normal' if game.playable(state) else 'disabled')
            if game.playable(state):
                self.status.configure(text=self.summary(settings), fg=MUTED)
            else:
                self.status.configure(text=_('Choose the game folder first.'), fg=MUTED)

    def summary(self, settings):
        return _('{}, {}, frame statistics {}').format(
            _('fullscreen') if settings['fullscreen'] else _('window'), settings['present_mode'],
            _('on') if settings['frame_stats'] else _('off'))

    def load_banner(self, folder):
        """The game's own key art (sce_sys/pic1.png in the dump), scaled down to fit."""
        path = Path(folder or '.') / 'sce_sys' / 'pic1.png'
        try:
            image = self.tk.PhotoImage(file=str(path))
            factor = max(1, image.width() // max(1, self.content.winfo_width() or self.px(850)))
            self.banner_image = image.subsample(factor) if factor > 1 else image
            # Show the middle band only.
            self.banner.configure(image=self.banner_image, height=self.px(230))
        except (self.tk.TclError, OSError):
            self.banner_image = False

    def detect_devices(self):
        found = game.devices()

        def done():
            self.devices = found or ([], [])
            self.refresh()
        self.later(done)

    def browse_game(self):
        if self.busy:
            return
        current = self.var('game_dir').get()
        chosen = self.filedialog.askdirectory(title=_('Choose the game folder (CUSA12781, with eboot.bin)'),
                                              initialdir=current or str(Path.home()), mustexist=True)
        if not chosen:
            return
        folder = Path(chosen)
        # A parent folder holding CUSA12781 is accepted too.
        if not (folder / 'eboot.bin').is_file() and (folder / game.TITLE_ID / 'eboot.bin').is_file():
            folder = folder / game.TITLE_ID
        state, info = game.check_game(str(folder))
        if not game.playable(state):
            self.messagebox.showerror('PortGaGotoku', self.game_message(state, info)[1].lstrip('✗ ') + '\n\n' +
                                      _('Chosen folder: {}').format(folder))
            if self.var('game_dir').get():
                return
        self.var('game_dir').set(str(folder))
        self.banner_image = None
        self.set_icon()
        self.collect()
        self.refresh()

    # ---- game process ------------------------------------------------------------------------
    def play(self):
        if self.busy:
            return
        self.collect()
        state, info = game.check_game(self.settings['game_dir'])
        if not game.playable(state):
            self.messagebox.showerror('PortGaGotoku', self.game_message(state, info)[1])
            return
        running = game.running_probes()
        if running and not self.messagebox.askyesno('PortGaGotoku', _(
                'bb-probe.exe is already running (PID {}). Two games on the same saves can damage them.\n\n'
                'Start anyway?').format(', '.join(map(str, running)))):
            return
        self.busy, self.stopping = True, False
        self.play_button.configure(state='disabled')
        self.set_log('')
        threading.Thread(target=self.run_game, args=(dict(self.settings),), daemon=True).start()

    def run_game(self, settings):
        """Helper thread: preparation if needed, then the game; its log is followed into the Log page."""
        if not game.prepared(settings['game_dir']):
            self.later(lambda: self.status.configure(text=_('Preparing the boot image…'), fg=GOLD))
            self.later(lambda: self.show('log'))
            log_path = game.new_log_path('-prepare')
            with open(log_path, 'w', encoding='utf-8') as log:
                def output(text):
                    log.write(text)
                    self.later(lambda t=text: self.append(t))
                ok = game.prepare(settings['game_dir'], output)
            if not ok:
                self.later(lambda: self.finished(None, _('Preparing the boot image failed: see the log ({}).').format(
                    log_path)))
                return
        log_path = game.new_log_path()
        try:
            process, job = game.start_game(settings, log_path)
        except OSError as error:
            self.later(lambda: self.finished(None, _('Could not start the game: {}').format(error)))
            return
        self.process, self.job, self.log_path = process, job, log_path

        def started():
            self.stop_button.configure(state='normal')
            self.status.configure(text=_('The game is running: it opens in its own window.'), fg=GOLD)
            self.log_name.configure(text=str(log_path))
            self.set_log('')
        self.later(started)
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
                text = chunk.decode('utf-8', errors='replace')
                self.later(lambda t=text: self.append(t))
            if code is not None:
                break
            threading.Event().wait(0.25)
        job.close()
        self.later(lambda: self.finished(code))

    def finished(self, code, message=None):
        self.busy = False
        self.process = self.job = None
        self.stop_button.configure(state='disabled')
        if code is not None:
            self.last_exit = code
            self.append(_('\n— the game ended (code {}) —\n').format(code))
        self.refresh()
        if message:
            self.status.configure(text=message, fg=BAD)
            self.show('log')
        elif code == 0:
            self.status.configure(text=_('The game was closed normally.'), fg=MUTED)
        elif self.stopping:
            self.status.configure(text=_('The game was stopped from the launcher (code {}).').format(code), fg=WARN)
        else:
            # Not a normal end: stay on the log so the reason is visible.
            self.status.configure(text=_('The game ended with code {}: the end of the log is shown.').format(code),
                                  fg=BAD)
            self.show('log')

    def stop(self):
        job = self.job
        if not job or self.stopping:
            return
        self.stopping = True
        self.stop_button.configure(state='disabled')
        self.status.configure(text=_('Closing the game (a save in progress is finished first)…'), fg=GOLD)
        threading.Thread(target=job.stop, daemon=True).start()

    def pump(self):
        while not self.ui_calls.empty():
            self.ui_calls.get_nowait()()
        self.root.after(100, self.pump)

    def append(self, text):
        self.log.configure(state='normal')
        self.log.insert('end', text)
        lines = int(self.log.index('end-1c').split('.')[0])
        if lines > MAX_LOG_LINES:
            self.log.delete('1.0', f'{lines - MAX_LOG_LINES}.0')
        self.log.see('end')
        self.log.configure(state='disabled')

    def set_log(self, text):
        self.log.configure(state='normal')
        self.log.delete('1.0', 'end')
        self.log.insert('end', text)
        self.log.configure(state='disabled')

    def copy_log(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(self.log.get('1.0', 'end'))

    def open_path(self, path):
        path = Path(path or game.DATA_DIR)
        if not path.exists():
            path.mkdir(parents=True, exist_ok=True)
        os.startfile(str(path))

    def close(self):
        if self.busy and not self.messagebox.askyesno('PortGaGotoku', _(
                'The game is still running. Close the launcher anyway?\n\nThe game stays open; its log is '
                'still written to the logs folder.')):
            return
        try:
            self.collect()
        except Exception:  # never keep the window open over a settings problem
            pass
        self.root.destroy()


def main():
    args = sys.argv[1:]
    if args and args[0] == '--script':
        sys.exit(run_script(args))
    settings = game.load_settings()
    pgg_lang.LANG = settings['ui_language'] or pgg_lang.windows_language()
    if '--play' in args and game.playable(game.check_game(settings['game_dir'])[0]):
        sys.exit(play_without_window(settings))
    # Without a usable game folder there is nothing to play yet: the launcher opens instead.
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)  # sharp text on scaled displays
    except (AttributeError, OSError):
        pass
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    root = tk.Tk()
    Launcher(root, tk, ttk, filedialog, messagebox)
    root.mainloop()


if __name__ == '__main__':
    main()
