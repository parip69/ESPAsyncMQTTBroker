#include <ESPAsyncMQTTBroker.h>
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif

ESPAsyncMQTTBroker mqtt;

void setup() {
  Serial.begin(115200);
  WiFi.begin("SSID", "PASSWORT");

  // Create a configuration object
  ESPAsyncMQTTBrokerConfig config;

  // Set the log flag to false to disable logging
  // Default is true
  config.log = false;

  // Set the configuration
  mqtt.setConfig(config);

  mqtt.begin();
}

void loop() {
}
