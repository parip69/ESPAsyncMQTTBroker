#include <ESPAsyncMQTTBroker.h>

ESPAsyncMQTTBroker mqtt;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
  WiFi.begin("SSID", "PASSWORT");

  mqtt.onMessage([](const char* clientId, const char* topic, const uint8_t* payload, size_t len) {
    if (strcmp(topic, "/ring") == 0) {
      // Create a null-terminated string from the payload to be safe
      char payloadStr[len + 1];
      memcpy(payloadStr, payload, len);
      payloadStr[len] = '\0';

      if (strcmp(payloadStr, "an") == 0) {
        digitalWrite(LED_BUILTIN, LOW);
      } else {
        digitalWrite(LED_BUILTIN, HIGH);
      }
    }
  });

  mqtt.begin();
}

void loop() {
  // The loop function is no longer needed for the broker
}
