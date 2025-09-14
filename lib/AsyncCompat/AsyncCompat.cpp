#include "AsyncCompat.h"
#include <AsyncMqttClient.h> // For types like AsyncMqttClientDisconnectReason

// --- Private Methods ---
void AsyncMQTTCompatClient::_handleBrokerMessage(const String& topic, const String& payload) {
    if (_onMessageCallback) {
        // We need to convert from String to char* to match the target API.
        // This is a bit tricky with memory management. We'll use temporary buffers.
        char topicBuf[topic.length() + 1];
        strcpy(topicBuf, topic.c_str());

        char payloadBuf[payload.length() + 1];
        strcpy(payloadBuf, payload.c_str());

        // Create dummy properties
        AsyncMqttClientMessageProperties props;
        props.qos = 0; // We don't have this info from the internal publish, a limitation.
        props.retain = false; // Same limitation.
        props.dup = false;

        _onMessageCallback(topicBuf, payloadBuf, props, payload.length(), 0, payload.length());
    }
}

uint16_t AsyncMQTTCompatClient::_getNextPacketId() {
    static uint16_t lastPacketId = 0;
    if (++lastPacketId == 0) lastPacketId = 1;
    return lastPacketId;
}


// --- Public API ---

AsyncMQTTCompatClient::AsyncMQTTCompatClient(ESPAsyncMQTTBroker& broker) : _broker(broker) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    sprintf(mac_str, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    _clientId = "esp-internal-" + String(mac_str);
}

AsyncMQTTCompatClient::~AsyncMQTTCompatClient() {
    if (connected()) {
        disconnect();
    }
}

// --- Configuration ---
AsyncMQTTCompatClient& AsyncMQTTCompatClient::setServer(const char* host, uint16_t port) {
    // Ignored
    return *this;
}

AsyncMQTTCompatClient& AsyncMQTTCompatClient::setCredentials(const char* username, const char* password) {
    _username = username;
    _password = password;
    return *this;
}

AsyncMQTTCompatClient& AsyncMQTTCompatClient::setClientId(const char* clientId) {
    _clientId = clientId;
    return *this;
}

AsyncMQTTCompatClient& AsyncMQTTCompatClient::setKeepAlive(uint16_t keepAlive) {
    _keepAlive = keepAlive;
    return *this;
}

AsyncMQTTCompatClient& AsyncMQTTCompatClient::setCleanSession(bool cleanSession) {
    _cleanSession = cleanSession;
    return *this;
}

AsyncMQTTCompatClient& AsyncMQTTCompatClient::setWill(const char* topic, uint8_t qos, bool retain, const char* payload, size_t length) {
    // The internal client connection doesn't support LWT yet. This is a potential improvement.
    _willTopic = topic;
    _willQos = qos;
    _willRetain = retain;
    if (payload && length > 0) {
        _willPayload = String(payload, length);
    } else {
        _willPayload = "";
    }
    return *this;
}

// --- Callbacks ---
AsyncMQTTCompatClient& AsyncMQTTCompatClient::onConnect(AsyncMqttClientOnConnectCallback callback) {
    _onConnectCallback = callback;
    return *this;
}

AsyncMQTTCompatClient& AsyncMQTTCompatClient::onDisconnect(AsyncMqttClientOnDisconnectCallback callback) {
    _onDisconnectCallback = callback;
    return *this;
}

AsyncMQTTCompatClient& AsyncMQTTCompatClient::onSubscribe(AsyncMqttClientOnSubscribeCallback callback) {
    _onSubscribeCallback = callback;
    return *this;
}

AsyncMQTTCompatClient& AsyncMQTTCompatClient::onUnsubscribe(AsyncMqttClientOnUnsubscribeCallback callback) {
    _onUnsubscribeCallback = callback;
    return *this;
}

AsyncMQTTCompatClient& AsyncMQTTCompatClient::onMessage(AsyncMqttClientOnMessageCallback callback) {
    _onMessageCallback = callback;
    return *this;
}

AsyncMQTTCompatClient& AsyncMQTTCompatClient::onPublish(AsyncMqttClientOnPublishCallback callback) {
    _onPublishCallback = callback;
    return *this;
}

// --- Actions ---
void AsyncMQTTCompatClient::connect() {
    if (connected()) return;

    auto messageCallback = [this](const String& topic, const String& payload) {
        this->_handleBrokerMessage(topic, payload);
    };

    bool success = _broker._internalConnect(_clientId, _username, _password, _cleanSession, _keepAlive, messageCallback);

    if (success) {
        _connected = true;
        if (_onConnectCallback) {
            _onConnectCallback(true); // Assume no session was present
        }
    }
}

void AsyncMQTTCompatClient::disconnect(bool force) {
    if (!connected()) return;

    _broker._internalDisconnect(_clientId);
    _connected = false;

    if (_onDisconnectCallback) {
        _onDisconnectCallback(AsyncMqttClientDisconnectReason::USER_REQUEST);
    }
}

bool AsyncMQTTCompatClient::connected() const {
    return _connected;
}

uint16_t AsyncMQTTCompatClient::publish(const char* topic, uint8_t qos, bool retain, const char* payload, size_t length) {
    if (!connected()) return 0;

    _broker.publish(topic, (const uint8_t*)payload, length, retain, qos, _clientId);

    uint16_t packetId = _getNextPacketId();
    if (_onPublishCallback) {
        _onPublishCallback(packetId);
    }
    return packetId;
}

uint16_t AsyncMQTTCompatClient::subscribe(const char* topic, uint8_t qos) {
    if (!connected()) return 0;

    bool success = _broker._internalSubscribe(_clientId, topic, qos);
    if (success) {
        uint16_t packetId = _getNextPacketId();
        if (_onSubscribeCallback) {
            _onSubscribeCallback(packetId, qos);
        }
        return packetId;
    }
    return 0;
}

uint16_t AsyncMQTTCompatClient::unsubscribe(const char* topic) {
    if (!connected()) return 0;

    bool success = _broker._internalUnsubscribe(_clientId, topic);
    if (success) {
        uint16_t packetId = _getNextPacketId();
        if (_onUnsubscribeCallback) {
            _onUnsubscribeCallback(packetId);
        }
        return packetId;
    }
    return 0;
}
