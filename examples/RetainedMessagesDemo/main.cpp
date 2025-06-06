#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "ESPAsyncMQTTBroker.h"

// WiFi credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Broker instance
ESPAsyncMQTTBroker broker;

// MQTT Clients
WiFiClient wifiClient1, wifiClient2;
PubSubClient client1(wifiClient1);
String client1Id = "RetainPublisherClient";

PubSubClient client2(wifiClient2);
String client2Id = "NewSubscriberClient";

const char* retainedTopic = "home/lights/kitchen";
bool client2Subscribed = false;
unsigned long client2SubscribeTime = 0;

void callbackClient1(char* topic, byte* payload, unsigned int length) {
    Serial.print("Client 1 received message on topic: ");
    Serial.println(topic);
    // Client 1 is mainly for publishing, but good to have a callback
}

void callbackClient2(char* topic, byte* payload, unsigned int length) {
    Serial.print("Client 2 (New Subscriber) received message on topic: ");
    Serial.println(topic);
    Serial.print("Payload: ");
    String msgContent = "";
    for (unsigned int i = 0; i < length; i++) {
        msgContent += (char)payload[i];
    }
    Serial.println(msgContent);
    if (strcmp(topic, retainedTopic) == 0) {
        if (msgContent == "ON") {
            Serial.println(">>> Client 2 received initial retained 'ON' message!");
        } else if (msgContent == "OFF") {
            Serial.println(">>> Client 2 received updated retained 'OFF' message!");
        } else if (length == 0) {
            Serial.println(">>> Client 2 received a message with empty payload (possibly after retained message was cleared).");
        }
    }
}

void setup_wifi() {
    delay(10);
    Serial.println();
    Serial.print("Connecting to ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
}

void connectClient(PubSubClient& client, const String& clientId, bool initialConnect = false) {
    int attempts = 0;
    while (!client.connected() && attempts < 3) {
        attempts++;
        Serial.print("Attempting MQTT connection for ");
        Serial.print(clientId);
        Serial.print(" (Attempt ");
        Serial.print(attempts);
        Serial.print(")...");
        if (client.connect(clientId.c_str())) {
            Serial.println("connected");
        } else {
            Serial.print("failed, rc=");
            Serial.print(client.state());
            Serial.println(" try again in 2 seconds");
            delay(2000);
        }
    }
     if (!client.connected() && initialConnect) {
        Serial.print("Failed to connect client: ");
        Serial.println(clientId);
        Serial.println("Example may not run as expected.");
    }
}

enum TestPhase {
    INIT,
    PUBLISH_RETAIN_ON,
    SUBSCRIBE_CLIENT2,
    PUBLISH_RETAIN_OFF,
    CLEAR_RETAIN,
    DONE
};
TestPhase currentPhase = INIT;
unsigned long phaseDelay = 5000; // 5 seconds between phases
unsigned long lastPhaseChange = 0;

void setup() {
    Serial.begin(115200);
    setup_wifi();

    broker.setDebugLevel(DEBUG_INFO); // Set to DEBUG_DEBUG for more broker details
    broker.begin();
    Serial.println("MQTT Broker Started.");

    client1.setServer(WiFi.localIP(), 1883);
    client1.setCallback(callbackClient1);
    connectClient(client1, client1Id, true);

    // Client 2 will connect later
    client2.setServer(WiFi.localIP(), 1883);
    client2.setCallback(callbackClient2);

    Serial.println("\n--- Retained Message Demo ---");
    currentPhase = INIT;
    lastPhaseChange = millis();
}

void loop() {
    if (!client1.connected() && currentPhase < DONE) {
        connectClient(client1, client1Id);
    }
    client1.loop();

    if (client2.connected()) {
        client2.loop();
    }

    unsigned long now = millis();

    if (now - lastPhaseChange > phaseDelay) {
        lastPhaseChange = now;

        switch (currentPhase) {
            case INIT:
                Serial.println("\nPHASE: INIT - Waiting for broker and client1 to be ready.");
                if (client1.connected()) {
                    currentPhase = PUBLISH_RETAIN_ON;
                } else {
                    Serial.println("Client1 not connected, retrying connection...");
                    connectClient(client1, client1Id);
                    if (!client1.connected()) lastPhaseChange = millis(); // retry this phase
                }
                break;

            case PUBLISH_RETAIN_ON:
                Serial.println("\nPHASE: PUBLISH_RETAIN_ON");
                Serial.print("Client1 publishing to '");
                Serial.print(retainedTopic);
                Serial.println("' with payload 'ON' and retain=true.");
                if (client1.publish(retainedTopic, "ON", true)) { // true for retained
                    Serial.println("  Publish 'ON' (retained) successful.");
                } else {
                    Serial.println("  Publish 'ON' (retained) failed.");
                }
                currentPhase = SUBSCRIBE_CLIENT2;
                break;

            case SUBSCRIBE_CLIENT2:
                Serial.println("\nPHASE: SUBSCRIBE_CLIENT2");
                Serial.println("Connecting Client2 and subscribing to '");
                Serial.print(retainedTopic);
                Serial.println("'.");
                Serial.println("Client2 should immediately receive the retained 'ON' message upon subscription.");
                connectClient(client2, client2Id, true);
                if (client2.connected()) {
                    client2.subscribe(retainedTopic);
                    client2Subscribed = true;
                    client2SubscribeTime = millis();
                } else {
                     Serial.println("  Client2 failed to connect. Retained message might not be received by Client2 yet.");
                }
                currentPhase = PUBLISH_RETAIN_OFF;
                break;

            case PUBLISH_RETAIN_OFF:
                Serial.println("\nPHASE: PUBLISH_RETAIN_OFF");
                Serial.print("Client1 publishing to '");
                Serial.print(retainedTopic);
                Serial.println("' with payload 'OFF' and retain=true.");
                Serial.println("This will replace the previous retained message. Client2 should receive this update.");
                if (client1.publish(retainedTopic, "OFF", true)) {
                    Serial.println("  Publish 'OFF' (retained) successful.");
                } else {
                    Serial.println("  Publish 'OFF' (retained) failed.");
                }
                currentPhase = CLEAR_RETAIN;
                break;

            case CLEAR_RETAIN:
                Serial.println("\nPHASE: CLEAR_RETAIN");
                Serial.print("Client1 publishing to '");
                Serial.print(retainedTopic);
                Serial.println("' with an EMPTY payload and retain=true.");
                Serial.println("This will clear the retained message for the topic.");
                if (client1.publish(retainedTopic, "", true)) { // Empty payload, retain=true
                    Serial.println("  Publish empty (retained) to clear successful.");
                } else {
                    Serial.println("  Publish empty (retained) to clear failed.");
                }
                Serial.println("Future subscribers to this topic will not receive an initial message.");
                currentPhase = DONE;
                break;

            case DONE:
                // Demo finished, just keep clients looping
                if (now - lastPhaseChange > 20000 && lastPhaseChange !=0 ) { // Print final note after a while
                     Serial.println("\n--- Demo Complete ---");
                     Serial.println("Monitor broker logs (DEBUG_INFO or DEBUG_DEBUG) to see retained message handling.");
                     lastPhaseChange = 0; // Prevent re-printing
                }
                break;
        }
    }

    // Allow some time for Client2 to process the message after subscribing
    if (client2Subscribed && now - client2SubscribeTime < 2000) {
        client2.loop();
    }
}
