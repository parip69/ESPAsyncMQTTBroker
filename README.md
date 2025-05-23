![Logo](logo.svg)

# ESPAsyncMQTTBroker

ESPAsyncMQTTBroker ist ein **asynchroner MQTT Broker (Server)**, der speziell für den **ESP32** entwickelt wurde. Er basiert auf der `AsyncTCP` Bibliothek und ermöglicht es dem ESP32, als vollwertiger MQTT-Nachrichtenvermittler im lokalen Netzwerk zu agieren, ohne auf externe Broker-Dienste oder eine Internetverbindung angewiesen zu sein.

## Hauptfunktionen

*   **MQTT-Protokollunterstützung:** Implementiert MQTT Version 3.1.1 und bietet grundlegende Unterstützung für MQTT 5.0 Features.
*   **Quality of Service (QoS):** Unterstützt QoS 0 (höchstens einmal), QoS 1 (mindestens einmal) und QoS 2 (genau einmal) für die zuverlässige Nachrichtenübermittlung zwischen Clients und Broker.
*   **Retained Messages:** Speichert Nachrichten mit dem Retain-Flag, sodass neue Abonnenten sofort den letzten bekannten Status eines Topics erhalten.
*   **Persistente Sessions:** Behält Session-Daten (z.B. Abonnements) für Clients, die sich mit `Clean Session = false` verbinden, auch über Verbindungsabbrüche hinweg.
*   **Asynchron & Non-Blocking:** Durch die Nutzung von `AsyncTCP` werden alle Netzwerkoperationen asynchron und nicht-blockierend ausgeführt, was eine hohe Performance und Reaktionsfähigkeit auf dem ESP32 ermöglicht.
*   **Konfigurierbare Authentifizierung:** Ermöglicht die Absicherung des Brokers durch optionalen Benutzernamen und Passwort.
*   **Callback-basierte Ereignisbehandlung:** Bietet eine flexible, ereignisgesteuerte API über Callbacks für verschiedene Broker-Events (z.B. Client verbindet/trennt, Nachricht empfangen).
*   **Topic-Filter mit Wildcards:** Unterstützt die MQTT-Standard-Wildcards `+` (Single-Level) und `#` (Multi-Level) in Topic-Filtern für flexible Abonnements.

## Wichtige Konzepte/Schlüsselwörter

*   **Broker:** Der ESPAsyncMQTTBroker agiert als Server, der MQTT-Nachrichten von publizierenden Clients empfängt und sie an Clients weiterleitet, die die entsprechenden Topics abonniert haben.
*   **Client:** Jedes Gerät oder jede Anwendung (z.B. ein weiterer ESP32, ein Smartphone, eine PC-Anwendung), das/die sich mit dem ESPAsyncMQTTBroker verbindet, um Nachrichten zu senden (publish) oder zu empfangen (subscribe).
*   **Topic:** Eine hierarchische Zeichenkette (z.B. `haus/wohnzimmer/licht` oder `sensor/temperatur/aussen`), die als Kanal für Nachrichten dient. Clients publizieren Nachrichten zu spezifischen Topics, und andere Clients abonnieren Topics, um diese Nachrichten zu empfangen.
*   **Publish/Subscribe (Pub/Sub):** Das grundlegende Kommunikationsmodell von MQTT. Clients (Publisher) senden Nachrichten zu einem Topic, ohne zu wissen, welche Clients (Subscriber) diese Nachrichten empfangen. Subscriber drücken ihr Interesse an bestimmten Topics aus und erhalten Nachrichten, die zu diesen Topics veröffentlicht werden. Dies entkoppelt die Kommunikationspartner.
*   **QoS (Quality of Service):** Definiert die Zuverlässigkeit der Nachrichtenübermittlung:
    *   `QoS 0 (At most once)`: Die Nachricht wird höchstens einmal gesendet. Es gibt keine Bestätigung vom Empfänger. Dies ist die schnellste Methode, aber Nachrichten können verloren gehen, wenn die Verbindung unterbrochen wird.
    *   `QoS 1 (At least once)`: Die Nachricht wird mindestens einmal gesendet. Der Sender erwartet eine Bestätigung und sendet die Nachricht erneut, falls keine Bestätigung eintrifft. Nachrichten können unter bestimmten Umständen doppelt beim Empfänger ankommen.
    *   `QoS 2 (Exactly once)`: Die Nachricht wird genau einmal gesendet. Dies ist die sicherste, aber auch langsamste Methode, da sie einen vierstufigen Handshake-Mechanismus verwendet, um sicherzustellen, dass die Nachricht genau einmal zugestellt wird und keine Duplikate entstehen. *ESPAsyncMQTTBroker unterstützt QoS 2 für die Kommunikation zwischen Client und Broker.*
*   **Retained Messages:** Wenn ein Client eine Nachricht mit dem "Retain"-Flag auf einem Topic veröffentlicht, speichert der Broker diese Nachricht (die letzte pro Topic). Sobald sich ein neuer Client auf dieses Topic (oder einen passenden Filter) registriert, sendet der Broker ihm sofort die zuletzt gespeicherte Retained Message. Dies ist nützlich, um den aktuellen Status eines Geräts oder Sensors vorzuhalten, sodass neue Clients nicht warten müssen, bis der Sensor das nächste Mal sendet.
*   **Keep-Alive:** Ein Mechanismus, um die Verbindung zwischen Client und Broker aktiv zu halten und Verbindungsabbrüche zu erkennen.
    *   **Client-Verantwortung:** Der Client sendet regelmäßig PINGREQ-Pakete an den Broker, wenn innerhalb des vom Client festgelegten Keep-Alive-Intervalls keine anderen Daten gesendet werden. Dies signalisiert dem Broker, dass der Client noch aktiv ist.
    *   **Broker-Verantwortung:** Der Broker antwortet auf PINGREQ mit PINGRESP. Wenn der Broker innerhalb einer bestimmten Zeit (typischerweise 1.5-mal das Keep-Alive-Intervall des Clients) keine Nachricht oder kein PINGREQ vom Client empfängt, geht er davon aus, dass die Verbindung unterbrochen ist, und schließt sie. ESPAsyncMQTTBroker implementiert dieses serverseitige Keep-Alive-Verhalten korrekt.
*   **Callbacks:** Funktionen in Ihrem ESP32-Sketch, die von der ESPAsyncMQTTBroker-Bibliothek aufgerufen werden, wenn bestimmte Ereignisse eintreten. Beispiele hierfür sind `onClientConnect` (wenn sich ein Client verbindet), `onMessage` (wenn eine Nachricht auf einem abonnierten Topic empfangen wird) und `onClientDisconnect` (wenn ein Client die Verbindung trennt). Dies ermöglicht eine reaktive, ereignisgesteuerte Programmierung.

## Grundlegende Benutzung

Hier ist ein einfaches Beispiel, wie Sie den ESPAsyncMQTTBroker auf einem ESP32 einrichten:

```cpp
#include <ESPAsyncMQTTBroker.h>
#include <WiFi.h> // Für ESP32 (oder ESP8266WiFi.h für ESP8266)

ESPAsyncMQTTBroker broker;

const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Callback-Funktion für eingehende Nachrichten
void onMessageCallback(String clientId, String topic, String message) {
  Serial.printf("Nachricht von Client %s auf Topic %s: %s\n", clientId.c_str(), topic.c_str(), message.c_str());

  // Beispiel: Einfaches Echo an alle Clients auf dem gleichen Topic (außer dem Sender selbst)
  // Die publish-Methode des Brokers hat einen optionalen Parameter, um den sendenden Client auszuschließen.
  // broker.publish(topic.c_str(), message.c_str(), false, 0, clientId); 
  
  // Besser: Gezielte Weiterleitung oder Verarbeitung.
  if (topic == "cmnd/licht/schalten") {
    if (message == "AN") {
      Serial.println("Befehl: Licht AN");
      // Hier Code zum Einschalten des Lichts
      broker.publish("stat/licht/status", "AN", true, 0); // Status mit Retain veröffentlichen
    } else if (message == "AUS") {
      Serial.println("Befehl: Licht AUS");
      // Hier Code zum Ausschalten des Lichts
      broker.publish("stat/licht/status", "AUS", true, 0); // Status mit Retain veröffentlichen
    }
  }
}

void onClientConnectCallback(String clientId, String clientIp) {
  Serial.printf("Client %s (IP: %s) verbunden.\n", clientId.c_str(), clientIp.c_str());
}

void onClientDisconnectCallback(String clientId) {
  Serial.printf("Client %s getrennt.\n", clientId.c_str());
}

void setup() {
  Serial.begin(115200);
  Serial.println("ESPAsyncMQTTBroker Beispiel");

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); // Kurze Verzögerung im WiFi-Verbindungs-Loop ist üblich
    Serial.print(".");
  }
  Serial.println("\nVerbunden mit WLAN!");
  Serial.print("IP-Adresse: ");
  Serial.println(WiFi.localIP());

  // Callbacks registrieren
  broker.onMessage(onMessageCallback);
  broker.onClientConnect(onClientConnectCallback);
  broker.onClientDisconnect(onClientDisconnectCallback);

  // Optionale Konfiguration: Authentifizierung
  // ESPAsyncMQTTBrokerConfig config;
  // config.username = "espuser";
  // config.password = "esppass";
  // broker.setConfig(config);
  
  // Optional: Debug-Level setzen (DEBUG_INFO, DEBUG_DEBUG, etc.)
  // broker.setDebugLevel(DEBUG_DEBUG);

  broker.begin();
  Serial.println("MQTT Broker gestartet.");
}

void loop() {
  // Die asynchrone Verarbeitung von ESPAsyncMQTTBroker erfordert keine
  // regelmäßigen Aufrufe von `broker.loop()` oder ähnlichem im Hauptloop.
  // Ihr eigener Code kann hier wie gewohnt ausgeführt werden.
}
```

## Konfiguration

Die Konfiguration des Brokers, wie z.B. die Einrichtung von Benutzername und Passwort für die Client-Authentifizierung, erfolgt über die `ESPAsyncMQTTBrokerConfig`-Struktur und die `setConfig()`-Methode vor dem Aufruf von `broker.begin()`.

```cpp
ESPAsyncMQTTBrokerConfig config;
config.username = "myuser";
config.password = "mypassword";
// config.ignoreLoopDeliver = true; // Verhindert, dass Nachrichten an den sendenden Client zurückgesendet werden (Standard: false)
broker.setConfig(config);
```

## Logging

ESPAsyncMQTTBroker bietet eine integrierte Logging-Funktionalität zur Diagnose und Fehlersuche.

*   **Debug-Level:** Sie können die Ausführlichkeit der Log-Ausgaben über die Methode `setDebugLevel(DebugLevel level)` steuern. Verfügbare Level sind:
    *   `DEBUG_NONE`
    *   `DEBUG_ERROR`
    *   `DEBUG_WARNING`
    *   `DEBUG_INFO`
    *   `DEBUG_DEBUG` (ausführlichste Ausgabe)
*   **Benutzerdefinierter Log-Handler:** Mit `setLoggingCallback(LoggingCallback callback)` können Sie eine eigene Funktion registrieren, die für jede Log-Nachricht des Brokers aufgerufen wird. Dies ermöglicht z.B. das Schreiben von Logs auf eine SD-Karte, das Senden über das Netzwerk oder die Integration in ein bestehendes Logging-System.

```cpp
// Beispiel: Debug-Level setzen
broker.setDebugLevel(DEBUG_INFO);

// Beispiel: Benutzerdefinierter Logging-Callback
void myCustomLogger(DebugLevel level, const String &message) {
  Serial.print("[CustomLog] ");
  if (level == DEBUG_ERROR) Serial.print("ERROR: ");
  Serial.println(message);
}
broker.setLoggingCallback(myCustomLogger);
```

## Bibliotheksabhängigkeiten

*   **AsyncTCP:** Diese Bibliothek ist die Grundlage für die asynchrone TCP-Kommunikation und eine zwingende Abhängigkeit für ESP32. Sie muss in Ihrem Projekt verfügbar sein.

Stellen Sie sicher, dass diese Abhängigkeit korrekt in Ihrer Entwicklungsumgebung (Arduino IDE, PlatformIO) installiert ist. Für PlatformIO wird dies typischerweise über die `platformio.ini`-Datei verwaltet:
```ini
lib_deps =
    me-no-dev/AsyncTCP @ ^1.1.1
    # ... Ihre anderen Bibliotheken
    # plus ESPAsyncMQTTBroker selbst, z.B. über Git oder als lokale Bibliothek
```

## Lizenz
Dieses Projekt ist unter der MIT-Lizenz veröffentlicht. Siehe die `LICENSE`-Datei für Details.

---
*Informationen aus der vorherigen README.md, die nicht direkt in die neue Struktur passen, aber erhalten bleiben könnten (z.B. spezifische Installationsanweisungen, GitHub Actions, Autor der ursprünglichen README), wurden hier vorerst weggelassen, um die Komplexität des Ersetzens zu reduzieren. Diese könnten bei Bedarf manuell wieder integriert werden.*