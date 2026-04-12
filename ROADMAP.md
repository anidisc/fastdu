# Roadmap di Sviluppo: fastdu

Questo documento delinea le proposte per l'introduzione di nuove funzionalità e le correzioni necessarie per rendere `fastdu` più robusto, preciso e versatile.

## 🚀 Nuove Funzionalità (Feature Request)

### 1. Sistema di Esclusione Avanzato
Attualmente il tool scansiona tutto ciò che incontra (tranne i symlink).
- [x] **Supporto file `.fastduignore`:** Implementare la lettura di un file di configurazione (stile `.gitignore`) per saltare directory pesanti ma irrilevanti (es. `node_modules`, `.git`, `cache`).
- [x] **Flag `--exclude`:** Aggiungere un'opzione da riga di comando per escludere pattern specifici durante la scansione.

### 2. Visualizzazione Grafica (TUI Enhancements)
- [x] **Barre di occupazione:** Aggiungere una colonna visuale con barre di caratteri (es. `[####------]`) per mostrare proporzionalmente l'ingombro di una cartella rispetto alla parent.
- [x] **Distribuzione per estensione:** Una vista dedicata (attivabile con un tasto) che mostri quali tipi di file (es. `.mp4`, `.log`, `.zip`) occupano più spazio nel path corrente.

### 3. Esportazione Dati
- [x] **Export JSON/CSV:** Permettere il salvataggio dei risultati della scansione in formati strutturati per analisi esterne o reportistica.
- [x] **Snapshot Comparison:** Funzione per confrontare la cache attuale con un riferimento (da file o istantaneo in memoria) per mostrare variazioni di spazio.
### 4. Integrazione di Sistema
- [x] **Apertura file esterna:** Aggiungere un comando (es. `o`) per aprire il file selezionato con l'applicazione predefinita del sistema (usando `xdg-open` su Linux).
- [x] **Integrazione Editor Esterno (Ctrl+E):** Permettere l'apertura e la modifica di file testuali tramite un editor esterno (es. vim, nano, nvim).
- [x] **Compressione ZIP (z):** Aggiungere la possibilità di comprimere file e cartelle in un archivio .zip direttamente dalla TUI, con supporto per rinomina e gestione conflitti.
- [x] **Estrazione Archivi (x):** Scompattare archivi (.zip, .tar, etc.) in una cartella o nella directory corrente con gestione conflitti per-file.
- [x] **Rinomina Elementi (ALT+r):** Rinominare file e directory con pre-caricamento del nome e gestione conflitti.
- [x] **Accesso alla Shell (Ctrl+S):** Uscire temporaneamente alla shell nella cartella corrente e tornare al programma con 'exit'.
- [x] **Supporto Mount-points:** Opzione per limitare la scansione al filesystem corrente (`-x` / `--one-file-system`).

### 5. UX & Personalizzazione (Modernizzazione TUI)
- [x] **Supporto Mouse:** Scrolling, selezione e navigazione tramite click.
- [x] **Editor di Linea Avanzato:** Tutti i prompt di input ora supportano lo spostamento del cursore (frecce), Home/End e inserimento.
- [x] **Breadcrumbs Navigabili:** Percorso cliccabile per navigazione rapida ai genitori.
...
### 9. Gestione File Avanzata (Next Goals)
- [x] **Creazione Rapida (Ctrl+n / ALT+n):** Tasto `Ctrl+n` per creare una nuova cartella (`mkdir`) e `ALT+n` per un nuovo file vuoto (`touch`).
- [x] **Ricerca Globale Istantanea:** Sfruttare la cache per cercare file in tutto l'albero scansionato, non solo nella cartella corrente.
- [x] **Ordinamento Avanzato (o):** Cicliare l'ordinamento tra Dimensione, Nome, Data di modifica ed Estensione.

- [x] **Supporto Nerd Fonts:** Opzione per mostrare icone specifiche per tipo di file (es. cartella, immagine, sorgente C) per una visualizzazione più moderna.
- [x] **File di Configurazione:** Permettere la personalizzazione di colori e tasti tramite un file `~/.config/fastdu/config.toml`.

### 10. Funzionalità Remote (Client/Server) - v0.70.2
- [x] **Modalità Server (`--server`):** Implementare un'interfaccia di comunicazione su `stdin`/`stdout` per permettere la scansione remota guidata da un client.
- [x] **Integrazione SSH:** Supporto per percorsi remoti (es. `user@host:/path`) avviando automaticamente il server remoto via SSH.
- [x] **Streaming della Cache:** Serializzazione efficiente e compressione dei dati di scansione per minimizzare la latenza di rete.
- [ ] **Operazioni Remote:** Permettere cancellazione, rinomina e spostamento di file sul server remoto tramite comandi inviati dal client.

### 6. Motore & Performance (Ottimizzazione Core)
- [x] **Integrazione `io_uring`:** Su Linux, utilizzare `io_uring` per le operazioni di `stat` e `openat` per ridurre l'overhead delle syscall durante la scansione parallela.
- [ ] **Scansione Differenziale Intelligente:** Implementare un sistema che monitora i cambiamenti del filesystem in background o verifica solo i path con `mtime` modificata rispetto alla cache.
- [ ] **Log Errori Permessi:** Creare una vista dedicata per visualizzare i file/directory che non è stato possibile scansionare per problemi di permessi, evitando di saltarli silenziosamente.

### 7. Analisi Avanzata & Intelligenza
- [x] **Rilevamento Duplicati:** Modalità per identificare file identici (stessa dimensione e hash) per suggerire la pulizia dello spazio.
- [x] **Visualizzazione ad Albero:** Aggiungere una modalità "Tree View" o "Treemap" testuale per avere una visione d'insieme della struttura del disco.
- [ ] **Consapevolezza Git:** Opzione per ignorare automaticamente i file definiti nei `.gitignore` presenti nelle sottocartelle.

### 8. Ecosistema & Estensioni
- [x] **Esplorazione Archivi:** Permettere di entrare nei file `.zip`, `.tar.gz` o `.7z` come se fossero directory normali per esplorarne il contenuto.
- [x] **Filesystem Remoti:** Supporto per la scansione e navigazione di server remoti tramite protocolli SSH/SFTP.
- [ ] **Anteprime Grafiche:** Visualizzazione di anteprime immagini (tramite protocollo Sixel o Kitty) direttamente all'interno della TUI.

---

## 🛠 Correzioni e Ottimizzazioni (Bugfix & Refactoring)

### 1. Precisione del Calcolo (Hard Links)
- [x] **Rilevamento Hard Links:** Attualmente, se due nomi puntano allo stesso `inode`, il programma potrebbe contare la dimensione due volte.
    - *Soluzione:* Implementare una hash table (o un bitset) di `(dev_id, inode)` per contare il peso del file solo la prima volta che viene incontrato.

### 2. Robustezza della Memoria
- [x] **Audit dei Memory Leak:** Verificare tutti i rami di errore (specialmente nel caricamento della cache e nella scansione parallela) dove `malloc` o `path_join` potrebbero non essere seguiti da `free`.
- [ ] **Gestione Path Lunghi:** Sebbene usi `PATH_MAX`, alcuni filesystem moderni o directory annidate possono superare questo limite. Valutare l'uso di percorsi dinamici o un uso più esteso di `openat()`.

### 3. Scalabilità della Cache
- [ ] **Lazy Loading della Cache:** Invece di caricare l'intero file `.fastdu_cache_v2` all'avvio (che può essere lento per milioni di entry), implementare un caricamento "on-demand" delle sottocartelle o usare un database leggero come **SQLite**.
- [ ] **Compressione della Cache:** I percorsi testuali ripetuti occupano molto spazio su disco. Valutare una compressione semplice (es. zstd) per il file di cache.

### 4. Concorrenza e Threading
- [x] **Ottimizzazione Mutex:** Ridurre la granularità dei lock sulla cache durante la scansione parallela per evitare che i thread rimangano in attesa l'uno dell'altro (lock contention).
- [x] **Limiti di Stack:** La funzione `copy_tree_with_progress` è ricorsiva. Per alberi di directory molto profondi, potrebbe causare uno stack overflow. Sarebbe preferibile una versione iterativa con una coda.

### 5. Interfaccia (Edge Cases)
- [x] **Gestione Terminale Piccolo:** Se la finestra ncurses è troppo stretta, i nomi lunghi e le date si sovrappongono. Implementare un sistema di troncamento intelligente (ellissi centrali o finali).
- [x] **Segnali POSIX:** Assicurarsi che `SIGWINCH` (ridimensionamento finestra) e `SIGINT` (interruzione) vengano gestiti correttamente in ogni fase (scansione, copia, navigazione).

---

## 📝 Note Tecniche
*   **Priorità Alta:** Rilevamento Hard Links e Gestione Memory Leak.
*   **Priorità Media:** Sistema di Esclusione e Barre di occupazione.
*   **Priorità Bassa:** Esportazione JSON e Comparazione Snapshot.
