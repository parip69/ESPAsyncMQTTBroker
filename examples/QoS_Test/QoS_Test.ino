#include <Arduino.h>
#include <WiFi.h>
#include <AsyncMqttClient.h>
#include "ESPAsyncMQTTBroker.h"

#define WIFI_SSID "your_ssid"
#define WIFI_PASSWORD "your_password"

// Broker and Client running on the same ESP32
IPAddress local_IP(127, 0, 0, 1);
#define MQTT_HOST local_IP
#define MQTT_PORT 1883

ESPAsyncMQTTBroker broker(MQTT_PORT);
AsyncMqttClient mqttClient;
TimerHandle_t mqttReconnectTimer;

void connectToWifi() {
  Serial.println("Connecting to Wi-Fi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void connectToMqtt() {
  Serial.println("Connecting to MQTT broker...");
  mqttClient.connect();
}

void onMqttConnect(bool sessionPresent) {
  Serial.println("Connected to MQTT.");
  Serial.print("Session present: ");
  Serial.println(sessionPresent);

  // Subscribe to the test topic with QoS 1
  uint16_t packetIdSub = mqttClient.subscribe("test/qos1", 1);
  Serial.print("Subscribing at QoS 1, packetId: ");
  Serial.println(packetIdSub);

  // Publish a message to the test topic with QoS 1
  String message = "hello from QoS 1 publisher";
  uint16_t packetIdPub = mqttClient.publish("test/qos1", 1, false, message.c_str());
  Serial.print("Publishing at QoS 1, packetId: ");
  Serial.println(packetIdPub);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.println("Disconnected from MQTT.");
  if (WiFi.isConnected()) {
    xTimerStart(mqttReconnectTimer, 0);
  }
}

void onMqttSubscribe(uint16_t packetId, uint8_t qos) {
  Serial.println("Subscribe acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
  Serial.print("  qos: ");
  Serial.println(qos);
}

void onMqttUnsubscribe(uint16_t packetId) {
  Serial.println("Unsubscribe acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  Serial.println("Publish received.");
  Serial.print("  topic: ");
  Serial.println(topic);
  Serial.print("  qos: ");
  Serial.println(properties.qos);
  Serial.print("  payload: ");
  Serial.write((uint8_t*)payload, len);
  Serial.println();
}

void onMqttPublish(uint16_t packetId) {
  Serial.println("Publish acknowledged.");
  Serial.print("  packetId: ");
  Serial.println(packetId);
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n\n--- QoS Test ---");

  // It's important to start the broker BEFORE the client tries to connect
  Serial.println("Starting MQTT Broker...");
  broker.begin();

  connectToWifi();

  mqttReconnectTimer = xTimerCreate("mqttTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)0, reinterpret_cast<TimerCallbackFunction_t>(connectToMqtt));

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onSubscribe(onMqttSubscribe);
  mqttClient.onUnsubscribe(onMqttUnsubscribe);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.onPublish(onMqttPublish);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);

  connectToMqtt();
}

void loop() {
  // The broker and client work asynchronously, so nothing is needed here.
  delay(1000);
}
