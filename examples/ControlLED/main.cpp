#include <ESPAsyncMQTTBroker.h>

ESPAsyncMQTTBroker mqtt;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
  WiFi.begin("SSID", "PASSWORT");

  mqtt.onMessage([](const String& topic, const String& payload) {
    if (topic == "/ring") {
      digitalWrite(LED_BUILTIN, payload == "an" ? LOW : HIGH);
    }
  });

  mqtt.begin();
}

void loop() {
  mqtt.loop();
}
