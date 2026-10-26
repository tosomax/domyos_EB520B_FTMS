# EB520 — evidenze del 26 ottobre 2026

Acquisizione reale con ESP32 su `/dev/ttyUSB0`. Log principale:
`captures/06-live-async.log`. Orari nei log in UTC (ora italiana = UTC+1).

## Identificazione letta dalla console

- Nome: Domyos-Bike-1696
- Indirizzo osservato: 1c:32:34:36:65:36, address type random (1)
- Produttore: DECATHLON
- Model Number: 8872851
- Hardware: 2.0.0
- Firmware Console: 1.6.0; Bluetooth: 3.2.0
- Bootloader Console: 1.1.0; Bluetooth Bootloader: 1.0.0

Questi sono valori letti via GATT. La stringa Bluetooth 3.2.0 è una versione
firmware dichiarata dal dispositivo, non la versione dello standard radio.

## Servizi enumerati

| UUID | Servizio |
|---|---|
| 1800 | Generic Access |
| 1801 | Generic Attribute |
| 180a | Device Information |
| 181c | User Data |
| 1826 | Fitness Machine |
| 49535343-fe7d-4ae5-8fa9-9fafd205e455 | Proprietario, RX scrivibile handle 62 / TX notificabile handle 64 |
| 02f00000-0000-0000-0000-00000000fe00 | Proprietario, caratteristiche handle 68/70/74/76 |

Il servizio User Data espone valori di profilo leggibili e un punto di controllo.
Non sono stati modificati. Nessun servizio Heart Rate separato è stato enumerato;
il battito è incluso nei dati Fitness Machine.

## Fitness Machine

| Caratteristica | Handle | Risultato |
|---|---:|---|
| 2acc Feature | 45 | `8756000004000000` |
| 2ad2 Indoor Bike Data | 47 | Notifiche reali, 23 byte, flags `0x0b76`, circa 4 Hz |
| 2ad3 Training Status | 50 | Letto e notificato; inizialmente `010d` |
| 2ad6 Supported Resistance Range | 53 | `0a0096000a00`, valori grezzi 10/150/10 |
| 2ad9 Control Point | 55 | Scrivibile e indicazioni; nessun comando inviato |
| 2ada Machine Status | 58 | Notifiche osservate `0202` e `04` durante arresto/ripresa |

Feature lette come due uint32 little-endian: `0x00005687` e `0x00000004`.
Indicano supporto dichiarato a velocità media, cadenza, distanza, resistenza,
energia, battito, tempo trascorso e potenza; il secondo campo dichiara il target
di resistenza. La presenza del Control Point e dei bit non dimostra ancora il
funzionamento del controllo fisico remoto.

## Pacchetto effettivamente ricevuto

Esempio: `760bec09fc03ae001503000700850010003b020900ce01`

| Offset (da zero) | Byte | Interpretazione della cattura |
|---|---:|---|
| 0 | 2 | Flags `0x0b76` |
| 2 | 2 | Velocità: uint16 LE / 100 = 25,4 km/h |
| 4 | 2 | Velocità media: uint16 LE / 100 = 10,2 km/h |
| 6 | 2 | Cadenza: uint16 LE / 2 = 87 RPM |
| 8 | 3 | Distanza: uint24 LE = 789 m |
| 11 | 2 | Resistenza grezza: 7 |
| 13 | 2 | Potenza: sint16 LE = 133 W |
| 15 | 2 | Energia totale: 16 kcal |
| 17 | 2 | Energia oraria: 571 kcal/h |
| 19 | 1 | Energia al minuto: 9 kcal/min |
| 20 | 1 | Battito: 0 BPM (nessuna misura valida verificata) |
| 21 | 2 | Tempo trascorso: 462 s |

Lo script `tools/decode.py` valida lunghezza e flags e conserva il valore di
resistenza grezzo. Il layout osservato usa 2 byte per la resistenza; non
applicare alla cieca un layout diverso. Confronto puntuale col display ancora
da completare; i valori mostrati sopra sono decodifiche, non misure indipendenti.
Non è determinato se i watt siano misurati o stimati dalla console.

## Prova fisica confermata dall'utente

L'utente ha pedalato, rallentato quasi a zero, impostato 7, pedalato nuovamente,
impostato il massimo 15 e pedalato prima di fermarsi. La sequenza coincide con
le notifiche acquisite. Durante la connessione il display mostra solo il livello
di resistenza: questo è confrontabile direttamente, le altre metriche no.

Prima sessione: 563 notifiche Indoor Bike Data in 140,479 secondi (4,01 Hz).
Valori di picco durante la pedalata, distinti per livello (non sono confronti
a cadenza controllata):

| Resistenza | Velocità massima ricevuta | Cadenza massima ricevuta | Potenza massima ricevuta |
|---:|---:|---:|---:|
| 1 | 20,3 km/h | 90 RPM | 76 W |
| 7 | 26,1 km/h | 87 RPM | 143 W |
| 15 | 33,1 km/h | 65 RPM | 256 W |

Alla fine velocità, cadenza e potenza tornano a zero; distanza ricevuta 949 m,
energia 21 kcal. I contatori includevano attività precedente all'inizio della
cattura. Battito sempre zero. Il tempo trascorso smette di avanzare durante
l'arresto pur continuando ad arrivare notifiche.

Seconda sessione, firmware v2: il campo battito passa da zero a valori non nulli
alle 21:00:48 UTC (22:00:48 italiane), inizialmente 85 BPM, poi 83–87 BPM,
anche con velocità e cadenza nulle. L'utente conferma di aver tenuto le mani
sui sensori e di aver pedalato brevemente durante questa seconda sessione.
È confermata la trasmissione di una misura cardiaca, non la sua accuratezza
clinica. Il flusso 2ada emette `0202` (pausa) e `04` (avvio/ripresa).

## Limiti della prima acquisizione

- Il servizio generico Service Changed bloccava la chiamata sincrona della
  libreria: escluso dalla sottoscrizione e usate API asincrone.
- Le prime cinque registrazioni delle notifiche sono accettate; le due dei
  servizi proprietari ricevono stato 128. Non attribuire questo risultato a
  protezione/autenticazione della bike: potrebbe essere un limite del client.
  Verificato nel SDK locale `CONFIG_BT_GATTC_NOTIF_REG_MAX=5`. Firmware v2:
  iscrizione ai soli cinque flussi Notify, senza le indicazioni dei punti di
  controllo inutilizzati. In `captures/07-live-v2.log` tutte e cinque le
  registrazioni e scritture CCCD hanno esito 0, inclusi gli handle 70 e 64.
  A riposo i canali proprietari non hanno ancora emesso dati: non significa che
  siano privi di funzioni; potrebbe servire un protocollo applicativo non provato.
- Il primo log `01-scan.log` ha payload ADV_RAW invalidi; vedere README.
- Nessuna prova Bluetooth Classic o intercettazione di app terze eseguita.

Riferimenti primari: [Fitness Machine Service](https://www.bluetooth.com/specifications/specs/fitness-machine-service-1-0/)
e [GATT Supplement](https://www.bluetooth.com/specifications/gss/).

## Controllo remoto verificato con il pannello

Nella successiva prova ESP32-S3 con display LVGL, Request Control `00` e
Set Target Resistance `04 LL` funzionano, con LL=livello×10 (per esempio
`04 46` richiede livello 7). Le indicazioni di risposta sono `80 00 01` e
`80 04 01`; la telemetria conferma poi il livello. L'utente ha verificato
fisicamente dati e comandi touch +/− e conferma che funziona tutto.
Vedere `PANEL.md` e `captures/panel-02.log` per questa prova.
