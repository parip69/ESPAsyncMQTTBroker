#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h> // For MQTT clients
#include "ESPAsyncMQTTBroker.h" // The broker library

// WiFi credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Broker instance
ESPAsyncMQTTBroker broker;

// MQTT Clients
WiFiClient wifiClient1;
PubSubClient client1(wifiClient1); // This client will use noLocal
String client1Id = "ClientWithNoLocal";

WiFiClient wifiClient2;
PubSubClient client2(wifiClient2); // This client will subscribe normally
String client2Id = "NormalSubscriberClient";

const char* testTopic = "noLocal/testTopic";
unsigned long lastMsgClient1 = 0;
unsigned long lastMsgClient2 = 0;
int client1MessagesReceived = 0;
int client2MessagesReceived = 0;

void callbackClient1(char* topic, byte* payload, unsigned int length) {
    Serial.print("Client 1 (noLocal) received message on topic: ");
    Serial.println(topic);
    Serial.print("Payload: ");
    for (unsigned int i = 0; i < length; i++) {
        Serial.print((char)payload[i]);
    }
    Serial.println();
    client1MessagesReceived++;
}

void callbackClient2(char* topic, byte* payload, unsigned int length) {
    Serial.print("Client 2 (normal) received message on topic: ");
    Serial.println(topic);
    Serial.print("Payload: ");
    for (unsigned int i = 0; i < length; i++) {
        Serial.print((char)payload[i]);
    }
    Serial.println();
    client2MessagesReceived++;
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

void reconnectClient(PubSubClient& client, const String& clientId, bool subscribeWithNoLocal) {
    int attempts = 0;
    while (!client.connected() && attempts < 5) {
        attempts++;
        Serial.print("Attempting MQTT connection for ");
        Serial.print(clientId);
        Serial.print(" (Attempt ");
        Serial.print(attempts);
        Serial.print(")...");

        // Attempt to connect
        // For PubSubClient, there's no direct way to set MQTT 5.0 properties like noLocal
        // during connect() or subscribe() in a standard way for all MQTT versions.
        // The noLocal flag is an MQTT 5.0 feature.
        // ESPAsyncMQTTBroker supports parsing it from MQTT 5.0 SUBSCRIBE packets.
        // PubSubClient is primarily an MQTT 3.1.1 client.
        //
        // TO TRULY TEST noLocal, we would need an MQTT 5.0 client library or
        // manually craft the SUBSCRIBE packet with the noLocal option.
        //
        // For this example, we'll simulate the scenario by having client1 subscribe normally,
        // but the text will explain that if it *were* an MQTT 5.0 client setting noLocal=true,
        // it should not receive its own messages. The broker's noLocal logic
        // relies on the 'excludeClientId' passed to its internal publish method.
        // When client1 publishes, the broker's handlePublish will call its internal publish
        // with client1Id as excludeClientId. If client1's subscription (if it were MQTT 5)
        // had noLocal=true, the broker would skip sending back to client1.

        if (client.connect(clientId.c_str())) {
            Serial.println("connected");
            if (subscribeWithNoLocal) {
                // PubSubClient's subscribe doesn't have a noLocal option.
                // We subscribe normally. The broker's internal logic handles 'excludeClientId'.
                // If client1 were an MQTT 5.0 client and set the noLocal subscription option,
                // the broker would honor it.
                client.subscribe(testTopic);
                Serial.println("Client1 subscribed to testTopic (simulating noLocal intention).");
                Serial.println("Broker's excludeClientId mechanism will prevent self-delivery if client1's subscription was marked noLocal by an MQTT5 client.");

            } else {
                client.subscribe(testTopic);
                Serial.println("Client2 subscribed to testTopic normally.");
            }
        } else {
            Serial.print("failed, rc=");
            Serial.print(client.state());
            Serial.println(" try again in 2 seconds");
            delay(2000);
        }
    }
     if (!client.connected()) {
        Serial.print("Failed to connect client: ");
        Serial.println(clientId);
    }
}


void setup() {
    Serial.begin(115200);
    setup_wifi();

    // Start the broker
    broker.setDebugLevel(DEBUG_DEBUG); // Enable detailed broker logging
    broker.begin();
    Serial.println("MQTT Broker Started");

    // Configure clients
    client1.setServer(WiFi.localIP(), 1883);
    client1.setCallback(callbackClient1);

    client2.setServer(WiFi.localIP(), 1883);
    client2.setCallback(callbackClient2);

    Serial.println("Setting up clients. Note: PubSubClient is MQTT 3.1.1.");
    Serial.println("True noLocal behavior requires an MQTT 5.0 client that can set subscription options.");
    Serial.println("This demo relies on the broker's 'excludeClientId' feature, which is one part of noLocal handling.");
}

void loop() {
    if (!client1.connected()) {
        reconnectClient(client1, client1Id, true);
    }
    client1.loop();

    if (!client2.connected()) {
        reconnectClient(client2, client2Id, false);
    }
    client2.loop();

    unsigned long now = millis();

    // Client1 publishes a message every 10 seconds
    if (client1.connected() && now - lastMsgClient1 > 10000) {
        lastMsgClient1 = now;
        String msg = "Message from Client1 (id: " + client1Id + ") at " + String(now);
        client1.publish(testTopic, msg.c_str());
        Serial.print("Client1 published: ");
        Serial.println(msg);
        Serial.println("--- EXPECTED BEHAVIOR ---");
        Serial.println("Client1 (if it used MQTT 5.0 noLocal=true subscription): Should NOT receive this message.");
        Serial.println("Client2 (normal subscriber): Should receive this message.");
        Serial.println("-------------------------");
    }

    // Check results after some time
    static unsigned long lastCheck = 0;
    if (now - lastCheck > 30000 && lastCheck != 0) { // Check every 30s after first publish
        Serial.println("\n--- SUMMARY ---");
        Serial.print("Client1 (simulating noLocal) messages received: ");
        Serial.println(client1MessagesReceived);
        Serial.print("Client2 (normal) messages received: ");
        Serial.println(client2MessagesReceived);
        Serial.println("Expected for true noLocal: Client1=0 (or few if initial retained), Client2 > 0");
        Serial.println("If Client1 received messages, it's because PubSubClient cannot set MQTT 5.0 noLocal subscription flag.");
        Serial.println("The broker's excludeClientId feature works if the subscription carries the noLocal flag.");
        Serial.println("----------------\n");
    }
    if (lastMsgClient1 > 0 && lastCheck == 0) { // Start check timer after first publish
        lastCheck = now;
    }


    // Allow broker to process (not strictly necessary with async broker, but good for client loops)
    delay(10);
}
