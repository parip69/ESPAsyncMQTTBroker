![Logo](logo.svg)

# ESPAsyncMQTTBroker

Ein asynchroner MQTT-Broker für den ESP32 auf Basis von `AsyncTCP`. Diese Bibliothek implementiert einen vollständigen MQTT Broker der direkt auf dem ESP32 läuft.

## Features

- **Vollständiger MQTT-Broker** direkt auf dem ESP32
- **Asynchrone Verarbeitung** - nicht-blockierende I/O für beste Performance
- **MQTT 3.1.1 und 5.0** Unterstützung (letztere mit grundlegenden Features)
- **QoS 0, 1 und 2** für zuverlässige Nachrichtenübertragung
- **Retained Messages** werden unterstützt
- **Persistente Sessions** für unterbrechungsfreie Kommunikation
- **Authentifizierung** von Clients über Benutzername/Passwort
- **Topic-Filter mit Wildcards** (# und +) für flexible Subscriptions
- **Memory-Optimierung** durch Smart Pointers und Puffergrößenlimits
- **Vollständige Callback-API** für alle MQTT-Ereignisse
- **Keine Internetverbindung erforderlich** – funktioniert komplett lokal
- Kompatibel mit PlatformIO und dem Arduino-Framework

## Installation

**PlatformIO**
```ini
lib_deps = 
    https://github.com/parip69/ESPAsyncMQTTBroker.git
```

**Arduino IDE**
1. Repository als ZIP herunterladen
2. In der Arduino IDE über "Sketch" → "Bibliothek einbinden" → "ZIP-Bibliothek hinzufügen"

## Einfaches Beispiel

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncMQTTBroker.h>

ESPAsyncMQTTBroker mqtt;

void setup() {
  Serial.begin(115200);
  WiFi.begin("SSID", "PASSWORT");
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWiFi verbunden");
  Serial.println("IP-Adresse: " + WiFi.localIP().toString());
  
  // MQTT Event-Callbacks einrichten
  mqtt.onClientConnect([](String clientId, String clientIp) {
    Serial.println("Client verbunden: " + clientId + " (IP: " + clientIp + ")");
  });
  
  mqtt.onMessage([](String clientId, String topic, String payload) {
    Serial.println("Nachricht: " + topic + " = " + payload + " (von " + clientId + ")");
  });
  
  mqtt.onClientDisconnect([](String clientId) {
    Serial.println("Client getrennt: " + clientId);
  });
  
  // Optional: Authentifizierung konfigurieren
  ESPAsyncMQTTBrokerConfig config;
  config.username = "user";
  config.password = "pass";
  mqtt.setConfig(config);
  
  // Broker starten
  mqtt.begin();
  
  // Eigene Nachricht veröffentlichen
  mqtt.publish("/status", "Der ESP32 MQTT-Broker läuft!");
}

void loop() {
  // Nichts zu tun hier - alles wird asynchron erledigt!
}
```

## Erweiterte Konfiguration

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncMQTTBroker.h>

ESPAsyncMQTTBroker mqtt;

void setup() {
  Serial.begin(115200);
  WiFi.begin("SSID", "PASSWORT");
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWiFi verbunden");
  
  // Debug-Level einstellen
  mqtt.setDebugLevel(DEBUG_INFO);
  
  // Logging-Callback (z.B. für Weiterleitung an WebSocket)
  mqtt.setLoggingCallback([](DebugLevel level, const String &message) {
    if (level <= DEBUG_INFO) {
      // Hier könnte man Logs an WebSocket senden
    }
  });
  
  // MQTT-Ereignisse
  mqtt.onClientConnect([](String clientId, String clientIp) {
    Serial.println("Client verbunden: " + clientId + " (IP: " + clientIp + ")");
  });
  
  mqtt.onMessage([](String clientId, String topic, String payload) {
    Serial.println("Nachricht: " + topic + " = " + payload);
    
    // Eigene Logik basierend auf eingehenden Nachrichten
    if (topic == "/ledControl") {
      if (payload == "on") digitalWrite(LED_BUILTIN, HIGH);
      else if (payload == "off") digitalWrite(LED_BUILTIN, LOW);
      
      // Bestätigung senden
      mqtt.publish("/ledStatus", payload);
    }
  });
  
  mqtt.onError([](String clientId, int errorCode, const String &errorMessage) {
    Serial.println("Fehler von Client " + clientId + ": " + errorMessage);
  });
  
  // Broker starten
  mqtt.begin();
  
  // Retained Message setzen
  mqtt.publish("/deviceInfo", "{\"name\":\"ESP32\",\"version\":\"1.5.0\"}", true);
}

void loop() {
  // Beispiel: Regelmäßige Statusaktualisierung (in einer echten Anwendung
  // würden Sie hier natürlich einen Timer oder Task verwenden)
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate > 60000) { // Alle 60 Sekunden
    lastUpdate = millis();
    
    // QoS 1 Nachricht senden
    mqtt.publish("/uptime", String(lastUpdate / 1000), false, 1);
  }
}
```

## Fehlerbehebung und FAQ

### Verbindungsprobleme
- Stellen Sie sicher, dass der ESP32 vor dem Start des MQTT-Brokers mit dem WLAN verbunden ist.
- Überprüfen Sie ob Firewall-Einstellungen den MQTT-Port (Standard: 1883) blockieren.
- Bei Authentifizierungsproblemen: Überprüfen Sie Benutzernamen und Passwort in der Broker-Konfiguration und im MQTT-Client.

### Memory-Management
- Wenn Sie Probleme mit dem Arbeitsspeicher haben, reduzieren Sie `MQTT_MAX_PACKET_SIZE`, `MQTT_MAX_TOPIC_SIZE` und `MQTT_MAX_PAYLOAD_SIZE` in der Header-Datei.
- Überwachen Sie den Heap mit `ESP.getFreeHeap()` für Langzeitstabilität.

### Tipps zur Leistungsoptimierung
- Begrenzen Sie die Anzahl gleichzeitiger Verbindungen.
- Verwenden Sie kurze Topic-Namen, um den Speicherbedarf zu reduzieren.
- Aktivieren Sie nur die Debug-Level, die Sie wirklich benötigen.

## Beispiele

- [`examples/BasicBroker`](examples/BasicBroker)
- [`examples/WithWebServer`](examples/WithWebServer)
- [`examples/ControlLED`](examples/ControlLED)

## Referenz zur API

Siehe die [vollständig dokumentierte Header-Datei](src/ESPAsyncMQTTBroker.h) für alle verfügbaren Methoden und Konfigurationsoptionen.

## Autor

**Kala69**

## Lizenz

MIT License
