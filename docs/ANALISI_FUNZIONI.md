# Analisi del codice e separazione funzioni

## Situazione iniziale

Il progetto partiva da un unico `src/main.cpp` di circa 2600 righe con responsabilita' miste:

- parsing HID;
- stato condiviso;
- persistenza JSON;
- rete Wi-Fi;
- web server;
- bootstrap applicativo.

Questo rendeva difficile:

- capire dove intervenire;
- stimare l'impatto di una modifica;
- delegare lavoro a un'IA senza farle toccare aree non correlate.

## Come sono state raggruppate le funzioni

### Modello e trasformazioni pure

Spostate in [src/app_types.cpp](/home/ib/Documents/PlatformIO/Projects/ESP32-S3-JOY/src/app_types.cpp):

- `axisDescriptorAt`
- `axisIndexFromName`
- `axisSourceLabel`
- `readAxisValue`
- `normalizeAxisValue`
- `applyAxisConfigToValue`
- `ppmPulseFromValue`
- `buildAxisSample`
- `zeroConfigFromCurrentState`
- definizione del range PPM `900..2100 us` tramite costanti condivise

Queste funzioni non dipendono da Wi-Fi, filesystem o USB.

### Configurazione e filesystem

Spostate in [src/app_storage.cpp](/home/ib/Documents/PlatformIO/Projects/ESP32-S3-JOY/src/app_storage.cpp):

- `parseConfigJson`
- `saveConfigToFilesystem`
- `loadConfigFromFilesystem`
- helper JSON (`extractJsonInt`, `extractJsonBool`, `extractJsonString`, ecc.)
- parsing e salvataggio delle modalita' canale `stepper`, `latch`, `selector`
- `startFileSystem`

Questa e' l'area giusta per aggiungere nuovi parametri persistenti.

### Stato condiviso e snapshot

Spostate in [src/app_state.cpp](/home/ib/Documents/PlatformIO/Projects/ESP32-S3-JOY/src/app_state.cpp):

- `resetConfigToDefaults`
- `initializeConfig`
- `copyConfigSnapshot`
- `setConfigSnapshot`
- `copyActiveDeviceSnapshot`
- `buildChannelOutputState`
- tracking del device HID attivo
- runtime condiviso dei canali avanzati

Questa separazione aiuta a mantenere unico il punto di verita' dello stato runtime.

### USB HID e parsing report

Spostate in [src/app_usb_hid.cpp](/home/ib/Documents/PlatformIO/Projects/ESP32-S3-JOY/src/app_usb_hid.cpp):

- `parseReportDescriptor`
- `decodeInputReport`
- `onInputReport`
- `handleConnectedDevice`
- `hidHostTask`
- `usbLibTask`
- `startUsbHostJoypad`

Questa area resta la piu' delicata, ma ora e' isolata dal resto.

### Rete Wi-Fi

Spostate in [src/app_network.cpp](/home/ib/Documents/PlatformIO/Projects/ESP32-S3-JOY/src/app_network.cpp):

- `registerWiFiEventLogger`
- `startNetwork`
- `startAccessPoint`
- `startStationMode`
- `maintainStationMode`

Questo rende piu' facile cambiare policy AP/STA senza toccare HID o storage.

### Web server e API

Spostate in [src/app_web.cpp](/home/ib/Documents/PlatformIO/Projects/ESP32-S3-JOY/src/app_web.cpp):

- `serveStaticFile`
- `handleConfigGet`
- `handleConfigPost`
- `handleConfigZero`
- `handleConfigReset`
- `handleStateGet`
- `handleNotFound`
- serializzazione JSON per UI
- descrizione live delle modalita' canale

### Uscita PPM reale

Spostata in [src/app_ppm_output.cpp](/home/ib/Documents/PlatformIO/Projects/ESP32-S3-JOY/src/app_ppm_output.cpp):

- `startPpmOutput`
- `pollPpmOutput`
- costruzione del frame PPM da snapshot/config
- applicazione del failsafe per canale quando il joypad non e' disponibile
- trasmissione hardware via `RMT`

Questa area e' il punto corretto per cambiare pin, polarita', frame length o strategia di generazione.
La generazione reale ora riusa la stessa risoluzione canali usata dalla preview web.

## Risultato della separazione

`main.cpp` ora contiene solo l'orchestrazione di alto livello.

Questo migliora:

- leggibilita';
- manutenibilita';
- testabilita' logica;
- facilita' di refactor incrementali;
- supporto al lavoro dell'IA, che puo' intervenire su un modulo preciso invece di navigare un file monolitico.

## Prossimi miglioramenti sensati

- introdurre test per parsing config e trasformazione assi;
- introdurre test per le modalita' canale `stepper`, `latch` e `selector`;
- estrarre una piccola libreria JSON per ridurre parser manuale;
- valutare export/import esplicito della configurazione canali dalla UI;
- spostare le credenziali Wi-Fi fuori da `platformio.ini` versionato.
