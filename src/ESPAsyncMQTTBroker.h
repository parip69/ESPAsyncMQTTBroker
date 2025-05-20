// @version: 1.4.150 Builddatum 18:53:45 02-04.2025
#ifndef ESP_ASYNC_MQTT_BROKER_H
#define ESP_ASYNC_MQTT_BROKER_H

#include <Arduino.h>
#include <AsyncTCP.h>
#include <vector>
#include <map>
#include <functional>
#include "esp_timer.h" // Für asynchrone Timer

// MQTT Pakettypen
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

// Debug Level
enum DebugLevel
{
    DEBUG_NONE = 0,
    DEBUG_ERROR = 1,
    DEBUG_INFO = 2,
    DEBUG_DEBUG = 3
};

// Neue Struktur für ein Abonnement
struct Subscription
{
    String filter;
    bool noLocal; // MQTT 5.0 noLocal-Flag
    // evtl. später noch weitere Flags (retainAsPublished, retainHandling…)
};

// Client-Struktur
struct MQTTClient
{
    AsyncClient *client;
    String clientId;
    bool connected;
    uint32_t lastActivity;
    uint16_t keepAlive; // Keep-Alive Intervall in Sekunden
    bool cleanSession;
    std::vector<Subscription> subscriptions;
    uint8_t protocolVersion; // MQTT Protokoll-Version: 4 = MQTT 3.1.1, 5 = MQTT 5.0
};

// Datenstruktur für gespeicherte Nachrichten (retained)
struct RetainedMessage
{
    String topic;
    uint8_t *payload;
    size_t length;
    uint8_t qos;
};

// Konfigurationsstruktur für den MQTT-Broker
struct ESPAsyncMQTTBrokerConfig
{
    String username = "";
    String password = "";
    bool ignoreLoopDeliver = false; // broker-spezifisch für MQTT 3.1.1
};

typedef std::function<void(String clientId, String clientIp)> ClientCallback;
typedef std::function<void(String clientId, String topic, String message)> MessageCallback;
typedef std::function<void(String clientId)> ClientDisconnectCallback;
typedef std::function<void(String clientId, int errorCode, const String &errorMessage)> ErrorCallback;
typedef std::function<void(String clientId, const String &topic)> SubscribeCallback;
typedef std::function<void(String clientId, const String &topic)> UnsubscribeCallback;

class ESPAsyncMQTTBroker
{
public:
    ESPAsyncMQTTBroker(uint16_t port = 1883);
    ~ESPAsyncMQTTBroker();
    void begin();
    void stop();
    // AsyncMqttClient‐konforme Signatur: topic, qos, retain, payload
    bool publish(const char *topic, uint8_t qos, bool retained = false, const char *payload = "");

    // Erweiterte publish-Methode mit Client-ID für noLocal-Unterstützung
    bool publish(const char *topic, uint8_t qos, bool retained, const char *payload, const String &excludeClientId);

    // Konfiguration und Debug-Einstellungen
    void setConfig(const ESPAsyncMQTTBrokerConfig &config);
    void setDebugLevel(DebugLevel level) { debugLevel = level; }

    // Callback-Setter
    void onClientConnect(ClientCallback callback) { clientConnectCallback = callback; }
    void onMessage(MessageCallback callback) { messageCallback = callback; }
    void onClientDisconnect(ClientDisconnectCallback callback) { clientDisconnectCallback = callback; }
    void onError(ErrorCallback callback) { errorCallback = callback; }
    void onSubscribe(SubscribeCallback callback) { subscribeCallback = callback; }
    void onUnsubscribe(UnsubscribeCallback callback) { unsubscribeCallback = callback; }

    // Behalte nur die Client-Info-Methode, entferne die Message-History-Methoden
    std::map<String, String> getConnectedClientsInfo() const { return connectedClientsInfo; }

private:
    AsyncServer *server;
    uint16_t port;
    std::vector<MQTTClient *> clients;
    std::vector<RetainedMessage *> retainedMessages;
    std::map<String, MQTTClient *> persistentSessions;
    ESPAsyncMQTTBrokerConfig brokerConfig;

    // Behalte die Client-Info-Map, entferne die Message-History
    std::map<String, String> connectedClientsInfo; // Client-ID -> IP

    // Asynchroner Timer für Keep-Alive-Timeouts
    esp_timer_handle_t timeoutTimer = NULL;

    // Callbacks
    ClientCallback clientConnectCallback = nullptr;
    MessageCallback messageCallback = nullptr;
    ClientDisconnectCallback clientDisconnectCallback = nullptr;
    ErrorCallback errorCallback = nullptr;
    SubscribeCallback subscribeCallback = nullptr;
    UnsubscribeCallback unsubscribeCallback = nullptr;

    DebugLevel debugLevel = DEBUG_DEBUG;

    // MQTT Paket-Verarbeitung
    void handleConnect(MQTTClient *client, uint8_t *data, size_t len);
    void handlePublish(MQTTClient *client, uint8_t *data, size_t len, uint8_t header);
    void handleSubscribe(MQTTClient *client, uint8_t *data, size_t len);
    void handleUnsubscribe(MQTTClient *client, uint8_t *data, size_t len);
    void handlePingReq(MQTTClient *client);
    void handleDisconnect(MQTTClient *client);

    // QoS 2 Handler
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
};

#endif // ESP_ASYNC_MQTT_BROKER_H
