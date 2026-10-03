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

### Unterstützt

- MQTT-Broker direkt auf dem ESP32 mit `AsyncTCP`
- Publish/Subscribe mit Topic-Filtern
- Retained Messages
- Verarbeitung der vorhandenen QoS-0/1/2-Publish-Abläufe
- Subscription-QoS wird gespeichert; der effektive Zustell-QoS ist das Minimum aus Publish-QoS und Subscription-QoS
- Dieselbe QoS-Begrenzung gilt auch für Retained Messages
- Callback `onMessage(clientId, topic, payload)`
- Payload wird unverändert weitergegeben; auch leere Payloads werden weitergeleitet
- Optionales Ausschließen eines Clients beim Broker-Publish über `excludeClientId`

### Teilweise unterstützt

- `noLocal`: Das Flag wird beim SUBSCRIBE eingelesen und in der Subscription gespeichert, bei der späteren Nachrichtenverteilung aber noch nicht ausgewertet.


### Noch nicht umgesetzt

- Wirksame `noLocal`-Filterung bei der Nachrichtenverteilung

- `ignoreLoopDeliver`: Das Konfigurationsfeld ist vorhanden, wird aktuell aber nicht ausgewertet und verändert das Laufzeitverhalten nicht.

> Stand: Version 2.0.221. Subscription-QoS wird bei normaler und Retained-Zustellung berücksichtigt. `noLocal` und `ignoreLoopDeliver` bleiben unverändert.

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

Die `clientId` bezeichnet den MQTT-Client, von dem die Nachricht beim Broker eingegangen ist. Die Payload selbst wird vom Broker unverändert weitergeleitet.

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
