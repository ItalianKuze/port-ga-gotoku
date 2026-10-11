#!/usr/bin/env python3
"""PortGaGotoku: automatic benchmark of Fist of the North Star: Lost Paradise (Windows).

    python scripts/bench_hokuto.py record          the player records the route once (F9 ... F9)
    python scripts/bench_hokuto.py walk [--runs 3]  the recorded route, 90 s measured
    python scripts/bench_hokuto.py still [--runs 3] standing after the load, 30 s measured

Every run plays on a fresh copy of the data (PortGaGotoku-data-bench next to PortGaGotoku-data: saves,
shader cache, settings and boot image copied from the real one, which is never written), so every run
starts from the same save and cache. The script waits for the title screen, presses Cross until the
game asks for the save list (the runtime answers it with the latest slot: Continue), waits for
"Load completed.", for the end of the loading screen (frame statistics at 28 FPS or more) and for the
delay the player took in the recording before pressing F9, then writes
"replay measure=90" (walk) or "measure=30" (still) into the scripted pad file: the recorded route plays
back and the measurement (gpu/shim/bbport_perf) starts on the same frame. When the summary is written
the game is closed like its window's X (a save in progress is finished), and scripts/perf_report.py
writes the report. Keep the game window alone: no input is needed or wanted.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
REAL_DATA = Path(os.environ.get('BB_DATA_DIR') or ROOT.parent / 'PortGaGotoku-data')
BENCH_DATA = REAL_DATA.parent / (REAL_DATA.name + '-bench')
ROUTE = REAL_DATA / 'bench' / 'route.txt'          # the recorded pad route (kept with the real data)
ROUTE_META = REAL_DATA / 'bench' / 'route.json'    # the delay from "Load completed." to F9
COPIED = ('user', 'cache', 'gpu', 'bbport.ini', 'launcher.json')

os.environ['BB_DATA_DIR'] = str(BENCH_DATA)  # pgg_game reads it at import
sys.path.insert(0, str(ROOT / 'launcher'))
import pgg_game as game  # noqa: E402


def say(text):
    print(time.strftime('%H:%M:%S'), text, flush=True)


def copy_data():
    """A fresh copy of the real data for one run; the boot image is copied once (it is large)."""
    BENCH_DATA.mkdir(exist_ok=True)
    for name in COPIED:
        source, target = REAL_DATA / name, BENCH_DATA / name
        if target.is_dir():
            shutil.rmtree(target)
        elif target.exists():
            target.unlink()
        if source.is_dir():
            shutil.copytree(source, target)
        elif source.exists():
            shutil.copy2(source, target)
    out = BENCH_DATA / 'out'
    stamp = REAL_DATA / 'out' / 'prepared.json'
    if not (out / 'prepared.json').exists() or (out / 'prepared.json').read_bytes() != stamp.read_bytes():
        if out.exists():
            shutil.rmtree(out)
        shutil.copytree(REAL_DATA / 'out', out)
    (BENCH_DATA / 'logs').mkdir(exist_ok=True)


class Log:
    """Follows the game log as it grows."""

    def __init__(self, path):
        self.path, self.position, self.lines, self.cursor = path, 0, [], 0

    def poll(self):
        try:
            with open(self.path, 'rb') as f:
                f.seek(self.position)
                data = f.read()
        except OSError:
            return []
        cut = data.rfind(b'\n') + 1
        self.position += cut
        new = data[:cut].decode('utf-8', errors='replace').splitlines()
        self.lines += new
        return new

    def wait(self, needle, timeout, process, pad=None, step=None):
        """Waits for a line containing `needle`; step() is called every 0.2 s meanwhile."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.poll()
            # Lines read but not looked at yet stay for the next wait.
            while self.cursor < len(self.lines):
                line = self.lines[self.cursor]
                self.cursor += 1
                if needle in line:
                    return line
            if process.poll() is not None:
                raise RuntimeError(f'the game ended (code {process.returncode}) before "{needle}"')
            if step:
                step()
            time.sleep(0.2)
        raise RuntimeError(f'"{needle}" did not appear within {timeout} s')


def wait_gameplay(log, process):
    """After the save is loaded the game shows a loading screen at a fixed 20 FPS for about 40 s:
    gameplay starts with the first frame statistics (every 5 s) at 28 FPS or more after one at 22 or
    less (the first window after the load can still hold menu frames)."""
    seen_loading = False
    while True:
        line = log.wait('Frame stats:', 300, process)
        try:
            fps = float(line.split('Frame stats:', 1)[1].split('FPS', 1)[0])
        except ValueError:
            continue
        if fps <= 22.0:
            seen_loading = True
        elif fps >= 28.0 and seen_loading:
            return


def write_pad(path, text):
    # Windows' stat has whole seconds: the runtime sees a change only in a new second, and it must
    # never read the file half written (it would not read it again in the same second): a whole
    # new file replaces the old one.
    temporary = path.with_suffix('.tmp')
    temporary.write_text(text, encoding='ascii')
    os.replace(temporary, path)
    time.sleep(1.2)


def start(extra_env, tag):
    settings = game.load_settings()
    if not game.playable(game.check_game(settings['game_dir'])[0]):
        raise SystemExit(f'No playable game folder in {game.SETTINGS_FILE}')
    log_path = game.new_log_path(f'-bench-{tag}')
    process, job = game.start_game(settings, log_path, extra_env)
    say(f'game started (log {log_path.name})')
    return process, job, Log(log_path)


def close(process, job):
    if process.poll() is None:
        job.stop(grace=20.0)
    code = process.wait()
    job.close()
    return code


def record():
    copy_data()
    ROUTE.parent.mkdir(parents=True, exist_ok=True)
    process, job, log = start({'BB_PAD_RECORD': str(ROUTE)}, 'record')
    print('\n  In the game: Continue (slot 02). When you have control, without moving, press F9.\n'
          '  0-60 s: the baseline route (walk towards the Eden gate, camera turns at ~20 and ~40 s);\n'
          '  60-90 s: stand still; then F9 again (stops the recording) and close the window.\n', flush=True)
    log.wait('SaveDataDialog LIST (load', 600, process)
    log.wait('Load completed.', 600, process)
    wait_gameplay(log, process)
    loaded = time.monotonic()
    log.wait('pad recording started', 600, process)
    delay = time.monotonic() - loaded
    log.wait('pad recording stopped', 600, process)
    ROUTE_META.write_text(json.dumps({'delay_after_gameplay_s': round(delay, 1),
                                      'recorded': time.strftime('%Y-%m-%d %H:%M')}, indent=1), encoding='utf-8')
    say(f'route saved: {ROUTE} ({sum(1 for _ in open(ROUTE))} samples), F9 {delay:.1f} s after the load')
    code = process.wait()
    job.close()
    say(f'game closed (code {code})')


def run(kind, index):
    if kind == 'walk' and not ROUTE.exists():
        raise SystemExit('No recorded route: run "record" first')
    meta = json.loads(ROUTE_META.read_text(encoding='utf-8')) if ROUTE_META.exists() else {}
    delay = float(meta.get('delay_after_gameplay_s', 5.0))
    copy_data()
    pad = BENCH_DATA / 'bench-pad.txt'
    pad.write_text('', encoding='ascii')
    env = {'BB_PAD_FILE': str(pad)}
    if kind == 'walk':
        env['BB_PAD_REPLAY'] = str(ROUTE)
    process, job, log = start(env, f'{kind}{index}')
    try:
        log.wait('Overlay: menu ready', 300, process)
        # Title screen and main menu: Cross until the game asks for its save list (Continue).
        last_press = [0.0]

        def press():
            if time.monotonic() - last_press[0] > 3.0:
                write_pad(pad, 'cross')
                write_pad(pad, '')
                last_press[0] = time.monotonic()
        time.sleep(10)
        log.wait('SaveDataDialog LIST (load', 240, process, step=press)
        log.wait('Load completed.', 240, process)
        wait_gameplay(log, process)
        say(f'gameplay; {delay:.1f} s to the start, as in the recording')
        time.sleep(delay)
        seconds = 90 if kind == 'walk' else 30
        write_pad(pad, ('replay ' if kind == 'walk' else '') + f'measure={seconds}')
        log.wait('Perf: measurement started', 30, process)
        summary_line = log.wait('files=', seconds + 60, process)
        summary = Path(summary_line.split('=', 1)[1].replace('-*.csv', '-summary.txt'))
    except RuntimeError as error:
        say(f'FAILED: {error}')
        close(process, job)
        return None
    code = close(process, job)
    say(f'game closed (code {code})')
    report = summary.with_name(summary.name.replace('-summary.txt', '.md'))
    phases = '0-60:walking,60-91:standing' if kind == 'walk' else '0-31:standing'
    subprocess.run([sys.executable, str(ROOT / 'scripts' / 'perf_report.py'), str(summary), '--log', str(log.path),
                    '--title', f'Benchmark {kind} #{index}', '--phases', phases, '--out', str(report)],
                   stdout=subprocess.DEVNULL, check=False)
    values = {}
    for line in open(summary, encoding='utf-8', errors='replace'):
        if '=' in line and not line.startswith(('thread=', 'files=', 'gpu=')):
            key, value = line.strip().split('=', 1)
            try:
                values[key] = float(value)
            except ValueError:
                pass
        elif line.startswith('thread=') and 'top_thread' not in values:
            _tid, name, percent = line.strip()[7:].split('|')
            values['top_thread'], values['top_thread_pct'] = name, float(percent)
    values['exit_code'] = code
    values['report'] = str(report)
    return values


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('kind', choices=('record', 'walk', 'still'))
    ap.add_argument('--runs', type=int, default=1)
    a = ap.parse_args()
    if a.kind == 'record':
        record()
        return
    results = []
    for i in range(1, a.runs + 1):
        say(f'== {a.kind} run {i}/{a.runs} ==')
        values = run(a.kind, i)
        if values:
            results.append(values)
            say(f'   {values["fps_avg"]:.1f} FPS, 1% low {values["fps_low1"]:.1f}, p99 {values["frame_ms_p99"]:.1f} ms, '
                f'CPU {values["cpu_translate_ms_per_frame"]:.2f} ms, GPU {values["gpu_ms_per_frame"]:.2f} ms, '
                f'{values.get("top_thread", "?")} {values.get("top_thread_pct", 0):.0f}%, exit {values["exit_code"]}')
    keys = ('fps_avg', 'fps_low1', 'frame_ms_p99', 'cpu_translate_ms_per_frame', 'gpu_ms_per_frame', 'top_thread_pct')
    if len(results) > 1:
        print('\n| | ' + ' | '.join(keys) + ' |\n|---|' + '---|' * len(keys))
        for i, r in enumerate(results, 1):
            print(f'| run {i} | ' + ' | '.join(f'{r[k]:.2f}' for k in keys) + ' |')
        print('| mean | ' + ' | '.join(f'{statistics.mean(r[k] for r in results):.2f}' for k in keys) + ' |')
        print('| spread % | ' + ' | '.join(
            f'{100 * (max(r[k] for r in results) - min(r[k] for r in results)) / max(1e-9, statistics.mean(r[k] for r in results)):.1f}'
            for k in keys) + ' |')
    out = BENCH_DATA / 'logs' / time.strftime(f'bench-{a.kind}-%Y%m%d-%H%M%S.json')
    out.write_text(json.dumps(results, indent=1), encoding='utf-8')
    say(f'results: {out}')


if __name__ == '__main__':
    main()
