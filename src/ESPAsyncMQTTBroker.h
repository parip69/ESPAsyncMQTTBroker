// @version: 1.5.1
#ifndef ESP_ASYNC_MQTT_BROKER_H
#define ESP_ASYNC_MQTT_BROKER_H

#include <Arduino.h>
#include <AsyncTCP.h>
#include <vector>
#include <map>
#include <vector>
#include <memory>
#include <functional>
#include "esp_timer.h"

#define MQTT_CONNECT 1
#define MQTT_CONNACK 2
#define MQTT_PUBLISH 3
#define MQTT_PUBACK 4
#define MQTT_PUBREC 5
#define MQTT_PUBREL 6
#define MQTT_PUBCOMP 7
#define MQTT_SUBSCRIBE 8
#define MQTT_SUBACK 9
#define MQTT_UNSUBSCRIBE 10
#define MQTT_UNSUBACK 11
#define MQTT_PINGREQ 12
#define MQTT_PINGRESP 13
#define MQTT_DISCONNECT 14

// QoS Level
#define MQTT_QOS0 0
#define MQTT_QOS1 1
#define MQTT_QOS2 2

// Andere Konstanten
#define MQTT_PROTOCOL_LEVEL 4 // MQTT 3.1.1
#define MQTT_PROTOCOL_LEVEL_5 5 // MQTT 5.0
#define MQTT_MAX_PACKET_SIZE 1024
#define MQTT_MAX_TOPIC_SIZE 128     // Maximale Größe für Topic
#define MQTT_MAX_PAYLOAD_SIZE 512   // Maximale Größe für Payload
#define MQTT_MAX_CLIENT_ID_SIZE 64  // Maximale Größe für Client ID
#define MQTT_MAX_USERNAME_SIZE 32   // Maximale Größe für Username
#define MQTT_MAX_PASSWORD_SIZE 32   // Maximale Größe für Password

// Eigene Implementation von std::make_unique (ab C++14 Standard)
#if __cplusplus < 201402L
namespace std {
    template<typename T, typename... Args>
    std::unique_ptr<T> make_unique(Args&&... args) {
        return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
    }
}
#endif

/**
 * Debug-Level für Logging
 *  DEBUG_NONE = 0,     ///< Keine Debug-Ausgaben
 *  DEBUG_ERROR = 1,    ///< Nur Fehler werden angezeigt
 *  DEBUG_WARNING = 2,  ///< Warnungen und Fehler werden angezeigt
 *  DEBUG_INFO = 3,     ///< Warnungen, Fehler und Informationen werden angezeigt
 *  DEBUG_DEBUG = 4     ///< Alle Details werden angezeigt (inklusive Debug-Informationen)
 */
enum DebugLevel
{
    DEBUG_NONE = 0,     ///< Keine Debug-Ausgaben
    DEBUG_ERROR = 1,    ///< Nur Fehler werden angezeigt
    DEBUG_WARNING = 2,  ///< Warnungen und Fehler werden angezeigt
    DEBUG_INFO = 3,     ///< Warnungen, Fehler und Informationen werden angezeigt
    DEBUG_DEBUG = 4     ///< Alle Details werden angezeigt (inklusive Debug-Informationen)
};

// Logger-Funktion, die verschiedene Log-Levels unterstützt
#define MQTT_LOG(level, format, ...) logMessage(level, format, ##__VA_ARGS__)

/**
 *  Repräsentiert ein MQTT-Abonnement für einen Client
 */
struct Subscription
{
    char filter[MQTT_MAX_TOPIC_SIZE + 1];    ///< Topic-Filter, mit dem eingehende Nachrichten verglichen werden
    bool noLocal;     ///< MQTT 5.0 noLocal-Flag: Bei true erhält der Client keine selbst veröffentlichten Nachrichten
};

/**
 * Repräsentiert einen verbundenen MQTT-Client
 */
struct MQTTClient
{
    AsyncClient *client;
    char clientId[MQTT_MAX_CLIENT_ID_SIZE + 1];
    bool connected;
    uint32_t lastActivity;
    uint16_t keepAlive;
    bool cleanSession;
    std::vector<Subscription> subscriptions;
    uint8_t protocolVersion;
    bool hasWill;
    bool gracefulDisconnect;
    char willTopic[MQTT_MAX_TOPIC_SIZE + 1];
    uint8_t willQos;
    bool willRetain;
    std::unique_ptr<uint8_t[]> willPayload;
    size_t willPayloadLen;
};

/**
 * Datenstruktur für gespeicherte (retained) Nachrichten
 */
struct RetainedMessage
{
    char topic[MQTT_MAX_TOPIC_SIZE + 1];
    std::unique_ptr<uint8_t[]> payload;
    size_t length;
    uint8_t qos;

    RetainedMessage(const char* t, const uint8_t* p, size_t len, uint8_t q)
        : length(len), qos(q) {
        strncpy(topic, t, MQTT_MAX_TOPIC_SIZE);
        topic[MQTT_MAX_TOPIC_SIZE] = '\0';
        if (len > 0 && p != nullptr) {
            size_t lenToCopy = (len > MQTT_MAX_PAYLOAD_SIZE) ? MQTT_MAX_PAYLOAD_SIZE : len;
            payload.reset(new uint8_t[lenToCopy]);
            memcpy(payload.get(), p, lenToCopy);
            length = lenToCopy;
        }
    }
};

/**
 * Konfigurationsstruktur für den MQTT-Broker
 */
struct ESPAsyncMQTTBrokerConfig
{
    char username[MQTT_MAX_USERNAME_SIZE + 1] = {0};
    char password[MQTT_MAX_PASSWORD_SIZE + 1] = {0};
    bool ignoreLoopDeliver = false;
    bool log = true;
};

struct IncomingQoS2Message
{
    char topic[MQTT_MAX_TOPIC_SIZE + 1];
    std::unique_ptr<uint8_t[]> payload;
    size_t length;
    size_t payload_len;
    bool retained;
    char senderClientId[MQTT_MAX_CLIENT_ID_SIZE + 1];
    char originalClientId[MQTT_MAX_CLIENT_ID_SIZE + 1];

    IncomingQoS2Message() : length(0), payload_len(0), retained(false) {
        topic[0] = '\0';
        senderClientId[0] = '\0';
        originalClientId[0] = '\0';
    }

    IncomingQoS2Message(const char* t, const uint8_t* p, size_t len, bool ret, const char* clientId)
        : length(len), payload_len(len), retained(ret) {
        strncpy(topic, t, MQTT_MAX_TOPIC_SIZE);
        topic[MQTT_MAX_TOPIC_SIZE] = '\0';
        strncpy(senderClientId, clientId, MQTT_MAX_CLIENT_ID_SIZE);
        senderClientId[MQTT_MAX_CLIENT_ID_SIZE] = '\0';
        strncpy(originalClientId, clientId, MQTT_MAX_CLIENT_ID_SIZE);
        originalClientId[MQTT_MAX_CLIENT_ID_SIZE] = '\0';

        if (len > 0 && p != nullptr) {
            size_t lenToCopy = (len > MQTT_MAX_PAYLOAD_SIZE) ? MQTT_MAX_PAYLOAD_SIZE : len;
            payload.reset(new uint8_t[lenToCopy]);
            memcpy(payload.get(), p, lenToCopy);
            length = payload_len = lenToCopy;
        }
    }
};

struct ClientInfo {
    char clientId[MQTT_MAX_CLIENT_ID_SIZE + 1];
    char ip[16]; // "255.255.255.255"
};

typedef std::function<void(const char* clientId, const char* clientIp)> ClientCallback;
typedef std::function<void(const char* clientId, const char* topic, const uint8_t* payload, size_t len)> MessageCallback;
typedef std::function<void(const char* clientId)> ClientDisconnectCallback;
typedef std::function<void(const char* clientId, int errorCode, const char* errorMessage)> ErrorCallback;
typedef std::function<void(const char* clientId, const char* topic)> SubscribeCallback;
typedef std::function<void(const char* clientId, const char* topic)> UnsubscribeCallback;
typedef std::function<void(DebugLevel level, const char* message)> LoggingCallback;

class ESPAsyncMQTTBroker
{
public:
    ESPAsyncMQTTBroker(uint16_t port = 1883);
    ~ESPAsyncMQTTBroker();
    void begin();
    void stop();
    bool publish(const char* topic, const uint8_t* payload, size_t len, bool retained = false, uint8_t qos = 0, const char* excludeClientId = nullptr);
    void setConfig(const ESPAsyncMQTTBrokerConfig &config);
    void setDebugLevel(DebugLevel level) { debugLevel = level; }
    void setLoggingCallback(LoggingCallback callback) { loggingCallback = callback; }
    void onClientConnect(ClientCallback callback) { clientConnectCallback = callback; }
    void onMessage(MessageCallback callback) { messageCallback = callback; }
    void onClientDisconnect(ClientDisconnectCallback callback) { clientDisconnectCallback = callback; }
    void onError(ErrorCallback callback) { errorCallback = callback; }
    void onSubscribe(SubscribeCallback callback) { subscribeCallback = callback; }
    void onUnsubscribe(UnsubscribeCallback callback) { unsubscribeCallback = callback; }
    const std::vector<ClientInfo>& getConnectedClientsInfo() const { return connectedClientsInfo; }

private:
    uint8_t _packet_buffer[MQTT_MAX_PACKET_SIZE];
    uint16_t port;
    std::unique_ptr<AsyncServer> server;
    std::map<AsyncClient *, std::unique_ptr<MQTTClient>> clients;
    std::vector<std::unique_ptr<RetainedMessage>> retainedMessages;
    std::vector<std::unique_ptr<MQTTClient>> persistentSessions;
    std::map<uint16_t, IncomingQoS2Message> incomingQoS2Messages;
    ESPAsyncMQTTBrokerConfig brokerConfig;
    DebugLevel debugLevel = DEBUG_INFO;
    esp_timer_handle_t timeoutTimer = nullptr;
    std::vector<ClientInfo> connectedClientsInfo;
    ClientCallback clientConnectCallback = nullptr;
    ClientDisconnectCallback clientDisconnectCallback = nullptr;
    MessageCallback messageCallback = nullptr;
    ErrorCallback errorCallback = nullptr;
    SubscribeCallback subscribeCallback = nullptr;
    UnsubscribeCallback unsubscribeCallback = nullptr;
    LoggingCallback loggingCallback = nullptr;

    void handleConnect(MQTTClient *client, uint8_t *data, size_t len);
    void handlePublish(MQTTClient *client, uint8_t *data, size_t len, uint8_t header);
    void handleSubscribe(MQTTClient *client, uint8_t *data, size_t len);
    void handleUnsubscribe(MQTTClient *client, uint8_t *data, size_t len);
    void handlePingReq(MQTTClient *client);
    void handleDisconnect(MQTTClient *client);
    void handlePubRec(MQTTClient *client, uint8_t *data, size_t len);
    void handlePubRel(MQTTClient *client, uint8_t *data, size_t len);
    void handlePubComp(MQTTClient *client, uint8_t *data, size_t len);
    void processPacket(MQTTClient *client, uint8_t *data, size_t len);
    bool topicMatches(const char* filter, const char* topic);
    void sendRetainedMessages(MQTTClient *client);
    bool authenticateClient(const char* username, const char* password);
    void onClient(AsyncClient *client);
    void checkTimeouts();
    void logMessage(DebugLevel level, const char* format, ...);
    bool isValidPublishTopic(const char* topic);
    bool isValidTopicFilter(const char* filter);
};

#endif // ESP_ASYNC_MQTT_BROKER_H
