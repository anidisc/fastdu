# fastdu

Un visualizzatore TUI (ncurses) veloce per l’uso del disco, scritto in C, con scansione parallela e cache su file.

Caratteristiche
- Scansione parallela: usa un work-queue profondo con più thread (pthreads)
- TUI reattiva: elenco ordinabile per dimensione o nome, ricerca incrementale, filtri (tutti/dir/file)
- Cache persistente: salva risultati di scansione in .fastdu_cache_v2 al root per riusi successivi
- Aggiornamento mirato: riscan automatico della directory selezionata se rilevate modifiche (mtime)
- Operazioni: selezione multipla (mark), spostamento e cancellazione con aggiornamento cache incrementale

Requisiti
- gcc (o un compilatore C11 compatibile)
- ncurses con supporto wide-char (linkato come -lncursesw)
- pthreads

Installazione (Fedora/RHEL)
- sudo dnf install gcc make ncurses-devel

Build
- make

Esecuzione
- ./fastdu [opzioni] [percorso]

Opzioni
- -R, --reload    forza la ricostruzione del cache
- -j N, --jobs N  imposta il numero di thread (default: CPU online, max 64)

Tasti TUI (principali)
- Navigazione: Frecce o j/k su/giù, Enter/Right l per entrare, Backspace/Left per uscire
- Vista: o cambia chiave sort (size/name), s cambia ordine (asc/desc)
- Filtri: t ruota all/dirs/files, T abilita/disabilita filtro per query
- Ricerca: f avvia ricerca, n/N successivo/precedente
- Rescan: r riscan directory selezionata, R riscan directory corrente
- Selezione multipla: Space marca/smarca; m sposta marcati nella dir corrente
- Eliminazione: d elimina marcati (se presenti) o l’elemento selezionato
- Aiuto/uscita: h mostra aiuto, q esce

Cache
- Il file .fastdu_cache_v2 è scritto nella directory root scansionata.
- Ogni entry contiene percorso relativo (percent-encoded), dimensione, timestamp di scansione, inode e mtime.
- Il cache viene invalidato/aggiornato quando vengono rilevate modifiche (mtime diverso) o a seguito di operazioni (move/delete).

Note di progettazione
- Sicurezza FS: non segue symlink (usa O_NOFOLLOW/fstatat) ed evita di includere il file di cache nel conteggio.
- Concorrenza: usa mutex per proteggere il cache e contatori atomici per progress/metriche.
- UI: disegno throttled della barra di avanzamento per ridurre flicker.

Struttura del codice
- fastdu.c    sorgente principale (util, cache, scanner, TUI, work-queue)
- Makefile    regole di build (gcc -O2 -Wall -Wextra -std=c11 -lncursesw -lpthread)

Esempi
- Avvio sul percorso corrente: ./fastdu
- Forza rescan con 8 thread: ./fastdu -R -j 8 /percorso/da/scansionare

Limitazioni note
- Non considera hard link multipli come deduplicabili tra directory diverse.
- Dimensioni mostrate sono la somma delle dimensioni dei file regolari (non il disk usage su blocchi).

License
- TBD (inserisci la licenza preferita, ad es. MIT)
