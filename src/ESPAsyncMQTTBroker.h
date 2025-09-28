// @version: 1.9.42
#ifndef ESP_ASYNC_MQTT_BROKER_H
#define ESP_ASYNC_MQTT_BROKER_H

#include <Arduino.h>
#include <AsyncTCP.h>
#include <vector>
#include <map>
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
#define MQTT_PROTOCOL_LEVEL 4   // MQTT 3.1.1
#define MQTT_PROTOCOL_LEVEL_5 5 // MQTT 5.0
#define MQTT_MAX_PACKET_SIZE 1024
#define MQTT_MAX_TOPIC_SIZE 256   // Maximale Größe für Topic
#define MQTT_MAX_PAYLOAD_SIZE 768 // Maximale Größe für Payload

// Eigene Implementation von std::make_unique (ab C++14 Standard)
#if __cplusplus < 201402L
namespace std
{
    template <typename T, typename... Args>
    std::unique_ptr<T> make_unique(Args &&...args)
    {
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
    DEBUG_NONE = 0,    ///< Keine Debug-Ausgaben
    DEBUG_ERROR = 1,   ///< Nur Fehler werden angezeigt
    DEBUG_WARNING = 2, ///< Warnungen und Fehler werden angezeigt
    DEBUG_INFO = 3,    ///< Warnungen, Fehler und Informationen werden angezeigt
    DEBUG_DEBUG = 4    ///< Alle Details werden angezeigt (inklusive Debug-Informationen)
};

// Logger-Funktion, die verschiedene Log-Levels unterstützt
#define MQTT_LOG(level, format, ...) logMessage(level, format, ##__VA_ARGS__)

/**
 *  Repräsentiert ein MQTT-Abonnement für einen Client
 */
struct Subscription
{
    String filter; ///< Topic-Filter, mit dem eingehende Nachrichten verglichen werden
    bool noLocal;  ///< MQTT 5.0 noLocal-Flag: Bei true erhält der Client keine selbst veröffentlichten Nachrichten
    // evtl. später noch weitere Flags (retainAsPublished, retainHandling…)
};

/**
 * Repräsentiert einen verbundenen MQTT-Client
 */
struct MQTTClient
{
    AsyncClient *client;
    String clientId;
    bool connected;
    uint32_t lastActivity;
    uint16_t keepAlive;
    bool cleanSession;
    std::vector<Subscription> subscriptions;
    uint8_t protocolVersion;
    bool hasWill;
    bool gracefulDisconnect;
    String willTopic;
    String willMessage;
    uint8_t willQos;
    bool willRetain;
    std::unique_ptr<uint8_t[]> willPayload;
    size_t willPayloadLen;

    // For QoS 1/2 messages sent *to* this client
    std::map<uint16_t, struct OutgoingQoSMessage> outgoingMessages;

    // KeepAlive tracking
    bool kaSeen;
};

/**
 * State of an outgoing QoS message
 */
enum class OutgoingQoSState
{
    AwaitingPuback, // For QoS 1
    AwaitingPubrec, // For QoS 2
    AwaitingPubcomp // For QoS 2
};

/**
 * Represents a QoS 1 or 2 message being sent to a subscriber
 */
struct OutgoingQoSMessage
{
    uint8_t qos;
    bool retain;
    String topic;
    std::unique_ptr<uint8_t[]> payload;
    size_t payloadLen;
    uint32_t sentTime;
    uint8_t retryCount;
    OutgoingQoSState state;
    uint16_t packetId;

    OutgoingQoSMessage() : qos(0), retain(false), payloadLen(0), sentTime(0), retryCount(0), state(OutgoingQoSState::AwaitingPuback), packetId(0) {}
};

/**
 * Datenstruktur für gespeicherte (retained) Nachrichten
 */
struct RetainedMessage
{
    String topic;
    std::unique_ptr<uint8_t[]> payload;
    size_t length;
    uint8_t qos;

    RetainedMessage(const String &t, const uint8_t *p, size_t len, uint8_t q)
        : topic(t), length(len), qos(q)
    {
        if (len > 0 && p != nullptr)
        {
            payload.reset(new uint8_t[len]);
            if (len <= MQTT_MAX_PAYLOAD_SIZE)
            {
                memcpy(payload.get(), p, len);
            }
            else
            {
                memcpy(payload.get(), p, MQTT_MAX_PAYLOAD_SIZE);
                length = MQTT_MAX_PAYLOAD_SIZE;
            }
        }
    }
};

/**
 * Konfigurationsstruktur für den MQTT-Broker
 */
struct ESPAsyncMQTTBrokerConfig
{
    String username = "";
    String password = "";
    bool ignoreLoopDeliver = false;
    bool log = true;
};

struct IncomingQoS2Message
{
    String topic;
    std::unique_ptr<uint8_t[]> payload;
    size_t length;
    size_t payload_len;
    bool retained;
    String senderClientId;
    String originalClientId;

    IncomingQoS2Message() : length(0), payload_len(0), retained(false) {}

    IncomingQoS2Message(const String &t, const uint8_t *p, size_t len, bool ret, const String &clientId)
        : topic(t), length(len), payload_len(len), retained(ret), senderClientId(clientId), originalClientId(clientId)
    {
        if (len > 0 && p != nullptr)
        {
            payload.reset(new uint8_t[len]);
            if (len <= MQTT_MAX_PAYLOAD_SIZE)
            {
                memcpy(payload.get(), p, len);
            }
            else
            {
                memcpy(payload.get(), p, MQTT_MAX_PAYLOAD_SIZE);
                length = MQTT_MAX_PAYLOAD_SIZE;
                payload_len = MQTT_MAX_PAYLOAD_SIZE;
            }
        }
    }
};

typedef std::function<void(String clientId, String clientIp, String username, int passwordLen)> ClientCallback;
typedef std::function<void(String clientId, String topic, String message)> MessageCallback;
typedef std::function<void(String clientId)> ClientDisconnectCallback;
typedef std::function<void(String clientId, int errorCode, const String &errorMessage)> ErrorCallback;
typedef std::function<void(String clientId, const String &topic)> SubscribeCallback;
typedef std::function<void(String clientId, const String &topic)> UnsubscribeCallback;
typedef std::function<void(DebugLevel level, const String &message)> LoggingCallback;

class ESPAsyncMQTTBroker
{
public:
    ESPAsyncMQTTBroker(uint16_t port = 1883);
    ~ESPAsyncMQTTBroker();
    void begin();
    void stop();
    bool publish(const char *topic, const char *payload, bool retained = false, uint8_t qos = 0);
    bool publish(const char *topic, const char *payload, bool retained, uint8_t qos, const String &excludeClientId);
    bool publish(const char *topic, uint8_t qos, bool retained, const char *payload);
    void setConfig(const ESPAsyncMQTTBrokerConfig &config);
    void setDebugLevel(DebugLevel level) { debugLevel = level; }
    void setLoggingCallback(LoggingCallback callback) { loggingCallback = callback; }
    void onClientConnect(ClientCallback callback) { clientConnectCallback = callback; }
    void onMessage(MessageCallback callback) { messageCallback = callback; }
    void onClientDisconnect(ClientDisconnectCallback callback) { clientDisconnectCallback = callback; }
    void onError(ErrorCallback callback) { errorCallback = callback; }
    void onSubscribe(SubscribeCallback callback) { subscribeCallback = callback; }
    void onUnsubscribe(UnsubscribeCallback callback) { unsubscribeCallback = callback; }
    std::map<String, String> getConnectedClientsInfo() const { return connectedClientsInfo; }
    bool setPort(uint16_t newPort);

private:
    uint16_t port;
    std::unique_ptr<AsyncServer> server;
    std::map<AsyncClient *, std::unique_ptr<MQTTClient>> clients;
    std::map<String, std::unique_ptr<RetainedMessage>> retainedMessages;
    std::map<String, std::unique_ptr<MQTTClient>> persistentSessions;
    std::map<uint16_t, IncomingQoS2Message> incomingQoS2Messages;
    ESPAsyncMQTTBrokerConfig brokerConfig;
    DebugLevel debugLevel = DEBUG_INFO;
    esp_timer_handle_t timeoutTimer = nullptr;
    std::map<String, String> connectedClientsInfo;
    uint16_t nextPacketId = 1;

    uint16_t getNextPacketId();

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
    void handlePuback(MQTTClient *client, uint8_t *data, size_t len);
    void handlePubRec(MQTTClient *client, uint8_t *data, size_t len);
    void handlePubRel(MQTTClient *client, uint8_t *data, size_t len);
    void handlePubComp(MQTTClient *client, uint8_t *data, size_t len);
    void processPacket(MQTTClient *client, uint8_t *data, size_t len);
    bool topicMatches(const Subscription &subscription, const String &topic);
    bool topicMatches(const String &subscription, const String &topic);
    void sendRetainedMessages(MQTTClient *client);
    bool authenticateClient(const String &username, const String &password);
    void onClient(AsyncClient *client);
    void checkTimeouts();
    void logMessage(DebugLevel level, const char *format, ...);
    bool isValidPublishTopic(const String &topic);
    bool isValidTopicFilter(const String &filter);
    bool publish(const char *topic, const uint8_t *payload, size_t payloadLen, bool retained, uint8_t qos, const String &excludeClientId);
};

#endif // ESP_ASYNC_MQTT_BROKER_H
