# PortGaGotoku

[English](README.md) · **Italiano**

PortGaGotoku fa girare *Fist of the North Star: Lost Paradise* (*Hokuto ga Gotoku*, PlayStation 4,
CUSA12781) in modo nativo su Windows, "come un port". È un fork di
[bbport](https://github.com/deadinside28/bloodborne_pc), il port nativo per PC di Bloodborne di
deadinside28: il codice x86-64 del gioco gira direttamente sulla CPU, un runtime sostituisce le librerie
di sistema della PS4 e la grafica viene tradotta in Vulkan da un renderer derivato da
[shadPS4](https://github.com/shadps4-emu/shadPS4).

> **Nessun file del gioco è incluso.** Serve un proprio dump legale del gioco (la cartella CUSA12781 con
> `eboot.bin` e `sce_sys`). Progetto non ufficiale, non affiliato né approvato da SEGA, Ryu Ga Gotoku
> Studio, Sony Interactive Entertainment o dai detentori dei diritti di Fist of the North Star.

## Stato

Sperimentale, giocabile sull'unica macchina su cui è stato provato (Windows 11, NVIDIA GeForce RTX
5070, controller 8BitDo in modalità XInput).

Funziona: loghi, schermata del titolo e menu, audio, filmati, gameplay (esplorazione e combattimento),
controller e tastiera, salvataggio e caricamento (Continua), il launcher, il menu in gioco, l'HUD delle
statistiche. Problemi noti e prossimi passi: [docs/STATUS_AND_ROADMAP.md](docs/STATUS_AND_ROADMAP.md),
in inglese. In breve: in cammino nelle zone affollate il frame rate scende sotto i 60 FPS (il thread
dei comandi GPU è al limite della CPU), uno sfarfallio sul cappuccio dei soldati alla porta di Eden, e
una sola macchina provata.

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
