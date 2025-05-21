![Logo](logo.svg)

# ESPAsyncMQTTBroker

Ein asynchroner MQTT-Broker für den ESP32 auf Basis von `ESPAsyncWebServer`.

## Features

- MQTT-Broker läuft direkt auf dem ESP32
- Volle Kontrolle über Topics, Clients und Nachrichten
- Optionales Webinterface zur Anzeige verbundener Clients und empfangener Nachrichten
- Keine Internetverbindung erforderlich – funktioniert komplett lokal
- Kompatibel mit PlatformIO und dem Arduino-Framework

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
#include <ESPAsyncMQTTBroker.h>

ESPAsyncMQTTBroker mqtt;

void setup() {
  Serial.begin(115200);
  WiFi.begin("SSID", "PASSWORT");

  mqtt.onMessage([](const String& topic, const String& payload) {
    Serial.printf("Topic: %s, Payload: %s\n", topic.c_str(), payload.c_str());
    if (topic == "/ring") {
      digitalWrite(LED_BUILTIN, payload == "an" ? LOW : HIGH);
    }
  });

  mqtt.begin();
}

void loop() {
}
```

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