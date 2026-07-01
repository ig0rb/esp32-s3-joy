# ESP32-S3-JOY

## Scopo del progetto

Firmware per `ESP32-S3` che:

- si comporta da `USB HID host` verso un joypad/controller;
- interpreta report descriptor e input report HID;
- mantiene uno stato runtime condiviso;
- espone una UI web per monitoraggio e configurazione;
- salva la configurazione in `LittleFS` su `/config.json`.

## Mappa veloce del codice

- `src/main.cpp`
  Bootstrap minimale: inizializza stato, filesystem, rete, web server e USB HID.
- `include/app_build_config.h`
  Default dei macro `APP_*`. I valori reali arrivano normalmente da `platformio.ini`.
- `src/app_types.cpp`
  Modello dati condiviso e funzioni pure: assi, normalizzazione, trim/expo/dead-zone, conversione PPM e modalita' canale.
- `src/app_storage.cpp`
  Persistenza e parsing JSON della configurazione, inclusi i blocchi `stepper`, `latch` e `selector`.
- `src/app_state.cpp`
  Stato condiviso dell'applicazione, snapshot thread-safe, tracking del device attivo e runtime condiviso dei canali avanzati.
- `src/app_network.cpp`
  Modalita' `AP/STA`, retry Wi-Fi, logging eventi di rete.
- `src/app_web.cpp`
  Web server, file statici da `data/`, API `/api/config` e `/api/state`, serializzazione della configurazione canali.
- `src/app_usb_hid.cpp`
  Parsing HID, gestione callback USB/HID, aggiornamento dello stato del joypad.
- `src/app_ppm_output.cpp`
  Generazione reale del segnale PPM su GPIO tramite RMT, con gestione failsafe e riuso della stessa logica canali della preview web.
- `data/index.html`
  UI web che consuma le API del firmware e permette di configurare `source`, `stepper`, `latch`, `selector` e calibrazione ADC.

## Regole di modifica consigliate

- Se tocchi il formato JSON delle API, aggiorna anche `data/index.html`.
- Se aggiungi nuovi campi HID o nuovi assi, allinea `app_types`, `app_usb_hid` e `app_web`.
- Se tocchi la logica PPM o il range dei pulse, allinea `app_ppm_output.*`, `app_types.*`, `data/index.html` e l'eventuale documentazione dei pin.
- Se tocchi le modalita' canale (`source`, `stepper`, `latch`, `selector`), allinea sempre `app_types`, `app_state`, `app_storage`, `app_web` e `app_ppm_output`.
- I valori di failsafe sono configurabili via web per canale e vengono salvati in `/config.json`.
- La calibrazione ADC usa `calibrationMin` e `calibrationMax` per asse, configurabili via web e salvati in `/config.json`.
- Lo stato condiviso deve passare da `app_state.*`; evita copie globali parallele.
- La configurazione persistente deve passare da `app_storage.*`; evita scritture dirette sparse su `LittleFS`.
- La logica Wi-Fi deve restare in `app_network.*`; evita side effect di rete dentro moduli web/HID.

## Comandi utili

- Build firmware: `platformio run`
- Caricamento filesystem web: `platformio run --target uploadfs`
- Upload firmware: `platformio run --target upload`
- Monitor seriale: `platformio device monitor`

## Note per IA / futuri refactor

- Il progetto oggi gestisce un solo `device snapshot` attivo lato API, anche se il tracking interno supporta piu' slot.
- `app_state.cpp` usa un mutex FreeRTOS per sincronizzare HID task, web server, runtime canali e logica principale.
- L'uscita PPM usa di default `GPIO4` con frame `18000 us`, impulso di separazione `300 us`, polarita' `active-low` e range `900..2100 us`.
- La catena USB/HID ha priorita' FreeRTOS piu' alta della task PPM per ridurre il ritardo tra input joypad e aggiornamento del frame PPM.
- Anche i canali derivati da assi usano il range PPM completo `900..2100 us`, con centro `1500 us`.
- Gli ingressi ADC possono essere calibrati dalla UI web: il range reale min/max sostituisce il range teorico `0..4095` nella normalizzazione.
- Le modalita' `stepper`, `latch` e `selector` mantengono stato runtime in RAM e non salvano lo stato corrente su filesystem.
- Quando il joypad e' assente o non ha stato valido, l'uscita PPM passa ai valori di failsafe configurati.
- `platformio.ini` contiene configurazioni sensibili di rete: meglio evitare di replicarle nella documentazione o nei commit.
- Prima di grossi refactor conviene sempre fare una `build` completa, perche' le dipendenze tra USB, Wi-Fi e WebServer emergono subito a compile-time.
