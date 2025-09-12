#include <ESPAsyncMQTTBroker.h>
#include <ESPAsyncWebServer.h>

ESPAsyncMQTTBroker mqtt;
AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);
  WiFi.begin("SSID", "PASSWORT");

  mqtt.begin();

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", "MQTT-Broker läuft!");
  });
  server.begin();
}

void loop() {
  // The loop function is no longer needed for the broker
}
