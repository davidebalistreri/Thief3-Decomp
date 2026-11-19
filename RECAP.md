# Thief: Deadly Shadows Remaster — stato al 19 novembre 2026

## Obiettivo e stato

L'obiettivo immediato è una bedroom renderizzata dal consumer D3D12 live,
seguita da un confronto nella stessa sessione fra originale D3D8, D3D12 flat
e D3D12 con `stone_wall_v2`, `wood_floor_v2`, `bed_high` e una candela
runtime. La fase successiva è in pausa fino alla verifica visiva di v0.1.

## Traguardi

- **M1 — movimento live:** nella sessione del 19 novembre il viewer ha
  prodotto 1.711 frame consecutivi in 49,201 s, con spostamento fino a
  665,22 unità T3 e rotazione fino a 124,99°. Il letto candidato è uscito
  cinque volte dalla selezione e rientrato quattro volte. Sono disponibili
  15 coppie D3D8/D3D12 distribuite lungo il movimento, con camera e viewport
  collegati allo stesso Present; non si osservano desync nei dettagli
  confrontabili.
- **Confronti M1:** nelle sette pose con letto selezionato l'IoU è
  99,989–100% rispetto a una rasterizzazione CPU delle geometrie candidate,
  non a una segmentazione del letto D3D8. Tre BSP candidati sono stati
  confrontati tramite bordi e texture proiettate; identità e binding GPU
  restano candidati e il confronto indipendente dei bordi nell'originale è
  forte solo per uno dei blocchi esaminati. Mancano ancora oggetti e
  superfici nel viewer. M1 è quindi verificata per la prova e i campioni
  descritti, non per l'intera stanza.
- **Misure M1:** nell'intervallo di telemetria D3D8 sono stati osservati
  1.669 Present in 47,615 s, pari a 35,05 Present/s e 28,53 ms medi fra
  Present; con proxy e diagnostica attivi non sono tempi GPU né un benchmark
  finale. Nel viewer: 0–1 mesh, 1–13 BSP, 2–25 draw e 38–562 triangoli.
- **Remaster Live v0.1:** catalogo, toggle flat/remaster e acquisizione del
  confronto sono preparati. L'ancora per gli override viene conservata quando
  il letto esce dalla selezione, soltanto nella stessa continuità. La candela
  usa campioni runtime candidati; posizione e spegnimento sono stati osservati
  in prove precedenti, mentre intensità e raggio sono calibrati artisticamente.
  Il confronto completo originale/flat/remaster nella stessa posa, inclusi
  candela accesa e spenta, **non è ancora verificato live**. I campioni non
  dimostrano da soli i binding GPU.

Displacement della parete e GI realtime vengono dopo una v0.1 visivamente
convincente. Nessuna priorità attuale all'ottimizzazione delle prestazioni.

## Checkpoint e verifiche

Il repository è sul branch `remaster/vertical-slice`. Il checkpoint WIP
iniziale è `de6a3831`; il codice successivo è congelato in `c7bb3f78`
(`chore(t3render): checkpoint WIP remaster v0.1`). Sono commit locali, non
pubblicati. Il repository Godot annidato in `build/assets/godot/remaster`
è separato, senza remote, e rimane al commit `746815f`.

Prima della pulizia è stata verificata la build del target
`t3render_experimental_combined_proxy`. Sono passati i test mirati di
`scene_live`, materiali e coordinamento v0.1, più 117 test di cattura,
runner, riepilogo trace e anteprima candela. Il confronto materiali è stato
rieseguito anche sulla scena catturata del frame 1799: 16 sezioni e 67
triangoli confrontati. Questi controlli non sostituiscono la prova live v0.1.

## Evidenze e pulizia

Le evidenze complete della sessione M1 sono in
[DIAGNOSTICA-M1-2026-11-19.zip](../DIAGNOSTICA-M1-2026-11-19.zip): report
finale, 15 coppie di immagini, analisi, scene, trace e log. L'archivio è stato
verificato con `unzip -t`; SHA-256:

`bbe3c55e53d7404c59df787b9e22aae863bde2e328fd748d63055e835fefda2d`

La cartella delle catture, le build rigenerabili, la cache degli asset, il
salvataggio candidato scaricato e le cache Python sono stati rimossi. Sono
stati conservati `build/assets/backup`, che contiene asset originali, e il
repository Godot annidato. Alcuni documenti storici tracciati possono ancora
contenere link a output di `build/` rimossi.

## Ripresa

1. Ricreare la build MinGW prima della prossima prova, dalla directory `t3sdk`:

   ```sh
   cmake -S t3render -B build/t3render-mingw -G Ninja \
     -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_BUILD_TYPE=Release \
     -DCMAKE_C_COMPILER=i686-w64-mingw32-gcc \
     -DCMAKE_CXX_COMPILER=i686-w64-mingw32-g++ \
     -DT3RENDER_BUILD_EXPERIMENTAL_COMBINED_PROXY=ON
   cmake --build build/t3render-mingw --target t3render_experimental_combined_proxy
   ```

2. Preparare una sessione con salvataggi isolati sul display integrato del
   Mac. Acquisire originale D3D8, D3D12 flat e remaster nella stessa posa
   e sessione; verificare l'effetto dello spegnimento della candela e
   ispezionare i risultati. Non usare Computer Use per la prova.
3. Solo dopo valutare parete e GI secondo la roadmap. Le istruzioni del
   runner e del confronto sono in `t3render/tools/README_tds_bedroom_batch.md`.
