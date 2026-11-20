# t3bench: bedroom v0.1

`t3bench` crea un pacchetto locale da una cattura v0.1 completa e verificata,
poi produce Flat, Remaster ON e Remaster OFF senza avviare TDS. La camera è
quella del frame ON per tutte le immagini generate. I confronti PPM e BMP affiancano,
da sinistra, D3D8 originale, Flat, ON e OFF; `metrics.json` registra differenze
RGB, impronte, draw, triangoli e tempi CPU comprensivi del readback GPU. I tempi
non misurano le prestazioni del gioco e cambiano fra esecuzioni.

Il pacchetto contiene il catalogo di geometria e materiali, le selezioni SceneIR,
le trasformazioni e i due campioni di luce, la camera, le quattro immagini live,
un manifesto e le impronte SHA-256. Il catalogo è un insieme di associazioni
candidate da asset offline, non una registrazione dei binding GPU. Anche i
campioni dei candelabri e il loro stato rimangono candidati; il renderer non
inventa uno stato OFF quando manca la relativa osservazione. Non pubblicare il
pacchetto: contiene dati e immagini derivati dal gioco.

Nelle nuove catture il viewer registra, per ogni immagine del confronto, le
mesh, i blocchi BSP, le sezioni e le trasformazioni esatti dello snapshot.
Il golden usa la selezione ON; per il frame Flat verifica anche letto e BSP
contro il replay del trace e controlla le mesh contro il catalogo. Nelle
catture storiche senza questi dati le mesh restano candidate ricavate dai draw
scoped e il manifesto conta le esclusioni. In entrambi i casi le selezioni non
provano i binding GPU né che ogni elemento sia visibile nell'immagine.

Il golden locale ricreato usa D3D8/Flat 808, ON 816 e OFF 853. La posizione
delle tre pose differisce fino a 0,4955 unità T3. L'immagine D3D8 e il campione
OFF non sono sincronizzati alla camera 816: le differenze rispetto al golden
includono questo scarto, oltre alle differenze del renderer. L'immagine D3D8 è
precedente a Present e include elementi che il viewer non riproduce. Identità
dei materiali, luce fisica, visibilità completa e parità dei pixel non sono
dimostrate. I risultati possono variare fra GPU, driver e versioni di T3Render;
il confronto è ripetibile solo a parità di ambiente e input.
La cattura e il benchmark usano il profilo di illuminazione v0.1: le due
candele osservate, senza la luce aggiuntiva, la nuova esposizione o i normal
map del pass v0.2. Le intensità rimangono una scelta artistica.

## Creazione

Servono una cattura completa con `batch.json`, trace, log del viewer, marker e
immagini ON/OFF, più il catalogo usato durante quella sessione. La cattura
storica `remaster-v01-live-20261120-010825` non era disponibile; quella nuova è
`build/t3render-captures/t3bench-v01-live-20261120-145436-f2a56a`. Il catalogo
usato ha SHA-256 `2ac1cafbf22959f32a029a99748b4eef9a9bc9232ac36d3ce36ed0cf25fef665`.
Il bundle di texture originale è stato ripristinato durante la cattura e il
bundle modificato è stato ripristinato dopo; l'utente ha confermato lo
spegnimento della candela sinistra. Pacchetto e cattura restano locali.
Per una nuova acquisizione usare il flusso v0.1 descritto in
[README_tds_bedroom_batch.md](README_tds_bedroom_batch.md): il runner registra
l'hash del catalogo prima e dopo la sessione. Gli hash prima/dopo non provano
quale catalogo sia stato effettivamente caricato dal processo.
Il manifesto e i risultati riportano anche i bit float32 esatti di vista,
proiezione e profondità del viewport; `verify` controlla che coincidano con la
camera nel pacchetto. I risultati live includono gli hash di catalogo, scena e
immagine D3D8 usati.
Quando sono presenti, `t3bench-review.json` e `texture-state.json` sono
controllati e registrati nel manifesto con le rispettive impronte. La conferma
umana e l'ispezione delle immagini non dimostrano i binding GPU.

Per acquisire una nuova sorgente su Highball, preparare un catalogo v10 per
la stessa installazione e scegliere un percorso nuovo per la cattura. Se
l'installazione contiene texture modificate, lo screenshot D3D8 le includerà:
conservare questa informazione insieme alla cattura. Con le variabili del
README del runner e `T3RENDER_CATALOG` impostata al catalogo assoluto, avviare
prima il watcher in un terminale:

```sh
python3 -B t3render/tools/trigger_live_v01.py --watch "$T3RENDER_OUTPUT" --timeout 600 --speak
```

Poi avviare il runner in un secondo terminale:

```sh
python3 -B t3render/tools/run_tds_bedroom_batch.py run \
  --engine "$T3RENDER_ENGINE" --bottle "$T3RENDER_BOTTLE" --system "$T3RENDER_SYSTEM" \
  --proxy "$PWD/build/t3render-mingw/experimental_combined_proxy/d3d8.dll" \
  --output-dir "$T3RENDER_OUTPUT" --live-catalog "$T3RENDER_CATALOG" \
  --steam-launch --isolated-saves --terminate-after-capture --manual-trigger \
  --delay 0 --timeout 600 --frames 3 --scene-bridge --submission-scope \
  --live-static-meshes --live-lights --preview-candle --candle-link-diagnostics \
  --candle-left --remaster-toggle
python3 -B t3render/tools/compare_live_v01.py "$T3RENDER_OUTPUT"
```

Caricare manualmente il salvataggio, inquadrare letto e candelabri e seguire
le indicazioni vocali; quando richiesto spegnere la candela sinistra senza
cambiare posa. Ispezionare le quattro immagini e confermare l'azione: i soli
contatori non dimostrano visibilità o interazione. Il watcher non invia input.
Per un nuovo pacchetto usare il percorso di questa cattura e lo stesso
catalogo nei comandi seguenti.

```sh
python3 -B t3render/tools/t3bench.py create \
  --capture build/t3render-captures/t3bench-v01-live-20261120-145436-f2a56a \
  --catalog build/t3bench-inputs/bedroom-v01-original.bin \
  --output build/t3bench/bedroom-v01-golden-copy
python3 -B t3render/tools/t3bench.py verify build/t3bench/bedroom-v01-golden-copy
```

`create` rifiuta confronti incompleti, camera incoerente, stati mancanti,
cataloghi non attestati, sezioni assenti e geometria dei candelabri non
utilizzabile. Richiede un compilatore C++ host per validare il catalogo e la
scena prima della copia. La directory di destinazione deve essere nuova.

## Renderer offline

Su Windows, oppure con MinGW e un ambiente Wine/D3D12 già configurato:

```sh
cmake -S t3render -B build/t3render-mingw -G Ninja \
  -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=i686-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=i686-w64-mingw32-g++ -DT3RENDER_BUILD_T3BENCH=ON
cmake --build build/t3render-mingw --target t3bench_renderer
python3 -B t3render/tools/t3bench.py run build/t3bench/bedroom-v01-golden \
  --renderer build/t3render-mingw/t3bench_renderer.exe --output build/t3bench/run-next
```

Su macOS con Highball, aggiungere al comando `run` le opzioni
`--highball-engine "$T3RENDER_ENGINE" --highball-bottle "$T3RENDER_BOTTLE"`.
Il runner configura le librerie del solo processo Wine; `Z:` deve mappare `/`.
Per altri ambienti Wine sono disponibili `--runner` e `--wine-paths`. Il
renderer è offscreen e non apre TDS. L'output deve essere una
directory nuova. Se il renderer fallisce, la directory può contenere file
parziali e il comando segnala il percorso.

## Live opt-in

Con il proxy sperimentale compilato, `run_tds_bedroom_batch.py run` accetta
`--t3bench-package /percorso/assoluto/al/golden`. L'opzione verifica il
pacchetto e abilita catalogo, bridge, submission scope e campionamento delle
mesh. Se viene passato anche `--live-catalog`, il suo hash deve coincidere con
quello del pacchetto. Usare le consuete opzioni di avvio e isolamento descritte
nel README del runner. Non è necessario attivare il toggle o il watcher v0.1.
Per esempio, con le variabili `T3RENDER_ENGINE`, `T3RENDER_BOTTLE`,
`T3RENDER_SYSTEM` e `T3RENDER_OUTPUT` del README del runner:

```sh
python3 -B t3render/tools/run_tds_bedroom_batch.py run \
  --engine "$T3RENDER_ENGINE" --bottle "$T3RENDER_BOTTLE" --system "$T3RENDER_SYSTEM" \
  --proxy "$PWD/build/t3render-mingw/experimental_combined_proxy/d3d8.dll" \
  --output-dir "$T3RENDER_OUTPUT" --t3bench-package "$PWD/build/t3bench/bedroom-v01-golden" \
  --live-only --delay 120 --timeout 360 --terminate-after-capture
```

Questo esempio chiude TDS dopo la prova temporizzata. Il proxy deve essere
compilato con `-DT3RENDER_BUILD_EXPERIMENTAL_COMBINED_PROXY=ON`.

Il viewer attende uno snapshot live pubblicato della mappa attesa con almeno
una mesh o un blocco BSP corrispondente al golden. Copia le trasformazioni delle
mesh corrispondenti, mantiene quelle golden per gli oggetti non presenti nello
snapshot parziale e usa sempre camera e luci ON/OFF registrate nel pacchetto.
La cattura è quindi una scena ibrida esplicita, non una fotografia sincronizzata
del mondo live. I conteggi dei match sono in `metrics.json`. Il lavoro D3D12
avviene nel worker del viewer, senza input, movimento di Garrett o modifica della
camera del gioco. Uno snapshot invalidato durante il render viene scartato.

La prova TDS locale `build/t3render-captures/t3bench-live-check-20261120-150245-441818`
ha prodotto automaticamente le tre immagini dopo il caricamento manuale della
bedroom, senza posizionare Garrett. Lo snapshot ha trovato cinque mesh e quattro
BSP del golden; camera e viewport coincidono e le immagini sono identiche al
render offline nello stesso ambiente. Il tentativo di caricare direttamente il
salvataggio con `--resume-bedroom` è invece terminato con un'eccezione nel gioco.
Impronte e risultati sono nel [resoconto della prova](../benchmarks/runtime/2026-11-20-t3bench-bedroom/result.json).

Il risultato è scritto una sola volta per sessione in
`<output-dir>/t3bench-live-<sessione>/` con Flat, ON, OFF, confronto e metriche.
`metrics.json` viene scritto per ultimo; gli eventi `t3bench_started`,
`t3bench_completed`, `t3bench_invalidated` o `t3bench_*_failed` sono nel log del
viewer. Una directory parziale non è un risultato completato. Nessun evento
significa che non è ancora arrivato uno snapshot utilizzabile o che la modalità
non è stata abilitata.

## Verifica sintetica D3D12

La fixture opzionale usa geometria, materiali e luci sintetici; non avvia TDS
né invia input. Verifica la ripetibilità delle tre immagini, rifiuta uno snapshot
di un'altra mappa e confronta le catture live con quelle offline usando una
camera live diversa. Il [risultato sintetico](../benchmarks/d3d12/2026-11-20-t3bench-synthetic/result.json)
è stato ottenuto su Highball CrossOver 26.3 r32; non prova il risultato della
bedroom reale o il comportamento durante il gioco.

```sh
cmake -S t3render -B build/t3render-mingw -DT3RENDER_BUILD_T3BENCH=ON \
  -DT3RENDER_BUILD_LIVE_D3D12_FIXTURE=ON
cmake --build build/t3render-mingw --target t3bench_renderer t3bench_live_fixture
env T3BENCH_D3D12_RENDERER="$PWD/build/t3render-mingw/t3bench_renderer.exe" \
  T3BENCH_D3D12_LIVE_FIXTURE="$PWD/build/t3render-mingw/t3bench_live_fixture.exe" \
  T3BENCH_D3D12_ENGINE="$T3RENDER_ENGINE" T3BENCH_D3D12_BOTTLE="$T3RENDER_BOTTLE" \
  python3 -B -m unittest t3render/tests/test_t3bench_d3d12.py -v
```

Per verificare anche un catalogo bedroom locale, aggiungere
`T3BENCH_REAL_CATALOG` e `T3BENCH_REAL_SCENE` ai parametri `env` sopra. Il test
usa la geometria e i materiali locali, ma camera, selezione, luci e immagine
D3D8 sono sintetiche; i file temporanei non costituiscono un golden. Verifica
che i tre render siano ripetibili e che ON e OFF differiscano.
