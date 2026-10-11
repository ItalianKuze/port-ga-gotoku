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
    for line in open(path, encoding='utf-8', errors='replace').readlines()[:60]:
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
        elif line.startswith('Runtime: gamepad connected:'):
            info['gamepad'] = line.split(':', 2)[2].strip()
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('summary')
    ap.add_argument('--log', help='the game log of the same run')
    ap.add_argument('--title', default='Measurement')
    ap.add_argument('--scene', default='', help='what was played during the measurement')
    ap.add_argument('--out')
    a = ap.parse_args()

    values, threads = read_summary(a.summary)
    base = a.summary[:-len('-summary.txt')]
    seconds = read_csv(base + '-seconds.csv')
    thread_rows = read_csv(base + '-threads.csv')
    info = log_header(a.log)

    def v(key, unit=''):
        value = values.get(key, '?')
        return f'{value}{unit}' if value not in ('?', '-1.000', '-1.0') else 'n/d'

    out = [f'# {a.title}', '']
    out += [f'- Data: {os.path.basename(base)[5:13]} · file: `{os.path.basename(base)}-*`',
            f'- Build: {info.get("build", "?")}',
            f'- Impostazioni: {info.get("settings", "?")}; presentazione effettiva: {info.get("present", "?")}',
            f'- GPU: {info.get("device", values.get("gpu", "?"))}',
            f'- Controller: {info.get("gamepad", "?")}',
            f'- Durata: {values.get("header", "?").replace("Perf summary: ", "")}']
    if a.scene:
        out += ['', '## Scena', '', a.scene]
    out += ['', '## Risultati', '',
            '| Misura | Valore |', '|---|---|',
            f'| FPS medio | {v("fps_avg")} |',
            f'| 1% low | {v("fps_low1")} FPS |',
            f'| 0,1% low | {v("fps_low01")} FPS |',
            f'| Tempo di frame p50 / p90 / p95 / p99 / p99,9 | {v("frame_ms_p50")} / {v("frame_ms_p90")} / '
            f'{v("frame_ms_p95")} / {v("frame_ms_p99")} / {v("frame_ms_p999")} ms |',
            f'| Frame più lungo | {v("frame_ms_max", " ms")} |',
            f'| Frame oltre 20 ms / oltre 34 ms | {v("frames_over_20ms")} / {v("frames_over_34ms")} |',
            f'| CPU di traduzione (thread GPU) per frame | {v("cpu_translate_ms_per_frame", " ms")} |',
            f'| GPU occupata per frame (timestamp query) | {v("gpu_ms_per_frame", " ms")} |',
            f'| Utilizzo GPU medio | {v("gpu_util_avg", " %")} |',
            f'| Utilizzo CPU di sistema medio | {v("cpu_util_avg", " %")} |',
            f'| Compilazioni di pipeline | {v("compiles")} |']
    out += ['', '## Thread (media sulla misura, % di un core)', '',
            '| Thread | TID | CPU |', '|---|---|---|']
    peaks = defaultdict(float)
    for row in thread_rows:
        try:
            peaks[int(row['tid'])] = max(peaks[int(row['tid'])], float(row['cpu_percent']))
        except (KeyError, ValueError):
            pass
    for name, tid, percent in threads[:12]:
        out.append(f'| {name} | {tid} | {percent:.1f} % (picco {peaks.get(tid, 0):.0f} %) |')
    saturated = [name for name, tid, percent in threads if percent >= 90.0]
    out += ['', 'Thread al 100% (media ≥ 90%): ' + (', '.join(saturated) if saturated else 'nessuno') + '.']
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
        out += ['', '## Andamento per secondo', '',
                f'FPS per secondo: min {min(fps):.1f}, mediana {statistics.median(fps):.1f}, max {max(fps):.1f} '
                f'({len(fps)} secondi). Dettaglio in `{os.path.basename(base)}-seconds.csv`.']
    text = '\n'.join(out) + '\n'
    if a.out:
        os.makedirs(os.path.dirname(a.out) or '.', exist_ok=True)
        open(a.out, 'w', encoding='utf-8').write(text)
    print(text)


if __name__ == '__main__':
    main()
