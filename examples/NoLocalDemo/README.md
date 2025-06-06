# MQTT NoLocal Demo for ESPAsyncMQTTBroker

This example demonstrates the `noLocal` functionality (an MQTT 5.0 feature) as supported by the `ESPAsyncMQTTBroker`.

## Behavior

The demo sets up:
1.  An `ESPAsyncMQTTBroker` instance on your ESP32.
2.  Two MQTT clients:
    *   `ClientWithNoLocal` (`client1`): This client publishes messages to `noLocal/testTopic` and also subscribes to this topic. The intention is to simulate a scenario where this client would use the `noLocal=true` subscription option if it were a full MQTT 5.0 client.
    *   `NormalSubscriberClient` (`client2`): This client subscribes to `noLocal/testTopic` normally.

**Expected Outcome (with a true MQTT 5.0 client for `client1`):**

*   When `client1` publishes a message:
    *   `client1` **should NOT** receive its own message because its subscription would have `noLocal=true`.
    *   `client2` **SHOULD** receive the message.

**Behavior with this Demo (using `PubSubClient`):**

The `PubSubClient` library is primarily for MQTT 3.1.1 and does not support setting MQTT 5.0 subscription options like `noLocal`. Therefore, `client1` in this demo cannot actually tell the broker "don't send my own messages back to me on this subscription."

However, the `ESPAsyncMQTTBroker` has the necessary internal mechanisms:
1.  The `Subscription` struct in the broker *can* store a `noLocal` flag.
2.  When the broker processes an incoming PUBLISH from a client (e.g., `client1`), it calls its internal publish distribution logic with an `excludeClientId` parameter set to `client1`'s ID.
3.  If `client1` had successfully made a subscription with `noLocal=true` (using an MQTT 5.0 capable client), the broker's logic would then check this flag for `client1`'s matching subscription and would not send the message back to `client1`.

**In this specific demo:**
*   `client1` will likely still receive its own messages because its subscription, made via `PubSubClient`, does not carry the `noLocal=true` flag to the broker.
*   `client2` will receive the messages.

The serial output and broker logs (if `DEBUG_DEBUG` is enabled) will show the broker's actions. The broker's `excludeClientId` logic is in place; the limitation is with the client library used in this example for demonstrating the full MQTT 5.0 feature. To see the true `noLocal` effect, an MQTT 5.0 client (e.g., MQTTX, Node-RED with MQTT 5.0 config, or an ESP32 MQTT 5.0 client library) would be needed to connect to the broker and set the `noLocal` subscription option.

## Setup

1.  Replace `"YOUR_WIFI_SSID"` and `"YOUR_WIFI_PASSWORD"` in `main.cpp` with your WiFi credentials.
2.  Ensure you have the `PubSubClient` library installed (e.g., via PlatformIO's library manager: `platformio lib install "knolleary/PubSubClient"`).
3.  Upload the code to your ESP32.
4.  Monitor the serial output.
