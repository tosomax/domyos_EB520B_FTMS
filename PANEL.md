# Pannello Domyos EB520

## Hardware e compilazione

Scheda identificata realmente su `/dev/ttyUSB0`: ESP32-S3 rev 0.2,
16 MB flash e 8 MB PSRAM OPI. Display ST7701 480×480, touch GT911 rilevato
all'indirizzo 0x5D. Il firmware usa Arduino ESP32 2.0.17, Arduino_GFX 1.4.7
e LVGL 8.4.0; pixel clock 8 MHz, draw buffer LVGL in PSRAM.

Il backup del firmware precedente è stato interrotto su richiesta dell'utente;
non è disponibile un'immagine di ripristino del software originale.

```sh
pio run -e panel_ftms
esptool --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 write-flash 0x0 .pio/build/panel_ftms/bootloader.bin 0x8000 .pio/build/panel_ftms/partitions.bin 0x10000 .pio/build/panel_ftms/firmware.bin
```

L'ambiente `esp32dev` conserva il precedente programma di indagine; `panel`
compila esclusivamente i file `src/panel/`. Nessun servizio cloud o Home
Assistant necessario, nessuna configurazione Wi-Fi presente.

## Interfaccia

Nell’intestazione sono separati lo stato della bike e quello dell’orologio:
`OROLOGIO COLLEGATO` in verde acqua quando il collegamento BLE è attivo,
`OROLOGIO NON COLLEGATO` in grigio altrimenti, anche con bike scollegata.

- Principale: velocità, watt, RPM, battito, km, kcal, tempo e resistenza.
- Icona in alto a destra: pagina dettagli con velocità media, kcal/h, kcal/min,
  tempo in secondi, numero pacchetti e intervallo della resistenza.
- In pausa il tempo lampeggia. Dati mancanti, battito zero o connessione scaduta
  mostrano `--`; i pulsanti si disabilitano quando i dati non sono aggiornati.
- Il livello visualizzato proviene sempre dai pacchetti della bike.

## Bluetooth e controllo

Ricerca di un nome che inizi con `Domyos-Bike-`, connessione e riconnessione
automatiche. Quattro sottoscrizioni: dati 2AD2, allenamento 2AD3, stato 2ADA,
indicazioni del controllo 2AD9. Il parser gestisce il layout EB520 di 23 byte
con flag 0x0B76 rilevato nelle prove precedenti; altri layout sono scartati e
segnalati su seriale.

I pulsanti richiedono un passo singolo, limitato a 1..15. Al primo comando
viene inviato Request Control (00), poi Set Target Resistance (04, livello×10),
in accordo con l'intervallo grezzo 10..150 letto dalla bike. Non viene inviato
Start, Stop, Reset o alcun comando automatico di resistenza al collegamento.
Il risultato viene confermato solo dopo risposta positiva del Control Point
e ricezione del livello richiesto nel flusso dati. Le richieste non si accumulano.
La verifica fisica del controllo deve essere registrata separatamente.

## Verifiche del 26 ottobre

- Compilazione riuscita, immagine caricata e hash verificato da esptool.
- Avvio reale: PSRAM 8386279 byte, flash 16777216 byte, touch a 0x5D,
  `DISPLAY_OK` e `UI_READY`.
- L'utente conferma di vedere l'intera schermata sul display.
- Connessione automatica reale a Domyos-Bike-1696; quattro sottoscrizioni
  accettate con stato 0; ricezione dei dati durante la pedalata.
- Touch +: Request Control e Set Target Resistance ricevono risposta 1
  (successo); il successivo flusso dati conferma i livelli richiesti.
- L'utente conferma «funziona tutto» dopo la prova dei dati e dei pulsanti +/−.
  Funzionamento fisico e touch confermati dall'utente.
- Evidenze seriali in `captures/panel-02.log`; hash del binario installato
  in `captures/panel-firmware.sha256`.

## Ponte T-Rex: configurazione successiva

La configurazione predefinita è `panel_ftms`, firmware `bridge-ftms-6`.
Il pannello riceve i dati dalla bike e li espone come unica periferica
`Domyos FTMS`. Sul T-Rex 3 Pro con firmware 6.4.16.3 l'utente ha confermato
potenza, cadenza, velocità e distanza nel ciclismo indoor. Non occorre
impostare una circonferenza ruota. Dettagli e prove in `AMAZFIT.md`.

La seriale a 115200 registra boot, touch, connessioni, campioni dati ogni due
secondi e risposte dei comandi. `+` e `-` su seriale eseguono lo stesso percorso
dei pulsanti, per diagnosi. Durante le prove usare:

```sh
python3 tools/capture.py --interactive --seconds 3600 --output captures/panel-live.log
```

## Riferimenti

- [Arduino_GFX: sequenza della scheda nella libreria](https://github.com/moononournation/Arduino_GFX/blob/v1.4.7/src/display/Arduino_RGB_Display.h)
- [Esempio LVGL della scheda](https://github.com/paulhamsh/LVGL_GUITION)
- [Fitness Machine Service](https://www.bluetooth.com/specifications/specs/fitness-machine-service-1-0/)
