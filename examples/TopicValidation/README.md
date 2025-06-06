# MQTT Topic Validation Demo for ESPAsyncMQTTBroker

This example demonstrates how the `ESPAsyncMQTTBroker` validates topic names for PUBLISH messages and topic filters for SUBSCRIBE messages.

## Behavior

The demo sets up:
1.  An `ESPAsyncMQTTBroker` instance on your ESP32 with `DEBUG_DEBUG` logging enabled to see detailed broker actions.
2.  One MQTT client (`TopicValidationTester`) that attempts various valid and invalid publish and subscribe operations.

**Key things to observe:**

*   **Broker Logs:** The broker's serial output (when `DEBUG_DEBUG` is set) will show:
    *   Acceptance of valid operations.
    *   Rejection messages for invalid topic names in PUBLISH packets (e.g., containing wildcards like `#` or `+`, or being empty). The broker might close the connection for such protocol violations.
    *   Rejection messages for invalid topic filters in SUBSCRIBE packets (e.g., misuse of wildcards like `#` not at the end of a multi-level filter, or `+` not as a standalone level).
*   **Client Behavior:**
    *   The `PubSubClient` library might prevent some invalid operations locally (e.g., publishing to an empty topic string).
    *   For invalid PUBLISH messages sent to the broker, the broker might disconnect the client. The example attempts to reconnect.
    *   For invalid SUBSCRIBE filters, the client should receive a `SUBACK` packet from the broker indicating failure for those specific filters (e.g., return code `0x80` for MQTT 3.1.1, or more specific codes like `0x8F` for MQTT 5.0 if the client indicated MQTT 5.0). `PubSubClient` itself doesn't expose these SUBACK return codes directly in a simple way, but the broker logs will show the validation.

**Test Cases Covered:**

*   Publishing to a valid topic.
*   Attempting to publish to topics containing wildcards (`#`, `+`).
*   Attempting to publish to an empty topic string.
*   Subscribing with a valid topic filter (e.g., `valid/topic/#`).
*   Attempting to subscribe with invalid topic filters (e.g., `invalid/filter#/test`, `invalid/filter+/element`, empty filter string).

## Setup

1.  Replace `"YOUR_WIFI_SSID"` and `"YOUR_WIFI_PASSWORD"` in `main.cpp` with your WiFi credentials.
2.  Ensure you have the `PubSubClient` library installed (e.g., `platformio lib install "knolleary/PubSubClient"`).
3.  Upload the code to your ESP32.
4.  Monitor the serial output from both the client (`TopicValidationTester`) and the MQTT Broker. The broker's logs are crucial for seeing the validation actions.

This demo helps verify that the broker correctly enforces MQTT topic rules, enhancing stability and standard conformance.
