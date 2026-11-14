# T3Render / DX12 — recap per l'agente remoto

Aggiornato il 14 novembre 2026. Questo documento riprende la milestone «replay
DX12 multi-draw e facade D3D8 sperimentale; RGB sintetico coincidente; 171 test
passati» e sostituisce il precedente recap diagnostico dove descrive la facade
come non integrata o gli observer come necessariamente separati.

## Obiettivo e stato pratico

L'obiettivo immediato è **un frame reale della bedroom di Garrett catturato da
TDS e riprodotto offline in DX12, inizialmente legacy-compatible**, con un'immagine
D3D8 sincronizzata per il confronto. Geometria/camera vengono prima di PBR,
replacement, lighting e GI. Godot rimane una golden reference; non serve
continuare il pass visivo lì. Lo stealth model originale deve restare separato.

**La bedroom non è ancora ricostruita in DX12.** Il progetto ha superato
diversi ostacoli di cattura, ma nessuna delle catture reali contiene dati
sufficienti a riprodurre l'inquadratura. Non esistono ancora uno screenshot
D3D8 sincronizzato o un confronto pixel-to-pixel del frame TDS.

Repository: https://github.com/davidebalistreri/Thief3-Decomp, branch
`remaster/vertical-slice`. La base pubblicata prima dell'automazione era
`39dc01d8`; usare l'HEAD del branch per il codice più recente. `origin` punta
al fork dell'utente e `upstream` a `Veradictus/Thief3-Decomp`.

```sh
git clone --branch remaster/vertical-slice https://github.com/davidebalistreri/Thief3-Decomp.git
cd Thief3-Decomp
git log -1 --oneline
```

## Evidenze e limiti

- Il probe DX12 offscreen è stato **eseguito** su Highball: device, queue,
  render target, shader e readback producono pixel. Replay sintetici D3D8/DX12
  coincidono in RGB; il confronto non prova parità TDS o Windows nativa.
- La facade combinata è integrata nel proxy **solo dietro flag sperimentali**
  ed è stata avviata reversibilmente in TDS. Osserva nella stessa draw VB/IB,
  dichiarazioni/programmi vertex, costanti, matrici e stati quando disponibili.
  Identità COM, alias, concorrenza e gameplay non sono attestati; l'utente ha
  segnalato testi troncati nei menu e la causa non è stata isolata.
- Un singolo quad candidato derivato da TDS è stato visualizzato in DX12 su
  sfondo nero: [manifest](benchmarks/d3d12/2026-11-14-tds-quad-diagnostic/result.json).
  Non è identificato, usa un'interpretazione FVF non verificata e omette il
  resto della scena. Non è il replay della bedroom.
- Il lettore riconosce `vs_1_0` e `vs_1_1`; alcuni programmi osservati
  coincidono byte per byte con shader del gioco. Non è prova del programma o
  delle costanti effettivamente consumati dalla GPU. Nel [frame precedente](benchmarks/runtime/2026-11-14-tds-vs10-bedroom/result.json),
  una prova CPU dei 18 draw colorati con componenti candidati ha collocato
  tutte le loro triangolazioni fuori dai piani di clip: non attribuirli alla
  stanza visibile senza ulteriori verifiche.
- Il confronto Highball sui buffer riscritti dopo `Unlock` ripetuta verifica
  le sole porzioni osservate e un pixel sintetico; il confronto RGB D3D8/DX12
  del [manifest](benchmarks/d3d8/2026-11-14-repeat-write-recovery/result.json)
  riguarda un draw sintetico separato, non quel buffer e non TDS.

## Ultime catture reali

Sono sessioni diverse, non immagini sincronizzate né necessariamente la
stessa camera. L'utente ha confermato la bedroom e l'inquadratura verso il
letto. Il proxy temporaneo è stato rimosso dopo ogni prova.

| Frame / manifest | Dati osservati | Conclusione |
|---|---|---|
| [1034](benchmarks/runtime/2026-11-14-tds-abandon-provenance/result.json) | 746 draw, 737 indicizzate stabili; 539 payload vertex e 390 index abbandonati. I 539 vertex usano un solo buffer managed WRITEONLY da 2.621.440 byte. 18 draw colorati hanno VB/IB, declaration, programma e costanti candidati. | La prima causa registrata per tutti quei payload è la policy sperimentale dopo `Unlock` ripetuta. Questo identifica lo scarto dell'observer, non il contenuto del driver. |
| [971](benchmarks/runtime/2026-11-14-tds-repeat-write-recovery/result.json) | 744 draw, 735 indicizzate con finestra buffer non disponibile; primo fallimento al controllo del risultato di `BeginLock`. | Il recupero ha fallito in questa sessione. La traccia non conserva i parametri del lock né il risultato rifiutato, quindi la causa precisa è ignota. |
| [1040](benchmarks/runtime/2026-11-14-tds-repeat-lock-range/result.json) | 354 draw, 345 indicizzate stabili; 303 vertex e 344 index `unknown_resource`; 338 dichiarazioni con `reset_attempt` precedente; nessun payload marcato come riscritto dopo `Unlock`. | Non dimostra che la correzione abbia risolto il frame precedente né che il recupero funzioni sui buffer della stanza. La causa individuale delle risorse non tracciate non è provata. |

La perdita di token durante Reset è una spiegazione plausibile delle risorse
non tracciate, non una prova per ciascun buffer. Non è dimostrato che il
cambio di focus provochi il Reset. La cronologia shader pre-Reset, quando
abilitata e abbinata a Reset/rebind osservati, resta una **ipotesi** separata.

## Modifiche recenti

- `6286746d`: policy opt-in che, dopo una `Unlock` ripetuta riconosciuta,
  scarta la shadow del solo buffer e conserva le altre; non presume che la
  `Unlock` sia innocua. `7c3df3cf` registra la prima causa di abbandono.
- `8ee2c28d`: storico declaration/program pre-Reset nell'observer combinato,
  esposto solo come ipotesi con limiti di memoria e controlli di rebind.
- `645f2e95`: `T3RENDER_EXPERIMENTAL_REPEAT_RECOVERY=1` avvia una nuova shadow
  interamente sconosciuta al successivo lock osservabile; solo scritture
  successive e completate diventano candidati. Non recupera byte precedenti.
- `edb007a5`: un lock ambiguo su una shadow già scartata non avvia una nuova
  epoca; un fallimento del registro registra risultato e range. Il test TDS
  successivo non ha isolato la causa del fallimento precedente.
- Suite host completa: 348 test passati; i dieci test mirati del batch sono
  passati anche dopo l'ultima modifica. ASan/UBSan e fixture Highball sintetica
  sono passati per la correzione. I test non dimostrano parità della facade
  durante il gioco.

## Automazione senza continui cambi di finestra

Il [runner temporizzato](tools/README_tds_bedroom_batch.md) prepara un'unica
sessione Wine, eventualmente esegue prima suite e fixture sintetica, poi
cattura tre frame consecutivi e produce `summary.json` senza byte del gioco.
Il timer consente all'utente di caricare il save e restare nel gioco; la scena
non viene riconosciuta automaticamente e va confermata. I tre frame usano la
**stessa configurazione**, non sono un A/B tra facade diverse. Il runner è stato
provato con processo simulato e l'analizzatore su tracce TDS esistenti: **non
è ancora stato collaudato con TDS reale**. Il desktop Wine è meno invasivo del
fullscreen diretto; non è necessario che il gioco catturi in background.

## Prossimi passi richiesti

1. Valutare ed eseguire il batch una sola volta con l'utente già pronto a
   caricare il save. Analizzare insieme i tre frame, distinguendo dati
   osservati, ipotesi pre-Reset, errori di range e risorse non tracciate.
2. Individuare nella vita reale dei VB/IB cosa avviene prima del frame: Reset,
   alias e lock effettivi. Estendere diagnostica o fixture solo per il caso
   rilevato. Non restaurare byte incerti o dedurre lo stato GPU dal solo
   successo della `Unlock`; verificare anche il problema dei testi del menu.
3. Quando un frame ha dati coerenti, aggiungere cattura immagine D3D8
   sincronizzata e replay DX12 legacy, poi correggere geometria, camera,
   depth/stati, shader e texture contro lo stesso frame.
4. Solo dopo una base ripetibile e confrontabile, introdurre PBR, mesh
   replacement e illuminazione moderna, senza modificare la stealth logic.

I manifest sotto `benchmarks/` sono nel repository. Le tracce grezze e le IR
parziali sono locali in `build/t3render-captures/`, contengono dati derivati
dal gioco e indirizzi transienti e non sono nel repository. Il riepilogo del
batch è privo di payload e indirizzi; controllarlo prima di condividerlo.
