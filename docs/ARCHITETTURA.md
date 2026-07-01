# Architettura

## Obiettivo

Il firmware e' organizzato attorno a un flusso semplice:

1. acquisizione input HID dal joypad USB;
2. trasformazione in stato standardizzato;
3. risoluzione della logica canali (`source`, `stepper`, `latch`, `selector`);
4. pubblicazione via API web;
5. configurazione persistente di trim, expo, dead-zone, failsafe e mapping canali.

## Flusso di bootstrap

`setup()` in [src/main.cpp](/home/ib/Documents/PlatformIO/Projects/ESP32-S3-JOY/src/main.cpp) esegue in ordine:

1. inizializzazione seriale;
2. creazione stato condiviso;
3. mount di `LittleFS`;
4. caricamento o creazione configurazione;
5. avvio campionamento ingressi ADC locali;
6. avvio rete `AP` o `STA`;
7. avvio web server;
8. avvio stack USB host HID.

`loop()` resta volutamente piccolo:

- mantiene la connessione Wi-Fi in `STA` quando il progetto e' in dev mode;
- serve le richieste HTTP della UI.

## Responsabilita' dei moduli

### `app_types`

Contiene il modello dati comune e la logica pura:

- descrittori degli assi;
- normalizzazione dei valori;
- applicazione di trim, dead-zone ed expo;
- conversione del valore normalizzato in impulso PPM nel range `900..2100 us`;
- definizione delle modalita' canale e delle relative struct di configurazione;
- costruzione di sample runtime per la UI.

Non deve avere side effect hardware o filesystem.

### `app_storage`

Gestisce:

- mount del filesystem;
- parsing del JSON di configurazione;
- sanitizzazione dei valori PPM e delle modalita' canale;
- salvataggio/caricamento di `/config.json`.

E' il solo punto che dovrebbe conoscere il path persistente della config.

### `app_state`

Gestisce lo stato condiviso tra task e moduli:

- configurazione corrente;
- revisione configurazione;
- stato del device HID attivo;
- snapshot degli ingressi ADC locali;
- stato runtime dei canali avanzati;
- snapshot letti dalle API web.

Usa un mutex FreeRTOS per ridurre i race condition tra callback HID, runtime canali e web server.

### `app_network`

Incapsula:

- modalita' `AP`;
- modalita' `STA` per sviluppo;
- retry e log di connessione;
- snapshot dello stato di rete per la UI.

### `app_web`

Espone:

- file statici da `data/`;
- `GET /api/state`;
- `GET/POST /api/config`;
- `GET /api/config/backup`;
- `POST /api/config/restore`;
- `POST /api/config/zero`;
- `POST /api/config/reset`.

Qui vive anche la serializzazione JSON usata dalla UI, inclusi i blocchi `stepper`, `latch` e `selector`.

### `app_analog_input`

Incapsula la lettura hardware degli ingressi analogici dell'ESP32-S3:

- configura pin, risoluzione e attenuazione ADC;
- campiona periodicamente gli ingressi locali;
- pubblica i valori nello stato condiviso;
- non conosce logiche canale, web o persistenza.

### `app_ppm_output`

Genera il segnale PPM reale tramite periferica `RMT`:

- usa la stessa pipeline logica della preview canali;
- parte con valori neutrali anche senza joypad collegato;
- aggiorna il frame quando cambiano stato HID o configurazione;
- applica i valori di failsafe per canale quando il joypad viene scollegato;
- usa il range `900..2100 us` con centro `1500 us`;
- usa di default `GPIO4` su `ESP32-S3-DevKitC-1`.

### `app_usb_hid`

Contiene la logica piu' specialistica:

- parser del report descriptor HID;
- decoder degli input report;
- callback USB/HID;
- task FreeRTOS legati allo stack USB host.

E' il modulo da toccare quando cambiano supporto a controller o semantica HID.

## Flusso dati principale

1. `app_usb_hid` riceve un report dal controller.
2. Il report viene decodificato in `GamepadState`.
3. `app_state` aggiorna snapshot HID e sequence.
4. `app_analog_input` aggiorna in parallelo gli snapshot ADC locali.
5. `app_state` risolve il runtime dei canali avanzati sui fronti dei pulsanti.
6. `app_web` legge snapshot e config.
7. La UI in `data/index.html` visualizza assi HID, ADC, pulsanti e mapping canali.
8. `app_ppm_output` converte i canali in un frame PPM fisico su GPIO.
9. Se una sorgente richiesta non e' disponibile, `app_ppm_output` usa il failsafe del singolo canale.

## Punti di estensione consigliati

- Nuovi endpoint HTTP: `app_web.cpp`
- Nuovi parametri persistenti: `app_types.h`, `app_storage.cpp`, `app_web.cpp`
- Nuove modalita' canale o policy runtime: `app_types.h`, `app_state.cpp`, `app_storage.cpp`, `app_web.cpp`, `app_ppm_output.cpp`
- Nuovi assi HID o nuove sorgenti analogiche: `app_types.h`, `app_analog_input.cpp` e `app_web.cpp`
- Nuove policy di rete: `app_network.cpp`
