# PortGaGotoku

[English](README.md) · **Italiano**

> **Nota:** "nativo" si riferisce solo alla CPU: il codice x86 del gioco gira direttamente sulla CPU,
> mentre le librerie di sistema PS4 sono reimplementate (HLE). La grafica è tradotta in Vulkan tramite il
> video core di shadPS4 incluso nel repo (`gpu/shadps4`). Non è una ricompilazione completa né una
> riscrittura del renderer.

PortGaGotoku fa girare *Fist of the North Star: Lost Paradise* (*Hokuto ga Gotoku*, PlayStation 4,
CUSA12781) su Windows, "come un port". È un fork di
[bbport](https://github.com/deadinside28/bloodborne_pc), il port nativo per PC di Bloodborne di
deadinside28: il codice x86-64 del gioco gira direttamente sulla CPU, un runtime sostituisce le librerie
di sistema della PS4 e la grafica viene tradotta in Vulkan da un renderer derivato da
[shadPS4](https://github.com/shadps4-emu/shadPS4).

> **Nessun file del gioco è incluso.** Serve un proprio dump legale del gioco (la cartella CUSA12781 con
> `eboot.bin` e `sce_sys`). Progetto non ufficiale, non affiliato né approvato da SEGA, Ryu Ga Gotoku
> Studio, Sony Interactive Entertainment o dai detentori dei diritti di Fist of the North Star.

## Stato

**Early alpha, sviluppato lentamente nel tempo libero.** Aspettati bug; nessuna data di rilascio.

Sperimentale, giocabile sull'unica macchina su cui è stato provato (Windows 11, NVIDIA GeForce RTX
5070, controller 8BitDo in modalità XInput).

Funziona: loghi, schermata del titolo e menu, audio, filmati, gameplay (esplorazione e combattimento),
controller e tastiera, salvataggio e caricamento (Continua), il launcher, il menu in gioco, l'HUD delle
statistiche. Problemi noti e prossimi passi: [docs/STATUS_AND_ROADMAP.md](docs/STATUS_AND_ROADMAP.md),
in inglese. In breve: in cammino circa 60 FPS, con cali occasionali; uno sfarfallio sul cappuccio dei
soldati alla porta di Eden (non verificato); una sola macchina provata.

## Download

1. Scarica lo `.zip` dell'ultima release da [Releases](https://github.com/ItalianKuze/port-ga-gotoku/releases)
   ed estrailo in una cartella in cui puoi scrivere (non `Programmi`).
2. Controlla i [requisiti](README.md#requirements) (Windows 10/11 a 64 bit, CPU con AVX, BMI1, MOVBE,
   LZCNT e POPCNT, scheda video Vulkan 1.3, WebView2) e tieni pronto il tuo dump del gioco.
3. Avvia `PortGaGotoku.exe`. Al primo avvio chiede la cartella del gioco: scegli la cartella CUSA12781
   (quella con `eboot.bin` e `sce_sys`). Il launcher la controlla, prepara l'immagine di avvio (i file
   del gioco vengono solo letti) e avvia il gioco. In seguito `Start Hokuto.exe` avvia direttamente il
   gioco con le impostazioni salvate.
4. Gli eseguibili non sono firmati e il launcher è impacchettato con PyInstaller: SmartScreen o un
   antivirus potrebbero segnalarli.

I log sono in `PortGaGotoku-data\logs\`, accanto alla cartella estratta. Per segnalare un problema apri
una [issue](https://github.com/ItalianKuze/port-ga-gotoku/issues/new/choose) con il modulo di
segnalazione e allega il log della sessione. I log possono contenere il tuo nome utente di Windows (nei
percorsi dei file): puoi sostituirlo prima di allegarli.

## Requisiti, compilazione e avvio

Come nel [README in inglese](README.md#requirements) e in [docs/BUILDING.md](docs/BUILDING.md). Il
launcher è in inglese e in italiano (selettore della lingua nelle Impostazioni); il menu in gioco anche
(Insert o L3+R3). F11: HUD delle statistiche. Per uscire: la X della finestra (Alt+F4 a schermo intero);
un salvataggio in corso viene prima completato.

Tutto ciò che il port scrive va in `PortGaGotoku-data`, accanto alla cartella del progetto o del
pacchetto: impostazioni, immagine di avvio, salvataggi (`user/savedata/1/CUSA12776`), backup dei
salvataggi, log, cache degli shader.

## Sviluppo, crediti, licenza

Vedi il [README in inglese](README.md#development). Il progetto è stato sviluppato con l'aiuto di un
assistente di programmazione basato su IA, sotto la direzione e con le prove del suo curatore.
Licenza: GNU GPL v2 o successiva ([LICENSE](LICENSE)).
