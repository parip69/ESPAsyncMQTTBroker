![Logo](logo.svg)

# ESPAsyncMQTTBroker

Ein asynchroner MQTT-Broker für den ESP32 auf Basis von `AsyncTCP`.

**Hier weiterarbeiten:** [TODO-Liste für die nächsten MQTT-3.1.1-Schritte](TODO.md).

## Features

- MQTT-Broker läuft direkt auf dem ESP32
- Volle Kontrolle über Topics, Clients und Nachrichten
- Callback-Schnittstellen für Clients, Nachrichten, Fehler und Subscriptions
- Keine Internetverbindung erforderlich – funktioniert komplett lokal
- Kompatibel mit PlatformIO und dem Arduino-Framework

## Aktueller Implementierungsstand

Stand: **2.0.224**. MQTT **3.1.1** ist der aktive unterstützte Protokollstand.
MQTT **5 wird nicht unterstützt**. Die MQTT-3.1.1-Arbeiten aus Paket 1 und 2
sind umgesetzt; eine unabhängige Konformitätszertifizierung liegt nicht vor.

Der bisherige Stand bleibt unter Tag `v2.0.222` erhalten.
Abgeschlossene Pakete und weitere Aufgaben stehen in [TODO.md](TODO.md).

### Binärdaten, Sessions und QoS in 2.0.224

- Binärpayloads bleiben einschließlich Nullbytes bei Live-Zustellung,
  Retained, Last Will und QoS-Wiederholungen unverändert.
- Alle vorgeschriebenen MQTT-Textfelder werden auf gültiges UTF-8 geprüft.
  Das Passwort ist ein unverändertes Binärfeld; Leerzeichen werden nicht entfernt.
- `CleanSession=0` erhält die Session einschließlich Subscriptions,
  Offline-Nachrichten und beider QoS-2-Richtungen. `CleanSession=1` löscht den
  vorherigen Zustand. Auch eine leere Session setzt bei Wiederaufnahme
  `Session Present=1`. DISCONNECT schließt die TCP-Verbindung.
- Offline werden passende Publish-QoS-1/2-Nachrichten gespeichert, auch
  bei Subscription-QoS 0. Offline-Publish-QoS 0 wird nicht gespeichert.
- Unbestätigte QoS-Nachrichten behalten ihre Identifier. Wiederaufnahme
  sendet PUBLISH mit DUP beziehungsweise PUBREL im vorhandenen Zustand.
  PUBREL-Wiederholungen lösen keine doppelte QoS-2-Anwendungszustellung aus.
- Zustandszugriffe aus AsyncTCP, `loop()` und öffentlichen Methoden sind
  synchronisiert. Große Empfangspuffer werden nach Verarbeitung freigegeben.

Sessions, Offline-Queues und Retained-Daten liegen im **RAM**. Sie überleben
TCP-Verbindungsabbrüche, jedoch keinen ESP32-Neustart oder Stromausfall.
Ein `stop()`/`begin()` am selben Broker-Objekt erhält persistente Sessions
und Retained-Daten; die Zerstörung des Objekts löscht sie.

#### Binär-API und bestehende Callbacks

```cpp
const uint8_t data[] = {0x41, 0x00, 0xFF};
bool accepted = broker.publish("device/binary", data, sizeof(data), false, 1);
broker.onBinaryMessage([](const String& clientId, const String& topic,
                         const uint8_t* payload, size_t length) {
    // payload mit length auswerten; der Zeiger gilt nur waehrend dieses Aufrufs.
});
```

Die Textüberladungen und `onMessage(clientId, topic, String)` bleiben verfügbar.
Der String behält die Payload-Länge einschließlich Nullbytes. `c_str()` als
C-String endet am ersten Nullbyte; für Binärdaten den neuen Callback oder
`length()` mit längenbewusster Verarbeitung verwenden. Bei QoS 2 werden
Nachrichten-Callbacks erst nach PUBREL einmal aufgerufen.

`publish()` liefert `true`, wenn der Broker die Nachricht übernommen hat,
auch ohne passende Empfänger. Das ist keine Empfangsbestätigung des Geräts.
Bei ungültigen Parametern oder Ressourcenmangel liefert die Methode `false`.
`getConnectedClientsInfo()` liefert eine synchronisierte **Kopie** der Map.
Callbacks sollten kurz bleiben und keine Arbeit eines anderen Tasks abwarten,
der seinerseits Broker-Methoden aufrufen muss.

#### Konfigurierbare ESP32-Grenzen

Diese Makros können als Build-Flags überschrieben werden:

| Makro | Standardwert |
| --- | ---: |
| `MQTT_MAX_PACKET_SIZE` | 4096 Byte Gesamtpaket |
| `MQTT_MAX_TOPIC_SIZE` | 256 Byte Topic/Filter |
| `MQTT_MAX_CLIENTS` | 16 TCP-Verbindungen |
| `MQTT_MAX_SESSIONS` | 16 persistente Sessions, verbunden und offline zusammen |
| `MQTT_MAX_SUBSCRIPTIONS` | 64 je Session |
| `MQTT_MAX_QUEUED_MESSAGES` | 32 ausgehende Nachrichten je Session; separat 32 eingehende QoS-2-Zustände |
| `MQTT_MAX_INFLIGHT_MESSAGES` | 16 ausgehende QoS-1/2-Nachrichten je Session |
| `MQTT_MAX_RETAINED_MESSAGES` | 64 Topics |
| `MQTT_MAX_STORED_BYTES` | 65536 Byte Zustandsbudget |
| `MQTT_RX_RESERVE_BYTES` | 8192 Byte Reserve innerhalb dieses Budgets |

Client-Identifier sind auf 255 Byte begrenzt. Das Zustandsbudget zählt
Payloads, Strings, Empfangspufferkapazität und geschätzte Strukturgrößen;
Allocator-, TCP-/Framework-Speicher und vorübergehende Paketkopien benötigen
zusätzlichen Heap. Grenzen passend zum verfügbaren RAM konfigurieren.

Volle Queues verdrängen keine bereits angenommenen QoS-Nachrichten.
Neue Veröffentlichungen werden vor Teilzustellung oder Retained-Änderungen
abgewiesen. Bei Netzwerk-PUBLISH wird eine nicht übernommene Nachricht
nicht positiv bestätigt und die Verbindung geschlossen. Bereits per PUBREC
angenommener QoS-2-Zustand bleibt in persistenten Sessions zur Fortsetzung
erhalten. Nicht erfüllbare Subscriptions erhalten SUBACK `0x80`;
fehlende Session-Kapazität wird mit CONNACK `0x03` abgewiesen.
Die Empfangsreserve lässt Platz für ACKs und Wiederverbindungen.

### Paket- und Zustellungskorrekturen in 2.0.223

Paket 1 aus der TODO-Liste ist umgesetzt: Paketlängen und Packet-Identifier,
große SUBACK-Antworten, Live-RETAIN, Topic-Matcher einschließlich `$`, höchste
Subscription-QoS-Auswahl und gezielte Retained-Auswahl je angefragtem Filter.

Live-Nachrichten werden einmal je Client mit dem höchsten passenden
Subscription-QoS zugestellt, begrenzt durch den Publish-QoS. QoS-bedingte
Wiederholungen bleiben erhalten. Mehrfilter-SUBSCRIBE wird wie einzelne
SUBSCRIBEs behandelt; passende Retained-Nachrichten können deshalb je Filter
erneut kommen. Alte, im aktuellen Paket nicht angefragte Filter lösen keine
erneute Retained-Zustellung aus.

**Verhaltensänderung:** Auch der ursprüngliche MQTT-Publisher erhält die
Nachricht, wenn er ein passendes Abonnement hat. Der automatische Ausschluss
entfällt gemäß Benutzerentscheidung zugunsten MQTT 3.1.1. Die ausdrücklich
aufrufbare Broker-API-Option `excludeClientId` bleibt erhalten. Anwendungen
müssen eigene empfangene Nachrichten entsprechend behandeln.

Der Matcher benötigt keine zusätzlichen Allokationen. SUBACK kommt ohne
temporären Returncode-/Filter-Vektor aus. QoS 0/1/2 bleiben erhalten;
begrenzter ESP32-Speicher rechtfertigt keine still gekürzten MQTT-Nutzdaten.
Eingehende PUBLISH-Gesamtpakete oberhalb des konfigurierten Limits werden
vor Bestätigung abgewiesen.

### Stabilitäts- und Validierungskorrekturen in 2.0.222

- Sicherer Abbruch nach `close()`, auch bei synchronem Disconnect-Callback;
  aktive Clientzuordnung wird nach Paketverarbeitung und relevanten Anwendungs-Callbacks erneut geprüft.
- Zentrale Fixed-Header- und Verbindungszustandsprüfung: genau ein CONNECT je
  TCP-Verbindung, keine normalen Requests vor dessen erfolgreicher Annahme.
- CONNECT prüft `MQTT`, Protocol Level 4, Flagkombinationen, Feldgrenzen und
  UTF-8-Textfelder vor Authentifizierung und Übernahme von Session/Will.
- SUBSCRIBE wird vollständig vor Änderungen validiert. Nur Optionsbytes
  `0x00`, `0x01`, `0x02` sind zulässig; reservierte Bits führen zur Trennung.
- `Subscription::noLocal` bleibt als internes reserviertes Feld bestehen und
  wird auf `false` gesetzt. `noLocal` ist keine MQTT-3.1.1-Funktion.

Normative Grundlage: [OASIS MQTT 3.1.1 einschließlich Approved Errata 01](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/mqtt-v3.1.1-errata01-os-complete.html).

### Unterstützt

- MQTT-Broker direkt auf dem ESP32 mit `AsyncTCP`
- Publish/Subscribe mit Topic-Filtern
- Retained Messages
- Verarbeitung der vorhandenen QoS-0/1/2-Publish-Abläufe
- Subscription-QoS wird gespeichert; der effektive Zustell-QoS ist das Minimum aus Publish-QoS und Subscription-QoS
- Dieselbe QoS-Begrenzung gilt auch für Retained Messages
- Callback `onMessage(clientId, topic, payload)`
- Bestehende JSON-/Text-Payload-Verarbeitung und Weiterleitung leerer Payloads
- Optionales Ausschließen eines Clients beim Broker-Publish über `excludeClientId`

`ignoreLoopDeliver` bleibt ohne Laufzeitwirkung. MQTT 5 und `noLocal`
sind keine Funktionen dieses MQTT-3.1.1-Brokers.

## Installation

### Arduino IDE
1. Repository als ZIP herunterladen
2. In der Arduino IDE über "Sketch" → "Bibliothek einbinden" → "ZIP-Bibliothek hinzufügen"

### PlatformIO
```ini
lib_deps =
    ESP32Async/AsyncTCP
    https://github.com/parip69/ESPAsyncMQTTBroker.git
```

## Beispiel

```cpp
#include <WiFi.h>
#include <ESPAsyncMQTTBroker.h>

ESPAsyncMQTTBroker mqtt;

void setup() {
  Serial.begin(115200);
  WiFi.begin("SSID", "PASSWORT");

  mqtt.onMessage([](const String& clientId,
                    const String& topic,
                    const String& payload) {
    Serial.printf("Client: %s, Topic: %s, Payload: %s\n",
                  clientId.c_str(), topic.c_str(), payload.c_str());

    if (topic == "/ring") {
      digitalWrite(LED_BUILTIN, payload == "an" ? LOW : HIGH);
    }
  });

  mqtt.begin();
}

void loop() {
  mqtt.loop();
}
```

Der Nachrichten-Callback erhält immer drei Werte:

```cpp
clientId, topic, payload
```

Die `clientId` bezeichnet den MQTT-Client, von dem die Nachricht beim Broker eingegangen ist.
Die bestehende JSON-/Text-Verarbeitung bleibt erhalten. Allgemeine Binärpayloads
mit eingebetteten Nullbytes werden noch nicht durchgängig unverändert weitergeleitet.

## Autor

**Kala69**

## Lizenz

MIT License

Das Standard-Paketlimit ist 4096 Byte inklusive Topic/Header, per
`-D MQTT_MAX_PACKET_SIZE=8192` bei Bedarf konfigurierbar. Eine separate
768-Byte-Payloadgrenze besteht nicht mehr. Speicher wird nach Bedarf belegt.
