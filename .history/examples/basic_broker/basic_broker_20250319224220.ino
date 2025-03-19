#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncMQTTBroker.h>

#define WIFI_SSID "YOUR_SSID"
#define WIFI_PASSWORD "YOUR_PASSWORD" 
#define MQTT_PORT 1883

ESPAsyncMQTTBroker mqttBroker(MQTT_PORT);

void setup() {
    Serial.begin(115200);
    
    // WLAN Verbindung
    Serial.println("Verbinde mit WLAN...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

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
    Serial.print("Port: ");
    Serial.println(MQTT_PORT);
    Serial.println("Warte auf Clients...");
}

void loop() {
    // Der Broker arbeitet asynchron im Hintergrund
    delay(1000);
}