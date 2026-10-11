# SPDX-License-Identifier: GPL-2.0-or-later
"""Translations of the PortGaGotoku launcher (the scheme of Supermedo's launcher: every text is
written in English in the code and looked up here by that text; a missing entry shows English).
tests/test_launcher.py checks that every text of pgg_launcher_win.py has an Italian entry."""
import ctypes
import os

LANG = 'en'

IT = {
    # Side bar, pages, buttons
    'Play': 'Gioca',
    'Display': 'Schermo',
    'Statistics': 'Statistiche',
    'Folders': 'Cartelle',
    'Log': 'Log',
    'PLAY': 'GIOCA',
    'Stop': 'Ferma',
    'Browse…': 'Sfoglia…',
    'Open': 'Apri',
    'Copy': 'Copia',
    'Open the logs folder': 'Apri la cartella dei log',
    'Launcher language': 'Lingua del launcher',
    'Windows language': 'Come Windows',
    'Applies when the launcher is opened again': 'Vale alla prossima apertura del launcher',
    'Build {}': 'Build {}',
    # Play page
    'Ready check': 'Controllo',
    '✓ Game found: {} (CUSA12781), version {}': '✓ Gioco trovato: {} (CUSA12781), versione {}',
    '⚠ CUSA12781 version {}: only version {} has been tried':
        '⚠ CUSA12781 versione {}: è stata provata solo la versione {}',
    '✗ Game folder not chosen: choose the CUSA12781 folder with eboot.bin':
        '✗ Cartella del gioco non scelta: scegli la cartella CUSA12781 con eboot.bin',
    '✗ The game folder does not exist any more: choose it again':
        '✗ La cartella del gioco non esiste più: sceglila di nuovo',
    '✗ No eboot.bin in this folder: it is not the game folder (CUSA12781)':
        '✗ In questa cartella non c\'è eboot.bin: non è la cartella del gioco (CUSA12781)',
    '✗ No readable sce_sys\\param.sfo in this folder: it is not a complete PS4 dump':
        '✗ In questa cartella manca sce_sys\\param.sfo leggibile: non è un dump PS4 completo',
    '✗ This folder holds {} ("{}"), not CUSA12781: choose the Fist of the North Star: Lost Paradise folder':
        '✗ Questa cartella contiene {} ("{}"), non CUSA12781: scegli la cartella di Fist of the North Star: '
        'Lost Paradise',
    '✓ Boot image ready': '✓ Immagine di avvio pronta',
    '• The boot image is prepared at the next start (a few seconds; the game files are only read)':
        '• L\'immagine di avvio viene preparata al prossimo avvio (pochi secondi; i file del gioco sono solo letti)',
    '• Graphics card: checking…': '• Scheda video: controllo in corso…',
    '• Controller: checking…': '• Controller: controllo in corso…',
    '✗ No Vulkan graphics card found (update the driver)':
        '✗ Nessuna scheda video Vulkan trovata (aggiorna il driver)',
    '⚠ Graphics card "{}" not found: the game chooses automatically':
        '⚠ Scheda video "{}" non trovata: il gioco sceglie da solo',
    '✓ Graphics card: {}{}': '✓ Scheda video: {}{}',
    ' (automatic)': ' (automatica)',
    '✓ Controller: {}': '✓ Controller: {}',
    '• No controller found when the launcher started (keyboard works; a controller connected later is taken by '
    'the game)':
        '• Nessun controller trovato all\'apertura del launcher (la tastiera funziona; un controller collegato '
        'dopo viene preso dal gioco)',
    '✓ Saves: slots {}': '✓ Salvataggi: slot {}',
    '• No saves yet: the game creates them in {}': '• Ancora nessun salvataggio: il gioco li crea in {}',
    '✓ Last game: closed normally': '✓ Ultima partita: chiusa normalmente',
    '✗ Last game: ended with code {} (see the Log page)':
        '✗ Ultima partita: terminata con codice {} (vedi la pagina Log)',
    'In the game: Insert or L3+R3 opens the port menu. The window closes with its X (Alt+F4 in fullscreen): a '
    'save in progress is finished first.':
        'Nel gioco: Insert o L3+R3 apre il menu del port. La finestra si chiude con la X (Alt+F4 a schermo '
        'intero): un salvataggio in corso viene prima completato.',
    'Choose the game folder (CUSA12781, with eboot.bin)': 'Scegli la cartella del gioco (CUSA12781, con eboot.bin)',
    'Chosen folder: {}': 'Cartella scelta: {}',
    # Display page
    'Read when the game starts.': 'Valgono dal prossimo avvio del gioco.',
    'Present mode': 'Modalità di presentazione',
    'Mailbox (recommended)': 'Mailbox (consigliata)',
    'Fifo (classic vsync)': 'Fifo (vsync classico)',
    'Immediate (no vsync, may tear)': 'Immediate (senza vsync, può strappare l\'immagine)',
    'All three were tried on Hokuto: same frame times (60 FPS standing, the drops while walking come from the '
    'frame work, not from the presentation).':
        'Provate tutte e tre su Hokuto: stessi tempi dei frame (60 FPS da fermi; i cali in cammino dipendono dal '
        'lavoro del frame, non dalla presentazione).',
    'Fullscreen': 'Schermo intero',
    'Alt+F4 closes the game (there is no X in fullscreen). Alt+Tab has not been tried yet.':
        'Alt+F4 chiude il gioco (a schermo intero non c\'è la X). Alt+Tab non è ancora stato provato.',
    'Graphics card': 'Scheda video',
    'Automatic (discrete GPU preferred)': 'Automatica (preferita la GPU dedicata)',
    'The list comes from Vulkan when the launcher starts.': 'L\'elenco viene da Vulkan all\'apertura del launcher.',
    'Output resolution and the graphics options of the port menu are not here: on Hokuto they have no effect yet '
    '(they will come with a graphics profile for Hokuto).':
        'La risoluzione di uscita e le opzioni grafiche del menu del port non sono qui: su Hokuto per ora non hanno '
        'effetto (arriveranno con un profilo grafico per Hokuto).',
    # Statistics page
    'Measurements for the performance work.': 'Misure per il lavoro sulle prestazioni.',
    'Frame statistics in the log': 'Statistiche dei frame nel log',
    'Every few seconds: FPS, worst frame, shader compiles (lines "Frame stats:" in the log).':
        'Ogni pochi secondi: FPS, frame peggiore, compilazioni di shader (righe "Frame stats:" nel log).',
    'On-screen statistics (HUD) will come in the next phase.': 'Le statistiche a schermo (HUD) arrivano nella prossima fase.',
    # Folders page
    'Everything the port writes stays in the data folder.': 'Tutto ciò che il port scrive resta nella cartella dati.',
    'Game folder ({})': 'Cartella del gioco ({})',
    'The folder of the dump with eboot.bin and sce_sys. It is only read.':
        'La cartella del dump con eboot.bin e sce_sys. Viene solo letta.',
    'Data folder': 'Cartella dati',
    'Saves': 'Salvataggi',
    'Save backups': 'Backup dei salvataggi',
    'Logs': 'Log',
    'Shader cache': 'Cache degli shader',
    'Settings: {}. The data folder is PortGaGotoku-data next to the program folder (BB_DATA_DIR changes it).':
        'Impostazioni: {}. La cartella dati è PortGaGotoku-data accanto alla cartella del programma (BB_DATA_DIR '
        'la cambia).',
    # Status bar and messages
    '{}, {}, frame statistics {}': '{}, {}, statistiche dei frame {}',
    'fullscreen': 'schermo intero',
    'window': 'finestra',
    'on': 'sì',
    'off': 'no',
    'Choose the game folder first.': 'Prima scegli la cartella del gioco.',
    'Could not save the settings: {}': 'Impossibile salvare le impostazioni: {}',
    'bb-probe.exe is already running (PID {}). Two games on the same saves can damage them.\n\nStart anyway?':
        'bb-probe.exe è già in esecuzione (PID {}). Due giochi sugli stessi salvataggi possono rovinarli.\n\n'
        'Avviare comunque?',
    'Preparing the boot image…': 'Preparazione dell\'immagine di avvio…',
    'Preparing the boot image failed: see the log ({}).': 'Preparazione dell\'immagine di avvio non riuscita: vedi il log ({}).',
    'Preparing the game failed. Log: {}': 'Preparazione del gioco non riuscita. Log: {}',
    'Could not start the game: {}': 'Impossibile avviare il gioco: {}',
    'The game is running: it opens in its own window.': 'Il gioco è in esecuzione: si apre nella sua finestra.',
    'Closing the game (a save in progress is finished first)…':
        'Chiusura del gioco (un salvataggio in corso viene prima completato)…',
    '\n— the game ended (code {}) —\n': '\n— il gioco è terminato (codice {}) —\n',
    'The game was closed normally.': 'Il gioco è stato chiuso normalmente.',
    'The game was stopped from the launcher (code {}).': 'Il gioco è stato fermato dal launcher (codice {}).',
    'The game ended with code {}: the end of the log is shown.':
        'Il gioco è terminato con codice {}: ecco la fine del log.',
    'The game ended with code {}. Log: {}': 'Il gioco è terminato con codice {}. Log: {}',
    'The game is still running. Close the launcher anyway?\n\nThe game stays open; its log is still written to the '
    'logs folder.':
        'Il gioco è ancora aperto. Chiudere comunque il launcher?\n\nIl gioco resta aperto; il suo log continua a '
        'essere scritto nella cartella dei log.',
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
