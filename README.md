# Eqtrum

Plugin VST3 (+ Standalone per debug) stereo per macOS, scritto in C++17 con JUCE 8.0.6 (scaricato da CMake con FetchContent al tag fisso). Un solo binario, due modalità selezionabili nella UI:

- **TRACK**: inserito su una traccia, applica l'EQ di quella traccia e pubblica spettro e stato in un registro condiviso in-process.
- **HUB**: inserito su main/gruppo/return, lascia passare l'audio invariato a latenza zero, mostra gli spettri di tutte le istanze TRACK sovrapposti e ne controlla a distanza l'EQ.

Funzioni: HP/LP 12/24 dB/oct, low/high shelf, 6 bell; fase Minimum (biquad) o Linear (FIR 4096/8192/16384 via `juce::dsp::Convolution`, latenza N/2 dichiarata all'host); dinamica per banda con sidechain VST3 opzionale; analizzatore FFT 4096 su 512 bande log; preset XML (5 di fabbrica); stato completo salvato nel progetto.

Avvio rapido:

```bash
cmake -B build -G Xcode
cmake --build build --config Release -j 8
# -> build/Eqtrum_artefacts/Release/VST3/Eqtrum.vst3 (copiato in ~/Library/Audio/Plug-Ins/VST3/)
```

## Design

```
CLASSI
- EqtrumAudioProcessor: APVTS, bus (main stereo + "Sidechain" stereo disabilitato), stato, Timer 30 Hz
  sul message thread (slot del registro, comandi remoti, setLatencySamples).
- EqEngine: EQ minimum phase, 10 bande di biquad TDF-II in double, smoothing, coefficienti ogni 32
  campioni, dinamica per banda (passa-banda -> inviluppo peak -> range*smoothstep((L-thr)/6 dB)).
- LinearPhaseBuilder (juce::Thread, priorità low): |H(f)| su griglia FFT -> IFFT -> shift N/2 ->
  Blackman. Ricostruisce solo se i parametri cambiano, al massimo ogni 100 ms.
- juce::dsp::Convolution: esegue la IR; latenza dichiarata N/2.
- SpectrumAnalyzer (juce::Thread, low): AbstractFifo -> FFT 4096 Hann -> 512 bande log -> smoothing.
- InstanceRegistry: singleton (static locale a funzione), 64 Slot; ogni slot ha owner id, nome,
  colore, visibile, heartbeat, spettro e spettro SC (seqlock), mirror dei parametri, coda SPSC da 512.
- PresetManager: 5 preset di fabbrica nel codice + XML utente in .../SpectrumMixer/Presets.
- Editor: SpectrumView (grafico, nodi), TrackListPanel (lista), pannello banda. I controlli agiscono
  su un "target": locale (propria traccia), remoto (HUB) o sola lettura (altre tracce, in TRACK).
THREAD
- Audio: solo atomic, AbstractFifo e un try-lock non bloccante. Niente allocazioni, lock o I/O.
- Message: Timer del processor + Timer dell'editor (30 fps).
- 1 thread di analisi per istanza (scrive anche l'heartbeat ogni ~10 ms) + 1 thread builder IR
  + il thread interno della Convolution di JUCE (costruisce l'engine FFT).
FLUSSO DATI
- Audio -> FIFO -> analisi -> seqlock dello slot -> UI di tutte le istanze (HUB incluso).
- UI HUB -> RemoteCommand{indice parametro, valore normalizzato} -> coda dello slot -> Timer della
  TRACK -> beginChangeGesture/setValueNotifyingHost/endChangeGesture (Ableton registra).
- Builder IR -> SpinLock (l'audio fa solo try-lock) -> loadImpulseResponse chiamata dal thread
  audio (JUCE richiede lo stesso thread di process(); la chiamata è wait-free).
- Cambio TRACK/HUB, fase o lunghezza FIR: fade-out 10 ms -> reset -> (Linear: attesa IR e N/2
  campioni di storia) -> fade-in 10 ms.
ID PARAMETRI (91, stabili)
- outGain, phase (Minimum|Linear), firLen (4096|8192|16384)
- b0 HP, b1 LP: b{n}_on, b{n}_freq, b{n}_q, b{n}_slope (12|24 dB/oct)
- b2 LS, b3 HS, b4..b9 Bell 1..6: b{n}_on, _freq, _gain, _q, _dynOn, _thr, _range, _att, _rel,
  _src (Internal|Sidechain)
DECISIONI
- HP/LP senza guadagno; 24 dB/oct = 2 biquad con Q Butterworth (0.541/1.307) scalati dal Q.
- Hop FFT = min(2048, fs/30): overlap >= 50% e ~30 agg/s anche a 44.1 kHz (con 50% esatto ~21/s).
- Modalità, nome, colore e visibilità sono stato, non parametri: non automatizzabili, fuori dai preset.
- Bypass dell'host: linea di ritardo pari alla latenza dichiarata.
```

## Struttura dei file

```
EqtrumVST3/
├── CMakeLists.txt
└── Source/
    ├── Params.h               ID, tipi di banda, layout APVTS, tabella indici stabile
    ├── Params.cpp
    ├── EqMath.h               coefficienti RBJ, modulo |H|, Biquad TDF-II
    ├── EqMath.cpp
    ├── EqEngine.h             DSP minimum phase + dinamica (thread audio)
    ├── EqEngine.cpp
    ├── InstanceRegistry.h     registro in-process: Slot, seqlock, coda SPSC
    ├── InstanceRegistry.cpp
    ├── SpectrumAnalyzer.h     thread di analisi FFT
    ├── SpectrumAnalyzer.cpp
    ├── LinearPhaseBuilder.h   thread che costruisce la IR a fase lineare
    ├── LinearPhaseBuilder.cpp
    ├── PresetManager.h        preset di fabbrica + XML utente
    ├── PresetManager.cpp
    ├── PluginProcessor.h      AudioProcessor
    ├── PluginProcessor.cpp
    ├── SpectrumView.h         grafico spettri/curva/nodi
    ├── SpectrumView.cpp
    ├── TrackListPanel.h       lista laterale delle tracce
    ├── TrackListPanel.cpp
    ├── PluginEditor.h         finestra principale
    └── PluginEditor.cpp
```

## Verifica e audit

### Come è stato verificato
- **Compilazione reale** di questo esatto CMakeLists (FetchContent di JUCE al tag `8.0.6`) su Linux, con Clang 18 e GCC 13: 0 errori. Tutti i sorgenti ricontrollati con `-Wall -Wextra -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual`: 0 warning su entrambi i compilatori.
- **Non** ho un Mac: Xcode/Apple Clang non è stato provato. Il codice non usa API specifiche di piattaforma e Apple Clang è molto vicino a Clang 18, ma il rischio residuo esiste (vedi punto incerto 1).
- **62 test funzionali headless** (harness separato, fuori dal deliverable) che istanziano il processor reale. Valori misurati:
  - bell +12 dB a 1 kHz → **+12,00 dB**; HP 24 dB/oct a 1 kHz, due ottave sotto → **−48,2 dB**; impostazioni di default bit-trasparenti.
  - Linear 4096: latenza dichiarata **2048**, picco dell'impulso a **2048**, IR simmetrica (differenza 0), modulo a 1 kHz **+11,99 dB**; FIR 16384 → latenza 8192; ritorno a Minimum → 0.
  - Render offline (non-realtime): IR attiva dal primo blocco (picco esattamente a N/2, −9,00 dB su −9 richiesti).
  - Bypass dell'host in Linear: segnale secco ritardato esattamente della latenza dichiarata.
  - Dinamica interna: segnale forte **−6,00 dB** (range −6), segnale debole **0,00 dB**; sidechain: riduzione −6,00 dB con SC forte, 0 dB con SC muto; LED di presenza OK.
  - HUB: audio bit-identico, latenza 0.
  - Stato: parametri, modalità, nome, colore e visibilità ripristinati; stato vuoto, troncato, spazzatura, con NaN o di versione futura → nessun crash, valori validi.
  - Registro: slot distinti, HUB senza slot, spettro pubblicato (picco a 1 kHz), "Track N", comandi remoti (parametro e rinomina), nome dall'host, slot liberato alla distruzione, slot orfano (heartbeat > 2 s) liberato.
  - Preset: factory, salva/ricarica (round trip esatto), elimina, richiamo dall'HUB in batch.
  - UI: manopola dell'HUB → parametro della TRACK remota; manopola TRACK → parametro locale; altre tracce in sola lettura (controlli disattivati); in Linear la dinamica è disattivata e lo segnala.
  - Blocchi da 1 campione, blocchi più grandi di quelli annunciati, input NaN/Inf → output finito.
- **AddressSanitizer + UBSan**: 0 errori. **ThreadSanitizer**: 0 avvisi (thread audio, analisi, builder, Convolution e message thread insieme).
- Screenshot della UI renderizzati offscreen e controllati, a 1100×680 e alla dimensione minima 960×600.

### Errori e problemi trovati (tutti corretti nequesto repository)
1. **API JUCE 8**: `AudioProcessor::TrackProperties` ora contiene `std::optional<String> name` e `std::optional<Colour> colour` (in JUCE 7 erano `String`/`Colour`). Il codice usa `has_value()` e `->`.
2. **Thread della Convolution**: la documentazione JUCE 8.0.6 dice che i metodi di `dsp::Convolution` non vanno chiamati in modo interlacciato da thread diversi, e che `loadImpulseResponse` è wait-free e va chiamata sullo stesso thread di `process()`. Chiamarla dal thread builder sarebbe stato un data race. Ora il builder consegna il buffer tramite `SpinLock` (lato audio solo try-lock) e il thread audio chiama `loadImpulseResponse` spostando il buffer, senza allocare.
3. **Dimensione blocco della Convolution**: i buffer interni di JUCE sono dimensionati su `maximumBlockSize`. `process()` ora riceve blocchi spezzati alla dimensione preparata (alcuni host superano la dimensione annunciata). La partizione è limitata a 64–2048 campioni, per evitare partizioni da 1 campione se l'host prepara con blocchi da 1.
4. **Bypass in Linear**: il `processBlockBypassed` di default di JUCE non ritarda l'audio. Avrebbe disallineato la traccia rispetto alla compensazione e attivato un `jassert` in Debug. Ora c'è una linea di ritardo pari alla latenza dichiarata, poi una ripartenza pulita.
5. **Export/freeze offline**: la IR costruita in modo asincrono (tempo reale) arrivava dopo secondi di audio renderizzato, lasciando l'inizio muto. Ora la IR viene costruita in modo sincrono in `prepareToPlay` e caricata prima di `Convolution::prepare()` (pattern documentato da JUCE). Il timeout di sicurezza richiede anche 1,5 s di tempo reale.
6. Indice negativo nel ring buffer del bypass → `(w - delay + size) & mask`.
7. `juce::Font(float)` è deprecato in JUCE 8 → uso `FontOptions`, il cui costruttore da `float` è `explicit`.
8. `static_cast` di un enum class a `float` → passa per `int`.
9. **Stato con NaN**: `NormalisableRange` non filtra i NaN, quindi un valore `"nan"` nell'XML sarebbe arrivato al DSP. Ora i figli PARAM con ID sconosciuto o valore non finito vengono rimossi prima di `replaceState`, e tornano al default.
10. Range logaritmici: le lambda di `NormalisableRange` limitano l'input e gestiscono i NaN (altrimenti `jassert` in Debug e NaN in Release).

### Regole del thread audio (verificate)
`processBlock` / `processBlockBypassed` usano solo: letture atomiche dei parametri, `getBusBuffer` (senza allocare), biquad su stack/membri, `AbstractFifo::prepareToWrite`, `SpinLock::ScopedTryLockType`, `Convolution::process/loadImpulseResponse/reset` (wait-free per documentazione JUCE), `Time::getMillisecondCounter`. Nessun `new`, nessun `std::vector` che cresce, nessuna `String`, nessun lock bloccante, nessun I/O. Inoltre `ScopedNoDenormals`, sanitizzazione NaN/Inf dell'input e dell'output (con reset dei filtri) e clamp a ±64.

## Build (macOS)

Requisiti: Xcode (12.4 o successivo; consigliato 14+), CMake ≥ 3.22, git, connessione internet al primo configure (scarica JUCE 8.0.6).

```bash
xcode-select --install          # solo se mancano i Command Line Tools
brew install cmake              # oppure il .dmg da cmake.org
cd /percorso/Eqtrum             # cartella con CMakeLists.txt

# Generatore Xcode
cmake -B build -G Xcode
cmake --build build --config Release -j 8

# In alternativa, Ninja (brew install ninja):
# cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build

# Binario universale (Apple Silicon + Intel), opzionale:
# cmake -B build -G Xcode -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
```

Risultato:
- VST3: `build/Eqtrum_artefacts/Release/VST3/Eqtrum.vst3`, copiato automaticamente (`COPY_PLUGIN_AFTER_BUILD`) in `~/Library/Audio/Plug-Ins/VST3/Eqtrum.vst3`
- Standalone (debug): `build/Eqtrum_artefacts/Release/Standalone/Eqtrum.app`
- Preset utente: `~/Library/SpectrumMixer/Presets/*.xml`

In Ableton Live 12: Settings → Plug-Ins → attiva "Use VST3 Plug-In System Folders" → Rescan. Lo trovi in Browser → Plug-Ins → Eqtrum.

Se Live non lo mostra:
```bash
codesign --verify --deep --verbose ~/Library/Audio/Plug-Ins/VST3/Eqtrum.vst3
codesign --force --deep -s - ~/Library/Audio/Plug-Ins/VST3/Eqtrum.vst3   # firma ad-hoc
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/Eqtrum.vst3  # se copiato da un altro Mac
```

## Checklist di test in Ableton Live 12

1. **Caricamento**: Eqtrum su 3 tracce audio (default TRACK) e sul Master in HUB. Nell'HUB: 3 righe nella lista, spettri colorati sovrapposti, linea bianca della somma.
2. **Nome e colore**: rinomina/colora le tracce in Live e verifica se la lista si aggiorna. Se non succede, restano "Track N" e un colore automatico: rinomina con doppio clic e cambia colore cliccando il quadratino.
3. **EQ**: trascina i nodi (frequenza/guadagno), rotella = Q, doppio clic = on/off; manopole e caselle numeriche; HP/LP 12/24 dB; output gain. Nessun click durante i movimenti.
4. **Controllo remoto + automazione**: nell'HUB seleziona una traccia, muovi un nodo; apri l'Eqtrum di quella traccia e verifica che i valori si muovano. Con Arrangement Record/Automation Arm attivi, muovi dall'HUB: deve comparire automazione sul parametro della traccia (non sull'HUB).
5. **Preset**: Save as (nome nel campo), < >, Delete (due clic), i 5 factory; richiamo dall'HUB sulla traccia selezionata; controlla i file in `~/Library/SpectrumMixer/Presets`.
6. **Sidechain**: sul basso apri la sezione Sidechain del device, Audio From = traccia kick. Bell 2: Dyn on, Source = Sidechain, Threshold −30, Range −6. Con il kick in play: LED verde "SC segnale", il basso si abbassa sulla banda, "Spettro SC" mostra lo spettro giallo tratteggiato. Disattivato il sidechain: "SC assente" e il detector torna al segnale interno.
7. **Latenza linear phase (null test)**: Options → Delay Compensation attivo. Duplica una traccia audio; sulla copia A: Eqtrum flat, Linear Phase, FIR 8192; sulla copia B: Utility con inversione di fase L+R. Insieme devono annullarsi (silenzio, o quasi). L'etichetta deve mostrare 85,3 ms a 48 kHz (4096 campioni). Ripeti con FIR 4096 e 16384 e con il device spento.
8. **Cambi di modalità senza click**: durante il play commuta Minimum/Linear, la lunghezza FIR e TRACK/HUB. Attesi un breve fade o mute (in Linear fino a N/2 + 60 ms), mai un click.
9. **Salva e riapri**: salva il set, chiudi Live, riapri. Parametri, modalità TRACK/HUB, nomi personalizzati, colori e visibilità devono essere ripristinati e l'HUB deve rivedere le tracce.
10. **Robustezza**: cambia sample rate (44,1 → 96 → 192 kHz) e buffer (32 → 2048) durante il play; Freeze/Flatten e Export Audio di una traccia in Linear (confronta con il play); elimina un device TRACK (sparisce dall'HUB) e fai Undo (riappare).
11. **CPU**: 20+ istanze TRACK + 1 HUB; controlla il CPU meter di Live con FIR 16384.

## Punti incerti e come verificarli

1. **Apple Clang/Xcode non provati** (compilato solo con Clang 18 e GCC 13 su Linux). → Test: la build descritta in "Build (macOS)".
2. **Live 12 potrebbe non chiamare `updateTrackProperties`** (VST3 `IInfoListener`/channel context). Non so con certezza se Live invia nome e colore della traccia. → Test: checklist 2. Se non arrivano funziona comunque il fallback "Track N" modificabile.
3. **Attivazione del bus sidechain**: il bus è disabilitato di default (come richiesto). Mi aspetto che Live lo attivi quando abiliti la sezione Sidechain, ma non l'ho verificato. → Test: checklist 6, il LED deve passare da "SC assente" a "SC segnale".
4. **Cambio di latenza a runtime**: Live riceve `kLatencyChanged` e ricompensa. Durante il play potrebbe esserci un breve dropout o servire stop/play. → Test: null test dopo un cambio Minimum→Linear fatto in play e poi da fermo.
5. **Bypass del device**: non so se il pulsante on/off di Live usa il parametro bypass VST3 (→ `processBlockBypassed`, latenza mantenuta) o il bypass interno di Live. → Test: null test della checklist 7 con Eqtrum spento.
6. **Condivisione in-process**: il registro funziona solo se tutte le istanze sono nello stesso processo. Live 12, a mia conoscenza, non isola i plugin in processi separati. → Test: checklist 1 (l'HUB vede le tracce). Se la lista resta vuota, Live li sta isolando: serve memoria condivisa cross-process, che è fuori perimetro.
7. **Automazione registrata da comandi remoti**: la TRACK applica i comandi a 30 Hz (Timer), quindi l'automazione registrata dall'HUB ha una granularità di circa 33 ms. → Test: checklist 4 e osserva la linea di automazione.
8. **Export offline con automazione in Linear**: la IR iniziale è corretta (costruita in `prepareToPlay`), ma se automatizzi parametri EQ in Linear durante un render offline la IR si ricostruisce in modo asincrono e non è sample-accurate. In Minimum è accurata a 32 campioni. → Test: Export con automazione in Linear vs play in tempo reale, con un null test.
9. **Tracce senza audio**: se Live smette di chiamare `process` su tracce inattive, i loro spettri nell'HUB decadono a zero. La traccia resta nella lista (l'heartbeat viene dal thread di analisi, non dall'audio). → Test: ferma il trasporto e verifica che le righe restino.
10. **Precisione della linear phase alle basse frequenze**: la IR è campionata su N/2+1 punti (risoluzione fs/N: 11,7 Hz con 4096 a 48 kHz, 47 Hz a 192 kHz). HP ripidi o bell stretti sotto ~100 Hz con FIR corto si discostano dalla curva minimum phase. → Test: confronta la curva con un'analisi (es. Spectrum di Live con rumore rosa) con FIR 4096 e 16384.
11. **Ridimensionamento della finestra in Live/macOS**: dovrebbe funzionare (editor ridimensionabile, limiti 960×600–2560×1600). → Test: trascina l'angolo della finestra del plugin.
12. **Numerazione "Track N"**: N è l'indice dello slot del registro, non l'ordine delle tracce in Live. → Nessun test necessario: rinomina se serve.
13. **Due build diverse caricate insieme** (es. una vecchia e una nuova) hanno registri separati e non si vedono. → Usa una sola build per volta.
