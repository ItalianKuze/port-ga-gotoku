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
    'Report a bug': 'Segnala un problema',
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
    '{} version {} (experimental, not supported)': '{} versione {} (sperimentale, non supportata)',
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
    'This folder holds {} ({}), not CUSA12781. If it is another edition of the game, you can try it as an experiment.':
        'Questa cartella contiene {} ({}), non CUSA12781. Se è un\'altra edizione del gioco, puoi provarla in via '
        'sperimentale.',
    '{} ({}) version {}: not supported, started as an experiment':
        '{} ({}) versione {}: non supportata, avvio sperimentale',
    'Try it anyway (experimental)': 'Prova comunque (sperimentale)',
    'Only CUSA12781 is supported. {} has never been tried: it may not start, or it may have errors. If you send a '
    'log, it tells which version this is.\n\nTry it anyway?':
        'È supportato solo CUSA12781. {} non è mai stato provato: potrebbe non avviarsi o avere errori. Se invii un '
        'log, indica quale versione è.\n\nProvare comunque?',
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
    # Saves panel
    'Slots': 'Slot',
    'Backups': 'Backup',
    'Import from PS4 or shadPS4…': 'Importa da PS4 o shadPS4…',
    'Made before the game writes a slot (the newest 10 per slot), before an import and before a restore.':
        'Fatti prima che il gioco scriva uno slot (gli ultimi 10 per slot), prima di un import e prima di un ripristino.',
    'Only with the game closed. Every change keeps a backup of the slot first.':
        'Solo a gioco chiuso. Ogni modifica tiene prima un backup dello slot.',
    'Confirm': 'Conferma',
    'Cancel': 'Annulla',
    '{} files': '{} file',
    'Export…': 'Esporta…',
    'Restore…': 'Ripristina…',
    'No backups yet': 'Ancora nessun backup',
    'Before the game saved': 'Prima di un salvataggio del gioco',
    'Before an import': 'Prima di un import',
    'Before a restore': 'Prima di un ripristino',
    'Close the game first: saves change only with the game closed.':
        'Chiudi prima il gioco: i salvataggi si modificano solo a gioco chiuso.',
    'Import into slot {}': 'Importa nello slot {}',
    'Restore slot {}': 'Ripristina lo slot {}',
    'Export slot {}': 'Esporta lo slot {}',
    'From: {}': 'Da: {}',
    'To: {}': 'In: {}',
    'Nothing is overwritten: the destination folder must not exist yet.':
        'Non viene sovrascritto nulla: la cartella di destinazione non deve esistere.',
    'The current slot is kept first in: {}': 'Lo slot attuale viene prima conservato in: {}',
    'Slot {} is empty: nothing to keep.': 'Lo slot {} è vuoto: nulla da conservare.',
    'Slot {} imported and verified.': 'Slot {} importato e verificato.',
    'Slot {} restored and verified.': 'Slot {} ripristinato e verificato.',
    'Slot {} exported and verified.': 'Slot {} esportato e verificato.',
    # Messages of pgg_main.py
    'Preparing the game failed. Log: {}': 'Preparazione del gioco non riuscita. Log: {}',
    'The game cannot start: {}. Extract the PortGaGotoku zip again; if an antivirus removed or blocked bb-probe.exe, restore it.':
        'Il gioco non può partire: {}. Estrai di nuovo lo zip di PortGaGotoku; se un antivirus ha rimosso o bloccato bb-probe.exe, ripristinalo.',
    'The game ended with code {}. Log: {}': 'Il gioco è terminato con codice {}. Log: {}',
    'Not enough memory to start the game: it reserves about 6 GB and Windows could not give them. Close other '
    'heavy applications, or enlarge the Windows page file, and try again.':
        'Memoria insufficiente per avviare il gioco: riserva circa 6 GB e Windows non è riuscito a darglieli. Chiudi '
        'le altre applicazioni pesanti, oppure aumenta il file di paging di Windows, e riprova.',
    'The game is still running. Close the launcher anyway?\n\nThe game stays open; its log is still written to the '
    'logs folder.':
        'Il gioco è ancora aperto. Chiudere comunque il launcher?\n\nIl gioco resta aperto; il suo log continua a '
        'essere scritto nella cartella dei log.',
    'Windows blocked the launcher\'s files because the zip was downloaded from the internet. Close this message, '
    'delete the extracted PortGaGotoku folder, right-click the zip → Properties → check Unblock → OK, and extract it '
    'again.':
        'Windows ha bloccato i file del launcher perché lo zip è stato scaricato da internet. Chiudi questo messaggio, '
        'elimina la cartella PortGaGotoku estratta, fai clic destro sullo zip → Proprietà → spunta Annulla blocco → '
        'OK, ed estrailo di nuovo.',
    'The launcher window needs Microsoft Edge WebView2 Runtime, which was not found. Install it from Microsoft\'s '
    'website.\n\n"Start Hokuto.exe" starts the game with the saved settings without this window.':
        'La finestra del launcher richiede Microsoft Edge WebView2 Runtime, che non è stato trovato. Installalo dal '
        'sito di Microsoft.\n\n"Start Hokuto.exe" avvia il gioco con le impostazioni salvate senza questa finestra.',
    'The launcher window could not be opened ({}).\n\nDetails: {}\n\n"Start Hokuto.exe" may still start the game '
    'with the saved settings.':
        'Non è stato possibile aprire la finestra del launcher ({}).\n\nDettagli: {}\n\n"Start Hokuto.exe" '
        'potrebbe comunque avviare il gioco con le impostazioni salvate.',
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
