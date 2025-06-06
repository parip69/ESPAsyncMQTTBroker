#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "ESPAsyncMQTTBroker.h"

// WiFi credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Broker instance
ESPAsyncMQTTBroker broker;

// MQTT Client
WiFiClient wifiClient;
PubSubClient client(wifiClient);
String clientId = "TopicValidationTester";

void callback(char* topic, byte* payload, unsigned int length) {
    Serial.print("Message arrived [");
    Serial.print(topic);
    Serial.print("] ");
    for (unsigned int i = 0; i < length; i++) {
        Serial.print((char)payload[i]);
    }
    Serial.println();
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

void reconnectClient() {
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
    if (!client.connected()) {
         Serial.println("Failed to connect client after multiple attempts.");
    }
}

void setup() {
    Serial.begin(115200);
    setup_wifi();

    // Start the broker with detailed logging
    broker.setDebugLevel(DEBUG_DEBUG);
    broker.begin();
    Serial.println("MQTT Broker Started with DEBUG_DEBUG logging.");
    Serial.println("Broker logs will show reasons for rejections if any.");

    // Configure client
    client.setServer(WiFi.localIP(), 1883);
    client.setCallback(callback);

    reconnectClient(); // Connect the client

    if (client.connected()) {
        Serial.println("\n--- Testing Topic Validations ---");

        // 1. Valid Publish
        Serial.println("\n1. Attempting VALID publish to 'valid/topic/test'");
        if (client.publish("valid/topic/test", "hello from valid topic")) {
            Serial.println("  SUCCESS: Publish command sent for 'valid/topic/test'.");
        } else {
            Serial.println("  FAILURE: Publish command for 'valid/topic/test' failed locally.");
        }
        broker.publish("broker/internal/valid", "Broker internal valid message"); // Broker direct publish

        // 2. Invalid Publish Topics (containing wildcards)
        Serial.println("\n2. Attempting INVALID publish to 'invalid/topic/#' (contains #)");
        if (client.publish("invalid/topic/#", "hello with #")) {
            Serial.println("  SUCCESS: Publish command sent for 'invalid/topic/#'. (Broker should reject or close connection)");
        } else {
            Serial.println("  FAILURE: Publish command for 'invalid/topic/#' failed locally.");
        }
        // Broker should log: "Ungültiger Publish-Topic: Topic 'invalid/topic/#' enthält Multi-Level Wildcard '#'."
        // and potentially close the connection. Let's try to reconnect if closed.
        if (!client.connected()) reconnectClient();


        Serial.println("\n3. Attempting INVALID publish to 'invalid/topic/+' (contains +)");
        if (client.publish("invalid/topic/+", "hello with +")) {
            Serial.println("  SUCCESS: Publish command sent for 'invalid/topic/+' (Broker should reject or close connection)");
        } else {
            Serial.println("  FAILURE: Publish command for 'invalid/topic/+' failed locally.");
        }
        if (!client.connected()) reconnectClient();


        Serial.println("\n4. Attempting INVALID publish to '' (empty topic)");
        if (client.publish("", "hello empty topic")) {
             Serial.println("  SUCCESS: Publish command sent for '' (Broker should reject or close connection)");
        } else {
            Serial.println("  FAILURE: Publish command for '' failed locally (PubSubClient might prevent this).");
        }
        if (!client.connected()) reconnectClient();


        // 5. Valid Subscribe
        Serial.println("\n5. Attempting VALID subscribe to 'valid/topic/#'");
        if (client.subscribe("valid/topic/#")) {
            Serial.println("  SUCCESS: Subscribe command sent for 'valid/topic/#'. (Broker should send SUBACK with success)");
        } else {
            Serial.println("  FAILURE: Subscribe command for 'valid/topic/#' failed locally.");
        }

        // 6. Invalid Subscribe Topic Filters
        Serial.println("\n6. Attempting INVALID subscribe to 'invalid/filter#/test' (# not last level element)");
        if (client.subscribe("invalid/filter#/test")) {
            Serial.println("  SUCCESS: Subscribe command sent for 'invalid/filter#/test'. (Broker should send SUBACK with failure code)");
        } else {
            Serial.println("  FAILURE: Subscribe command for 'invalid/filter#/test' failed locally.");
        }
        // Broker should log: "Ungültiger Topic-Filter: '#' darf nicht Teil einer Ebene sein" or similar
        // And SUBACK should indicate failure (e.g., 0x8F for MQTT 5, 0x80 for MQTT 3.1.1)

        Serial.println("\n7. Attempting INVALID subscribe to 'invalid/filter+' (contains + within a level, not as a whole level)");
        // This is actually valid if it's 'invalid/filter/+' but 'invalid/filter+' is tricky.
        // Let's try 'invalid/filter+/element'
        if (client.subscribe("invalid/filter+/element")) {
            Serial.println("  SUCCESS: Subscribe command sent for 'invalid/filter+/element'. (Broker should send SUBACK with failure code)");
        } else {
            Serial.println("  FAILURE: Subscribe command for 'invalid/filter+/element' failed locally.");
        }
        // Broker should log: "Ungültiger Topic-Filter: '+' darf nicht Teil einer Ebene sein"

        Serial.println("\n8. Attempting INVALID subscribe to '' (empty filter string)");
        // PubSubClient might prevent sending an empty subscribe topic.
        // The broker's isValidTopicFilter already checks for empty.
        if (client.subscribe("")) {
             Serial.println("  SUCCESS: Subscribe command sent for ''. (Broker should send SUBACK with failure code)");
        } else {
            Serial.println("  FAILURE: Subscribe command for '' failed locally (PubSubClient might prevent this).");
        }

        Serial.println("\n--- Test Scenarios Complete ---");
        Serial.println("Check broker logs for messages about topic validation and client disconnections.");
        Serial.println("For failed subscriptions, the broker should send a SUBACK with a failure code.");
        Serial.println("For invalid publishes, the broker might close the connection due to protocol violation.");
    } else {
        Serial.println("MQTT Client not connected. Cannot run validation tests.");
    }
}

void loop() {
    if (client.connected()) {
        client.loop();
    } else {
        // Attempt to reconnect if tests caused disconnection
        // reconnectClient(); // Be careful with reconnecting in a loop without delay
    }
    delay(100); // Keep loop responsive
}
