#include <WiFi.h>
#include "ESPAsyncMQTTBroker.h"

const char* ssid = "DeinSSID";
const char* password = "DeinPasswort";

ESPAsyncMQTTBroker mqttBroker(1883);

void setup() {
    Serial.begin(115200);
    
    // WLAN Verbindung
    Serial.println("Verbinde mit WLAN...");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) { 
        delay(500); 
        Serial.print(".");
    }
    Serial.println("\nWLAN verbunden!");
    Serial.print("IP-Adresse: ");
    Serial.println(WiFi.localIP());

    // MQTT Broker starten
    mqttBroker.begin();
    Serial.println("MQTT Broker gestartet");
}

void loop() {
    // Alles läuft asynchron – kein delay nötig!
}
