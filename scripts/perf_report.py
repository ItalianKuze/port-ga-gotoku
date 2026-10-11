#!/usr/bin/env python3
"""PortGaGotoku: a performance measurement as a Markdown report (docs/perf/).

A measurement (F9 in the game, or the in-game menu) writes PortGaGotoku-data/logs/perf-<stamp>-
summary.txt, -frames.csv, -seconds.csv and -threads.csv (gpu/shim/bbport_perf.cpp). This script
reads them and the game log of the same run (its first lines name the build and the launcher's
settings) and writes the report:

    python3 scripts/perf_report.py <logs>/perf-<stamp>-summary.txt --log <logs>/hokuto-<stamp>.log \\
        --title "Baseline 0.4" --scene "..." --out docs/perf/baseline-0.4.md
"""
import argparse
import csv
import os
import statistics
from collections import defaultdict


def read_summary(path):
    values, threads = {}, []
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('thread='):
            tid, name, percent = line[len('thread='):].split('|')
            threads.append((name or f'tid {tid}', int(tid), float(percent)))
        elif '=' in line and not line.startswith('Perf summary'):
            key, value = line.split('=', 1)
            values[key] = value
        elif line.startswith('Perf summary'):
            values['header'] = line
    return values, threads


def read_csv(path):
    try:
        with open(path, encoding='utf-8', errors='replace', newline='') as f:
            return list(csv.DictReader(f))
    except OSError:
        return []


def log_header(path):
    """Build id and launcher settings from the game log's first lines."""
    info = {}
    if not path:
        return info
    for line in open(path, encoding='utf-8', errors='replace').readlines()[:2000]:
        if line.startswith('== PortGaGotoku launcher, build '):
            info['build'] = line.strip('= \n')[len('PortGaGotoku launcher, build '):]
        elif line.startswith('== present mode '):
            info['settings'] = line.strip('= \n')
        elif line.startswith('PortGaGotoku ') and 'build' not in info:
            info['build'] = line.split(' ', 1)[1].strip()
        elif line.startswith('GPU: Selected Vulkan device:'):
            info['device'] = line.split(':', 2)[2].strip()
        elif line.startswith('GPU: Present mode:'):
            info['present'] = line.split(':', 2)[2].strip()
        elif line.startswith('Runtime: gamepad connected:') and 'gamepad' not in info:
            info['gamepad'] = line.split(':', 2)[2].strip()
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('summary')
    ap.add_argument('--log', help='the game log of the same run')
    ap.add_argument('--title', default='Measurement')
    ap.add_argument('--scene', default='', help='what was played during the measurement')
    ap.add_argument('--phases', default='', help='parts of the run, e.g. "0-60:walking,60-90:standing"')
    ap.add_argument('--out')
    a = ap.parse_args()

    values, threads = read_summary(a.summary)
    base = a.summary[:-len('-summary.txt')]
    seconds = read_csv(base + '-seconds.csv')
    thread_rows = read_csv(base + '-threads.csv')
    info = log_header(a.log)

    def v(key, unit=''):
        value = values.get(key, '?')
        return f'{value}{unit}' if value not in ('?', '-1.000', '-1.0') else 'n/a'

    out = [f'# {a.title}', '']
    out += [f'- Date: {os.path.basename(base)[5:13]} · files: `{os.path.basename(base)}-*`',
            f'- Build: {info.get("build", "?")}',
            f'- Settings: {info.get("settings", "?")}; present mode in use: {info.get("present", "?")}',
            f'- GPU: {info.get("device", values.get("gpu", "?"))}',
            f'- Controller: {info.get("gamepad", "?")}',
            f'- Length: {values.get("header", "?").replace("Perf summary: ", "")}']
    if a.scene:
        out += ['', '## Scene', '', a.scene]
    out += ['', '## Results', '',
            '| Measure | Value |', '|---|---|',
            f'| Average FPS | {v("fps_avg")} |',
            f'| 1% low | {v("fps_low1")} FPS |',
            f'| 0.1% low | {v("fps_low01")} FPS |',
            f'| Frame time p50 / p90 / p95 / p99 / p99.9 | {v("frame_ms_p50")} / {v("frame_ms_p90")} / '
            f'{v("frame_ms_p95")} / {v("frame_ms_p99")} / {v("frame_ms_p999")} ms |',
            f'| Longest frame | {v("frame_ms_max", " ms")} |',
            f'| Frames over 20 ms / over 34 ms | {v("frames_over_20ms")} / {v("frames_over_34ms")} |',
            f'| CPU translation (GPU thread) per frame | {v("cpu_translate_ms_per_frame", " ms")} |',
            f'| GPU busy per frame (timestamp queries) | {v("gpu_ms_per_frame", " ms")} |',
            f'| Average GPU utilisation | {v("gpu_util_avg", " %")} |',
            f'| Average system CPU | {v("cpu_util_avg", " %")} |',
            f'| Pipeline compiles | {v("compiles")} |']
    out += ['', '## Threads (average over the measurement, % of one core)', '',
            '| Thread | TID | CPU |', '|---|---|---|']
    peaks = defaultdict(float)
    for row in thread_rows:
        try:
            peaks[int(row['tid'])] = max(peaks[int(row['tid'])], float(row['cpu_percent']))
        except (KeyError, ValueError):
            pass
    for name, tid, percent in threads[:12]:
        out.append(f'| {name} | {tid} | {percent:.1f} % (peak {peaks.get(tid, 0):.0f} %) |')
    busy = [f'{name} ({percent:.0f}%, peak {peaks.get(tid, 0):.0f}%)' for name, tid, percent in threads if percent >= 80.0]
    out += ['', 'Threads near a full core (average ≥ 80%): ' + (', '.join(busy) if busy else 'none') + '.']
    if a.phases:
        frames = [(float(r['t_s']), float(r['frame_ms'])) for r in read_csv(base + '-frames.csv')]
        out += ['', '## Phases', '', '| Phase | Frames | Average FPS | 1% low | p50 | p99 | over 20 ms |',
                '|---|---|---|---|---|---|---|']
        for part in a.phases.split(','):
            span, name = part.split(':', 1)
            lo, hi = (float(x) for x in span.split('-'))
            ms = sorted((m for t, m in frames if lo <= t < hi), reverse=True)
            if not ms:
                continue
            worst = ms[:max(1, len(ms) // 100)]
            asc = sorted(ms)
            out.append(f'| {name} ({span} s) | {len(ms)} | {1000 * len(ms) / sum(ms):.1f} | '
                       f'{1000 * len(worst) / sum(worst):.1f} | {asc[len(asc) // 2]:.2f} ms | '
                       f'{asc[min(len(asc) - 1, int(len(asc) * 0.99))]:.2f} ms | {sum(m > 20 for m in ms)} |')
    if seconds:
        def column(key):
            result = []
            for row in seconds:
                try:
                    result.append(float(row[key]))
                except (KeyError, ValueError):
                    pass
            return result
        fps = column('fps')
        out += ['', '## Per second', '',
                f'FPS per second: min {min(fps):.1f}, median {statistics.median(fps):.1f}, max {max(fps):.1f} '
                f'({len(fps)} seconds). Details in `{os.path.basename(base)}-seconds.csv`.']
    text = '\n'.join(out) + '\n'
    if a.out:
        os.makedirs(os.path.dirname(a.out) or '.', exist_ok=True)
        open(a.out, 'w', encoding='utf-8').write(text)
    print(text)


if __name__ == '__main__':
    main()
