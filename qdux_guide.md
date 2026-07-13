# Guida e Analisi Completa di qdux

`qdux` è un analizzatore di utilizzo del disco per terminale estremamente veloce, scritto in C11 e basato su **ncurses** per la TUI (Terminal User Interface) e **pthreads** per la parallelizzazione delle scansioni.

Questo documento fornisce un'analisi dettagliata del funzionamento interno del codice sorgente ([qdux.c](file:///home/anidisc/github/qdux/qdux.c)) e una guida completa all'uso delle funzionalità dell'applicazione.

---

## 1. Analisi Funzionale del Codice (`qdux.c`)

L'architettura del programma è suddivisa nei seguenti moduli funzionali principali:

### 1.1 Il Sistema di Cache Shardata
Per velocizzare le scansioni successive alla prima, `qdux` salva lo stato di scansione in un file chiamato `.qdux_cache_v3` nella cartella principale. 
* **Sharding per la Concorrenza:** Per ridurre la contesa del lock tra i thread worker durante le scansioni parallele, la cache in memoria è divisa in **64 shard** (`CACHE_SHARDS`). Ogni shard è gestito in modo indipendente e protetto da un mutex dedicato (`pthread_mutex_t mu`).
* **Rilevamento delle Modifiche:** Ogni record registra inode (`ino`) e data di modifica (`mtime`). Se la data di modifica della cartella sul disco non coincide con quella memorizzata, il sottoalbero corrispondente viene ricalcolato, mentre le porzioni non modificate vengono riutilizzate istantaneamente.

### 1.2 Motore di Scansione Parallela (BFS)
La scansione parallela avviene nella funzione `scan_dir_parallel_deep`:
* **Task Queue:** Utilizza una coda di compiti a livello di directory (`TaskQueue`), in cui ogni directory da scansionare viene inserita come un singolo compito (`DirTask`).
* **Worker & Finalizer Loop:** Un pool di thread worker (`worker_loop`) estrae le directory dalla coda, raccoglie le dimensioni dei file e inserisce le sotto-directory trovate nella coda di scansione. Una volta che tutti i discendenti di una directory sono stati scansionati, il thread finalizzatore (`finalizer_loop`) calcola la dimensione totale complessiva e aggiorna la cache.
* **Supporto Asincrono con `io_uring`:** Su sistemi Linux compatibili, `qdux` può sfruttare `io_uring` per effettuare chiamate di stato asincrone (`statx`), massimizzando le prestazioni dell'I/O su dischi SSD veloci.

### 1.3 Interfaccia Utente (TUI Ncurses)
L'interfaccia terminale offre diverse modalità di visualizzazione:
* **Tree View (Modalità Albero):** Espande e comprime i nodi mantenendo l'indentazione.
* **Miller Columns (Colonne Ranger):** Visualizzazione affiancata classica (Cartella Genitore $\rightarrow$ Selezione Attuale $\rightarrow$ Anteprima Contenuto).
* **Gestione Temi & Colori:** Configurazione flessibile di colori e temi preimpostati (es. Dracula, TokyoNight, Light, Pastel).
* **Footer Multi-riga & Layout Adattivo:** I messaggi di stato ed i prompt di inserimento lunghi vengono automaticamente mandati a capo su più righe. La TUI sposta verso l'alto la barra del footer e riduce l'altezza della lista dei file per adattarsi, ripristinando il layout originario a comparsa.

### 1.4 Gestione Archivi
Integrato con `libarchive` per comprimere file o cartelle selezionati in formato `.zip` (`zip_compress_items`) ed estrarre archivi sul posto (`archive_extract_to`). Durante l'estrazione o compressione, la barra di stato mostra l'avanzamento in tempo reale leggendo il descrittore del file compresso tramite `lseek`.

### 1.5 Protocollo Client-Server Remoto
Una delle caratteristiche avanzate di `qdux` è la possibilità di esplorare file system remoti via SSH:
* **Server Mode (`--server`):** Esegue `qdux` sulla macchina remota inviando lo stato del file system su `stdout` in un formato testuale TSV ed esegue comandi remoti guidati.
* **Client Mode (`--connect URI`):** Connette la TUI locale a un server tramite una sessione SSH persistente configurata con socket multiplexing per ridurre al minimo la latenza.

---

## 2. Guida all'Uso ed Opzioni CLI

### 2.1 Sintassi da Riga di Comando
```bash
qdux [opzioni] [percorso]
```

#### Opzioni principali:
* `-h, --help`: Mostra la guida dei comandi ed esce.
* `-v, --version`: Mostra la versione attuale di `qdux`.
* `-R, --reload`: Ignora il database `.qdux_cache_v3` ed esegue una scansione parallela completa da zero.
* `-j N, --jobs N`: Imposta il numero di thread worker (Default: numero di CPU, massimo 64).
* `-x, --one-file-system`: Rimane sullo stesso file system (non attraversa i mount point).
* `-e PAT, --exclude PAT`: Esclude file e cartelle che corrispondono esattamente al pattern `PAT`.
* `--diff FILE`: Compara la cartella attuale con un file snapshot precedentemente salvato.
* `--export FMT FILE`: Esporta i risultati dell'analisi in formato `json` o `csv` sul percorso indicato.
* `-D, --decorative`: Abilita l'interfaccia decorativa (linee di separazione extra, bordi, intestazioni colonne).
* `-nf, --nerd-fonts`: Abilita il rendering delle icone grafiche per i file (richiede un font compatibile sul terminale).
* `--connect URI`: Si connette ad un server remoto tramite l'URI specificato (formato `utente@host:/percorso`).

---

## 3. Scorciatoie da Tastiera all'interno della TUI

L'interfaccia interattiva supporta i seguenti comandi da tastiera:

### Navigazione
| Tasto | Azione |
|---|---|
| `Freccia Su` / `Freccia Giù` o `j` / `k` | Sposta la selezione. |
| `Invio` / `Freccia Destra` o `l` | Apre / espande la cartella selezionata. |
| `Backspace` / `Freccia Sinistra` o `h` | Ritorna alla cartella genitore. |
| `b` / `e` | Vai rispettivamente all'inizio o alla fine della lista attuale. |

### Visualizzazioni speciali e Ricerca
| Tasto | Azione |
|---|---|
| `v` | Anteprima del file di testo selezionato (con scorrimento integrato). |
| `a` | Attiva/disattiva la vista ad albero (Tree View). |
| `M` | Attiva/disattiva le colonne in stile Miller (simile al file manager Ranger). |
| `E` | Mostra la distribuzione dello spazio occupato in base all'estensione del file. |
| `U` | Avvia il **Duplicate Finder**: analizza i file duplicati trovando lo spazio sprecato. |
| `/` | Esegue una ricerca globale su tutta la cache (sia cartelle che file). |
| `f` | Trova per nome all'interno della cartella corrente (premi `n` / `N` per il successivo/precedente). |
| `F` | Ricerca tramite espressione regolare (Regex). |
| `t` | Filtra gli elementi visualizzati (Tutti / Solo Directory / Solo File). |
| `T` | Attiva/disattiva il filtro testuale basato sulla stringa cercata. |
| `Ctrl + T` | Resetta immediatamente tutti i filtri di ricerca. |

### Operazioni sui File
| Tasto | Azione |
|---|---|
| `SPAZIO` | Seleziona/deseleziona l'elemento corrente per operazioni di massa (Mark). |
| `Ctrl + A` | Seleziona o deseleziona tutti gli elementi della lista corrente. |
| `L` | Visualizza la lista interattiva degli elementi marcati (permettendone la rimozione singola o globale). |
| `m` | Sposta tutti gli elementi marcati nella cartella attuale. |
| `c` | Copia tutti gli elementi marcati nella cartella attuale (mostrando la percentuale di avanzamento). |
| `d` | Elimina gli elementi marcati o, in alternativa, l'elemento attualmente evidenziato. |
| `ALT + r` | Rinomina l'elemento selezionato. |
| `Ctrl + n` | Crea una nuova directory nel percorso corrente. |
| `ALT + n` | Crea un nuovo file vuoto. |
| `z` | Comprime gli elementi selezionati in un archivio `.zip`. |
| `x` | Estrae l'archivio selezionato. |

### Utilità Generali
| Tasto | Azione |
|---|---|
| `r` | Riesegue la scansione singola della directory selezionata. |
| `R` | Esegue una riscansione parallela completa della cartella attuale. |
| `O` | Apre il file o la directory con l'applicazione di sistema predefinita (`xdg-open`). |
| `Ctrl + E` | Apre il file selezionato con l'editor esterno impostato in `$EDITOR` o `vim`. |
| `Ctrl + S` | Sospende temporaneamente `qdux` e avvia una subshell nella cartella attuale (digita `exit` per tornare). |
| `o` | Cambia la chiave di ordinamento (Dimensione $\rightarrow$ Nome $\rightarrow$ Data modifica $\rightarrow$ Delta). |
| `s` | Inverte l'ordine di ordinamento (Crescente $\leftrightarrow$ Decrescente). |
| `K` | Cicla tra i temi di colore disponibili. |
| `Y` | Cattura uno snapshot attuale per la modalità **DIFF** (evidenzia lo spazio occupato/liberato). |
| `I` | Cambia la formattazione della dimensione (Valore numerico $\rightarrow$ Percentuale rispetto al padre $\rightarrow$ Nascosto). |
| `TAB` / `Ctrl + i` | Mostra o nasconde la barra grafica della dimensione a sinistra. |
| `h` | Mostra la schermata di aiuto integrata. |
| `q` | Esci da `qdux`. |

---

## 4. Configurazione (`config.toml`)

Il programma cerca il file di configurazione in `~/.config/qdux/config.toml`. Un esempio di configurazione valida è:

```toml
# Scelta del tema estetico predefinito
theme = "dracula" # Opzioni: dark, dracula, tokyonight, light, pastel

# Editor di testo predefinito (sovrascrive $EDITOR)
editor = "nano"

# Personalizzazione dei colori principali
dir_fg = "blue"
file_fg = "white"
size_s_fg = "green"   # File piccoli (< 1 MB)
size_m_fg = "yellow"  # File medi (1 MB - 1 GB)
size_l_fg = "red"     # File grandi (> 1 GB)

# Associazione delle estensioni ad applicativi di apertura personalizzati
[associations]
pdf = "zathura"
png = "feh"
jpg = "feh"
mp4 = "mpv"
zip = "file-roller"
```
