#!/usr/bin/env python3
"""The game files PortGaGotoku runs: Fist of the North Star: Lost Paradise, CUSA12781.

The check reads sce_sys/param.sfo: another TITLE_ID (another game, edition or region) is refused;
another APP_VER than the one tested (01.00) only gets a warning, as in the launcher.
PGG_SKIP_GAME_CHECK=1 (or bbport's BB_SKIP_GAME_CHECK=1) skips it.
"""
import os
from pathlib import Path

SUPPORTED_TITLE = 'CUSA12781'
TESTED_VERSION = '01.00'


def problem(game):
    """None for the supported game, else (kind, title, version): kind is 'other_title' or
    'unreadable' (no readable param.sfo)."""
    if (os.environ.get('PGG_SKIP_GAME_CHECK') or os.environ.get('BB_SKIP_GAME_CHECK')) == '1':
        return None
    from prepare import sfo  # same directory
    try:
        info = sfo((Path(game) / 'sce_sys/param.sfo').read_bytes())
    except (OSError, ValueError, IndexError):
        return 'unreadable', '?', '?'
    title, version = info.get('TITLE_ID', '?'), info.get('APP_VER', '?')
    if title != SUPPORTED_TITLE:
        return 'other_title', title, version
    return None


def warning(game):
    """A warning for a version other than the tested one, or None."""
    from prepare import sfo
    try:
        version = sfo((Path(game) / 'sce_sys/param.sfo').read_bytes()).get('APP_VER', '?')
    except (OSError, ValueError, IndexError):
        return None
    if version != TESTED_VERSION:
        return f'{SUPPORTED_TITLE} version {version}: only version {TESTED_VERSION} has been tested.'
    return None


def explain(kind, title, version):
    """What is wrong and what to do, for the log (English)."""
    return {
        'other_title': f'Found {title} version {version} (sce_sys/param.sfo). Only Fist of the North Star: '
                       f'Lost Paradise {SUPPORTED_TITLE} is supported: choose its game folder.',
        'unreadable': f'No readable sce_sys/param.sfo in the game folder: it is not a complete dump of '
                      f'{SUPPORTED_TITLE}.',
    }[kind]
