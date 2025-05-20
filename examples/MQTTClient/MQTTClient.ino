#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <AsyncMqttClient.h>
#include <ESPAsyncMQTTBroker.h> // Optional - nur wenn Sie dynamisch zwischen Client/Broker wechseln wollen

// WiFi-Konfiguration
const char* ssid = "MeinWLAN";     // Ändern Sie dies auf Ihre WLAN-SSID
const char* password = "MeinPasswort"; // Ändern Sie dies auf Ihr WLAN-Passwort

// MQTT-Client Konfiguration
const char* mqttBrokerHost = "192.168.1.100"; // Ändern Sie dies auf die IP-Adresse Ihres MQTT-Brokers
const uint16_t mqttBrokerPort = 1883;
const char* mqttClientId = "ESP32Client1";
const char* mqttUsername = ""; // Optional
const char* mqttPassword = ""; // Optional

// Themen
const char* ledControlTopic = "led/control";
const char* ledStatusTopic = "led/status";
const char* temperatureTopic = "sensors/temperature";

// Status-LED
const int ledPin = 2; // Integrierte LED an den meisten ESP32-Boards
bool ledState = false;

// Temperatursensor simulieren
float temperature = 20.0;

// MQTT-Client Instanz
AsyncMqttClient mqttClient;

// Timers für Reconnect und Sensor-Updates
unsigned long lastTemperatureUpdate = 0;
const long temperatureUpdateInterval = 10000; // 10 Sekunden

// MQTT Ereignis-Handler
void onMqttConnect(bool sessionPresent) {
  Serial.println("Mit MQTT-Broker verbunden!");
  Serial.printf("Session Present: %d\n", sessionPresent);
  
  // Themen abonnieren
  uint16_t packetIdSub = mqttClient.subscribe(ledControlTopic, 1);
  Serial.printf("Abonnieren des Topics %s, packetId: %d\n", ledControlTopic, packetIdSub);
  
  // LED-Status veröffentlichen
  String status = ledState ? "on" : "off";
  mqttClient.publish(ledStatusTopic, 1, true, status.c_str());
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.println("Vom MQTT-Broker getrennt!");

  if (WiFi.isConnected()) {
    // Wenn WLAN verbunden ist, versuchen wir uns erneut mit dem MQTT-Broker zu verbinden
    Serial.println("Versuche erneut, Verbindung zum MQTT-Broker herzustellen...");
    mqttClient.connect();
  }
}

void onMqttSubscribe(uint16_t packetId, uint8_t qos) {
  Serial.printf("Abonnement bestätigt, packetId: %d, QoS: %d\n", packetId, qos);
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, 
                  size_t len, size_t index, size_t total) {
  // Payload zu String konvertieren
  String message;
  for (size_t i = 0; i < len; i++) {
    message += (char)payload[i];
  }
  
  Serial.printf("Nachricht empfangen auf Topic: %s - %s\n", topic, message.c_str());
  
  // LED-Steuerung verarbeiten
  if (String(topic) == ledControlTopic) {
    if (message == "on" || message == "true") {
      digitalWrite(ledPin, HIGH);
      ledState = true;
      // Status-Update senden
      mqttClient.publish(ledStatusTopic, 1, true, "on");
    } 
    else if (message == "off" || message == "false") {
      digitalWrite(ledPin, LOW);
      ledState = false;
      // Status-Update senden
      mqttClient.publish(ledStatusTopic, 1, true, "off");
    }
  }
}

void connectToWifi() {
  Serial.println("Verbinde mit WLAN...");
  WiFi.begin(ssid, password);
}

void connectToMqtt() {
  Serial.println("Verbinde mit MQTT-Broker...");
  mqttClient.connect();
}

void setup() {
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  
  // WLAN-Ereignis-Handler
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    Serial.println("Mit WLAN verbunden!");
    connectToMqtt();
  }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_CONNECTED);
  
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    Serial.println("Von WLAN getrennt!");
  }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  
  // MQTT-Client konfigurieren
  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onSubscribe(onMqttSubscribe);
  mqttClient.onMessage(onMqttMessage);
  
  mqttClient.setServer(mqttBrokerHost, mqttBrokerPort);
  mqttClient.setClientId(mqttClientId);
  
  // Optional: Authentifizierung hinzufügen, wenn erforderlich
  if (strlen(mqttUsername) > 0) {
    mqttClient.setCredentials(mqttUsername, mqttPassword);
  }
  
  // Mit WLAN verbinden
  connectToWifi();
}

void loop() {
  // Temperatur aktualisieren und senden
  if (millis() - lastTemperatureUpdate > temperatureUpdateInterval) {
    lastTemperatureUpdate = millis();
    
    // Simulierte Temperaturänderung
    temperature = 20.0 + ((float)random(-20, 20) / 10.0);
    String temperatureStr = String(temperature, 1); // Eine Nachkommastelle
    
    // Temperatur veröffentlichen, wenn verbunden
    if (mqttClient.connected()) {
      mqttClient.publish(temperatureTopic, 1, true, temperatureStr.c_str());
      Serial.printf("Temperatur gesendet: %s°C\n", temperatureStr.c_str());
    }
  }
}
