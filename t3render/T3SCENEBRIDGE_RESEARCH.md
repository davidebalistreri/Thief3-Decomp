# T3SceneBridge: ricerca statica iniziale

Stato al 14 novembre 2026. Questa è ricerca sul binario, non uno snapshot
runtime né una Scene IR. Nessun hook è stato installato. Il target è il
`T3Main.exe` Steam x86 descritto in [target.md](../docs/target.md), SHA-256
`09e0be902062c53cbb1736cffa1bd3ec883bd5e864f8e7f3f99bc0d06f6ad0f8`.
I risultati REA sotto usano Hopper; sono statici e non attestano l'esecuzione.
Hopper non espone i confini completi delle funzioni e può mancare chiamate
indirette; indirizzi e pseudocodice vanno corroborati con il sorgente matched
e con osservazioni runtime.

## Candidato per il bridge

La catena statica individuata è:

```text
scene render 0x109fcf10
  -> 0x10917f80(pass, configurazione)
    -> Class_10924E80::0x10926f20
      -> 0x109231a0       [condizionale: costruzione/visibilità]
      -> 0x10926800       [condizionale: iterazione/submission]
```

`0x10917f80` assegna il pass a `0x10efef54` e chiama il metodo su
`DAT_10f2c734`, soggetto a guardie: vedere il sorgente byte-matched
[`Unsorted_10917F30.cpp`](../src/Game/Unsorted_10917F30.cpp). Le chiamate a
`0x109231a0` e `0x10926800` si trovano rispettivamente a `0x1092709b` e
`0x109271a9`; prima della seconda `ECX` riceve lo stesso oggetto in `ESI`.
La selezione è condizionale: la sola presenza della catena non garantisce che
essa venga eseguita in ogni frame o che una lista sia completa. Evidenze REA:
`ev_321eb3a67d273cf8a45c354d4e1176170714cbe0e99d0974ffd28ca006a430ae`,
`ev_26f6ccaf1a869c4fdb73b1706e27ad46372b3c45dd4c44fb58895de004271219`.

`0x109231a0` seleziona una camera, chiama `0x10917900` per testare bounds
contro sei piani e invia alcuni candidati a `0x10926230`. Le implementazioni
byte-matched di [`FUN_10917900`](../src/Game/Class_1091E400.cpp) e
[`FUN_10926230`](../src/Game/Unsorted_10926230.cpp) mostrano che quest'ultima,
quando il campo `+0x60` è zero, imposta il byte `+0x48` e aggiunge oggetti
univoci all'array del destinatario `+0x50` (count) / `+0x58` (data), salvo
quelli di tipo `+0x44 == 3`, inoltrati a un'altra lista. Non tutti i percorsi
di popolamento o i tipi sono decodificati. Evidenze REA:
`ev_8c4b94a1e3a0befcef04df4656774cd30a2628ea726f4ee2a03a240478a83670`,
`ev_a16b2b70591abdb3414094498e347e4ca8373529a7a13bd323b26ce9b8d9adbb`.

All'entrata di `0x10926800`, `ECX` è dunque un **candidato concreto** per
osservare questa raccolta prima delle chiamate virtuali della funzione. Il
codice percorre `this+0x50` elementi tramite `this+0x58`, calcola distanze
rispetto al vettore `currentCamera+0x34`, ed esegue successivamente lo slot
virtuale `+4` degli elementi con byte `+0x48` non nullo a `0x10926e35`.
Subito dopo aggiorna il contatore `Objects rendered`, registrato in
`0x10eff198`: ciò collega la lista a un'operazione di rendering, ma non prova
che ogni chiamata produca un draw D3D8, che ogni elemento sia un UObject, o
che la lista comprenda tutta la scena. Altri elementi sono trattati in rami
diversi. L'array `this+0x74` / `this+0x7c` è percorso prima; dopo una chiamata
virtuale a `0x10926a57`, ciascun elemento espone una seconda raccolta
`+0x120` / `+0x128`, con stride 8. La sua semantica resta da dimostrare.
La funzione modifica anche le selezioni/flag durante il proprio lavoro, quindi
la lista all'ingresso non equivale automaticamente agli oggetti effettivamente
emessi. Evidenze REA:
`ev_d22a7d0e1650cb59974f1679adfec875ebbcaa245633f3f6d5561cda1e295345`,
`ev_40fe44fa00b851712b2546f384895f1950512e9eae20e1a613f14c3d0fe8244d`,
`ev_93aff2d7bd47ed14ae4d2891d35c80a6cb824ef94791cd04cbb5a3481659549b`.

Il fatto che il punto preceda le chiamate virtuali di questa funzione non prova
che preceda *tutti* i draw del frame; può essere invocato per più pass. Il
producer `0x109231a0` usa anche una strada alternativa oltre ai sei piani.
Servono prove runtime prima di chiamare queste strutture inventario completo
degli oggetti visibili o usarle come contratto del renderer.

## Camera e frame

Il sorgente byte-matched di
[`SetCurrentCamera`](../src/Game/Unsorted_10919240.cpp) salva la camera in
`0x10f2c8c0`, imposta il viewport, ricostruisce la proiezione e aggiorna
altri stati. `0x109231a0` usa invece `0x10f2c8c4` per la selezione se non è
nullo, altrimenti `0x10f2c8c0`: la camera di culling e quella corrente
possono quindi differire. `0x10926800` legge `0x10f2c8c0` per le distanze.
Non assumere che i due puntatori coincidano.

In [`Unsorted_10917C90.cpp`](../src/Game/Unsorted_10917C90.cpp), `0x10917d20`
copia 64 byte in `camera+0x04`, assegna i campi `+0x180` e `+0x184` e
ricostruisce la proiezione; la camera qui è `0x10f2c738`, la cui identità
rispetto alle altre va verificata durante l'esecuzione.
[`0x10917f30`](../src/Game/Unsorted_10917F30_2.cpp), che nei pass mostrati
viene chiamata fra `0x10917d20` e `0x10917f80`, chiama
`SetCurrentCamera(0x10f2c738, 0x10f2c73c)`: per quel percorso collega
staticamente la camera configurata alla corrente e imposta l'eventuale camera
di culling. L'identità nel punto del probe e il pass effettivo restano da
osservare. Evidenza REA:
`ev_934b12bc1ca2a8b3766d4314a5062edaa217932d82bcdc17eedef2d9babcfdad`.
[`cCamera.cpp`](../src/Game/cCamera.cpp)
e [`cCamera_2.cpp`](../src/Game/cCamera_2.cpp) collocano la proiezione a
`+0x64`; la seconda copia anche il viewport. `0x1091e400` restituisce
`camera+0x124`, usato da
[`SetWorldTransform`](../src/Game/Unsorted_10919190.cpp) nel prodotto con
la world. I 64 byte a `+0x04` e la matrice a `+0x124` non vanno etichettati
come view senza verificarne orientamento e composizione.

In `0x109fcf10`, le coppie `0x10917d20` / `0x10917f80` compaiono per più
pass, per esempio a `0x109fd420` / `0x109fd438` e `0x109fd55a` /
`0x109fd572`. Il valore near 1 e far 32000 è documentato in
[`engine.md`](../docs/engine.md). Nel precedente batch, il frame 2245 ha 334
draw, di cui 262 con la stessa tripla view/projection/viewport: viewport
`[0,0,1024,768,0,0.5]` e proiezione con termini principali circa
`1.2060982`, `1.60813093`, `1.00003123`, `1`, `-1.00003123`, compatibili
con near 1 e far 32000 nella convenzione D3D. Sono stati osservati **10**
insiemi distinti: non c'è prova che il gruppo maggiore corrisponda a un
particolare pass Flesh o alle raccolte sopra, né di un'unica camera per frame.
Evidenze statiche: `ev_0f94369191edd2f3d2a8ceae5ab6ca650879d84476e4f4643590e6181f8bc538`,
`ev_ebd9dc7416dd1912cb9f32d8f987ac7335a6dc7ee1cea5aee8eda9a2a908c53b`,
`ev_e831056abdee18d5b91a8e6c3e87d7cbbf5d66ace75ac1f23c6bfb640f69aa5c`.

## Correlazione offline: candidato letto

Nel precedente batch bedroom confermato, frame 2245, le operazioni 8 e 9
presentano una world identity più traslazione `[3398, 1513.99988, 272]`.
Nell'export locale dell'intera mappa, `StaticMeshActor__1520` è l'unico
attore entro 0.1 unità da quella posizione: gruppo `GarrettsApartment`, mesh
`INNbed1A1`, skin `SQ1int`, rotazione `[0,0,0]`, scala 1. L'asset letto da
`t3mesh.py` ha 257 vertici e 708 indici; due sezioni hanno rispettivamente
160 triangoli, 210 vertici, primo indice 24 e 68 triangoli, 47 vertici,
primo indice 504, in accordo con le operazioni. La skin riporta i materiali
`BF`, `GENgoodnessA_D`, `GENiFABRICwoolA2_D`. La corrispondenza è un
candidato forte tra stati D3D8 ed export offline; non identifica ancora un
attore o asset runtime e non prova i dati realmente forniti alla GPU: i
buffer di quelle draw sono `unknown_resource`. I dati sorgente restano locali:
`build/t3render-captures/bedroom-batch-20261114-224853/bedroom.jsonl` e
`build/assets/godot/SouthQuarter1_int1/SouthQuarter1_int1.actors.json`.
Il verificatore trova 14 corrispondenze, 7 per ciascuna sezione, con HRESULT
draw e Present pari a zero. Con `T3RENDER_GAME_DIR` impostato sulla cartella
del gioco, dalla radice di `t3sdk`:

```sh
python3 -B t3render/tools/correlate_trace_actor.py \
  build/t3render-captures/bedroom-batch-20261114-224853/bedroom.jsonl \
  build/assets/godot/SouthQuarter1_int1/SouthQuarter1_int1.actors.json \
  SouthQuarter1_int1 --game-dir "$T3RENDER_GAME_DIR" \
  --actor StaticMeshActor__1520 --frame 2245
```

## BSP e letto nel frame sincronizzato 3377

Nella nuova campagna `bedroom-provenance-20261114-001`, il frame 3377 ha 340
draw e `Present` riuscita. Il record `frame_image` riporta immagine 1024×768
acquisita prima della stessa `Present`; il relativo file locale è
`bedroom.jsonl.frame-3377.png`. La scena è stata confermata come bedroom.
La trace ha SHA-256
`d869dd24450daf4553845cae7aa85b96856c5125eea5fdb094b0b944f75bc818`.

I draw 8/9 hanno ancora world e firme del letto sopra: i rispettivi 210 e 47
vertici della shadow hanno **posizioni e UV identici bit per bit** a
`INNbed1A1`, e gli indici u16 corrispondono. La risorsa offline usa vertici
di 64 byte, la shadow runtime di 32: non sono confronti byte per byte dei
record completi. Le chiamate e il `Present` hanno HRESULT zero; il matching
non identifica il puntatore di un actor runtime né dimostra il contenuto GPU
dopo l'invio. Il verificatore accetta la declaration packed identificata e
riporta `asset_payload_comparison` per ciascuna delle 14 corrispondenze:
posizioni, UV0 e indici risultano uguali in tutte. Il controllo è ripetibile con:

```sh
python3 -B t3render/tools/correlate_trace_actor.py \
  build/t3render-captures/bedroom-provenance-20261114-001/bedroom.jsonl \
  build/assets/godot/SouthQuarter1_int1/SouthQuarter1_int1.actors.json \
  SouthQuarter1_int1 --game-dir "$T3RENDER_GAME_DIR" \
  --actor StaticMeshActor__1520 --frame 3377
```

La scansione euristica di `SouthQuarter1_int1.gmp` produce 45 blocchi. Per
i seguenti, i draw indicati hanno world identity e view/projection/viewport
identici al draw 8. Le shadow di **entrambi** VB e IB coincidono byte per
byte con le rispettive sezioni del GMP: sono osservazioni delle copie dei
lock, non un readback GPU. Gli offset sono relativi all'export `Level`.

| Blocco / offset | Draw | Sezioni e bounds T3 |
|---|---|---|
| 5 / 163376 | 5, 6 | `STMwall512D_D`, `HSEfloorB_D`; `[3408,1408,272]`–`[3648,1680,480]` |
| 7 / 165945 | 45, 46 | `HSEfloorB_D`, `STMwall512D_D`; `[3392,1408,272]`–`[3408,1680,480]` |
| 19 / 190995 | 20, 21 | `HSEfloorB_D`, `STMwall512D_D`; `[3056,1440,272]`–`[3376,1696,496]` |

Il GMP usato ha SHA-256
`ed7d475b1efdc713d28b2a978e11fb0f662bdb17c5c2769d77c96e1f74b7ceba`.
La prova non usa l'ID numerico come identità: mantiene hash, indice e offset.
Con `T3RENDER_GAME_DIR` impostato sulla root del gioco, il confronto è
ripetibile dalla radice di `t3sdk`:

```sh
python3 -B t3render/tools/correlate_trace_bsp.py \
  build/t3render-captures/bedroom-provenance-20261114-001/bedroom.jsonl \
  "$T3RENDER_GAME_DIR/Content/T3/Maps/SouthQuarter1_int1.gmp" \
  --frame 3377 --reference-op 8 --block 5 --block 7 --block 19
```

Il verificatore richiede i binding e gli intervalli coerenti, shadow complete,
draw stabile e frame senza Reset, e non emette payload o indirizzi. Trova
7 corrispondenze per sezione dei blocchi 5 e 7, 4 per sezione del blocco 19.
Gli altri pass, il contenuto post-submit, depth, culling e occlusioni restano
da verificare prima di dichiarare la stanza renderizzata con parità.

Come controllo spaziale indipendente, assumendo convenzione D3D a vettori
riga, si inverte la view campionata: la camera è circa
`[3570.396,1577.226,353.697]`, dentro l'AABB del blocco 5. Proiettando
l'anchor del letto con view e projection dello stesso draw si ottiene NDC
`[-0.1490,-0.3193,0.99492]`. Intersecando l'asse di vista con i triangoli
offline, senza culling o mesh, il primo hit è la parete del blocco 7 a
`[3392,1483.536,307.392]`, distanza circa 206.754 unità. Questo delimita una
fixture candidata attorno alla camera, non prova visibilità o pixel: il
blocco 19, in particolare, può estendersi oltre una separazione della stanza.

## Prossimo probe read-only

Un probe opzionale può iniziare all'entrata `0x10926800`: catturare in modo
limitato `ECX`, pass `0x10efef54`, identità delle tre possibili camera,
conteggi e copie dei soli campi supportati, prima di dereferenziare gli
elementi. Il prologo nel binario verificato inizia con
`83 ec 30 a0 78 1a f3 10` (`sub esp, 0x30`; `mov al, [0x10f31a78]`), con
un unico riferimento diretto individuato all'entrata; REA non ha trovato
riferimenti agli inizi delle due istruzioni successive. Questo non certifica
da solo la sicurezza di un detour: vanno verificati nel processo build, byte,
integrità del prologo, trampolino, eventuali hook terzi e semantica di thread
e lifetime. Evidenze byte/xref: `ev_db457f7ee457d88ede7eddbc2ede99fd21e0d2178ed03d82eeefbf699ec7bc86`,
`ev_97517b3b732c21dc8cb6fd781526cb3d5f5c0b95e87ba5246db7323c00d59c7b`.

L'attuale thunk `AddCodeHook` dell'SDK salva registri generali e flags, non
esplicitamente lo stato x87/SSE; non lo si assume trasparente per un callback
arbitrario. Il probe deve preservare lo stato richiesto, essere disattivabile,
usare letture bounded e protette con esito esplicito, copiare soltanto dati
validati nel callback e non conservare puntatori oltre il loro lifetime.
Deve registrare thread, ordine e un marker condiviso con l'intervallo tra
`Present` della trace; una callback del message loop o un timestamp da soli
non dimostrano lo stesso frame. In caso di ambiguità, la correlazione resta
`unknown`. Non serializzare indirizzi o tipi D3D nella futura Scene IR.

Per chiudere la correlazione semantica servono: identificare la classe e il
proprietario degli elementi; ricondurre almeno uno di essi a un actor runtime
e al suo asset/transform/skin; risolvere sezioni e materiali con
`t3mesh.py`; quindi confrontare lo stesso pass e lo stesso intervallo di
Present con la trace. Un export della mappa, una posizione vicina o un
puntatore isolato non provano questi collegamenti.
