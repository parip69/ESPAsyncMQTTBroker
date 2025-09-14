#ifndef ASYNC_MQTT_COMPAT_H
#define ASYNC_MQTT_COMPAT_H

#include <Arduino.h>
#include "../../src/ESPAsyncMQTTBroker.h" // Use relative path to broker
#include <functional>

// Forward declaration to avoid circular dependency if ever needed
class ESPAsyncMQTTBroker;

// Callback type definitions to match AsyncMqttClient
using AsyncMqttClientOnConnectCallback = std::function<void(bool sessionPresent)>;
using AsyncMqttClientOnDisconnectCallback = std::function<void(AsyncMqttClientDisconnectReason reason)>;
using AsyncMqttClientOnSubscribeCallback = std::function<void(uint16_t packetId, uint8_t qos)>;
using AsyncMqttClientOnUnsubscribeCallback = std::function<void(uint16_t packetId)>;
using AsyncMqttClientOnMessageCallback = std::function<void(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total)>;
using AsyncMqttClientOnPublishCallback = std::function<void(uint16_t packetId)>;

class AsyncMQTTCompatClient {
public:
    AsyncMQTTCompatClient(ESPAsyncMQTTBroker& broker);
    ~AsyncMQTTCompatClient();

    // Configuration methods mirroring AsyncMqttClient
    AsyncMQTTCompatClient& setServer(const char* host, uint16_t port); // Note: host/port are ignored for local broker
    AsyncMQTTCompatClient& setCredentials(const char* username, const char* password);
    AsyncMQTTCompatClient& setClientId(const char* clientId);
    AsyncMQTTCompatClient& setKeepAlive(uint16_t keepAlive);
    AsyncMQTTCompatClient& setCleanSession(bool cleanSession);
    AsyncMQTTCompatClient& setWill(const char* topic, uint8_t qos, bool retain, const char* payload, size_t length);

    // Callback setters
    AsyncMQTTCompatClient& onConnect(AsyncMqttClientOnConnectCallback callback);
    AsyncMQTTCompatClient& onDisconnect(AsyncMqttClientOnDisconnectCallback callback);
    AsyncMQTTCompatClient& onSubscribe(AsyncMqttClientOnSubscribeCallback callback);
    AsyncMQTTCompatClient& onUnsubscribe(AsyncMqttClientOnUnsubscribeCallback callback);
    AsyncMQTTCompatClient& onMessage(AsyncMqttClientOnMessageCallback callback);
    AsyncMQTTCompatClient& onPublish(AsyncMqttClientOnPublishCallback callback);

    // Connection methods
    void connect();
    void disconnect(bool force = false);
    bool connected() const;

    // Publish/Subscribe methods
    uint16_t publish(const char* topic, uint8_t qos, bool retain, const char* payload = nullptr, size_t length = 0);
    uint16_t subscribe(const char* topic, uint8_t qos);
    uint16_t unsubscribe(const char* topic);

private:
    ESPAsyncMQTTBroker& _broker;
    String _clientId;
    String _username;
    String _password;
    bool _cleanSession = true;
    uint16_t _keepAlive = 15;

    // Will properties
    String _willTopic;
    String _willPayload;
    uint8_t _willQos = 0;
    bool _willRetain = false;

    bool _connected = false;

    // Callbacks
    AsyncMqttClientOnConnectCallback _onConnectCallback = nullptr;
    AsyncMqttClientOnDisconnectCallback _onDisconnectCallback = nullptr;
    AsyncMqttClientOnMessageCallback _onMessageCallback = nullptr;
    // ... other callbacks would be stored here

    // Internal handler methods to receive events from the broker
    void _onBrokerMessage(String clientId, String topic, String message);
    void _onBrokerConnect(String clientId, String ip);
    void _onBrokerDisconnect(String clientId);

    uint16_t _getNextPacketId();
};

#endif // ASYNC_MQTT_COMPAT_H
