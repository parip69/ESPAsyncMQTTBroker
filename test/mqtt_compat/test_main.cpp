#include <Arduino.h>
#include <unity.h>
#include <ESPAsyncMQTTBroker.h>
#include <AsyncMqttClient.h>
#include <WiFi.h>

// Broker instance
ESPAsyncMQTTBroker broker;

// Client instances
AsyncMqttClient lwtClient;
AsyncMqttClient witnessClient; // Subscribes to the LWT topic
AsyncMqttClient retainClient;
AsyncMqttClient retainSubscriber;

// Test state flags
volatile bool lwtMessageReceived = false;
volatile bool retainMessageReceived = false;
volatile bool retainMessageDeleted = false;

String lwtReceivedPayload = "";
String retainReceivedPayload = "";

// LWT Witness Callback
void onWitnessMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
    if (strcmp(topic, "lwt/topic") == 0) {
        lwtReceivedPayload = String(payload, len);
        lwtMessageReceived = true;
    }
}

// Retain Subscriber Callback
void onRetainSubscriberMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
    if (strcmp(topic, "retain/topic") == 0) {
        if (len > 0) {
            retainReceivedPayload = String(payload, len);
            retainMessageReceived = true;
        } else {
            // Empty payload means the retained message was cleared
            retainMessageDeleted = true;
        }
    }
}

void setUp(void) {
    // Reset flags before each test
    lwtMessageReceived = false;
    retainMessageReceived = false;
    retainMessageDeleted = false;
    lwtReceivedPayload = "";
    retainReceivedPayload = "";

    // Set up clients
    lwtClient.onMessage(onWitnessMessage);
    lwtClient.setServer(WiFi.localIP(), 1883);

    witnessClient.onMessage(onWitnessMessage);
    witnessClient.setServer(WiFi.localIP(), 1883);

    retainClient.setServer(WiFi.localIP(), 1883);
    retainSubscriber.onMessage(onRetainSubscriberMessage);
    retainSubscriber.setServer(WiFi.localIP(), 1883);
}

void tearDown(void) {
    // Disconnect clients
    if (lwtClient.connected()) lwtClient.disconnect();
    if (witnessClient.connected()) witnessClient.disconnect();
    if (retainClient.connected()) retainClient.disconnect();
    if (retainSubscriber.connected()) retainSubscriber.disconnect();
    delay(100); // Give time for disconnects to process
}

void test_lwt_is_published_on_unclean_disconnect() {
    // 1. Witness client connects and subscribes to LWT topic
    witnessClient.connect();
    while(!witnessClient.connected()) { delay(10); }
    uint16_t packetId = witnessClient.subscribe("lwt/topic", 1);
    TEST_ASSERT_NOT_EQUAL(0, packetId);
    delay(100); // Wait for subscription to be processed

    // 2. LWT client connects with a will
    lwtClient.setWill("lwt/topic", 1, false, "unclean_disconnect");
    lwtClient.connect();
    while(!lwtClient.connected()) { delay(10); }
    TEST_ASSERT_TRUE(lwtClient.connected());

    // 3. Simulate unclean disconnect of LWT client (by stopping the underlying TCP client)
    lwtClient.disconnect(true); // Force unclean disconnect
    delay(500); // Give broker time to publish LWT

    // 4. Check if witness received the LWT message
    TEST_ASSERT_TRUE_MESSAGE(lwtMessageReceived, "LWT message was not received by witness client");
    TEST_ASSERT_EQUAL_STRING("unclean_disconnect", lwtReceivedPayload.c_str());
}

void test_retain_message_is_delivered_to_new_subscriber() {
    // 1. Retain client connects and publishes a retained message
    retainClient.connect();
    while(!retainClient.connected()) { delay(10); }
    uint16_t pubId = retainClient.publish("retain/topic", 1, true, "i_was_retained");
    TEST_ASSERT_NOT_EQUAL(0, pubId);
    retainClient.disconnect();
    delay(100); // Wait for broker to process retain

    // 2. New subscriber connects and subscribes
    retainSubscriber.connect();
    while(!retainSubscriber.connected()) { delay(10); }
    uint16_t subId = retainSubscriber.subscribe("retain/topic", 1);
    TEST_ASSERT_NOT_EQUAL(0, subId);

    // 3. Check if the retained message was received immediately upon subscribing
    delay(500); // Give time for message delivery
    TEST_ASSERT_TRUE_MESSAGE(retainMessageReceived, "Retained message was not delivered to new subscriber");
    TEST_ASSERT_EQUAL_STRING("i_was_retained", retainReceivedPayload.c_str());
}

void test_retain_message_is_deleted() {
    // Pre-condition: ensure a retained message exists
    retainClient.connect();
    while(!retainClient.connected()) { delay(10); }
    retainClient.publish("retain/topic", 1, true, "to_be_deleted");
    delay(100);

    // 1. Subscriber connects and should receive the message
    retainSubscriber.connect();
    while(!retainSubscriber.connected()) { delay(10); }
    retainSubscriber.subscribe("retain/topic", 1);
    delay(500);
    TEST_ASSERT_TRUE(retainMessageReceived);
    TEST_ASSERT_EQUAL_STRING("to_be_deleted", retainReceivedPayload.c_str());
    retainSubscriber.disconnect();
    delay(100);

    // Reset flag
    retainMessageReceived = false;

    // 2. Publisher sends an empty retained message to the same topic
    retainClient.publish("retain/topic", 1, true, nullptr, 0);
    delay(100);
    retainClient.disconnect();
    delay(100);

    // 3. New subscriber connects, SHOULD NOT receive a message.
    // We will use the same subscriber, but we need to check for a non-event, which is tricky.
    // A better way is to subscribe and ensure onMessage is NOT called.
    // For simplicity here, we'll just check the flag is still false.
    retainMessageReceived = false; // reset again just in case
    retainSubscriber.connect();
    while(!retainSubscriber.connected()) { delay(10); }
    retainSubscriber.subscribe("retain/topic", 1);
    delay(500); // Wait and see if a message arrives
    TEST_ASSERT_FALSE_MESSAGE(retainMessageReceived, "A message was received, but it should have been deleted.");
}


void setup() {
    delay(2000); // Service delay

    // Connect to WiFi
    WiFi.begin("ssid", "password"); // NOTE: Dummy credentials, PlatformIO will use its own env
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        if (++retries > 20) {
            Serial.println("Failed to connect to WiFi. Tests will fail.");
            // We can't proceed, but Unity needs to run to show failure
            break;
        }
    }
    Serial.println("\nWiFi connected.");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    // Start broker
    broker.begin();

    UNITY_BEGIN();
    RUN_TEST(test_lwt_is_published_on_unclean_disconnect);
    RUN_TEST(test_retain_message_is_delivered_to_new_subscriber);
    RUN_TEST(test_retain_message_is_deleted);
    UNITY_END();
}

void loop() {
    // Keep clients connected
    // This is not strictly needed for these tests but good practice
}
