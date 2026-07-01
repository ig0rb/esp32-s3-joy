# ESP32-S3-JOY

Firmware per ESP32-S3 che consente di collegare un joypad USB e convertirne gli input in segnale PPM, con gestione avanzata dello stato, configurazione via web e salvataggio persistente.

## Funzionalità principali

- **USB HID Host**: Collegamento e gestione di joypad/controller USB tramite stack HID host.
- **Parsing HID**: Interpretazione dinamica di report descriptor e input report HID per supportare diversi controller.
- **Stato condiviso**: Gestione thread-safe dello stato del joypad e del sistema tramite mutex FreeRTOS.
- **Web UI**: Interfaccia web (su ESP32) per monitoraggio stato e configurazione di assi, failsafe e logiche canale.
- **Configurazione persistente**: Salvataggio e caricamento della configurazione in formato JSON su LittleFS (`/config.json`).
- **Backup e restore JSON**: Export/import della configurazione con controllo di compatibilita' su schema e layout del firmware, con reboot automatico dopo il restore.
- **Output PPM**: Generazione segnale PPM su GPIO configurabile, con gestione failsafe per ogni canale e range `900..2100 us`.
- **Ingressi analogici locali**: Supporto a ingressi ADC dell'ESP32-S3, esposti come sorgenti aggiuntive assegnabili ai canali PPM.
- **Canali avanzati**: Ogni canale puo' essere configurato come `source`, `stepper`, `latch` o `selector`.
- **Modalità Wi-Fi**: Supporto AP/STA, gestione retry e logging eventi di rete.

## Struttura del progetto

- `src/` — Codice sorgente principale (logica, stato, parsing HID, output PPM, web server)
- `include/` — Header file condivisi
- `lib/esp32_usb_host_hid/` — Libreria per gestione USB HID host
- `data/` — File statici per la web UI (es. `index.html`)
- `docs/` — Documentazione tecnica e analisi
- `test/` — Test e script di esempio
- `tools/` — Script di utilità (es. server statico per sviluppo web UI)

## File principali

- `src/main.cpp`: Bootstrap, inizializzazione moduli principali
- `src/app_types.cpp`: Modello dati, normalizzazione assi, conversione PPM
- `src/app_usb_hid.cpp`: Parsing HID, callback USB, aggiornamento stato joypad
- `src/app_state.cpp`: Stato condiviso, snapshot thread-safe, runtime canali avanzati
- `src/app_web.cpp`: Web server, API REST, file statici e serializzazione config
- `src/app_storage.cpp`: Persistenza configurazione JSON e parser manuale della config
- `src/app_ppm_output.cpp`: Generazione segnale PPM, gestione failsafe, riuso della logica canali
- `src/app_analog_input.cpp`: Campionamento periodico degli ingressi ADC locali e pubblicazione nello stato condiviso
- `src/app_network.cpp`: Gestione Wi-Fi, AP/STA, logging

## Comandi utili

- Build firmware: `platformio run`
- Upload firmware: `platformio run --target upload`
- Caricamento filesystem web: `platformio run --target uploadfs`
- Monitor seriale: `platformio device monitor`

## Note

- Su molte ESP32-S3 DevKitC-1 il LED RGB di stato usa `GPIO48`, ma funziona
  solo se il jumper `RGB` sulla scheda e' chiuso/presente. Se il LED resta
  spento, controlla prima questo collegamento hardware.

## Configurazione tramite settings.ini

Il file `settings.ini` (da creare copiando e rinominando `settings.ini.example`) permette di personalizzare rapidamente i parametri principali del firmware senza modificare il codice sorgente. Alcuni dei parametri configurabili:

- **dev_mode**: Abilita funzioni di debug/log.
- **ap_ssid, ap_password, ap_channel, ap_ip, ...**: Configurazione della rete Wi-Fi in modalità Access Point (nome, password, canale, IP, gateway, subnet, max client).
- **wifi_ssid, wifi_password, wifi_connect_timeout_ms**: Parametri per la connessione a una rete Wi-Fi esistente (SSID, password, timeout).
- **ui_poll_ms**: Frequenza di aggiornamento della UI web.
- **analog_input_count, analog_input_*_pin, analog_read_interval_ms, analog_read_resolution_bits, analog_samples_per_read**: Parametri per abilitare e campionare gli ingressi ADC locali.
- **ppm_channel_count, ppm_output_pin, ppm_frame_us, ppm_pulse_us, ppm_min_sync_us, ppm_active_low**: Parametri per la generazione del segnale PPM (numero canali, pin di uscita, durata frame, impulso, sincronizzazione, polarità).

Modifica `settings.ini` secondo le tue esigenze prima di compilare e caricare il firmware. I valori di default sono pensati per un uso tipico, ma puoi adattarli facilmente.

## Mappatura canali

La configurazione canali disponibile via web supporta queste modalita':

- `source`: collega un asse HID, un ingresso ADC o un pulsante direttamente al canale PPM.
- `stepper`: usa due pulsanti `up/down` per muovere il canale a step tra `min` e `max`.
- `latch`: un pulsante alterna tra `activePulse` e `resetPulse`.
- `selector`: piu' pulsanti impostano valori PPM discreti e il canale mantiene l'ultima selezione.

I valori PPM sono clampati a `900..2100 us`, con centro `1500 us`. Questo vale sia per i canali da asse sia per quelli avanzati.

Di default gli ingressi analogici locali sono:

- `adc1` su `GPIO5`
- `adc2` su `GPIO6`

---
## Autore

Progetto sviluppato da [ib].
