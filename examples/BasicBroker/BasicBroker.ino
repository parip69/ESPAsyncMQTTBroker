/*
 * ESPAsyncMQTTBroker - Einfaches Beispiel
 * 
 * Dieses Beispiel zeigt, wie man einen MQTT-Broker auf einem ESP8266/ESP32 einrichtet
 * und grundlegende Funktionen wie Verbindungen, Nachrichten und Abonnements überwacht.
 * 
 * Der Broker ermöglicht, dass Ihre IoT-Geräte direkt miteinander kommunizieren können,
 * ohne einen externen MQTT-Server zu benötigen.
 * 
 * Copyright (c) 2023 Kala69
 */

 #include <ESPAsyncMQTTBroker.h>

 #ifdef ESP8266
   #include <ESP8266WiFi.h>
   const int LED_PIN = LED_BUILTIN;    // Eingebaute LED für ESP8266
 #elif defined(ESP32)
   #include <WiFi.h>
   const int LED_PIN = 2;              // Standard-LED-Pin für ESP32
 #else
   #error "Dieses Beispiel unterstützt nur ESP8266 und ESP32"
 #endif
 
 // WLAN-Zugangsdaten
 const char* ssid = "DEIN_WLAN_NAME";
 const char* password = "DEIN_WLAN_PASSWORT";
 
 // MQTT-Broker auf Port 1883 (Standard-MQTT-Port)
 ESPAsyncMQTTBroker mqttBroker(1883);
 
 // Statistiken
 int clientsConnected = 0;
 int messagesReceived = 0;
 unsigned long startTime = 0;
 
 void setupWiFi() {
   Serial.print("Verbinde mit WLAN ");
   Serial.println(ssid);
   
   // Setze WiFi-Modus auf Station (Client)
   WiFi.mode(WIFI_STA);
   WiFi.begin(ssid, password);
   
   // Warte auf Verbindung und blinke LED während des Verbindens
   while (WiFi.status() != WL_CONNECTED) {
     digitalWrite(LED_PIN, !digitalRead(LED_PIN));
     delay(250);
     Serial.print(".");
   }
   
   // LED anschalten, wenn verbunden
   digitalWrite(LED_PIN, HIGH);
   
   Serial.println("");
   Serial.println("WiFi verbunden!");
   Serial.print("IP-Adresse: ");
   Serial.println(WiFi.localIP());
   Serial.print("Signal-Stärke (RSSI): ");
   Serial.println(WiFi.RSSI());
 }
 
 void setupMQTTBroker() {
   // Konfiguration für den MQTT-Broker
   ESPAsyncMQTTBrokerConfig config;
   
   // Optional: Authentifizierung aktivieren
   // config.username = "user";
   // config.password = "password";
   
   mqttBroker.setConfig(config);
   
   // Debug-Level festlegen
   // DEBUG_NONE: Keine Debug-Ausgaben
   // DEBUG_ERROR: Nur Fehler
   // DEBUG_INFO: Informationen und Fehler
   // DEBUG_DEBUG: Alle Debug-Informationen
   mqttBroker.setDebugLevel(DEBUG_INFO);
   
   // Client-Verbindungs-Callback
   mqttBroker.onClientConnect([](String clientId, String clientIp) {
     clientsConnected++;
     Serial.println("➕ Neuer Client verbunden: " + clientId + " von IP: " + clientIp);
     Serial.println("   Aktive Clients: " + String(clientsConnected));
   });
   
   // Client-Trennungs-Callback
   mqttBroker.onClientDisconnect([](String clientId) {
     clientsConnected--;
     Serial.println("➖ Client getrennt: " + clientId);
     Serial.println("   Aktive Clients: " + String(clientsConnected));
   });
   
   // Nachrichten-Callback
   mqttBroker.onMessage([](String clientId, String topic, String message) {
     messagesReceived++;
     Serial.println("📨 Nachricht erhalten von " + clientId + ":");
     Serial.println("   Topic: " + topic);
     Serial.println("   Inhalt: " + message);
     
     // LED kurz blinken lassen bei neuer Nachricht
     digitalWrite(LED_PIN, LOW);
     delay(50);
     digitalWrite(LED_PIN, HIGH);
   });
   
   // Abonnement-Callback
   mqttBroker.onSubscribe([](String clientId, const String& topic) {
     Serial.println("🔔 Client " + clientId + " abonniert: " + topic);
   });
   
   // Abonnement-Kündigung-Callback
   mqttBroker.onUnsubscribe([](String clientId, const String& topic) {
     Serial.println("🔕 Client " + clientId + " kündigt Abo: " + topic);
   });
   
   // Fehler-Callback
   mqttBroker.onError([](String clientId, int errorCode, const String& errorMessage) {
     Serial.println("❌ Fehler für Client " + clientId + ": " + errorMessage + " (Code: " + String(errorCode) + ")");
   });
   
   // MQTT-Broker starten
   mqttBroker.begin();
   
   Serial.println("\n🚀 MQTT-Broker gestartet auf Port 1883");
   Serial.println("-----------------------------------");
   Serial.println("Verbinden Sie einen MQTT-Client mit der IP: " + WiFi.localIP().toString());
   Serial.println("Beispiel-Verbindungsbefehl für mosquitto_pub:");
   Serial.println("mosquitto_pub -h " + WiFi.localIP().toString() + " -t test/topic -m \"Hallo Welt\"");
 }
 
 void setup() {
   // Serielle Verbindung starten
   Serial.begin(115200);
   delay(500);
   Serial.println("\n\n=============================================");
   Serial.println("ESP Asynchroner MQTT-Broker - Einfaches Beispiel");
   Serial.println("=============================================\n");
   
   // LED-Pin konfigurieren
   pinMode(LED_PIN, OUTPUT);
   digitalWrite(LED_PIN, LOW);
   
   // WLAN-Verbindung einrichten
   setupWiFi();
   
   // MQTT-Broker einrichten und starten
   setupMQTTBroker();
   
   // Startzeit merken für Laufzeitberechnung
   startTime = millis();
 }
 
 void checkWiFiConnection() {
   static unsigned long lastWiFiCheck = 0;
   
   if (millis() - lastWiFiCheck > 10000) {  // Alle 10 Sekunden prüfen
     lastWiFiCheck = millis();
     
     if (WiFi.status() != WL_CONNECTED) {
       Serial.println("❌ WiFi-Verbindung verloren! Versuche erneut zu verbinden...");
       setupWiFi();
     }
   }
 }
 
 void printStats() {
   static unsigned long lastStatusTime = 0;
   
   if (millis() - lastStatusTime > 60000) {  // Alle 60 Sekunden
     lastStatusTime = millis();
     
     unsigned long uptime = millis() - startTime;
     unsigned long days = uptime / (24 * 60 * 60 * 1000);
     uptime %= (24 * 60 * 60 * 1000);
     unsigned long hours = uptime / (60 * 60 * 1000);
     uptime %= (60 * 60 * 1000);
     unsigned long minutes = uptime / (60 * 1000);
     uptime %= (60 * 1000);
     unsigned long seconds = uptime / 1000;
     
     Serial.println("\n📊 MQTT-Broker Statistik:");
     Serial.println("-----------------------------------");
     Serial.println("Laufzeit: " + String(days) + " Tage, " + String(hours) + ":" + String(minutes) + ":" + String(seconds));
     Serial.println("IP-Adresse: " + WiFi.localIP().toString());
     Serial.println("Verbundene Clients: " + String(clientsConnected));
     Serial.println("Empfangene Nachrichten: " + String(messagesReceived));
     Serial.println("Freier Heap: " + String(ESP.getFreeHeap()) + " Bytes");
     Serial.println("-----------------------------------\n");
   }
 }
 
 void loop() {
   // Der MQTT-Broker arbeitet asynchron, daher ist hier keine spezifische Broker-Logik erforderlich
   
   // Überprüfe WiFi-Verbindung in regelmäßigen Abständen
   checkWiFiConnection();
   
   // Zeige Status-Informationen an
   printStats();
   
   // Fügen Sie hier weitere Funktionalität hinzu, wenn nötig
   // z.B. Lesen von Sensoren und Veröffentlichen der Werte über den Broker
 }
 
