# BluetoothMax

BluetoothMax ist ein unabhängiges Open-Source-Gateway für das MILLENNIUM-
ChessLink-Protokoll. Es ersetzt das Kabel zwischen einem Bluetooth-fähigen
MILLENNIUM-E-Board und einem kompatiblen Schachcomputermodul durch Bluetooth LE.

Der aktuelle Prototyp verbindet:

```text
MILLENNIUM Supreme T2 BT E-Board
              ⇅ Bluetooth LE
        ESP32-C3 SuperMini
              ⇅ UART / 3,3 V
       HW-027 mit MAX3232
              ⇅ RS-232
       4-poliger Mini-DIN
              ⇅
MILLENNIUM The King Schachcomputermodul
```

## Projektstatus

Das Projekt befindet sich in der Prototyp- und Protokolltestphase. Die
bidirektionale Verbindung zum Original-E-Board und zum Original-The-King-Modul
ist hergestellt. Aktuell wird die vollständige Mode-B-/ChessLink-Kompatibilität
für Spiel und Analyse getestet.

Die veröffentlichte Firmware für Anwender bleibt unverändert, bis eine neue
Version ausdrücklich freigegeben wird.

## Bauteile

- ESP32-C3 SuperMini
- HW-027 RS-232-zu-TTL-Modul mit MAX3232
- DC/DC-Abwärtswandler von 9 V auf 5 V
- 4-poliger Mini-DIN-Stecker beziehungsweise passende Anschlussleitung
- Leitungen und geeignete Steck- oder Lötverbindungen
- optional: Gehäuse, Zugentlastung und Isoliermaterial

## Elektrische Schnittstellen

### ChessLink-Kabelseite

| Mini-DIN-Pin | Funktion |
|---:|---|
| 1 | +9 V Versorgung |
| 2 | GND |
| 3 | TxD |
| 4 | RxD |

Die TxD/RxD-Bezeichnungen sind immer aus Sicht des jeweils sendenden Gerätes zu
prüfen. Entscheidend ist die Signalrichtung im folgenden Verdrahtungsplan.

### Serielle Parameter

- 38400 Baud
- 7 Datenbits
- ungerade Parität
- 1 Stoppbit (`7O1`)

## Verdrahtungsplan

### Versorgung

```text
The King +9 V  ──> DC/DC IN+
The King GND   ──> DC/DC IN-

DC/DC OUT 5 V ──> ESP32-C3 5V/VBUS
DC/DC GND      ──> ESP32-C3 GND

ESP32-C3 3V3   ──> HW-027 VCC (+), TTL-Seite
ESP32-C3 GND   ──> HW-027 GND (-), TTL-Seite
```

Alle Komponenten benötigen eine gemeinsame Masse. Die 9 V der Kabelseite
dürfen niemals direkt an einen GPIO oder an den 3,3-V-Pin des ESP32 gelangen.

### Datenleitungen

```text
The King TX
    ──> HW-027 RS-232-Eingang
    ──> HW-027 TTL-Ausgang
    ──> ESP32-C3 GPIO20 (RX)

ESP32-C3 GPIO21 (TX)
    ──> HW-027 TTL-Eingang
    ──> HW-027 RS-232-Ausgang
    ──> The King RX
```

GPIO20 ist in der Firmware ausschließlich der Empfangspfad vom King. GPIO21 ist
der Sendepfad zum King.

## Wichtige Sicherheitshinweise

- Den ESP32 ausschließlich an der **TTL-Seite** des HW-027 anschließen.
- Vor dem Anschluss Versorgungsspannungen und Masse mit einem Multimeter prüfen.
- Das HW-027 für die TTL-Seite mit 3,3 V betreiben.
- Niemals RS-232-Pegel direkt mit einem ESP32-GPIO verbinden.
- Bei Mini-DIN-Steckern auf die Blickrichtung achten: Lötseite und Steckseite
  erscheinen spiegelverkehrt.
- Arbeiten an Versorgung und Verdrahtung nur im ausgeschalteten Zustand.

## Firmware

PlatformIO-Ziel für den aktuellen Prototyp:

```ini
[env:esp32-c3-supermini]
```

Die Firmware arbeitet als bidirektionales Gateway zwischen der seriellen
Mode-B-Schnittstelle des Schachcomputers und dem transparenten BLE-UART-Dienst
des E-Boards.

## Marken-, Urheberrechts- und Protokollhinweis

BluetoothMax ist ein unabhängiges, inoffizielles Interoperabilitätsprojekt und
steht in keiner Verbindung zu MILLENNIUM 2000 GmbH. Es wird von MILLENNIUM weder
unterstützt noch empfohlen.

MILLENNIUM, ChessLink und zugehörige Produktnamen, Marken, Dokumentationen und
Protokollspezifikationen bleiben Eigentum ihrer jeweiligen Rechteinhaber.
Dieses Projekt beansprucht keine Rechte am ChessLink-Protokoll. Es verteilt
keine originale MILLENNIUM-Firmware, -Software oder sonstige urheberrechtlich
geschützte Inhalte.
