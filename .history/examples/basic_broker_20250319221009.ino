#include <WiFi.h>
#include "ESPAsyncMQTTBroker.h"

const char* ssid = "DeinSSID";
const char* password = "DeinPasswort";

ESPAsyncMQTTBroker mqttBroker(1883);

void setup() {
    Serial.begin(115200);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println("\nWLAN verbunden!");

    mqttBroker.begin();
}

void loop() {
    // Alles läuft asynchron – kein delay nötig!
}
