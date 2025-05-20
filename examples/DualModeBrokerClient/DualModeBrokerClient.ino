#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <AsyncMqttClient.h>
#include <ESPAsyncMQTTBroker.h>
#include <SPIFFS.h>

// WiFi-Konfiguration
const char* ssid = "MeinWLAN";     // Ändern Sie dies auf Ihre WLAN-SSID
const char* password = "MeinPasswort"; // Ändern Sie dies auf Ihr WLAN-Passwort

// MQTT-Konfiguration
const char* mqttExternalBroker = "192.168.1.200"; // Externe MQTT-Broker-Adresse
const int mqttPort = 1883;
const char* mqttClientId = "ESP32DualMode";
const char* mqttRootTopic = "esp32";

// Betriebsmodus-Einstellungen
bool isBrokerMode = true; // true = Broker-Modus, false = Client-Modus
const int modePin = 13;   // Pin zum Umschalten des Modus (mit Pull-up)

// Status-LED
const int ledPin = 2;     // Integrierte LED

// Web-Server für Konfiguration
AsyncWebServer server(80);

// MQTT-Instanzen
ESPAsyncMQTTBroker mqttBroker;
AsyncMqttClient mqttClient;

// Timers
unsigned long lastStatusUpdate = 0;
const long statusInterval = 5000; // 5 Sekunden

// Vorwärtsdeklarationen
void setupBrokerMode();
void setupClientMode();
void publishStatusMessage(bool retain = false);
void checkModeSwitch();

// MQTT-Nachrichtenverarbeitung für Broker-Modus
void onBrokerMessage(String clientId, String topic, String message) {
  Serial.printf("[Broker] Nachricht von %s auf Topic '%s': %s\n", 
                clientId.c_str(), topic.c_str(), message.c_str());
  
  // LED-Steuerung
  if (topic == mqttRootTopic + String("/led/control")) {
    if (message == "on") {
      digitalWrite(ledPin, HIGH);
      mqttBroker.publish((mqttRootTopic + String("/led/status")).c_str(), 0, true, "on");
    } else if (message == "off") {
      digitalWrite(ledPin, LOW);
      mqttBroker.publish((mqttRootTopic + String("/led/status")).c_str(), 0, true, "off");
    }
  }
}

// MQTT-Nachrichtenverarbeitung für Client-Modus
void onClientMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, 
                    size_t len, size_t index, size_t total) {
  String message;
  for (size_t i = 0; i < len; i++) {
    message += (char)payload[i];
  }
  
  Serial.printf("[Client] Nachricht auf Topic '%s': %s\n", topic, message.c_str());
  
  // LED-Steuerung
  if (String(topic) == mqttRootTopic + String("/led/control")) {
    if (message == "on") {
      digitalWrite(ledPin, HIGH);
      mqttClient.publish((mqttRootTopic + String("/led/status")).c_str(), 0, true, "on");
    } else if (message == "off") {
      digitalWrite(ledPin, LOW);
      mqttClient.publish((mqttRootTopic + String("/led/status")).c_str(), 0, true, "off");
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  pinMode(modePin, INPUT_PULLUP);
  
  // SPIFFS für Webserver starten
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS-Initialisierung fehlgeschlagen!");
  }
  
  // Mit WLAN verbinden
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("");
  Serial.println("Mit WLAN verbunden");
  Serial.print("IP-Adresse: ");
  Serial.println(WiFi.localIP());
  
  // Modus basierend auf dem Pin-Status bestimmen
  isBrokerMode = digitalRead(modePin) == HIGH;
  
  // Entsprechenden Modus einrichten
  if (isBrokerMode) {
    Serial.println("Starte im MQTT-Broker-Modus");
    setupBrokerMode();
  } else {
    Serial.println("Starte im MQTT-Client-Modus");
    setupClientMode();
  }
  
  // Webserver für Modus-Konfiguration einrichten
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = "<html><body style='font-family: Arial; margin: 20px;'>";
    html += "<h1>ESP32 MQTT Modus-Konfiguration</h1>";
    html += "<p>Aktueller Modus: <strong>";
    html += isBrokerMode ? "Broker" : "Client";
    html += "</strong></p>";
    html += "<p><a href='/mode?broker=true' style='background: #4CAF50; color: white; padding: 10px 15px; text-decoration: none; border-radius: 4px;'>Broker-Modus</a> ";
    html += "<a href='/mode?broker=false' style='background: #2196F3; color: white; padding: 10px 15px; text-decoration: none; border-radius: 4px;'>Client-Modus</a></p>";
    html += "<p>IP-Adresse: " + WiFi.localIP().toString() + "</p>";
    html += "</body></html>";
    request->send(200, "text/html", html);
  });
  
  server.on("/mode", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("broker")) {
      bool newMode = (request->getParam("broker")->value() == "true");
      
      if (newMode != isBrokerMode) {
        isBrokerMode = newMode;
        
        // Stoppe aktuellen Modus
        if (isBrokerMode) {
          mqttClient.disconnect();
          setupBrokerMode();
        } else {
          mqttBroker.stop();
          setupClientMode();
        }
      }
    }
    request->redirect("/");
  });
  
  server.begin();
  Serial.println("HTTP-Server gestartet");
}

void loop() {
  // Status-Updates senden
  if (millis() - lastStatusUpdate > statusInterval) {
    lastStatusUpdate = millis();
    publishStatusMessage();
  }
  
  // Betriebsmoduswechsel prüfen (Hardware-Button)
  checkModeSwitch();
}

void setupBrokerMode() {
  // MQTT-Broker mit Debug-Level konfigurieren
  mqttBroker.setDebugLevel(DEBUG_INFO);
  
  // Client-Verbindungs-Callback
  mqttBroker.onClientConnect([](String clientId, String clientIp) {
    Serial.printf("Client verbunden: %s (%s)\n", clientId.c_str(), clientIp.c_str());
  });
  
  // Nachrichtenempfang-Callback
  mqttBroker.onMessage(onBrokerMessage);
  
  // Broker starten
  mqttBroker.begin();
  Serial.println("MQTT-Broker gestartet auf Port 1883");
  
  // LED-Status veröffentlichen
  publishStatusMessage(true);
}

void setupClientMode() {
  // MQTT-Client konfigurieren
  mqttClient.setServer(mqttExternalBroker, mqttPort);
  mqttClient.setClientId(mqttClientId);
  
  // Callbacks
  mqttClient.onConnect([](bool sessionPresent) {
    Serial.println("Mit MQTT-Broker verbunden!");
    
    // LED-Control-Topic abonnieren
    String topic = mqttRootTopic + String("/led/control");
    mqttClient.subscribe(topic.c_str(), 1);
    
    // LED-Status veröffentlichen
    publishStatusMessage(true);
  });
  
  mqttClient.onDisconnect([](AsyncMqttClientDisconnectReason reason) {
    Serial.println("Vom MQTT-Broker getrennt!");
    
    // Versuchen, die Verbindung wiederherzustellen
    if (WiFi.isConnected()) {
      mqttClient.connect();
    }
  });
  
  mqttClient.onMessage(onClientMessage);
  
  // Verbindung herstellen
  mqttClient.connect();
}

void publishStatusMessage(bool retain) {
  String uptime = String(millis() / 1000);
  String mode = isBrokerMode ? "broker" : "client";
  
  if (isBrokerMode) {
    mqttBroker.publish((mqttRootTopic + String("/status/uptime")).c_str(), 0, retain, uptime.c_str());
    mqttBroker.publish((mqttRootTopic + String("/status/mode")).c_str(), 0, retain, mode.c_str());
    mqttBroker.publish((mqttRootTopic + String("/status/ip")).c_str(), 0, retain, WiFi.localIP().toString().c_str());
  } else if (mqttClient.connected()) {
    mqttClient.publish((mqttRootTopic + String("/status/uptime")).c_str(), 0, retain, uptime.c_str());
    mqttClient.publish((mqttRootTopic + String("/status/mode")).c_str(), 0, retain, mode.c_str());
    mqttClient.publish((mqttRootTopic + String("/status/ip")).c_str(), 0, retain, WiFi.localIP().toString().c_str());
  }
}

void checkModeSwitch() {
  static bool lastPinState = digitalRead(modePin);
  bool currentPinState = digitalRead(modePin);
  
  // Modus wechseln, wenn sich der Pin-Status ändert
  if (currentPinState != lastPinState) {
    delay(50); // Entprellung
    
    if (digitalRead(modePin) == currentPinState) {
      isBrokerMode = currentPinState;
      
      Serial.printf("Modus geändert zu: %s\n", isBrokerMode ? "Broker" : "Client");
      
      // Alten Modus stoppen und neuen starten
      if (isBrokerMode) {
        mqttClient.disconnect();
        setupBrokerMode();
      } else {
        mqttBroker.stop();
        setupClientMode();
      }
    }
    
    lastPinState = currentPinState;
  }
}
