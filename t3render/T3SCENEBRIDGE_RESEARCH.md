# T3SceneBridge: ricerca statica iniziale

La sezione iniziale descrive la ricerca statica del 14 novembre 2026, non uno
snapshot runtime né una Scene IR. La successiva
[osservazione runtime](#probe-runtime-bedroom-frame-2661) documenta il probe
e i suoi limiti; il [frame 2722](#probe-runtime-e-snapshot-bedroom-frame-2722)
contiene la prima selezione runtime con geometria asset-backed. Il target è il
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

## Elemento Flesh, actor e risorsa mesh: evidenze statiche

Analisi aggiuntiva sullo stesso binario SHA-256 sopra; non è una verifica
runtime. Per gli oggetti creati dal percorso individuato, il collegamento
bidirezionale è più forte di una corrispondenza per posizione:

1. `0x1097a990` riceve l'actor e legge `actor+0xb4`. Nel ramo in cui il
   getter `0x10991eb0` trova una mesh e il campo è nullo, alloca un oggetto
   di `0x128` byte e chiama il costruttore `0x10976b20`; a `0x1097aaa3` e
   `0x1097aaac` scrive rispettivamente `actor+0xb4 = oggetto` e
   `oggetto+0x54 = actor`. Un altro ramo ripete gli store a `0x1097aee8` e
   `0x1097aeef`. Il costruttore scrive vtable `0x10e4e8c0` e tipo 4 a
   `oggetto+0x44` (`ev_a7a92347de2d97fa82f9b2d3d2bfa8c5ae70a209e7b1d660b3774b37187ebeb8`,
   `ev_8b4dcfe898fc3e74147a5c41c87146d059ab9b326059694787b48d09b47552af`).
2. `0x109209e0` inserisce l'oggetto nel registro `0x10f2c740+0x30`;
   `0x1097afb0`, durante la rimozione, ricerca proprio `actor+0xb4` nella
   lista di `0x10f2c734+0x50`, lo rimuove se presente e azzera entrambi i
   collegamenti prima dell'eventuale rilascio. Questo collega la famiglia di
   oggetti actor alla stessa raccolta percorsa da `0x10926800`, non dimostra
   che un dato actor vi sia presente in un determinato frame
   (`ev_6f7e01f7e2098843809d33f6288565f4fab07662634397f861ced9baa2303790`,
   `ev_99ce64640e1e5fdd6e2d7fd0d47c0b4dfc098f9b1fe98c40bc26e70d2bd8b9f3`).
3. Sempre in `0x1097a990`, una chiamata a `0x109769f0` usa
   `actor+0x2c` (Location) e una matrice 3×3 ricavata dalla virtuale
   dell'actor per costruire 16 float a `oggetto+0x04`. La traslazione è a
   `oggetto+0x34`, `+0x38`, `+0x3c`; la conversione completa della matrice
   e il suo eventuale uso nei singoli pass devono ancora essere verificati
   (`ev_d2baa4a4de38c510b7611c27d3c7b2b89b12704120630237f64ec29ef5257ce7`).

Il getter mesh `0x10991eb0` richiede a `0x1098e330` la proprietà
`0x4002002b`. Il tipo atteso è `UStaticMesh`: il suo costruttore a
`0x10971ca0` imposta vtable `0x10e4dfa8`, mentre
`UStaticMesh::Serialize` (`0x10972150`) legge il nome asset, richiede una
risorsa di tipo 4 al resource manager e salva il risultato a `proxy+0x58`.
Per la classe di risorsa il costruttore `0x10dc8020` usa la vtable
`0x10ec0e20` e passa il nome al costruttore base `0x10dc6b50`, che lo copia
nel `cString` a `risorsa+0x08`. Il metodo `0x10dc6e20` usa quella stringa
per ottenere il basename senza estensione; la lunghezza della stringa è
memorizzata quattro byte prima del puntatore ai caratteri
([cString](../include/Game/cString.h)). I nomi vanno letti con limiti e
conservati prima della normalizzazione; un puntatore, una classe o una vtable
diversi restano `unknown`. Evidenze:
`ev_243849bfa153cfc23202956a948a8be087dade2e46c1a9fe59eb2f3df4a307fe`,
`ev_e09a547735faeb39515a0f2f861479594745d2ccd1447458760283cd6bff3196`,
`ev_6ef34c6ea2c9d5b0110d262452adf1baaebfc376d1b07c9633b6c9d3db9d20bb`,
`ev_0ed267532bcff8f303f2e2ae03f1d5aa6f043595f11c3d57628d68ebfcf17ad3`,
`ev_3b20d6ba2de2b0f7c6138a48e86845ac969165ec855f9884a9cb83c814e97555`,
`ev_6899ba4f98f68e6d19dd80178000f5b1e553569ed82fd420805e1c866c6c7b76`.

Nel GMP offline verificato, l'actor `StaticMeshActor__1520` è export 119 e
la sua proprietà `StaticMesh` punta all'export 1406, chiamato
`StaticMesh__3203`; quest'ultimo contiene `INNbed1A1`, mentre `ObjectMesh`
dell'actor contiene nome `INNbed1A1` e skin `SQ1int`. Sono valori offline,
non prova che il save runtime conservi la stessa proprietà o lo stesso nome.
Il riscontro del frame 3377 sopra associa con forza la geometria al GMP, ma
non prova ancora questa relazione actor/property durante l'esecuzione.

Per un probe strettamente read-only, `0x10ad3df0` mostra come cercare
direttamente il valore della proprietà nel property hash: `actor+0x28`
identifica otto bucket `u16`; per ID `0x2b` si parte dal bucket 3 e si
seguono slot da 8 byte nel pool `0x10f7b5c8`: `next:u16`,
`id_e_flag:u16`, `value:u32`. L'ID è `id_e_flag & 0x7fff`; con bit
`0x8000` il dato è inline a `slot+4`, altrimenti `slot+4` punta ai dati.
Il pool e i suoi limiti sono documentati in [handoff](../docs/handoff.md).
La lettura è una *candidata* per la proprietà dell'actor, non una promessa di
risolvere tutte le proprietà ereditate. In particolare, il getter effettivo
chiama un metodo virtuale del descrittore; l'implementazione di
`UObjectProperty` a `0x10aeecc0` ha un ramo che può creare oggetti quando
il flag `+0x3c & 0x400000` è impostato. Non chiamare il getter dal hook per
la sola osservazione (`ev_ad3680e2ab4b5c6237b12c09a61c7f51acf925d2de164369729f586d673f3568`,
`ev_a9e56ae2de5ba8b765bff5ee1ed087806e901e18e3278ce5884cc05cb56e5ec7`).

Una futura prova deve limitare profondità e ciclo delle liste, controllare
build e vtable, risolvere l'actor tra gli UObject vivi e verificare
`actor+0xb4 == elemento` nello stesso callback prima di leggere i campi;
validare analogamente tipo/vtable/array e nome della mesh, rileggere dati
critici e segnalare esplicitamente ogni dato incompleto. Per interpretare il
valore raw come equivalente alla copia del getter vanno inoltre verificati il
descrittore, il tipo, la dimensione e il flag sopra. Servono marker di pass,
thread, generazione e relazione esplicita con `Present`: puntatori riusati,
reload, concorrenza e una callback in un altro frame invalidano la prova.
Non serializzare i puntatori nella Scene IR.

## Probe runtime bedroom: frame 2661

La campagna `scene-bridge-bed-20261115-001` ha prodotto una trace locale
completa con SHA-256
`51e097188c226c5cb6137115bc1ad1ce2e13537deea3a031df50d06624f9e9f2`.
Nei frame 2661–2663 il probe è installato e i due campionamenti per frame
ricadono nello stesso intervallo fra `Present` e sullo stesso thread dei draw;
non sono riportati overlap o Reset. Questo stato non prova, da solo, che una
camera o un actor siano associati a uno specifico draw. Il probe usato per
questa cattura sopprimeva gli ingressi ricorsivi senza segnalarli: l'assenza
di ricorsione non è quindi dimostrata. Questo limite non invalida le altre
osservazioni riportate.

Nel frame 2661, al pass 0, le scansioni bounded riportano un solo
`StaticMeshActor__1520` nella tabella osservata e una sua sola occorrenza
nella lista di 47 elementi. Membership, backlink, classe e letture ripetute
sono coerenti; la location è `[3398, 1513.9998779, 272]`, il rotator
`[0, 0, 0]`. La proprietà **locale** letta senza getter porta al nome risorsa
candidato `innbed1a1`, uguale a `INNbed1A1` senza distinguere maiuscole.
Restano possibili modifiche concorrenti non intercettate e riuso degli
indirizzi; la ricerca non risolve le proprietà ereditate. Nel pass 1 la lista
osservata ha 4 elementi e non contiene il backlink del letto.

I draw 8/9, successivi alla callback del pass 0, hanno world bit-identica alla
matrice dell'elemento e view/projection bit-identiche ai campi camera `+0xe4`
e `+0x64`. I 14 draw candidati associati offline alle due sezioni hanno
posizioni, UV0 e indici delle shadow identici ai dati `INNbed1A1` letti da
`t3mesh.py`; la skin `SQ1int` fornisce rispettivamente i materiali
`GENgoodnessA_D` e `GENiFABRICwoolA2_D`. La skin e i nomi dei materiali qui
provengono dall'asset offline; la trace non è un readback GPU e non dimostra
che una specifica chiamata dell'actor abbia emesso quei draw.

La camera configurata e quella corrente coincidono nel probe, mentre il
puntatore alternativo di culling è nullo. Il viewport conservato nella camera
è `[0, 0, 1024, 768, 0, 1]`; quello dei draw 8/9 è
`[0, 0, 1024, 768, 0, 0.5]`. Non sono uguali. La proiezione catturata è
compatibile con la convenzione D3D a profondità clip `0..1`, near 1 e far
circa 32000; l'inversione numerica dei coefficienti float dà circa 32019.

**Spiegazione statica candidata del viewport.** Nel binario verificato,
`0x10926f20` chiama `GetViewport` a `0x10927126`, copia il risultato,
sostituisce MinZ e MaxZ con i campi `+0x10` e `+0x14` dell'argomento di
configurazione e chiama `SetViewport` a `0x10927177`. La call al probe è
successiva, a `0x109271a9`, e ritorna a `0x109271ae`. Questo percorso può
quindi modificare la profondità sul device senza modificare la copia nella
camera. La trace osserva i viewport diversi, ma non intercetta la `SetViewport`,
il suo HRESULT o i valori dell'argomento: associare quella specifica chiamata
al valore `0.5` è un'inferenza, non un evento runtime catturato. Evidenza
statica delle istruzioni: `ev_086a364883326e32434d954f8ee5b036d2faa7acabd574ee30f65fb82dc11c98`.

Con il percorso del gioco in `T3RENDER_GAME_DIR`, dalla root di `t3sdk`:

```sh
python3 -B t3render/tools/analyze_scene_bridge.py \
  build/t3render-captures/scene-bridge-bed-20261115-001/bedroom.jsonl \
  --frame 2661 --draw 8 --pass 0
python3 -B t3render/tools/correlate_trace_actor.py \
  build/t3render-captures/scene-bridge-bed-20261115-001/bedroom.jsonl \
  build/assets/godot/SouthQuarter1_int1/SouthQuarter1_int1.actors.json \
  SouthQuarter1_int1 --game-dir "$T3RENDER_GAME_DIR" \
  --actor StaticMeshActor__1520 --frame 2661
```

## Nodo mesh e prossima osservazione

Nel binario verificato, il percorso di `0x1097a990` passa il nodo ottenuto
tramite `0x10932e00` a `0x10dcaf90`; quest'ultima lo conserva a
`elemento+0x108`. Il nodo appartiene al pool `0x10f31bb8`, con nome a `+0xb0`
e vtable costruita a `0x10e49d14`. L'analisi statica di `0x10944f00` e
`0x10944a80` collega i campi del nodo `+0x04/+0x08/+0x0c/+0x10` ai conteggi
vertici/triangoli/indici/sezioni del render blob, e `+0x74..+0x88` ai bounds.
Le relative evidenze sono `ev_bc56dba55e2ccd81479f5f7a36bae6870d07ce58f58ad459048657dc143f754b`,
`ev_474aa90122148530942929d5d517ea2a5a7aad28a1a1f123d8a56bf0ff460175`,
`ev_4fde95416af3a2feef605f2aaaf90b8d477c8d03ad3786fa2836f38a0abd870a`,
`ev_426f8a79dc5e9c8a0fe652780b254a7227cb44cc6a5df394d298e809bbf6285b`.

Il nodo riferisce le sezioni a `+0x30` con stride `0x24`; `0x10dcaf90`
inizializza un array di materiali per istanza a `elemento+0x104` e
`0x10dcae20` ne aggiorna i nomi. La lettura proposta controlla appartenenza
al pool, limiti, backlink, vtable, stringhe, intervalli e riletture. La copia
è limitata a 16 sezioni; dati mancanti o troncati rimangono sconosciuti.
L'evidenza statica delle assegnazioni materiali è
`ev_7dc9fdf1925b16214d47ac728b8d009feb5188a56983afc2d1bc586d8b30b00a`.
Queste erano le garanzie disponibili prima della cattura successiva: la
verifica sul salvataggio è descritta nella sezione del frame 2722.

Per il BSP, `0x10a23930` passa `*0x10f2c740+0x0c` a `0x10947fc0`, che
verifica la firma `VBSP`; `0x10947670` costruisce un array di oggetti
`Class_10E4A668` con stride `0x1ac`, salvandolo a `VBSP+0x54`. La lista del
vecchio frame 2661 contiene nove voci di tipo 5 e vtable `0x10e4a668`, ma
quella cattura non ha controllato che appartenessero a tale array. Il nuovo
probe verifica questa appartenenza direttamente, campiona conteggi, matrice,
bounds e sezioni dei membri osservati, e limita la copia a 16 blocchi e 16
sezioni per blocco. Gli indici runtime restano candidati rispetto all'ordine
euristico del parser offline, non identità dimostrate. Evidenze:
`ev_08d510ab1dcbdab31547d4f0c050bebe2b69c6350daa55ad17671da762f7123b`,
`ev_30ca74e6783cac94c59286a61615a234b60b9dd8f2ce2deeb73e01577bb1778b`,
`ev_782dfbb3c9e62cea95c4155c7f94e0e056282f10973f2c2070ce3bb4b72239bb`.

Il probe registra anche la catena bounded della proprietà locale, compresi
descrittore, slot e collegamenti, nella trace diagnostica; gli indirizzi
transitori non sono identificativi Scene IR. Il controllo della finestra per
uno snapshot richiede esplicitamente il rilevamento di ricorsione: nel frame
storico 2661 tale controllo è `unknown`. Il nuovo
[confronto con la fixture](tools/README_bridge_runtime_checkpoint.md)
riporta separatamente questa limitazione, uguaglianza numerica/bit della
trasformazione e i dati mesh o BSP mancanti. Non produce uno snapshot runtime
né considera validato il bridge sulla sola base di nomi o conteggi.

## Probe runtime e snapshot bedroom: frame 2722

La cattura `scene-bridge-runtime-20261115-001` ha SHA-256
`d926202c7dbdcd8c8717a6e901d8a321d255fe97b2d9715ef291493b95734f37`;
lo screenshot `bedroom.jsonl.frame-2722.png` è stato acquisito prima dello
stesso `Present`. L'osservazione di `0x10926800` è nel pass 0, tra due
`Present` osservate sul thread 1348, senza Reset, overlap o ricorsione
rilevati. La view derivata e la projection coincidono bit per bit con il draw
8. Il viewport della camera ha depth `0..1`, quello del draw `0..0.5`.
L'inverso della view dà una posizione circa `[3571.6482,1576.4877,353.8444]`.

Una sola voce della lista è collegata tramite membership e backlink a
`StaticMeshActor__1520`; l'audit della proprietà `StaticMesh` è coerente e il
nodo collegato porta il nome `innbed1a1`. La matrice runtime ha traslazione
`[3398,1513.9998779,272]` e coincide numericamente con la fixture, a parte
il segno di alcuni zeri. Conteggi 257 vertici/708 indici/3 sezioni, bounds e
materiali `BF`, `GENgoodnessA_D`, `GENiFABRICwoolA2_D` coincidono. Non è
stato osservato un ID risorsa runtime qualificato né il nome della skin.
Per le due sezioni visibili del letto, i draw 8/9 successivi all'osservazione
hanno posizioni, UV e indici identici all'asset nelle shadow dei lock; ciò
non dimostra che i draw siano nel pass del probe, che l'attore osservato li abbia emessi, né
che il contenuto inviato alla GPU coincida con la shadow.

Nove voci della lista appartengono all'array runtime `VBSP`; il confronto
incrocia **tutti** i candidati stabili con tutti i 45 blocchi offline, senza
assumere identici gli indici. I tre confronti seguenti sono univoci per
conteggi, bounds, sezioni, materiali, intervalli di indici e contenimento degli
indici negli intervalli di vertici osservati. SceneIR non conserva gli
intervalli originali di vertici: il contenimento non è un confronto esatto
di quegli intervalli. Le sezioni hanno anche VB/IB delle shadow identici al
GMP nello stesso frame e thread, con la stessa ultima osservazione del probe:

| Indice VBSP runtime | Indice scanner offline | Draw delle due sezioni |
|---|---|---|
| 7 | 5 | 5, 6 |
| 9 | 7 | 62, 63 |
| 21 | 19 | 45, 46 |

Questo è un matching per metadati più confronto di contenuto indipendente,
non una prova del collegamento diretto tra i payload e le voci VBSP. I limiti
di scansione, i campi mancanti e le corrispondenze multiple impediscono una
selezione univoca nel produttore.

Il produttore [runtime snapshot](tools/build_bridge_runtime_snapshot.py)
usa la selezione, camera e transform osservate dal probe; usa invece geometria
e ID della fixture offline, solo dopo i confronti sopra. Emette
`source.kind=runtime`, frame e fingerprint della trace, e un manifest con la
provenienza per componente, i draw e la mappa degli indici. La sezione `BF`
non è stata confrontata ed è esclusa dalla preview; anche skin, ID qualificato
runtime, luci e visibilità completa rimangono sconosciuti. La procedura è in
[README_bridge_runtime_checkpoint.md](tools/README_bridge_runtime_checkpoint.md).

Il consumer DX12 ha renderizzato 340 triangoli dello snapshot in
`build/t3render-offline/bedroom-runtime-2722.png`; il confronto locale
`bedroom-runtime-comparison-2722.png` accosta il D3D8 sincronizzato e il
render diagnostico. Letto e pareti sono riconoscibili, ma la scena è parziale,
con colori piatti, culling/depth approssimati e senza illuminazione o texture.
Non si afferma parità geometrica completa o validazione definitiva del bridge:
il legame diretto fra risorsa runtime, payload e draw resta da dimostrare.
