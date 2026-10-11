# SPDX-License-Identifier: GPL-2.0-or-later
"""Translations of the PortGaGotoku launcher (the scheme of Supermedo's launcher: every text is
written in English in the code and looked up here by that text; a missing entry shows English).
The window (launcher/ui) receives the table through the bridge. tests/test_launcher.py checks that
every text of pgg_main.py, ui/app.js and ui/index.html has an Italian entry, and no entry is unused."""
import ctypes
import os

LANG = 'en'

IT = {
    # Top bar, folders menu
    'Log': 'Log',
    'Folders': 'Cartelle',
    'Settings': 'Impostazioni',
    'Close': 'Chiudi',
    'Port data': 'Dati del port',
    'Saves': 'Salvataggi',
    'Save backups': 'Backup dei salvataggi',
    'Logs': 'Log',
    'Shader cache': 'Cache degli shader',
    'Game folder': 'Cartella del gioco',
    # Home
    'Play': 'Gioca',
    'Play again': 'Gioca di nuovo',
    'Preparing…': 'Preparazione…',
    'Running': 'In esecuzione',
    'Closing…': 'Chiusura…',
    'Stop': 'Ferma',
    'Fullscreen': 'Schermo intero',
    'Window': 'Finestra',
    'Game ready · {} v{}': 'Gioco pronto · {} v{}',
    '{} version {} (not tried)': '{} versione {} (non provata)',
    'Controller: checking…': 'Controller: controllo…',
    'Graphics card: checking…': 'Scheda video: controllo…',
    'No controller (the keyboard works)': 'Nessun controller (la tastiera funziona)',
    'No Vulkan graphics card': 'Nessuna scheda video Vulkan',
    '{} not found': '{} non trovata',
    '1 save': '1 salvataggio',
    '{} saves': '{} salvataggi',
    'No saves yet': 'Ancora nessun salvataggio',
    'Preparing the boot image (a few seconds)…': 'Preparazione dell\'immagine di avvio (pochi secondi)…',
    'The game is running in its own window. In the game, Insert or L3+R3 opens the port menu.':
        'Il gioco è aperto nella sua finestra. Nel gioco, Insert o L3+R3 apre il menu del port.',
    'Closing the game: a save in progress is finished first…':
        'Chiusura del gioco: un salvataggio in corso viene prima completato…',
    'Last game: closed normally.': 'Ultima partita: chiusa normalmente.',
    'Last game: stopped from the launcher (code {}).': 'Ultima partita: fermata dal launcher (codice {}).',
    'Last game: ended with an error (code {}).': 'Ultima partita: terminata con un errore (codice {}).',
    'At the first start the boot image is prepared (a few seconds).':
        'Al primo avvio viene preparata l\'immagine di avvio (pochi secondi).',
    'bb-probe.exe is already running (PID {}). Two games on the same saves can damage them.\n\nStart anyway?':
        'bb-probe.exe è già in esecuzione (PID {}). Due giochi sugli stessi salvataggi possono rovinarli.\n\n'
        'Avviare comunque?',
    # Welcome
    'Welcome': 'Benvenuto',
    'To play Fist of the North Star: Lost Paradise you need your own dump of the game: the folder ':
        'Per giocare a Fist of the North Star: Lost Paradise serve il tuo dump del gioco: la cartella ',
    ' with eboot.bin and sce_sys.': ' con eboot.bin e sce_sys.',
    'Browse…': 'Sfoglia…',
    'Continue': 'Continua',
    'The game files are only read.': 'I file del gioco vengono solo letti.',
    'No folder chosen': 'Nessuna cartella scelta',
    'No folder chosen yet': 'Nessuna cartella scelta',
    'Found: {} ({}), version {}': 'Trovato: {} ({}), versione {}',
    'Found {} version {}: only version {} has been tried': 'Trovato {} versione {}: è stata provata solo la versione {}',
    'The game folder does not exist any more: choose it again': 'La cartella del gioco non esiste più: sceglila di nuovo',
    'No eboot.bin in this folder: it is not the game folder':
        'In questa cartella non c\'è eboot.bin: non è la cartella del gioco',
    'No readable sce_sys\\param.sfo in this folder: it is not a complete PS4 dump':
        'In questa cartella manca sce_sys\\param.sfo leggibile: non è un dump PS4 completo',
    'This folder holds {}, not the game. Choose the CUSA12781 folder of the dump.':
        'Questa cartella contiene {}, non il gioco. Scegli la cartella CUSA12781 del dump.',
    # Settings panel
    'Display': 'Schermo',
    'Presentation': 'Presentazione',
    'Mailbox is recommended. Fifo: classic vsync. Immediate: no vsync. All three were tried on Hokuto.':
        'Mailbox è consigliata. Fifo: vsync classico. Immediate: senza vsync. Provate tutte e tre su Hokuto.',
    'Alt+F4 closes the game. Alt+Tab has not been tried yet.': 'Alt+F4 chiude il gioco. Alt+Tab non è ancora stato provato.',
    'Graphics card': 'Scheda video',
    'Automatic': 'Automatica',
    'Automatic ({})': 'Automatica ({})',
    'Performance': 'Prestazioni',
    'Frame statistics in the log': 'Statistiche dei frame nel log',
    'On-screen statistics (HUD) will come later.': 'Le statistiche a schermo (HUD) arriveranno più avanti.',
    'Game and data': 'Gioco e dati',
    'Change…': 'Cambia…',
    'Saves, save backups, logs, shader cache': 'Salvataggi, backup, log, cache degli shader',
    'Open folder': 'Apri cartella',
    'Language': 'Lingua',
    'Windows language': 'Come Windows',
    'Settings apply from the next start of the game': 'Valgono dal prossimo avvio del gioco',
    # Log drawer
    'Copy': 'Copia',
    'Open logs folder': 'Apri cartella log',
    'Close the log': 'Chiudi il log',
    'The game closed with an error (code {})': 'Il gioco si è chiuso con un errore (codice {})',
    'No game started from this window yet': 'Nessuna partita avviata da questa finestra',
    # Messages of pgg_main.py
    'Preparing the game failed. Log: {}': 'Preparazione del gioco non riuscita. Log: {}',
    'The game ended with code {}. Log: {}': 'Il gioco è terminato con codice {}. Log: {}',
    'The game is still running. Close the launcher anyway?\n\nThe game stays open; its log is still written to the '
    'logs folder.':
        'Il gioco è ancora aperto. Chiudere comunque il launcher?\n\nIl gioco resta aperto; il suo log continua a '
        'essere scritto nella cartella dei log.',
    'The launcher window needs Microsoft Edge WebView2 Runtime ({}).\n\n"Gioca Hokuto.exe" starts the game with the '
    'saved settings without this window.':
        'La finestra del launcher richiede Microsoft Edge WebView2 Runtime ({}).\n\n"Gioca Hokuto.exe" avvia il '
        'gioco con le impostazioni salvate senza questa finestra.',
}

TABLES = {'it': IT}


def tr(text):
    return TABLES.get(LANG, {}).get(text) or text


def windows_language():
    """'it' when Windows' display language is Italian, else 'en'."""
    try:
        lang = ctypes.windll.kernel32.GetUserDefaultUILanguage()
        return 'it' if lang & 0x3ff == 0x10 else 'en'  # LANG_ITALIAN
    except (AttributeError, OSError):
        return 'it' if os.environ.get('LANG', '').startswith('it') else 'en'
