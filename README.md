![Logo](logo.svg)

# ESPAsyncMQTTBroker

Ein asynchroner MQTT-Broker für den ESP32 auf Basis von `AsyncTCP`.

## Features

- MQTT-Broker läuft direkt auf dem ESP32
- Volle Kontrolle über Topics, Clients und Nachrichten
- Callback-Schnittstellen für Clients, Nachrichten, Fehler und Subscriptions
- Keine Internetverbindung erforderlich – funktioniert komplett lokal
- Kompatibel mit PlatformIO und dem Arduino-Framework

## Aktueller Implementierungsstand

Stand: **2.0.222**. MQTT **3.1.1** ist der aktive unterstützte Protokollstand.
MQTT **5 wird nicht unterstützt**. Die Bibliothek beansprucht weiterhin keine
vollständige MQTT-3.1.1-Konformität.

**Stabiler geprüfter Stand: Tag `v2.0.222`.** 50 lokale C++-Tests sowie
55 Tests auf dem echten Broker sind bestanden. Die dokumentierten
Fingerprint-/MQTT-/Ausgangspfade mit Parip99 `.99`, gumi77 `.77` und
Shelly `.127` wurden geprüft. Eine genaue physische 500-ms-Pulsdauer wurde
nicht separat bestätigt. Details stehen in `agent_worklog.md`.
Weitere MQTT-3.1.1-Restpunkte werden ausschließlich in einer folgenden
Version bearbeitet; 2.0.222 bleibt eingefroren.

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

- Topic-Matcher-Randfälle und `$`-Wildcard-Sonderregel
- Auswahl des höchsten QoS bei überlappenden Subscriptions
- RETAIN-Flag bei Live-Zustellung und Retained-Auswahl über mehrere alte Filter
- Mehrbyte-Remaining-Length bei großen SUBACK-Antworten
- Vollständige Binärpayload-Weiterleitung sowie globale UTF-8-Prüfung aller Paketarten
- Vollständige persistente Sessions, Offline-QoS-Queue und QoS-2-Wiederaufnahme
- Weitere Paketlängen-/Identifierprüfungen außerhalb CONNECT/SUBSCRIBE,
  Ressourcenlimits und allgemeine Task-Synchronisierung
- Bestehender Ausschluss des ursprünglichen Publishers bleibt erhalten.

`ignoreLoopDeliver` bleibt ohne Laufzeitwirkung. `excludeClientId`, Topics,
JSON-Verarbeitung und Zustell-QoS wurden funktional nicht verändert.

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

## Beispiele

- [`examples/BasicBroker`](examples/BasicBroker) - Grundlegende Broker-Funktionalität
- [`examples/WithWebServer`](examples/WithWebServer) - MQTT-Broker mit Webserver
- [`examples/ControlLED`](examples/ControlLED) - Steuerung einer LED über MQTT
- [`examples/SimpleMQTTBroker`](examples/SimpleMQTTBroker) - Einfacher MQTT-Broker ohne Extras
- [`examples/MQTTClient`](examples/MQTTClient) - ESP32 als MQTT-Client
- [`examples/DualModeBrokerClient`](examples/DualModeBrokerClient) - ESP32 als Broker und Client (umschaltbar)

## GitHub Actions

Dieses Repository nutzt GitHub Actions, um automatisch die `examples/BasicBroker`-Version bei jedem Push zu bauen.

## Autor

**Kala69**

## Lizenz

MIT License
