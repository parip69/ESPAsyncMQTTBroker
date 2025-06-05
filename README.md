![Logo](logo.svg)

# ESPAsyncMQTTBroker

Ein asynchroner MQTT-Broker für den ESP32 auf Basis von `ESPAsyncWebServer`.

## Features

- MQTT-Broker läuft direkt auf dem ESP32
- Volle Kontrolle über Topics, Clients und Nachrichten
- Optionales Webinterface zur Anzeige verbundener Clients und empfangener Nachrichten
- Keine Internetverbindung erforderlich – funktioniert komplett lokal
- Kompatibel mit PlatformIO und dem Arduino-Framework

## Flexible TCP-Schicht

Die Bibliothek verwendet jetzt eine abstrahierte TCP-Kommunikationsschicht. Dies ermöglicht es, verschiedene zugrundeliegende asynchrone TCP-Bibliotheken zu unterstützen.

Standardmäßig wird `AsyncTCP` (von me-no-dev) verwendet. Stellen Sie sicher, dass diese Bibliothek installiert ist, da sie für die grundlegende Funktionalität benötigt wird.

Fortgeschrittene Benutzer können eigene TCP-Implementierungen bereitstellen, indem sie Klassen erstellen, die die Schnittstellen `ITcpServer` (definiert in `src/ITcpServer.h`) und `ITcpClient` (definiert in `src/ITcpClient.h`) implementieren. Um eine benutzerdefinierte TCP-Implementierung zu verwenden, müsste typischerweise ein eigener Server-Adapter (der `ITcpServer` implementiert) erstellt werden, der wiederum Client-Adapter (die `ITcpClient` implementieren) bereitstellt. Eine Instanz dieses benutzerdefinierten Server-Adapters müsste dann dem `ESPAsyncMQTTBroker` übergeben werden. (Hinweis: Dies könnte zukünftige Anpassungen am Konstruktor des Brokers oder die Verwendung eines Factory-Musters erfordern, um benutzerdefinierte Adapter direkt zu injizieren. Derzeit ist die Standardimplementierung `AsyncTcpServerAdapter` fest verdrahtet.)

Die Datei `build_flags.ini` im Hauptverzeichnis kann für erweiterte Build-Konfigurationen verwendet werden, beispielsweise um Präprozessor-Flags zu definieren, mit denen zwischen verschiedenen TCP-Implementierungen ausgewählt werden kann, falls die Bibliothek zukünftig mehrere anbieten sollte.

## Installation

### Arduino IDE
1. Repository als ZIP herunterladen.
2. Stellen Sie sicher, dass die Abhängigkeit `AsyncTCP` von me-no-dev (https://github.com/me-no-dev/AsyncTCP) ebenfalls installiert ist. Sie können diese Bibliothek über den Arduino Library Manager suchen und installieren.
3. In der Arduino IDE über "Sketch" → "Bibliothek einbinden" → "ZIP-Bibliothek hinzufügen" (für ESPAsyncMQTTBroker).


### PlatformIO
Die Abhängigkeit `AsyncTCP` wird automatisch mitinstalliert.
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
  // Die loop-Funktion des Brokers wird intern durch Timer gehandhabt.
  // Ein expliziter Aufruf von mqtt.loop() ist nicht mehr nötig.
  // Ihr eigener loop-Code kann hier stehen.
}
```
**Hinweis:** Die `mqtt.loop()` Methode ist nicht mehr erforderlich, da interne Operationen nun über Timer gesteuert werden.

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

**Kala69** (Ursprünglicher Autor)
Beitragende: (Ihr Name/GitHub-Handle hier, falls gewünscht)

## Lizenz

MIT License