#include <Arduino.h>
#include <WiFi.h>
#include "ESPAsyncMQTTBroker.h"
#include "AsyncCompat.h" // Include the new compatibility layer

// 1. Instantiate the broker
ESPAsyncMQTTBroker broker;

// 2. Instantiate the compatibility client, passing it the broker instance
AsyncMQTTCompatClient localClient(broker);

void onMqttConnect(bool sessionPresent) {
  Serial.println("Connected to local broker.");

  // 3. Subscribe to a topic
  uint16_t packetIdSub = localClient.subscribe("local/test/topic", 1);
  Serial.print("Subscribing to 'local/test/topic', QoS 1, packetId: ");
  Serial.println(packetIdSub);

  // 4. Publish a message to the same topic
  const char* message = "Hello from local client!";
  uint16_t packetIdPub = localClient.publish("local/test/topic", 1, false, message);
  Serial.print("Publishing to 'local/test/topic', QoS 1, packetId: ");
  Serial.println(packetIdPub);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.println("Disconnected from local broker.");
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  Serial.println(">>> Message Received <<<");
  Serial.print("  Topic: ");
  Serial.println(topic);
  Serial.print("  Payload: ");
  Serial.println(payload);
  Serial.println("----------------------");
}

void onMqttPublish(uint16_t packetId) {
  Serial.print("Publish acknowledged, packetId: ");
  Serial.println(packetId);
}


void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("--- AsyncMQTTCompatClient Demo ---");

  // WiFi connection is not strictly necessary for the local client to work,
  // but the broker might be used by network clients simultaneously.
  WiFi.mode(WIFI_STA);
  WiFi.begin("DUMMY_SSID", "DUMMY_PASSWORD");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected.");

  // Start the broker so it can listen for network clients
  broker.begin();
  Serial.println("MQTT Broker started.");

  // Set up the local client's callbacks
  localClient.onConnect(onMqttConnect);
  localClient.onDisconnect(onMqttDisconnect);
  localClient.onMessage(onMqttMessage);
  localClient.onPublish(onMqttPublish);

  // "Connect" the local client to the local broker
  localClient.connect();
}

void loop() {
  // No code needed here for this demo.
  // The communication is handled by callbacks.
}
