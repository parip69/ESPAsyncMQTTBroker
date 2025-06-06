# MQTT Retained Messages Demo for ESPAsyncMQTTBroker

This example demonstrates the lifecycle of retained messages using the `ESPAsyncMQTTBroker`.

## Behavior

The demo illustrates the following aspects of retained messages:
1.  **Publishing a Retained Message:** A client publishes a message with the `retain` flag set to `true`. The broker stores this message.
2.  **Delivery to New Subscribers:** Another client connects *after* the retained message was published and subscribes to the topic. It should immediately receive the stored retained message.
3.  **Updating a Retained Message:** The first client publishes a new message to the same topic, also with `retain=true`. This new message replaces the previously stored one. Existing subscribers (like the second client) receive this update.
4.  **Clearing a Retained Message:** The first client publishes an empty message (zero-length payload) to the topic with `retain=true`. This instructs the broker to delete the retained message for that topic. Future subscribers will not receive an initial message for this topic.

**Demo Flow:**

The `main.cpp` code proceeds in phases:
1.  **INIT:** Broker and `client1` (publisher) start.
2.  **PUBLISH_RETAIN_ON:** `client1` publishes "ON" to `home/lights/kitchen` with `retain=true`.
3.  **SUBSCRIBE_CLIENT2:** `client2` (new subscriber) connects and subscribes to `home/lights/kitchen`. It should immediately receive "ON".
4.  **PUBLISH_RETAIN_OFF:** `client1` publishes "OFF" to `home/lights/kitchen` with `retain=true`. `client2` should receive this "OFF" message.
5.  **CLEAR_RETAIN:** `client1` publishes an empty payload to `home/lights/kitchen` with `retain=true`. This clears the retained message.
6.  **DONE:** The demo concludes.

## Setup

1.  Replace `"YOUR_WIFI_SSID"` and `"YOUR_WIFI_PASSWORD"` in `main.cpp` with your WiFi credentials.
2.  Ensure you have the `PubSubClient` library installed (e.g., `platformio lib install "knolleary/PubSubClient"`).
3.  Upload the code to your ESP32.
4.  Monitor the serial output. The broker logs (especially if `setDebugLevel` is set to `DEBUG_INFO` or `DEBUG_DEBUG` in the example) will also show details about storing and clearing retained messages.

This demo helps in understanding how the `ESPAsyncMQTTBroker` handles the MQTT retained message feature according to the standard.
