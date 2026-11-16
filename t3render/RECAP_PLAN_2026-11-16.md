# Recap T3SceneBridge e T3Render — 16 novembre 2026

## Scopo e stato

Il lavoro ha portato la bedroom di Thief: Deadly Shadows da una cattura D3D8
a snapshot semantici parziali e a un renderer D3D12 separato, con materiali
remaster e prime tecniche moderne. Il percorso letto/BSP del bridge è
verificato nelle catture; **il renderer PBR moderno rimane standalone**.
Un viewer live flat sperimentale è collegato al bridge, ma non ha ancora
prodotto frame verificati nel gioco.

L'obiettivo è stato creato il 15 novembre alle 12:28 CET. Alla rilevazione del
16 novembre il tempo attivo contabilizzato era circa **13 h 37 min**: non è la
durata continuativa tra le due date. Il frame 3377 del 14 novembre è un
antefatto conservato come riferimento, precedente alla creazione dell'obiettivo.
Branch: `remaster/vertical-slice`. Alla prima stesura il commit del renderer
era `cd7eb91e`; il checkpoint successivo comprende i commit fino a
`17675fba` e il lavoro su catalogo e driver incluso insieme a questo recap.
La rilevazione delle 13 h 37 min precede quest'ultimo checkpoint: non è una
misura aggiornata del tempo totale.

La direzione successivamente concordata è **usare D3D8 come oracle per camera,
geometria e diagnosi**, senza cercare la perfetta parità visiva legacy. Le nuove
feature vanno su SceneIR → T3Render, dando priorità a illuminazione, GI e
geometria delle pareti; DXR resta opzionale e non prioritario.

## Risultati in breve

| Ambito | Stato e significato |
|---|---|
| Bridge v0 | Validati i percorsi di submission per il letto e un BSP nelle catture precedenti; non è una prova generale di tutti gli oggetti. |
| Cattura combinata | Nei frame 2863–2865 ownership, lock shadow e screenshot coesistono per il letto e il BSP verificati. |
| SceneIR v1 | Schema API-neutral e snapshot runtime parziali; geometria e molte associazioni provengono da asset offline verificati. |
| Replay D3D8 → DX12 | Bedroom riconoscibile e confronti diagnostici; parità legacy completa non dimostrata e non più prioritaria. |
| Materiali e mesh | Override semantici per pietra, pavimento e letto; altri asset Godot riutilizzati nella preview. |
| Renderer moderno | PBR/HDR, ombre, GI approssimata e altri effetti funzionano in preview separate, non come renderer di produzione. |
| Switch letto | Originale/high-poly/originale su catture e nel viewer flat con dati sintetici; non modifica gameplay o collisioni e non è verificato live. |
| Infrastruttura live | Residency, finestre `Present`, matcher e adattatore alimentano un viewer D3D12 flat sperimentale; il percorso non è stato verificato in TDS. |

## Prove: dal frame D3D8 alla submission semantica

Nel frame **3377**, screenshot D3D8 acquisito prima del relativo `Present`, i
draw 8/9 hanno posizioni, UV e indici delle copie dei lock corrispondenti alle
sezioni di `INNbed1A1`. Per i blocchi GMP 5, 7 e 19 sono state trovate sezioni
BSP con VB/IB corrispondenti. Le copie dei lock non sono readback GPU e questo
frame, da solo, non identifica l'actor che ha emesso i draw.

Il probe read-only ha poi identificato `StaticMeshActor__1520`, il nodo mesh
`element +0x108` e il nome runtime `innbed1a1`. La traslazione osservata è
`[3398, 1513.9998779296875, 272]` in unità T3. Mesh e fixture concordano su
257 vertici, 708 indici e tre sezioni (`BF`, `GENgoodnessA_D`,
`GENiFABRICwoolA2_D`); la sezione nascosta `BF` e la skin runtime non sono
interamente dimostrate.

La ricerca ha corretto una prima ipotesi: lo slot `+4` osservato nella zona di
`0x10926800` non è la submission dei draw per questi oggetti. Il contratto
congelato è **elemento → scope della virtual call `+0x24` → draw**, con
pass/stage, thread e intervallo `Present` controllati. Nel frame **1001**:

- letto: `StaticMeshActor__1520 → INNbed1A1` → sezioni 1/2 → draw 8/9;
- BSP: indice runtime VBSP 7 → blocco GMP 5 → sezioni 0/1 → draw 5/6.

Il frame 1001 aveva screenshot e ownership diretta, ma le lock shadow di questi
draw erano `unknown_resource` dopo un Reset precedente. Il frame 2722 forniva
separatamente il confronto delle shadow, senza provare la proprietà diretta
delle sue draw.

Il successivo batch **2863–2865** ha permesso una prova combinata. Il
verificatore è stato rieseguito sui tre frame senza riferimento esterno: per il
letto e il BSP sopra riporta submission valida e lock shadow `matched` su tutte
le quattro sezioni. Nel **2864** i draw sono 530/531 per il
BSP e 533/534 per il letto; lo screenshot 1024×768 è associato allo stesso
intervallo `Present`. Il confronto copre posizioni, UV0 e indici nelle copie
dei lock, non il contenuto effettivamente letto dalla GPU.

Il frame 2864 è quindi un riferimento combinato utile; il 3377 rimane il
benchmark storico. Il confronto non prova la scena completa, tutte le classi o
le risorse GPU. Le impronte della chiave durevole del letto sono tratte dalla
fixture offline dopo il matching univoco, non dal resource manager runtime.
Gli indirizzi di processo rimangono nelle trace locali e non negli ID SceneIR.

## SceneIR e sorgenti dei dati

`SceneSnapshot` contiene camera, risorse mesh, istanze e transform, BSP,
materiali, luci e, in v1, submission con pass, stage, draw e stato delle lock
shadow. Il formato valida referenze e sezioni, distingue `source=runtime` da
`source=offline` e non serializza COM, UObject o indirizzi runtime.

L'importer offline `SouthQuarter1_int1` usa `t3map.py`, `t3mesh.py` e
`t3bsp.py`: la fixture locale attuale comprende 98 risorse mesh, 453 istanze,
45 blocchi BSP, 60 materiali e 39 luci. Non rappresenta un frame né la
visibilità di un salvataggio; gli attachment irrisolti restano esclusi e la
scansione BSP è euristica.

La selezione runtime locale aggiornata del frame 2864 contiene 60 submission,
33 istanze mesh e nove blocchi BSP; 18 submission hanno shadow confrontate e
42 restano `unverified`. Le 32 associazioni aggiuntive di asset mesh sono
candidati offline, non binding runtime osservati. Uno snapshot separato
identifica nove luci candidate nello stesso frame: cinque omni e quattro spot.
Posizioni e identità restano candidate; colore, intensità, attivazione,
direzione e apertura non sono misure complete dello stato runtime.

La camera del frame 2864 è runtime e sincronizzata; la posizione ricavata dalla
view è circa `[3571.703, 1576.414, 354.380]` in unità T3. Il viewport dei
draw è `0..0.5`, diverso dal `0..1` del probe; near/far non sono osservati.
Matrice row-major e vettori riga sono espliciti. La scala `0.01905 m/unità` è
inferita dagli asset, non calibrata runtime; winding assoluto, UV/tangent space
e clip depth non vengono inventati quando l'evidenza manca.

## Checkpoint successivo: preparazione al consumer live

Tre commit successivi al primo recap aggiungono la gestione delle risorse
residenti (`58c6ab3f`), il collettore delimitato da `Present` (`fa5ebac9`)
e il matching semantico degli scope (`17675fba`). Insieme a questo documento
sono stati aggiunti un catalogo binario degli asset offline, un driver che
collega probe, scope, draw, `Present` e `Reset`, e un adattatore Windows
sperimentale opt-in nel proxy combinato.

Il catalogo esportato dalla fixture `SouthQuarter1_int1` contiene il letto,
45 blocchi BSP e nove materiali. Il loader controlla struttura e checksum
SHA-256; il checksum rileva corruzione, non autentica la provenienza degli
asset. Il catalogo resta **offline**: non è un frame, né prova che il gioco
abbia caricato o stia mostrando quelle risorse.

Il driver può pubblicare una selezione solo attraverso finestre `Present`
validate, con thread e stato camera/draw controllati; rifiuta sovrapposizioni
e gestisce il Reset nei casi sintetici testati. I binding del letto e BSP
rimangono candidati. A quel checkpoint il matcher BSP dipendeva dal letto nella
lista osservata; il cambio mappa e il lifecycle generale non sono completi, e non
è provata la copertura di tutti i callback nel gioco. I frame 2864/2865 citati
nel resto del recap sono catture precedenti, non nuove prove dell'adattatore.

Il codice può esporre `RuntimeCurrent()` nello stesso processo. Il consumer
D3D12 flat aggiunto successivamente non ha ancora dimostrato di seguire Garrett
in tempo reale. La documentazione tecnica e i comandi sono in
[scene live](ir/README_scene_live.md).

## Renderer, materiali e luce

Il renderer PBR/HDR usa un target lineare `RGBA16F`, BRDF GGX/Schlick,
decodifica sRGB, luci puntiformi con decadimento quadratico e conversione a
display separata. Le luci spot del pacchetto v6 richiedono asse e coni
espliciti nella calibrazione: per `Light__15` un esperimento con asse da
rotazione offline e cono artistico 20°/35° ha modificato 77.544 pixel nel
frame 2864. Non dimostra il cono reale del gioco.

Sono disponibili shadow map cubiche, occlusori candidati offline e un filtro
opzionale con penombra dipendente dalla distanza dell'ostacolo. La modalità
world-space `--world-indirect` prepara un rimbalzo diffuso CPU con 128 campioni,
test di visibilità e sorgenti anche fuori schermo tra le geometrie incluse nel
pacchetto. Un test sintetico ha verificato rimbalzo colorato, blocco da
geometria e da caster solo-ombra; un oggetto emissivo opaco continua a
occludere. Le superfici non presenti nel pacchetto non possono riflettere o
emettere. Si tratta di un'approssimazione per vertice, non di GI completa o in
tempo reale: il ciclo originale/high-poly/originale della bedroom ha richiesto
circa 36 s nella build ottimizzata locale, con due bake iniziali.

Esistono inoltre anteprime opzionali di rimbalzo screen-space, GTAO, emissione
delle candele, volumetrici, riflessi screen-space, accumulo temporale, upscale
spaziale e curva filmic. Sono limitate a geometria statica e a dati disponibili:
non equivalgono a GI globale, TAA o upscaling di produzione. Nel confronto
temporale world-space sui frame 2864/2865, CPU e GPU hanno differito in media
di circa 0,0057 livelli per canale, massimo 5/255. Le finestre `Present` sono
state validate; il confronto non attesta illuminazione runtime completa.

Gli override sono basati su nomi/chiavi semantiche, non su draw number:
`STMwall512D_D → stone_wall_v2`, `HSEfloorB_D → wood_floor_v2` e
`INNbed1A1 → bed_high`. Sono presenti anche mapping per legni, candelabro,
arco e dettagli decorativi generati dal lavoro Godot. L'applicazione attuale
è alla selezione della bedroom e ai pacchetti di preview: non è ancora un
sistema di sostituzione generale inserito nel gioco.

La trave `StaticMeshActor__258 / GENrafters1` ha una discrepanza osservata nel
confronto legacy. Un override diagnostico per `D_384__5` migliora quel
confronto, ma non prova quale sia la causa né corregge il gioco originale. Il
nuovo rimbalzo modifica l'illuminazione della stanza; **non è ancora dimostrato
che risolva il problema specifico della trave**.

## Verifiche e limiti

- Suite completa del checkpoint corrente: **634 test passati**; build x86
  MinGW e fixture D3D12 su Highball/Wine eseguite localmente. In una sessione
  precedente due test del runner sintetico erano andati in timeout, poi
  erano passati isolatamente e nella suite completa successiva.
- Checkpoint successivo: sette test host mirati passati e proxy x86 compilato;
  fixture sintetica del driver e fixture Windows del catalogo passate su
  Highball senza TDS. La seconda ha accettato il catalogo locale e rifiutato
  una copia corrotta. Non verificano i callback del proxy nel gioco.
- Fixture GPU: source fuori schermo, occlusione, caster solo-ombra, emissivo
  opaco, camera invariata, spot, modalità temporale, combinazione con GTAO,
  volumetrici e riflessi. Lo switch letto e i frame 2864/2865 sono stati
  verificati anche con GI world-space.
- I dati di gioco, le trace grezze, i pacchetti contenenti geometria e i file
  generati sono locali sotto `build/`. Il pacchetto diagnostico separato
  contiene screenshot e prove selezionate, non le trace o gli asset originali.
- Non sono stabiliti: binding completi runtime, stato effettivo delle luci,
  contenuto GPU letto indietro, visibilità totale, parità pixel, integrazione
  live D3D12, prestazioni in tempo reale o modifica della luce del gioco D3D8.

## Per riprendere

1. Usare 2864 e 3377 come riferimenti, mantenendo distinti i rispettivi scope.
2. Verificare il consumer D3D12 persistente in
   TDS camera, geometria e movimento; completare lifecycle, Reset e cambio
   mappa prima di considerarlo utilizzabile. Le catture con gioco richiedono
   coordinamento con chi carica il salvataggio.
3. Estendere gli override semantici live oltre il letto e accertare lo stato delle luci runtime,
   distinguendo i parametri misurati dalle calibrazioni artistiche.
4. Investire su HDR/PBR, luci fisiche, ombre e GI in tempo reale, poi su
   emissivi, volumetrici, riflessi e pipeline temporale; migliorare la geometria
   delle pareti e verificare la trave con la nuova illuminazione. DXR rimane
   una scelta successiva. Non serve perfezionare il replay D3D8.

Riferimenti: [ricerca bridge](T3SCENEBRIDGE_RESEARCH.md),
[checkpoint runtime](tools/README_bridge_runtime_checkpoint.md),
[schema SceneIR](ir/README_scene_ir.md), [import offline](tools/README_scene_ir_import.md),
[renderer PBR](tools/README_scene_pbr.md), [temporale](tools/README_scene_temporal.md),
[benchmark sequenza](benchmarks/runtime/2026-11-16-scene-sequence/result.json).

## Aggiornamento successivo: viewer live e letto

Il consumer D3D12 persistente e il catalogo per l'adattatore sperimentale sono
stati implementati dopo il checkpoint sopra. Due tentativi in TDS hanno
inizializzato il viewer senza produrre frame verificati; è stato segnalato lag.
L'ottimizzazione successiva e il nuovo override non sono stati provati nel
gioco, per scelta dell'utente: prestazioni e integrazione live restano aperte.

Il catalogo offline v2 può includere `bed_high` come sostituzione semantica
dell'istanza `StaticMeshActor__1520 / INNbed1A1` nel viewer flat, mantenendo
intatti snapshot e gameplay. La prova GPU Highball con il GLB reale e camera
sintetica ha verificato sostituzione, ripristino, movimento, visibilità e Reset;
non dimostra la sincronizzazione in TDS né un rendering PBR live. Dettagli e
limiti sono documentati nel [README del consumer](ir/README_scene_live.md).

Successivamente il viewer sperimentale ha acquisito gli albedo Godot di muro
e pavimento tramite un catalogo offline v3. La selezione divide le sezioni BSP
usando materiale e trasformazione del letto nello stesso snapshot; se il letto
non è selezionato, torna agli ID originali. Il frame acquisito 2864 concorda
con il precedente selettore offline su 16 sezioni e 67 triangoli. Fixture GPU
e worker con asset offline e camera sintetica hanno verificato texture, UV,
scomparsa del letto e Reset. Normali, roughness e AO sono nel catalogo ma non
ancora usati dal viewer: PBR, integrazione TDS e prestazioni rimangono aperti.

Il modello residente ora distingue evidenza e copertura delle luci per singolo
campo e può associare un campione a una finestra `Present` tramite lettura
interamente delimitata. Il convertitore accetta per il rendering soltanto una
raccolta completa e proprietà verificate; i test usano luci sintetiche. Il
probe TDS non produce ancora questi campioni e le nove luci candidate non sono
state promosse a misure certe.
