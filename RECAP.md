# Thief: Deadly Shadows Remaster — stato al 20 novembre 2026

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
  La comparativa live del 20 novembre soddisfa la checklist della posa:
  **v0.1 DONE per la scena acquisita**. Le intensità restano artistiche e i
  campioni non dimostrano da soli i binding GPU.

Displacement della parete e GI realtime vengono dopo una v0.1 visivamente
convincente. Nessuna priorità attuale all'ottimizzazione delle prestazioni.

## Verifica live v0.1 — 20 novembre

La [tavola comparativa finale](build/t3render-captures/remaster-v01-live-20261120-010825/remaster-v0.1-comparison.png)
proviene da una sola sessione nella bedroom: originale D3D8 e flat condividono
il Present 1406; remaster ON è il frame 1411 e OFF il frame 1713. Lo scarto
massimo fra ciascuna coppia di pose è 0,430 unità T3, con orientamento
identico a precisione float32. Il [report di verifica](build/t3render-captures/remaster-v01-live-20261120-010825/remaster-v0.1-review.json)
conserva i dati della comparazione, le impronte e i limiti.

Il letto high-poly con UV corretti è interamente in vista; parete e pavimento
usano gli override PBR. Pane, mobile, bersaglio, casse e due candelabri sono
visibili; nei due frame remaster non si vedono materiali diagnostici né errori
evidenti di geometria o UV alla risoluzione acquisita. L'utente ha confermato
di aver spento la candela sinistra senza cambiare posa: il suo candidato passa
da acceso a spento, la fiamma scompare e l'altro candelabro resta visibile e
modulato. Lo spegnimento opposto era stato osservato nei dati runtime di una
sessione precedente ed è coperto da fixture sintetiche; non è stato ripetuto
in questa cattura. Le intensità e i raggi sono calibrati artisticamente.

Il proxy acquisito ha SHA-256
`5b5c21b26a8c42f1f5a0edcefa05e4baec36af6c4c98a58b46cf346216358f60`;
il catalogo v10 per le casse ha SHA-256
`e42c20c82a0c9de34e765ae0cad4b6b3b94ceaec9c7fa99e3e78d11a5bdc130b`
nei controlli prima e dopo la sessione. Il processo non registra l'hash del
catalogo caricato. Il pannello D3D8 è acquisito prima di Present e non prova
le trasformazioni colore del display; il viewer non riproduce HUD e indicatore
dell'oggetto in mano. Salvataggi originali invariati, copia isolata e proxy
temporaneo rimossi. Non sono stati abilitati GI o displacement.

## Confronto e oggetti bedroom — 19 novembre (storico)

### Checklist della posa e preparazione successiva

La checklist descrive la tavola precedente e la preparazione successiva;
la verifica live più recente è riportata sopra:

| Elemento visibile | Ultima tavola live | Preparazione successiva |
| --- | --- | --- |
| Pareti, pavimento e struttura della stanza | Presenti; override pietra e legno contati | Le superfici non sostituite possono usare texture originali selezionate |
| Letto e tessili | Letto high-poly presente; tessili provvisori | UV dei cuscini corretti e lana/lino nel catalogo, non ancora live |
| Mobile e cassa | Candidati selezionati; materiali provvisori | Texture originale del mobile disponibile nel catalogo |
| Casse accanto alla porta | Assenti nel remaster | Tre candidati aggiunti al catalogo e al matcher; ricostruzione offline, non ancora live |
| Pane sul mobile | Mancante | Matcher corretto e texture originale pronti; selezione live non dimostrata |
| Libro verticale | Presente come colore uniforme | Texture originale nel catalogo |
| Bersaglio e arco | Presenti, con dischi rosso e grigio uniformi | Texture originali disponibili per bersaglio e ornamento |
| Candela sinistra | ON/OFF osservato dopo l'interazione confermata | Comportamento mantenuto |
| Candela sul comodino | Mancante nella tavola; candidato ON/OFF osservato in una prova precedente | Secondo campionamento indipendente pronto, non ancora verificato live |

### Casse presso la porta — 20 novembre

Nell'originale è visibile una cassa accanto all'apertura, assente dalla
ricostruzione precedente. Nella lista e nei draw dei frame 884–886 sono
presenti tre candidati `D_1224__0`, `D_1224__22` e `D_1224__23`: le
submission della stage 1 sono stabili e contano ciascuna 216 vertici e 108
triangoli, come la mesh della mappa `GENxGOODScrateB`. I payload dei buffer
non sono disponibili, quindi questi riscontri non provano i binding GPU.

Il matcher e il [catalogo v10 aggiornato](build/remaster-v01/bedroom-v01-door-crates.bin)
includono soltanto questi tre attori aggiuntivi. Il catalogo conserva letto,
tessili, materiali e impronte del v10 precedente; SHA-256
`e42c20c82a0c9de34e765ae0cad4b6b3b94ceaec9c7fa99e3e78d11a5bdc130b`.
La [ricostruzione offline prima/dopo](build/remaster-v01/offline-door-crates-before-after.png)
usa camera e trasformazioni delle casse del frame 885 e gli altri candidati
della ricostruzione del frame 1084. Le casse aggiungono 3.103 pixel al frame
remaster ON e altrettanti all'OFF simulato; il flat cambia di 3.104 pixel.
Il [report](build/remaster-v01/offline-door-crates-report.json) conserva input,
limiti e impronte. Le due candele e l'OFF restano simulati: l'immagine non
dimostra selezione o visibilità live della build modificata.

La build MinGW del proxy e le fixture D3D12 x86 per catalogo e matcher passano;
sono passati 40 test host mirati (uno opzionale saltato). Il watcher richiede
ora anche i tre candidati delle casse nei frame della prossima comparativa,
senza trattare i conteggi come prova di visibilità. I confronti storici restano
leggibili. Nessuna nuova prova live è stata avviata.

Il precedente [catalogo v10 di verifica](build/remaster-v01/bedroom-v01-review.bin)
ha SHA-256 `3a20da135c66295b4f4136cd95fed64a233aaa9b9c0583c054afc7bd0a754364`.
Mantiene il letto con UV dei cuscini corretti e i materiali v9; aggiunge gli
UV del candelabro generato e le albedo metallica e della cera. La cera usa la
luminanza della texture T3, tinta come il materiale generato e resa opaca;
entrambi riutilizzano i canali ausiliari del legno. La lana esistente è
schiarita nel solo catalogo con un fattore 5 in luce lineare, senza cambiare
il PNG sorgente. Sono scelte di anteprima, non misure del gioco.

La [ricostruzione offline ON/OFF](build/remaster-v01/offline-v01-review-onoff.png)
usa la camera del frame 885 e i candidati di geometria del frame 1084; il
collegamento della seconda candela e lo spegnimento della prima sono
ricostruiti/simulati. Mostra due candelabri e una coperta più leggibile, ma
non prova selezione, interazione o completezza in TDS. Il confronto offline
con la lana precedente cambia 12.820 pixel su ciascun frame remaster, con
il flat identico. Il [report](build/remaster-v01/offline-v01-review-report.json)
conserva le impronte degli input e dei frame. La fixture D3D12 x86 passa con il catalogo v10 esatto;
metallo e cera sono campionati e il materiale non è applicato ad altre mesh.
Nel GLB del candelabro 304 triangoli metallici e 88 della cera hanno UV
collassati, concentrati sulle superfici dei tappi e degli anelli: non è una
verifica delle cuciture visibili nella partita.

Una correzione successiva proietta i tappi su un piano e separa gli UV alla
cucitura dei cilindri del candelabro. Il [GLB corretto](build/remaster-v01/candlestick-high-uv.glb)
non ha triangoli metallici o della cera con UV collassati; geometria e normali
coincidono con il GLB precedente. La mesh aggiornata è incorporata nel proxy.
Nella [ricostruzione offline](build/remaster-v01/offline-candlestick-uv-onoff.png),
che mantiene gli stessi limiti di quella precedente, cambiano 78 pixel sia nel
frame ON sia nell'OFF simulato; il flat è identico. Dettagli e impronte sono
nel [report](build/remaster-v01/offline-candlestick-uv-report.json). È passata
la fixture D3D12 x86 con il catalogo v10, oltre ai test host del candelabro;
la correzione non è stata verificata in TDS.

Il controllo sintattico Python del generatore è passato sul terminale
dell'utente. La rigenerazione completa Blender esporta il candelabro, ma poi
si ferma sul rosone perché manca l'asset esportato
`SouthQuarter1_int1/textures/civgoodness_d.png`. La prova è stata eseguita in
una directory temporanea, senza rigenerare gli altri asset del progetto.

Il [catalogo v9](build/remaster-v01/bedroom-v01-originals-v2.bin), SHA-256
`0a619dba367b61ade094de259b524f632c218b09dd840cb682c7143ef3f3472f`, contiene
il letto con UV corretti, i tessili e dodici texture diffuse originali di
questi oggetti e superfici. I bundle corrispondono alle impronte della mappa;
normal, roughness e AO per queste texture restano approssimazioni del legno.
Il runner accetta i cataloghi v8–v10 con il campionamento delle mesh.

La [comparativa offline dei materiali](build/remaster-v01/offline-materials-comparison.png)
usa la camera del frame 885 e una selezione di 25 mesh e quattro BSP
ricostruita dal frame 1084 precedente. Pane e binding sono candidati inseriti
offline; compare una sola candela e lo spegnimento è simulato. Non è una
nuova cattura live. Rispetto al catalogo tessili v8 cambiano 166.271 pixel
nel remaster e il flat è identico byte per byte. In questa ricostruzione il
bersaglio, il pane, il libro e l'ornamento non usano più i colori uniformi;
la coperta rimane molto scura. Mancano la prova della seconda candela, la
completezza della stanza e la verifica in gioco dei nuovi materiali.

Il comodino candidato `D_1224__21` era selezionato nell'ultima cattura; nella
ricostruzione offline il letto high-poly ne copre una parte. La proiezione
CPU limitata a questi due candidati non dimostra il risultato GPU o il binding.
Alla camera del frame 885, il letto e i bounds candidati di entrambe le
candele rientrano nel margine del 2%; il collegamento della seconda candela è
ricostruito dai dati registrati, non campionato nella precedente build.

Per la prossima prova il watcher richiede entrambe le candele inquadrate e
accese inizialmente, poi verifica che quella non azionata resti accesa. Richiede
anche una selezione ciascuno per cassa, mobile, pane e bersaglio e le tre casse
accanto alla porta. Le tre pose
devono avere scarto inferiore a 0,5 unità T3 per ogni coppia e orientamento
identico sui tre assi a precisione float32. Questi controlli prevengono una
comparativa palesemente incompleta, ma non provano visibilità, UV o identità
GPU: resta necessaria l'ispezione delle immagini.

La fixture x86/D3D12 e i test host passano con la build attuale. Il proxy
compilato ha SHA-256
`5b5c21b26a8c42f1f5a0edcefa05e4baec36af6c4c98a58b46cf346216358f60`.
Fino alla nuova disponibilità del 20 novembre non erano state avviate altre
prove. Il risultato della sessione successiva è riportato sopra.

L'ultima prova richiesta dall'utente ha prodotto una [tavola a quattro
pannelli](build/t3render-captures/remaster-v01-bread-final-20261119-1549utc/remaster-v0.1-comparison-pose-native.png).
La selezione offline usa frame già acquisiti: originale e flat condividono il
Present 885; ON è il frame 902 e OFF il 946. Rispetto al flat gli scarti sono
0,206 e 0,183 unità T3; fra ON e OFF sono 0,278, con orientamento della
camera identico a precisione float32 per tutte le coppie. La selezione
minimizza prima il massimo scarto e poi la somma degli
scarti fra 120 combinazioni compatibili. Il [rapporto](build/t3render-captures/remaster-v01-bread-final-20261119-1549utc/remaster-v0.1-comparison-pose-native.json)
conserva anche i frame indicati dai marker originali, rimasti invariati; la
prima tavola usava i frame 886, 896 e 946. I pannelli sono ridimensionati senza
correzioni gamma aggiuntive; la cattura D3D8 precede il Present e non prova
l'eventuale trasformazione colore del display. La versione precedente con
gamma 1,818 è conservata. L'utente conferma di aver spento la candela sinistra
senza cambiare posa; il campione passa da ON a OFF e la fiamma scompare. Il
letto high-poly è selezionato e il viewer conta tre draw della pietra e quattro del
legno. Il pane e la seconda candela mancano ancora; alcuni materiali sono
provvisori, quindi **v0.1 non è DONE**. La sessione ha rimosso il proxy e la
copia isolata; i salvataggi originali risultano invariati.

Il [registro di deploy](build/t3render-captures/remaster-v01-bread-final-20261119-1549utc/deployment.json)
identifica il proxy della cattura con SHA-256
`874e2cdc91845a820b491cf0ef043b75176f9b7762287a8d543ef76eed6307a6` e
indica il percorso del catalogo `bedroom-v01-props-v2.bin`. Il file ora a
quel percorso ha SHA-256
`dadb49c471dcc541dd950b0c4544b75f71e726cc027dcb9824c994b390694d8a` e
contiene il letto precedente alla correzione UV; il registro non conserva
l'hash del catalogo caricato, quindi non ne prova il contenuto in quel momento.
Al momento della cattura il checkpoint `c7bb3f78` era antenato di HEAD e il
solo commit successivo conteneva il recap; il worktree aveva modifiche non
committate e il registro di deploy non ne salva lo stato. Il registro non
prova quindi la revisione esatta dei sorgenti del proxy catturato.

Nell'ultima telemetria del viewer, 188 campioni del pane si fermano dopo il
controllo di stabilità della lista e prima del completamento del controllo di
unicità. La trace di quel frame mostra un solo `D_125__3`, ma anche un altro
owner con lo stesso numero FName e nome non disponibile. Un test sintetico
con un nome non leggibile durante lo stesso controllo riproduce quel
checkpoint: è una possibile spiegazione, non la causa dimostrata della prova
live. La posizione di quell'owner dista circa 0,000031 unità T3 dal sacco
offline `D_415__3`; questa corrispondenza non ne prova l'identità runtime.
Nei tre frame D3D8 acquisiti, il draw di quell'owner ha 33 vertici e 32
triangoli, come il sacco offline; quello del pane ne ha rispettivamente 99 e
92, come il mesh del pane offline. Le sei submission risultano stabili e i
draw hanno successo. I buffer catturati sono però indisponibili: posizione e
conteggi rafforzano la corrispondenza col sacco, ma non provano l'identità
dell'owner né che il suo prefisso fosse leggibile al sampler.

Il sampler ora, soltanto per il pane, può escludere un nome non leggibile per
intero se un prefisso letto due volte prova che è diverso da `D_125` e la
tabella e lo slot restano stabili. Un prefisso sconosciuto, uguale o variabile
continua a respingere il candidato; i controlli successivi rimangono attivi.
I test sintetici riproducono il caso e i rifiuti; la modifica non è stata
verificata nel gioco e il pane potrebbe incontrare altri ostacoli. La ricerca
statica della tabella nomi nel binario `T3Main.exe` (SHA-256
`09e0be902062c53cbb1736cffa1bd3ec883bd5e864f8e7f3f99bc0d06f6ad0f8`)
è descritta dall'evidenza REA `ev_f00481c8931408c5a5b45b388df668697d820afc6be4cc90f39af7e4761612ad`:
usa un hash e confronta il testo dei nomi; il nuovo controllo non assume che
indici differenti garantiscano nomi differenti. Nel percorso di allocazione
analizzato, `FNameEntry` riserva `strlen(nome) + 0x1d` byte e scrive il testo
da `+0x1c` (REA `ev_2bce512bc52ae205da8edab5c418ff0150874328c4b03a158e796fa4d53b074a`
e `ev_0ebc99b1bff7b71b30c3fed780dc9fe127f0dd91f5bf69494098dfd0c00adcdc`);
la lettura precedente richiedeva invece 64 byte anche per un nome breve. La
differenza può causare un fallimento di lettura, senza dimostrare il motivo
del caso osservato.

La tavola sopra è stata acquisita prima della correzione e non la verifica.
L'utente ha chiesto che fosse l'ultima prova; non sono previsti altri avvii.

Una verifica offline del letto usato nella tavola ha trovato UV collassati su
384 dei 4.992 triangoli dei cuscini. Il generatore ora mappa separatamente la
fascia laterale e il test Godot controlla anche gli UV dei cuscini. Il nuovo
[GLB del letto](build/remaster-v01/bed-high-pillow-uv.glb) non ha triangoli
dei cuscini con UV collassati; geometria e normali coincidono per triangolo,
come gli UV degli altri materiali. È stato preparato un [catalogo
separato](build/remaster-v01/bedroom-v01-pillow-uv.bin), con il relativo
[manifesto](build/remaster-v01/bedroom-v01-pillow-uv.bin.manifest.json): la
fixture x86/D3D12 con questo catalogo è passata. Il GLB precedente in
`highpoly/generated/bed_high.glb` e i cataloghi precedenti sono invariati;
per usare la correzione nel progetto Godot occorre rigenerare il GLB. Il test
Godot non è stato eseguito e la correzione non è verificata nella tavola live.
Il confronto dei selettori di materiali sulla scena runtime acquisita è passato
per sette sezioni e 67 triangoli sostituiti.

Lo [scope offline del frame 886](build/t3render-captures/remaster-v01-bread-final-20261119-1549utc/scoped-886.json),
ricavato dalla traccia dell'ultima tavola, ha superato il confronto fra i
selettori host su dieci sezioni. Nella geometria candidata, i 73 triangoli di
parete e i 22 di pavimento individuati dagli override non hanno UV collassati
né geometria degenere. Sul GLB corretto un controllo analogo non trova
triangoli degeneri o con UV collassati in nessuna delle sei sezioni del letto
(20.420 triangoli); la geometria convertita compare una sola volta nel
catalogo tessili. Sono controlli sui dati candidati offline, non una verifica
delle cuciture visibili o dei binding GPU del gioco.

Il renderer può ora usare le texture già generate di lana e lino, soltanto per
le due sezioni del letto high-poly. Il [catalogo tessili
v8](build/remaster-v01/bedroom-v01-textiles.bin) usa anche il GLB con UV
corretti; il relativo [manifesto](build/remaster-v01/bedroom-v01-textiles.bin.manifest.json)
identifica le sorgenti e i canali costanti. I cataloghi precedenti mantengono
i colori di riserva. Le normal map sono incluse, ma il percorso PBR ordinario
non le applica; roughness del lino e AO dei tessili sono costanti. Sono passati
19 test del catalogo, la fixture host di binding e la fixture x86/D3D12 sia
con il catalogo v8 sia con il precedente. Il proxy ricompilato dopo questa
modifica ha SHA-256 `fdac3362abfd339c334b7efa173ec92860123baf99eb6253f6cf1e746e3addf3`.
Una ricostruzione offline del frame
1084, con candidati e stato OFF simulati, cambia 16.446 pixel nell'area del
letto e lascia invariato il flat; la coperta resta molto scura. La tavola live
precede queste modifiche e non ne verifica la resa.

Il renderer è stato esteso in modo circoscritto ai due candelabri della
bedroom, `D_384__5` e `D_384__13`: ciascuno richiede la propria osservazione
di posizione e stato. Lo spegnimento di quello selezionato non spegne l'altro;
osservazioni mancanti non vengono sostituite con quelle della prima candela.
Il gate del confronto resta riferito alla candela scelta. La build e le fixture
host/x86/D3D12 verificano entrambi accesi e la transizione di una sola candela,
ma non dimostrano il nuovo comportamento nel gioco.
Sono passati 36 test host mirati, le due fixture D3D12 x86 e la verifica dei
contatori JSON del viewer. Il proxy usato per quei test aveva SHA-256
`da1e5441c2dd8a1fb658e45f9b98a9a54797a07b8a3d98778e055d634243e3eb`;
la tavola live resta quella della build precedente.

Una rilettura dell'ultima traccia e della telemetria della stessa sessione
aggiunge evidenza sul secondo candelabro. Nel frame 886 sono presenti entrambi
gli attori visivi candidati `D_384__5` e `D_384__13` e i corrispondenti
candidati luce: per entrambi la posizione della luce differisce dalla
trasformazione visiva dello stesso vettore, circa `(0,1365, -0,0414, 30,9423)`
unità T3. La proiezione del candidato luce del secondo candelabro è circa
`(765, 344)` nel frame originale 1024×768, vicino alla candela sul comodino.
Negli otto frame ON selezionati entrambi i candidati luce sono
presenti e hanno modulazione non nulla. Negli otto frame OFF, dopo l'azione
confermata dall'utente, il candidato della candela sinistra non è nella lista,
mentre quello del secondo candelabro rimane presente, a posizione invariata e
con modulazione non nulla. Questi campioni non provano il binding né lo stato
ON/OFF dell'altro attore: la vecchia build non lo campionava indipendentemente
e non lo disegnava nella tavola.

Anche una prova precedente mostra la transizione opposta: negli otto frame ON
4372–4379 sono elencati entrambi i candidati, mentre negli otto frame OFF
4590–4597 quello del comodino (`D_384:6`) ha il flag candidato spento e non è
nella lista; quello sinistro (`D_384:14`) resta presente, modulato e nella
stessa posizione. Il [report delle due sessioni](build/remaster-v01/independent-candle-runtime-evidence.json)
riporta intervalli e impronte. L'azione dell'utente nella prova più vecchia non
è confermata dai log; le identità restano candidate. La vecchia build disegnava
una sola candela, quindi questa evidenza non verifica il nuovo renderer a due
candele nel gioco né sostituisce la comparativa richiesta.

Una ricostruzione temporanea offline del frame 1084 ha riutilizzato la camera,
la trasformazione osservata del secondo candelabro e il suo candidato nella
lista luci. Il suo collegamento è stato costruito sinteticamente e lo
spegnimento della prima candela è simulato: l'anteprima mostra il secondo
candelabro illuminato in entrambi i casi. Non è una nuova cattura runtime e
non dimostra lo stato o il binding del secondo attore nel gioco.

La verifica live successiva alla correzione ha prodotto una [nuova tavola a
quattro pannelli](build/t3render-captures/remaster-v01-live-20261119-161424/remaster-v0.1-comparison.png).
Originale e flat sono collegati allo stesso Present; nella stessa sessione
gli scarti rispetto al flat sono 0,389 unità T3 per ON e 0,224 per OFF, con
rotazione 0°. L'utente conferma lo spegnimento della candela sinistra; il
campione passa da ON a OFF e la fiamma scompare. Il letto high-poly rientra
nell'inquadratura; il viewer conta tre draw della pietra e quattro del legno.
La fascia verde non compare più nei due frame remaster. Anche questa prova
lascia invariati i salvataggi originali e rimuove il proxy temporaneo.

La tavola resta incompleta: il pane non è selezionato, il viewer mostra una
sola candela e bersaglio, coperta e altri oggetti usano materiali provvisori.
Queste differenze non consentono ancora di dichiarare **v0.1 DONE** o di
concludere che tutti gli UV visibili siano corretti.

La prova immediatamente precedente ha prodotto questa [tavola live a
quattro pannelli](build/t3render-captures/remaster-v01-live-20261119-155213/remaster-v0.1-comparison.png).
Originale e flat sono associati allo stesso Present; rispetto al flat, ON e
OFF hanno rotazione 0° e scarti di posizione rispettivamente 0,454 e 0,075
unità T3. L'utente ha confermato di aver spento la candela sinistra al segnale:
il candidato passa da ON a OFF e la fiamma scompare. Nel remaster sono
selezionati un letto high-poly, tre draw di `stone_wall_v2` e quattro di
`wood_floor_v2`; sono presenti anche i candidati mobile e bersaglio, ma il
pane non viene selezionato e il viewer mostra ancora una sola candela.
Salvataggi originali invariati e proxy temporaneo rimosso. La prova mostra
materiali provvisori e una fascia verde sul bordo destro: **v0.1 non è DONE**.

Il draw dell'arco `StaticMeshActor__58`, stesso mesh e materiale dell'arco
`StaticMeshActor__59` già trattato, proietta sulla fascia verde. La build ora
applica anche a quell'istanza il materiale pietra già usato per l'altra. In una
ricostruzione D3D12 offline della geometria del frame 889 la modifica cambia
8.554 pixel nell'area x=978–1023, y=96–513 e sostituisce il verde diagnostico
nei punti campionati. La tavola di quella prova conserva i pixel acquisiti
prima della correzione; la prova successiva non mostra più la fascia verde.

Nel frame 2645 la traccia mostra il candidato `D_125__3` una sola volta nella
lista, con owner stabile e draw da 99 vertici e 92 triangoli, coerente con il
pane offline. I buffer del draw non sono disponibili come prova del payload
GPU e il viewer non seleziona il pane. La traccia non conserva il dettaglio
del campionamento generico necessario per stabilire il motivo del rifiuto.
La build ora registra solo contatori aggregati, senza indirizzi, per distinguere
campionamento, matching, draw e ricontrollo di questo candidato. Il trattamento
circoscritto dei nomi parziali è descritto sopra.
La diagnostica distingue anche i controlli sul nodo mesh e lo stato della
proprietà asset. La build MinGW del proxy e le due fixture sintetiche x86/D3D12
passano; la telemetria JSON è stata verificata, così come i test host dei
matcher e delle sonde. I contatori raccolti nell'ultima prova sono riportati
sopra.

La seconda acquisizione live ha prodotto una tavola a quattro pannelli nella
stessa sessione: la rotazione coincide e lo scarto di posizione è inferiore
a 0,1 unità T3. La luce candidata cambia stato dopo l'interazione, ma il
letto è tagliato, il pavimento non è visibile e il viewer non mostra tutta
la stanza. Il confronto non soddisfa ancora la definizione di v0.1.

Il codice ora permette il candelabro e le fiamme anche nel confronto remaster.
Un catalogo v7 opt-in aggiunge la cassa `D_1224__21` e abilita i candidati
`StaticMeshActor` già predisposti. Nel frame acquisito la proiezione della
mesh offline della cassa sulla trasformazione osservata coincide visivamente
con il mobile; i parametri del draw sono coerenti con 216 vertici e 108
triangoli. Non è una prova del binding GPU. Una successiva prova live ha
selezionato la cassa e altre mesh; alcuni oggetti e le texture originali dei
mobili restano fuori copertura.
Il viewer può selezionare anche la candela sul mobile a sinistra `D_384__13`.
Nella [comparativa live a quattro pannelli](build/t3render-captures/remaster-v01-left-20261119-v2/remaster-v0.1-comparison.png)
originale e flat sono collegati allo stesso Present; rispetto al flat, gli
scarti sono 0,195 unità T3 e 0° per ON, 0,208 unità e 0° per OFF. L'utente
ha confermato lo spegnimento della candela di sinistra dopo il segnale; il
campione candidato passa da ON a OFF. La stessa immagine mostra letto e
pavimento, ma il letto è tagliato a destra, il mobile sotto la candela manca e
gli altri mobili hanno colori provvisori. La scena rimane incompleta e troppo
scura per validare geometria e UV: **v0.1 non è DONE**. Il viewer segue una
sola candela alla volta. La cassa selezionata e 22 mesh sono state contate in
quel frame; ciò non prova da solo i binding GPU.

La seconda candela non è un problema del salvataggio: nelle acquisizioni
mostrate il preview campionava e renderizzava un solo collegamento alla volta.
La nuova implementazione con campioni indipendenti è descritta sopra; il
confronto acquisito non dimostra ancora quel comportamento.

Il frame D3D8 1084 contiene un draw del mobile `D_443__1`; la proiezione della
mesh offline `SEAinFURNcabinetC1` con la matrice osservata coincide con il
[piano in primo piano](build/t3render-captures/remaster-v01-left-20261119-v2/cabinet-projection-1084-diagnostic.png).
Il catalogo e il matcher ora includono questo candidato, il pane `D_125__3` e
il bersaglio `D_548__0` in modo esplicito, con ricontrollo a fine scope. I
rispettivi draw compaiono nella stessa traccia. Sono verificati in fixture,
non ancora in TDS. Il catalogo preparato è
`build/remaster-v01/bedroom-v01-props-v2.bin`. Non sono state aggiunte le
texture originali dei mobili.

Una [ricostruzione offline](build/t3render-captures/remaster-v01-left-20261119-v2/offline-v2-review/on.png)
usa la camera della traccia, i nuovi candidati e lo stesso backend D3D12, ma
non dimostra che i nuovi matcher li selezionino nel gioco. Per eliminare i
colori diagnostici sono stati aggiunti abbinamenti **circoscritti a questa
bedroom**: materiali legno/pietra già disponibili e colori provvisori per
alcuni oggetti e tessuti. Il legno generato del letto usa `wood_floor_v2`,
come nell'anteprima PBR preesistente. L'ambiente artistico è stato alzato
per rendere leggibile la geometria anche a candela spenta: non è luce misurata
nel gioco. Nella prova offline ON e OFF differiscono e il flat è invariato;
input, limiti e hash sono nel [report](build/t3render-captures/remaster-v01-left-20261119-v2/offline-v2-review/report.json).
In quella posa la proiezione del letto supera il bordo di circa 18 pixel;
una rotazione teorica di circa 3° verso destra lo fa rientrare, senza provare
che la candela rimanga raggiungibile.

Il watcher ora richiede un margine del 2% sui bounds proiettati del letto
high-poly e della candela selezionata, sia prima della cattura sia nei tre
frame flat, ON e OFF scelti. La fiamma è inclusa quando accesa; dati mancanti,
fuori campo o non coerenti con lo snapshot non sbloccano la prova. Sul frame
1084 ricostruito offline, il bounding box del letto arriva a x=1045 su 1024;
con una rotazione teorica di 3° arriva a x=992 e anche la candela rientra nel
margine. Questi bounds conservativi non verificano occlusioni, geometria/UV
visibili o distanza di interazione. I log storici restano leggibili senza tale
controllo. Con `--speak` vengono annunciati preparazione, inizio acquisizione,
spegnimento e completamento; se una posa ferma non supera il gate, la voce può
segnalare letto non rilevato, candela spenta, dati mancanti o bordi troppo
vicini. Gli avvisi sono limitati nel tempo e non si sovrappongono.

Una [seconda anteprima offline](build/t3render-captures/remaster-v01-left-20261119-v2/offline-v2-review/rotated-3deg-offline-on.png)
usa gli stessi candidati del frame 1084, il catalogo v2 e il renderer D3D12,
con la sola camera ruotata teoricamente di circa 3° dalla posizione registrata.
Mostra letto intero, pavimento e candela, ma non è un'immagine acquisita dal
gioco né prova che la candela sia raggiungibile o che i nuovi oggetti vengano
selezionati in quella posa. Coperta, pane e bersaglio hanno ancora materiali
provvisori evidenti. L'immagine riporta esplicitamente il carattere sintetico;
SHA-256 `773c380c851c61b299e51e101c4bebf0f9fc422a513a53c5464694674caefd9d`.

Il letto `bed_high.glb` locale ha SHA-256
`57280338da9b45b8559b3e30403dbbeb14399b0c686479eae5af3213be84deec`;
il catalogo preparato contiene lo stesso identificativo e la geometria
serializzata attesa dopo la conversione. Rispetto all'`INNbed1A1` offline,
gli estremi del bounding box differiscono al massimo di 2,48 cm. Applicando
ai dati GLB i criteri di `highpoly_geometry.gd`, i 520 triangoli controllati
della testiera e i 3.219 dei profili in legno non hanno UV collassati; il
rapporto fra le densità orizzontale e verticale della testiera è 1,00.
Questi controlli non rilevano tutte le cuciture e non verificano il binding o
la resa nel gioco.

La build MinGW del proxy e le due fixture sintetiche Highball x86/D3D12 sono
passate; sono passati anche i test host di proiezione e watcher e la verifica
della comparativa storica. Questi controlli precedono le prove live descritte
sopra. L'utente ha chiesto di non effettuare altri avvii dopo l'ultima prova.

Nella prova precedente la pubblicazione si era fermata con un `DrawPrimitiveUP`
senza scope pendente; la causa non è stata dimostrata. Un recupero x86
circoscritto invalida la vecchia finestra solo quando i riferimenti allo stack
indicano che la chiamata è uscita. Le fixture coprono anche chiamate annidate,
thread e stack diversi e un'uscita non locale su Highball; il comportamento
non è stato attivato nella prova successiva, che ha pubblicato frame; resta
quindi da verificare nel gioco quando quel caso si ripresenta.

## Checkpoint e verifiche

Il repository è sul branch `remaster/vertical-slice`. Il checkpoint WIP
iniziale è `de6a3831`; il codice successivo è congelato in `c7bb3f78`
(`chore(t3render): checkpoint WIP remaster v0.1`). Sono commit locali, non
pubblicati. Il repository Godot annidato in `build/assets/godot/remaster`
è separato e senza remote: il checkpoint iniziale era `746815f`, mentre le
correzioni UV sono nel commit `50d833c`.

Il proxy del checkpoint è stato ricostruito offline dai sorgenti `c7bb3f78`
in una directory isolata: [DLL baseline](build/remaster-v01/checkpoint-c7bb3f78-d3d8.dll)
e [manifesto](build/remaster-v01/checkpoint-c7bb3f78-d3d8.dll.manifest.json).
La build MinGW x86 e 17 test host del catalogo sono passati. Questa build è
successiva alla cattura, non è stata caricata nel gioco e non contiene le
correzioni successive; il checkpoint non supporta i cataloghi v7 e v8.
L'hash del proxy differisce da quello registrato per la cattura, ma il linker
include un timestamp e la differenza non dimostra da sola sorgenti diversi.

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

La comparativa live del 20 novembre e i cataloghi correnti sono output locali
in `build/`, ignorati da Git; i commit non li includono.

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
