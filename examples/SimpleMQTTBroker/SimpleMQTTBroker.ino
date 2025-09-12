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
void onMQTTMessage(const char* clientId, const char* topic, const uint8_t* payload, size_t len) {
  char payloadStr[len + 1];
  memcpy(payloadStr, payload, len);
  payloadStr[len] = '\0';

  Serial.printf("Nachricht empfangen von %s auf Topic '%s': %s\n", 
                clientId, topic, payloadStr);
  
  // Beispiel: Bei Nachricht auf Topic "led/control" die LED steuern
  if (strcmp(topic, "led/control") == 0) {
    bool turnOn = (strcmp(payloadStr, "on") == 0 || strcmp(payloadStr, "true") == 0);
    digitalWrite(ledPin, turnOn ? HIGH : LOW);
    ledState = turnOn;

    // Statusbestätigung senden
    const char* status = turnOn ? "on" : "off";
    mqttBroker.publish("led/status", (const uint8_t*)status, strlen(status), true, 0);
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
  mqttBroker.onClientConnect([](const char* clientId, const char* clientIp) {
    Serial.printf("Client verbunden: %s (%s)\n", clientId, clientIp);
    
    // Optionales Willkommenssignal senden
    char welcomeMsg[128];
    snprintf(welcomeMsg, sizeof(welcomeMsg), "Willkommen %s!", clientId);
    mqttBroker.publish("broker/clients", (const uint8_t*)welcomeMsg, strlen(welcomeMsg), false, 0);
  });
  
  // Client-Trennungen überwachen
  mqttBroker.onClientDisconnect([](const char* clientId) {
    Serial.printf("Client getrennt: %s\n", clientId);
  });
  
  // Nachrichtenempfang-Handler festlegen
  mqttBroker.onMessage(onMQTTMessage);
  
  // Abonnement-Ereignisse überwachen (Optional)
  mqttBroker.onSubscribe([](const char* clientId, const char* topic) {
    Serial.printf("Client %s hat Topic abonniert: %s\n", clientId, topic);
  });
  
  // MQTT-Broker starten
  mqttBroker.begin();
  Serial.println("MQTT-Broker gestartet auf Port 1883");
  
  // Anfangsstatus der LED veröffentlichen
  const char* initialStatus = "off";
  mqttBroker.publish("led/status", (const uint8_t*)initialStatus, strlen(initialStatus), true, 0);
}

void loop() {
  // Beispiel: Alle 10 Sekunden Systemdaten veröffentlichen
  static unsigned long lastPublish = 0;
  if (millis() - lastPublish > 10000) {
    lastPublish = millis();
    
    char buffer[32];

    // Uptime in Minuten
    int uptime = lastPublish / 60000;
    snprintf(buffer, sizeof(buffer), "%d", uptime);
    mqttBroker.publish("system/uptime", (const uint8_t*)buffer, strlen(buffer), true, 0);
    
    // Freier Heap-Speicher
    snprintf(buffer, sizeof(buffer), "%u", ESP.getFreeHeap());
    mqttBroker.publish("system/heap", (const uint8_t*)buffer, strlen(buffer), true, 0);
    
    Serial.printf("Systemdaten veröffentlicht - Uptime: %d min, Heap: %u\n",
                  uptime, ESP.getFreeHeap());
  }
}
