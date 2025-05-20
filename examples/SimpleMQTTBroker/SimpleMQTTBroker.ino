#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ESPAsyncMQTTBroker.h>

// WiFi-Konfiguration
const char* ssid = "MeinWLAN";     // Ändern Sie dies auf Ihre WLAN-SSID
const char* password = "MeinPasswort"; // Ändern Sie dies auf Ihr WLAN-Passwort

// MQTT-Broker Instanz erstellen (Standard-Port 1883)
ESPAsyncMQTTBroker mqttBroker;

// Status-LED
const int ledPin = 2; // Integrierte LED an den meisten ESP32-Boards
bool ledState = false;

// Diese Funktion wird aufgerufen, wenn eine MQTT-Nachricht empfangen wird
void onMQTTMessage(String clientId, String topic, String message) {
  Serial.printf("Nachricht empfangen von %s auf Topic '%s': %s\n", 
                clientId.c_str(), topic.c_str(), message.c_str());
  
  // Beispiel: Bei Nachricht auf Topic "led/control" die LED steuern
  if (topic == "led/control") {
    if (message == "on" || message == "true") {
      digitalWrite(ledPin, HIGH);
      ledState = true;
      // Statusbestätigung senden
      mqttBroker.publish("led/status", 0, true, "on");
    } 
    else if (message == "off" || message == "false") {
      digitalWrite(ledPin, LOW);
      ledState = false;
      // Statusbestätigung senden
      mqttBroker.publish("led/status", 0, true, "off");
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  
  // Mit WLAN verbinden
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("");
  Serial.println("Mit WLAN verbunden");
  Serial.print("IP-Adresse: ");
  Serial.println(WiFi.localIP());
  
  // MQTT Broker konfigurieren
  
  // Debug-Level festlegen (Optional)
  mqttBroker.setDebugLevel(DEBUG_INFO); // Optionen: DEBUG_NONE, DEBUG_ERROR, DEBUG_INFO, DEBUG_DEBUG
  
  // Client-Verbindungen überwachen
  mqttBroker.onClientConnect([](String clientId, String clientIp) {
    Serial.printf("Client verbunden: %s (%s)\n", clientId.c_str(), clientIp.c_str());
    
    // Optionales Willkommenssignal senden
    String welcomeMsg = "Willkommen " + clientId + "!";
    mqttBroker.publish("broker/clients", 0, false, welcomeMsg.c_str());
  });
  
  // Client-Trennungen überwachen
  mqttBroker.onClientDisconnect([](String clientId) {
    Serial.printf("Client getrennt: %s\n", clientId.c_str());
  });
  
  // Nachrichtenempfang-Handler festlegen
  mqttBroker.onMessage(onMQTTMessage);
  
  // Abonnement-Ereignisse überwachen (Optional)
  mqttBroker.onSubscribe([](String clientId, const String &topic) {
    Serial.printf("Client %s hat Topic abonniert: %s\n", clientId.c_str(), topic.c_str());
  });
  
  // MQTT-Broker starten
  mqttBroker.begin();
  Serial.println("MQTT-Broker gestartet auf Port 1883");
  
  // Anfangsstatus der LED veröffentlichen
  mqttBroker.publish("led/status", 0, true, "off");
}

void loop() {
  // Beispiel: Alle 10 Sekunden Systemdaten veröffentlichen
  static unsigned long lastPublish = 0;
  if (millis() - lastPublish > 10000) {
    lastPublish = millis();
    
    // Uptime in Minuten
    int uptime = lastPublish / 60000;
    String uptimeStr = String(uptime);
    
    // Freier Heap-Speicher
    String heapStr = String(ESP.getFreeHeap());
    
    // Veröffentlichen der Systemdaten
    mqttBroker.publish("system/uptime", 0, true, uptimeStr.c_str());
    mqttBroker.publish("system/heap", 0, true, heapStr.c_str());
    
    Serial.printf("Systemdaten veröffentlicht - Uptime: %d min, Heap: %s\n", 
                  uptime, heapStr.c_str());
  }
}
