# T3Render: dal replay D3D8 alla scena semantica

Aggiornamento del 15 novembre 2026. T3Render è il nuovo renderer di T3; D3D12 è il
primo backend previsto, non il suo modello di dati. Vulkan è un backend futuro
per Windows/Linux/SteamOS; Metal nativo resta un'opzione successiva. Nessun
backend aggiuntivo è attualmente implementato o garantito.

La [campagna successiva](benchmarks/runtime/2026-11-14-tds-provenance-screen/result.json)
ha prodotto un backbuffer D3D8 sincronizzato e una ricostruzione DX12 parziale
con bedroom riconoscibile, geometria e camera della stessa trace. Le differenze
restano esplicite: materiali e illuminazione diagnostici, pass mancanti e
nessuna parità completa. La [ricerca bridge](T3SCENEBRIDGE_RESEARCH.md)
documenta anche una raccolta pre-draw osservata a runtime e i limiti della
correlazione con il letto. Nel frame 1001 sono ora verificati direttamente gli
scope di submission del letto e di un blocco BSP; i byte geometrici della
stessa selezione sono confrontati nel frame separato 2722. Provenienza e
limiti sono nel [checkpoint](T3SCENEBRIDGE_RESEARCH.md#submission-runtime-frame-1001).

## Obiettivo e confine tra i due percorsi

Il frame reale **3377** della bedroom è il benchmark legacy: conserva la
cattura D3D8, la ricostruzione DX12 e il confronto dello stesso frame. Il
replay resta un oracle diagnostico; estenderlo solo quando serve a spiegare
discrepanze concrete. Geometria e camera hanno preceduto materiali e luce, e
l'immagine riconoscibile resta parziale: non dimostra parità del renderer.
La priorità è ora SceneBridge → Scene IR → T3Render.

Una [prima preview offline](tools/README_scene_ir_preview.md) usa la Scene IR
per letto e BSP, con una camera osservata separatamente nel frame 2661. Le
immagini sono diagnostiche, a colori piatti, e non dimostrano che gli oggetti
offline fossero visibili o invariati in quel frame runtime.

La traccia D3D8 conserva draw, stati e provenienza dei dati per diagnosi e
replay legacy. La [**Scene IR v0**](ir/README_scene_ir.md) descrive invece oggetti, geometria,
materiali, luci e camera: non è uno specchio degli stati D3D8. Un adattatore
può condividere risorse o rappresentazioni numeriche con il replay, ma non
trasforma dati ignoti in fatti né usa la traccia come contratto permanente per
il renderer moderno. La facade COM resta diagnostica e sperimentale: ampliare
fixture o copertura solo per sbloccare evidenze necessarie al checkpoint.

```text
T3 runtime ──> T3SceneBridge ──> Scene IR ──> T3Render ──> D3D12 / Vulkan / Metal
    └────────> capture D3D8 ───> legacy replay DX12 + confronto D3D8 (diagnostica)
T3SDK asset parsers ───────────> importer offline ──> Scene IR (fixture locale)
```

L'[importer offline](tools/README_scene_ir_import.md) prepara fixture, ma un
export della mappa non è un frame live: non contiene necessariamente la camera,
visibilità, modifiche del salvataggio o luci del momento. Non chiamarlo replay
TDS né misurare la sua parità contro un frame non sincronizzato.

## Base concreta e limiti

- [`t3bsp.py`](../tools/assets/t3bsp.py) individua render block BSP nella
  `Level` con una scansione euristica; le posizioni sono in coordinate mondo.
  L'indice che collega i blocchi a zone/foglie non è decodificato e i block ID
  possono ripetersi. Non usare `bsp_*`, ID da solo o quota come identità stabile.
- [`t3mesh.py`](../tools/assets/t3mesh.py) legge posizioni, normali, UV, tangenti,
  indici, sezioni, skin, nomi di materiali e hardpoint; `t3map.py` ricava dagli
  attori esportati trasformazioni, mesh/skin e proprietà delle luci. I parsers
  sono dati di asset, non prova di ciò che la GPU disegna nel frame.
- L'export locale `SouthQuarter1_int1.actors.json` contiene 780 record, di cui
  512 con mesh e 39 con dati luce, per **l'intera mappa**, non solo la bedroom.
  È una fotografia dell'export, non l'inventario live nel save.
- L'SDK offre callback sul message loop, accesso agli UObject e hook per il
  build verificato ([API](../sdk/include/t3sdk/t3sdk.h)); nessuno di questi è
  già un hook di invio scena. Le callback non garantiscono che camera e risorse
  siano campionate nello stesso frame di rendering.
- In analisi statica del `T3Main.exe` x86 Steam documentato in
  [`docs/target.md`](../docs/target.md), SHA-256
  `09e0be902062c53cbb1736cffa1bd3ec883bd5e864f8e7f3f99bc0d06f6ad0f8`,
  Hopper/REA ha individuato che `0x10928760` inizializza contatori nominati «Visible
  Lights/Object», «Visible Objects/Light», «Shadow Casting Lts/Obj»,
  «Shadow Casting Objs/Lt» e «Objects rendered»
  (dossier `ev_40fe44fa00b851712b2546f384895f1950512e9eae20e1a613f14c3d0fe8244d`).
  I riferimenti a quei contatori portano a `0x10926800`
  (`ev_cd33f1335672a5d8f872ba0b37bdc3a2168bd9a6431d459ab2613c3478f67418`;
  vedi anche `config/PC_20040610/symbols.txt`). È una **pista per la ricerca**,
  non la dimostrazione di una submission specifica: il decompilatore ha limiti
  sui confini delle funzioni e le chiamate indirette. Il probe read-only del
  frame 2661 e i suoi limiti sono descritti nella
  [ricerca bridge](T3SCENEBRIDGE_RESEARCH.md#probe-runtime-bedroom-frame-2661).
- Nella facade sperimentale, un reset con ticket accettato chiama
  `BufferProxy::BeginFacadeReset`, che rilascia i token delle shadow dei buffer
  e cancella le shadow persistenti prima della chiamata nativa
  ([implementazione](src/buffer_proxy.cpp), [facade](tests/d3d8_device_facade.hpp)).
  Questo spiega perché il reset sia una pista concreta per la perdita di
  osservabilità, ma non dimostra l'origine di ogni `unknown_resource` nei
  frame catturati né se il reset sia riuscito.
- `0x10992300` è documentato come calcolo della posizione della luce al
  hardpoint (`docs/assets.md`); non è identificato come submission della luce.
  Intensità/raggio fisici e mappatura del light model T3 restano da calibrare.

## Contratto iniziale della Scene IR

La versione 0 è circoscritta alla geometria statica e versionata. Il contratto
implementato e i test sono descritti in [Scene IR v0](ir/README_scene_ir.md).
Le voci seguenti includono requisiti futuri; non assumere che ogni campo
dell'engine o ogni conversione sia già noto.

- **Scene snapshot**: versione, identità della mappa e della sorgente,
  generazione/lifetime, frame ID e provenienza `offline` o `runtime`;
  campi mancanti espliciti. La sincronizzazione con la camera dev'essere
  verificata, non dedotta dal numero di frame.
- **Camera**: vista, proiezione, viewport e range di profondità con convenzioni
  esplicite. Conservare dati grezzi quando la conversione non è validata.
- **Geometry**: mesh immutabile, vertici, normali, tangenti, UV, indici,
  sezioni e bounds; istanze separate con trasformazione. Identificare le risorse
  tramite una chiave qualificata da mappa/asset e fingerprint di sorgente;
  per il BSP mantenere indice/offset/ID e provenienza finché non esiste una
  chiave semantica verificata. Revisioni e riutilizzo di indirizzi non devono
  confondere risorse differenti.
- **Materiale**: nome T3 e riferimenti alle texture originali; override
  nominativo separato con albedo, normal, roughness, metallic, AO e height
  opzionali. Nessuna conversione implicita da dati sconosciuti a PBR certo.
- **Luce visuale**: tipo, trasformazione/hardpoint, colore, raggio, cono,
  stato e shadow intent quando noti; salvare valori T3 e provenienza anche
  quando si derivano parametri fisici. Non scrivere proprietà di gioco.
- Definire e testare unità, assi, handedness, winding, tangent space, layout
  delle matrici, clip depth e spazio colore prima della serializzazione
  stabile; i backend eseguono la conversione esplicita. I dati di origine T3
  e l'export Godot hanno convenzioni diverse.
- Nessun puntatore a UObject/COM o tipo D3D nella Scene IR serializzata.
  Il bridge copia o possiede dati con lifetime definito e gestisce unload,
  reload, cambio livello, modifiche di save e invalidazione. Il renderer legge
  snapshot consistenti; callback/hook e thread devono essere documentati.

## Roadmap e criteri di uscita

1. **Legacy frame**: batch di cattura reale, recupero dei soli dati necessari,
   immagine D3D8 sincronizzata, replay DX12 e confronto ripetibile della stessa
   camera. Segnalare esplicitamente dati mancanti e differenze.
2. **Scoperta bridge**: seguire staticamente le piste di geometria, istanze,
   camera e luci; osservare in modo read-only il punto candidato in TDS e
   dimostrare identità, ordine, ciclo di vita e coerenza tra frame e save.
   Il checkpoint iniziale mesh statica + un BSP è superato nel frame 1001;
   sospendere l'espansione della ricerca sul renderer legacy. Lifecycle generale,
   variazioni di save e luci restano requisiti per le successive estensioni.
   Niente soppressione del renderer originale in questa fase.
3. **Scene IR API-neutral**: schema/versione, convenzioni, provenienza e test
   di conversione; import offline T3SDK come fixture, con dati del gioco solo
   locali. Confrontare inventario/bounds/trasformazioni con sorgenti originali.
4. **Bedroom statica**: BSP + static meshes da Scene IR in T3Render/D3D12,
   camera runtime e confronto con TDS; mantenere la geometria gameplay intatta.
5. **PBR**: override nominativi e materiali completi riutilizzando gli asset
   esistenti, con verifica su pietra, pavimento e legno.
6. **Replacement/decorative**: mapping mesh/istanze e layer decorativi, senza
   cambiare collisioni o hardpoint di gioco.
7. **Lighting HDR fisico**, 8. **soft shadows**, 9. **GI**, 10. **volumetrics**,
   11. **rough/RT reflections**, 12. **TAA, exposure, tonemapping e upscale**:
   introdurre e misurare ciascun pass dopo il precedente. RT resta opzionale e
   richiede capability del backend/hardware.
13. **Dinamici**: movable, skinned e particelle progressivamente, con lifecycle
   e test propri. 14. Disabilitare gradualmente il world D3D8 solo dopo la
   verifica di copertura e comportamento dell'integrazione.

La convivenza di renderer diversi richiede un progetto esplicito per ordine dei
pass, depth/occlusione condivisa o compositing, sincronizzazione GPU, alpha,
HUD, resize/reset e fallback. Non presumere che due device o API condividano
buffer di profondità o present automaticamente; verificare separatamente
Windows e Highball. Fino ad allora l'output statico può restare offline.

Il renderer moderno non modifica AI, physics, gameplay, collisioni, hardpoint,
stealth o light gem. Le luci visuali sono una copia/interpretazione separata.
Le misure devono includere identificativi di build/configurazione, camera,
fonte degli asset, immagini e differenze; asset e tracce derivati dal gioco
restano locali e non vengono pubblicati.
