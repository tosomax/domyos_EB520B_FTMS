# Domyos EB520 → Amazfit T-Rex 3 Pro

## Configurazione verificata il 26 ottobre 2026

Un solo pannello ESP32-4848S040 riceve i dati BLE della bike e li inoltra
all'orologio come unica periferica **Domyos FTMS**. Firmware del pannello
`bridge-ftms-6`, ambiente PlatformIO predefinito `panel_ftms`.
Firmware T-Rex 3 Pro indicato dall'utente: **6.4.16.3**.

Dopo associazione e pedalata in Ciclismo indoor, l'utente conferma il
funzionamento di **potenza, cadenza, velocità e distanza** sull'orologio.
Non occorre un secondo ESP32 né configurare la circonferenza ruota.
Display e tasti touch della resistenza restano disponibili sul pannello.

Per l'uso: accendere la bike, attendere i dati sul pannello, associare soltanto
Domyos FTMS negli accessori del T-Rex e avviare Ciclismo indoor.
Rimuovere le vecchie associazioni Domyos dei profili sperimentali.

## Protocollo del ponte

- Servizio sportivo Fitness Machine Service 1826; annuncio Indoor Bike 0x0020.
- Indoor Bike Data 2AD2: 18 byte, flag 0x0954; velocità, cadenza, distanza,
  watt, kcal totali, kcal/h, kcal/min e tempo trascorso.
- Feature 2ACC: 0x00005206; nessun target setting dichiarato.
- Notifiche ogni 500 ms, soltanto con dati della bike recenti (entro 1500 ms).
- Distanza, kcal e tempo hanno offset relativi alla sessione del ponte:
  i totali preesistenti della console non vengono importati come nuova attività.
- Training Status e Fitness Machine Status espongono lo stato della bike.
- Control Point: Request Control e start/stop/reset virtuali. Questi comandi
  non sono inoltrati alla bike; la resistenza reale si regola dal pannello.
- Il battito delle impugnature resta sul pannello; il ponte non sostituisce
  il sensore cardiaco dell'orologio.

Kcal e tempo sono presenti nel flusso FTMS; non è stato verificato se il T-Rex
usi questi valori o i propri calcoli. Salvataggio e sincronizzazione delle
serie finali in Zepp non sono ancora stati verificati separatamente.

## Evidenze

`captures/bridge-07-ftms.log` registra connessione del watch, sottoscrizione
Indoor Bike Data e trasmissione durante la pedalata, poi ritorno a zero.
Esempio: 40 W, 65 RPM, 15,70 km/h; distanza crescente fino a 78 m nella prova.
La conferma della visualizzazione dei quattro campi viene dall'utente.
Hash del binario caricato: `captures/bridge-ftms-firmware.sha256`.

Build e caricamento sono descritti in [PANEL.md](PANEL.md).
Sorgente: `src/panel/ftms_bridge.cpp`; i vecchi profili restano disponibili
come ambienti sperimentali, senza essere la configurazione predefinita.

## Perché i primi tentativi erano incompleti

| Profilo provato | Risultato osservato sul T-Rex |
|---|---|
| CPS, anche con eventi ruota e pedivella | Solo potenza |
| CSC sola pedivella, Domyos Cadence | Cadenza |
| CSC ruota e pedivella, Domyos Motion | Velocità e distanza; cadenza non visualizzata |
| Due identità Power e Motion sullo stesso pannello | Il secondo accessorio veniva rifiutato |
| FTMS, Domyos FTMS | Potenza, cadenza, velocità e distanza insieme |

I contatori corretti presenti nei pacchetti CPS/CSC non garantivano che il
watch li utilizzasse. Il rifiuto di due identità virtuali non dimostra un
limite hardware complessivo di due connessioni dell'ESP32.

La ricerca iniziale aveva mancato la nota ufficiale Amazfit sul supporto
FTMS: la pagina prodotto, September 2026 / OTA 11.1, dichiara il supporto di
trainer e spin bike FTMS con potenza, cadenza, velocità e distanza.
Non è stata trovata una mappatura ufficiale tra OTA 11.1 e 6.4.16.3;
il funzionamento della versione dell'utente è ora verificato fisicamente.

[Note ufficiali Amazfit T-Rex 3 Pro](https://us.amazfit.com/products/t-rex-3-pro).
