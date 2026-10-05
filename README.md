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

Stand: **2.0.223**. MQTT **3.1.1** ist der aktive unterstützte Protokollstand.
MQTT **5 wird nicht unterstützt**. Die Bibliothek beansprucht weiterhin keine
vollständige MQTT-3.1.1-Konformität.

Der bisherige Stand bleibt unter Tag `v2.0.222` erhalten.
Offene MQTT-3.1.1-Arbeiten stehen in [TODO.md](TODO.md).

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

### Bewusst offene Einschränkungen

- Vollständige Binärpayload-Weiterleitung sowie globale UTF-8-Prüfung aller Paketarten
- Vollständige persistente Sessions, Offline-QoS-Queue und QoS-2-Wiederaufnahme
- Ressourcenlimits, erschöpfte Packet-Identifier und allgemeine Task-Synchronisierung

`ignoreLoopDeliver` bleibt ohne Laufzeitwirkung. `excludeClientId`, Topics,
JSON-Format und öffentliche Callback-Schnittstellen bleiben erhalten.

## Installation

### Arduino IDE
1. Repository als ZIP herunterladen
2. In der Arduino IDE über "Sketch" → "Bibliothek einbinden" → "ZIP-Bibliothek hinzufügen"

### PlatformIO
```ini
lib_deps =
    me-no-dev/AsyncTCP
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
