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
- [ ] **Snapshot Comparison:** Funzione per confrontare due file di cache diversi e mostrare dove lo spazio è aumentato o diminuito nel tempo.

### 4. Integrazione di Sistema
- [x] **Apertura file esterna:** Aggiungere un comando (es. `o`) per aprire il file selezionato con l'applicazione predefinita del sistema (usando `xdg-open` su Linux).
- [x] **Supporto Mount-points:** Opzione per limitare la scansione al filesystem corrente (`-x` / `--one-file-system`), evitando di entrare in partizioni montate o dischi esterni.

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
