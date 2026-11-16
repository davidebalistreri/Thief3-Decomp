# T3SceneBridge / T3Render — passaggio di consegne

Checkpoint tecnico: **16 novembre 2026**, branch `remaster/vertical-slice`,
codice fino a `5398f9f15d91643bd2ff64ebec3dc34e33fb51c3`. Il commit del
presente documento può essere successivo; il manifest dello zip identifica
esattamente i sorgenti inclusi. Il [recap completo](RECAP_PLAN_2026-11-16.md)
contiene la cronologia e i limiti delle singole prove.

## Direzione

D3D8 resta un oracle per camera, geometria e diagnosi: non è prioritario
replicarne tutta la resa. La pipeline desiderata è T3 gameplay → T3SceneBridge
→ SceneIR → T3Render/D3D12. La prossima prova decisiva è un consumer live che
segua movimento e visibilità della bedroom; poi servono luci runtime affidabili,
illuminazione fisica, GI realtime e geometria remaster. DXR viene dopo.

L'utente ha chiesto di rinviare la verifica del lag e di mettere in pausa
l'obiettivo dopo questo passaggio. Prima di una nuova sessione TDS occorre
coordinarsi con lui. La bozza appena iniziata per i legni non è stata completata
e non è inclusa nel checkpoint.

## Prove disponibili

| Evidenza | Cosa dimostra | Limite |
|---|---|---|
| Frame 3377 | Screenshot D3D8 sincronizzato; letto `INNbed1A1` e blocchi BSP correlati tramite posizioni, UV e indici. | Le copie dei lock non sono readback GPU; il solo frame non identifica l'actor. |
| Frame 1001 | `StaticMeshActor__1520 → INNbed1A1` e VBSP indice 7 → GMP blocco 5, tramite scope di submission e draw. | La submission verificata usa la virtual call `+0x24`, non lo slot `+4`; le lock shadow di quel frame erano incomplete. |
| Frame 2863–2865 | Nello stesso intervallo `Present` coincidono screenshot, ownership e copie dei lock per letto e un BSP. Nel 2864: draw 533/534 letto, 530/531 BSP. | Prova circoscritta a queste sezioni; non certifica l'intera scena o i dati GPU. |
| Camera 2864 | Posizione derivata dalla view circa `[3571.703, 1576.414, 354.380]` unità T3; viewport dei draw `0..0.5`. | Near/far e alcune convenzioni restano ignoti; `0.01905 m/unità` è inferito dagli asset. |

Il letto osservato ha traslazione `[3398, 1513.9998779296875, 272]`,
257 vertici, 708 indici e tre sezioni; la sezione nascosta e la skin runtime
non sono interamente dimostrate. La geometria e molte associazioni degli
snapshot parziali provengono dagli asset offline dopo matching: distinguerle
dalle osservazioni runtime. Il frame 2864 è il riferimento combinato; 3377 è
il benchmark storico.

## Stato attuale

- **SceneIR:** modello API-neutral con camera, mesh, istanze, BSP, materiali e
  luci; distingue runtime/offline e non serializza puntatori COM o UObject.
  `SouthQuarter1_int1` è una fixture offline, non un frame.
- **Viewer live:** proxy e consumer D3D12 opt-in esistono. Due tentativi TDS
  hanno inizializzato la finestra, senza produrre frame verificati; l'utente ha
  riportato forte lag. Il replay offline dei frame 1243–1245 del tentativo
  diagnostico li rifiuta: mancano viewport validato e witness indipendente.
- **Lifecycle:** Reset, cache, trasformazioni e visibilità parziale sono
  esercitati in fixture. Una transizione atomica tra cataloghi è implementata,
  ma nessun evento affidabile di cambio/reload mappa è collegato in TDS; il
  contesto mappa del probe luci richiede ancora integrazione. Il catalogo v5
  può essere senza letto e anche solo BSP o solo mesh, ma un BSP senza witness
  runtime idoneo può essere rifiutato.
- **Materiali/mesh:** muro, pavimento e letto high-poly hanno override nel
  viewer sperimentale; sono verificati con asset offline e fixture sintetiche,
  non in TDS. Legni, arco, candelabri e dettagli Godot restano da collegare
  al percorso live.
- **Luci:** nove candidate nel 2864, cinque omni e quattro spot. Posizioni e
  identità restano candidate; colore, intensità, raggio, direzione, cono e
  stato non sono tutti affidabili. Il probe attuale pubblica copertura parziale
  e non abilita il PBR verificato.
- **Rendering:** PBR/HDR diretto e shadow map esistono nel consumer e sono
  testati con luci sintetiche. GI CPU per vertice, GTAO, volumetrici, SSR e
  temporal sono anteprime separate; non sono una pipeline realtime validata.
  La correzione della trave non è dimostrata.

## Verifiche e riproduzione

Al checkpoint più recente: 21 test host live, sette test di replay delle
catture e 14 test del runner simulato passati; successivamente 16 test del
catalogo passati. Il proxy x86 MinGW e le fixture DX12 sono compilati. La
fixture Highball/DX12 ha verificato lifecycle e renderer sintetici; il loader
Windows ha accettato sottoinsiemi **offline modificati** con 45 BSP/324
candidati e con zero BSP/324 candidati, rifiutando copie corrotte. I messaggi
di avvio del runner nei test erano mock, non un avvio di TDS.

Dal checkout `source/`, con toolchain disponibile:

```sh
python3 -B -m unittest discover -s t3render/tests -p 'test_scene_live*.py'
python3 -B -m unittest discover -s t3render/tests -p 'test_replay_scene_live_capture.py'
python3 -B -m unittest discover -s t3render/tests -p 'test_tds_bedroom_batch.py'
```

I test sulle catture e sugli asset locali richiedono variabili e file sotto
`build/`: senza questi input vengono saltati. Non trattare i test sintetici
come prova di integrazione live o di prestazioni.

## Contenuto dello zip

`images/` contiene screenshot del gioco e preview storiche; `reports/` contiene
prove e risultati selezionati; `source/` contiene sorgenti tracciati e licenze.
`MANIFEST.json` e `SHA256SUMS` indicano provenienza e integrità. **Non sono
inclusi** trace grezze, salvataggi, cataloghi binari o asset originali del gioco.
Per eseguire il confronto completo servono copie locali autorizzate di tali
input. Screenshot e preview sono condivisi nello zip su richiesta dell'utente.
