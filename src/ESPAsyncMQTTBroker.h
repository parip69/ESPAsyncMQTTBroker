// ❤️ 📂 🎉 ❤️  🎉 Grosse Optimierung  🎉  ❤️ 📂 🎉❤️️️
// @ 2.0.224
#ifndef ESP_ASYNC_MQTT_BROKER_H
#define ESP_ASYNC_MQTT_BROKER_H

#include <Arduino.h>
#include <AsyncTCP.h>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <deque>
#include <array>
#include <mutex>
#include <atomic>
#include <new>
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
#ifndef MQTT_MAX_PACKET_SIZE
#define MQTT_MAX_PACKET_SIZE 4096 // Gesamtpaket inklusive Topic und Header; Speicher nach Bedarf.
#endif
#ifndef MQTT_MAX_TOPIC_SIZE
#define MQTT_MAX_TOPIC_SIZE 256   // Per Build konfigurierbare Topic-Grenze.
#endif
// Payload nutzt den vorhandenen Paketrahmen; Topic/Header zaehlen mit.
#define MQTT_MAX_PAYLOAD_SIZE MQTT_MAX_PACKET_SIZE

// Speichergrenzen sind konfigurierbar; angenommene QoS-Nachrichten nie verwerfen.
#ifndef MQTT_MAX_CLIENTS
#define MQTT_MAX_CLIENTS 16
#endif
#ifndef MQTT_MAX_SESSIONS
#define MQTT_MAX_SESSIONS 16
#endif
#ifndef MQTT_MAX_SUBSCRIPTIONS
#define MQTT_MAX_SUBSCRIPTIONS 64
#endif
#ifndef MQTT_MAX_QUEUED_MESSAGES
#define MQTT_MAX_QUEUED_MESSAGES 32
#endif
#ifndef MQTT_MAX_INFLIGHT_MESSAGES
#define MQTT_MAX_INFLIGHT_MESSAGES 16
#endif
#ifndef MQTT_MAX_STORED_BYTES
#define MQTT_MAX_STORED_BYTES 65536
#endif
#ifndef MQTT_MAX_RETAINED_MESSAGES
#define MQTT_MAX_RETAINED_MESSAGES 64
#endif
// Reserve fuer Empfang, CONNECT und ACKs: volle Queues muessen drainierbar bleiben.
#ifndef MQTT_RX_RESERVE_BYTES
#define MQTT_RX_RESERVE_BYTES (2U * MQTT_MAX_PACKET_SIZE)
#endif

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
    String filter;         ///< Topic-Filter, mit dem eingehende Nachrichten verglichen werden
    uint8_t qos = 0;       ///< Maximaler QoS dieser Subscription (0..2)
    bool noLocal = false;  ///< Reserviert; MQTT 3.1.1 setzt dieses Feld immer auf false
    // evtl. später noch weitere Flags (retainAsPublished, retainHandling…)
};

struct IncomingQoS2Message;

/**
 * Repräsentiert einen verbundenen MQTT-Client
 */
struct MQTTClient
{
    AsyncClient *client = nullptr;
    bool ownsTransport = false;
    String clientId;
    bool connected = false;
    bool closing = false;     ///< Nach Close keine weitere Paketverarbeitung
    bool connectSeen = false; ///< CONNECT darf pro TCP-Verbindung nur einmal auftreten
    uint32_t lastActivity = 0;
    uint16_t keepAlive = 0;
    bool cleanSession = true;
    std::vector<Subscription> subscriptions;
    std::vector<uint8_t> rxBuffer;
    bool processingRx = false; ///< Reentranten Empfang erst nach dem laufenden Paket bearbeiten.
    uint8_t protocolVersion = MQTT_PROTOCOL_LEVEL;
    bool hasWill = false;
    bool gracefulDisconnect = false;
    String willTopic;
    String willMessage;
    uint8_t willQos = 0;
    bool willRetain = false;
    std::unique_ptr<uint8_t[]> willPayload;
    size_t willPayloadLen = 0;

    // For QoS 1/2 messages sent *to* this client
    std::map<uint16_t, struct OutgoingQoSMessage> outgoingMessages;
    std::deque<struct OutgoingQoSMessage> pendingMessages;
    uint16_t nextPacketId = 1;

    // QoS2-Eingangs-State pro Client statt global,
    // weil packetId nur pro Verbindung eindeutig ist.
    std::map<uint16_t, struct IncomingQoS2Message> incomingQoS2Messages;

    // KeepAlive tracking
    bool kaSeen = false;
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
    uint64_t sequence = 0;
    bool transmitted = false;

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
            payload.reset(new (std::nothrow) uint8_t[len]);
            if (payload)
            {
                memcpy(payload.get(), p, len);
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
    bool ignoreLoopDeliver = false; ///< Konfigurationsfeld vorhanden; aktuell noch nicht ausgewertet (keine Laufzeitwirkung)
    bool log = true;
};

struct IncomingQoS2Message
{
    String topic;
    std::unique_ptr<uint8_t[]> payload;
    size_t payload_len; // BP3-07: Einziges Größenfeld (vorher doppelt mit 'length')
    bool retained;
    String senderClientId;
    String originalClientId;
    uint8_t qos = 2;

    IncomingQoS2Message() : payload_len(0), retained(false) {}

    IncomingQoS2Message(const String &t, const uint8_t *p, size_t len, bool ret, const String &clientId)
        : topic(t), payload_len(len), retained(ret), senderClientId(clientId), originalClientId(clientId)
    {
        if (len > 0 && p != nullptr)
        {
            payload.reset(new (std::nothrow) uint8_t[len]);
            if (payload)
            {
                memcpy(payload.get(), p, len);
            }
        }
    }
};

typedef std::function<void(const String& clientId, const String& clientIp, const String& username, int passwordLen)> ClientCallback;
typedef std::function<void(const String& clientId, const String& topic, const String& message)> MessageCallback;
// Datenzeiger gilt während des Aufrufs; Nutzdaten sind kein C-String.
typedef std::function<void(const String& clientId, const String& topic, const uint8_t* payload, size_t length)> BinaryMessageCallback;
typedef std::function<void(const String& clientId)> ClientDisconnectCallback;
typedef std::function<void(const String& clientId, int errorCode, const String& errorMessage)> ErrorCallback;
typedef std::function<void(const String& clientId, const String& topic)> SubscribeCallback;
typedef std::function<void(const String& clientId, const String& topic)> UnsubscribeCallback;
typedef std::function<void(DebugLevel level, const String &message)> LoggingCallback;

class ESPAsyncMQTTBroker
{
public:
    ESPAsyncMQTTBroker(uint16_t port = 1883);
    ~ESPAsyncMQTTBroker();
    void begin();
    void stop();
    void loop(); // Muss regelmaessig aus der Haupt-Loop aufgerufen werden (BP1-01)
    bool publish(const char *topic, const char *payload, bool retained = false, uint8_t qos = 0);
    bool publish(const char *topic, const char *payload, bool retained, uint8_t qos, const String &excludeClientId);
    bool publish(const char *topic, uint8_t qos, bool retained, const char *payload);
    bool publish(const char *topic, const uint8_t *payload, size_t payloadLen,
                 bool retained = false, uint8_t qos = 0, const String &excludeClientId = "");
    void setConfig(const ESPAsyncMQTTBrokerConfig &config);
    void setDebugLevel(DebugLevel level) { std::lock_guard<std::recursive_mutex> lock(stateMutex); debugLevel = level; }
    void setLoggingCallback(LoggingCallback callback) { std::lock_guard<std::recursive_mutex> lock(stateMutex); loggingCallback = callback; }
    void onClientConnect(ClientCallback callback) { std::lock_guard<std::recursive_mutex> lock(stateMutex); clientConnectCallback = callback; }
    void onMessage(MessageCallback callback) { std::lock_guard<std::recursive_mutex> lock(stateMutex); messageCallback = callback; }
    void onBinaryMessage(BinaryMessageCallback callback) { std::lock_guard<std::recursive_mutex> lock(stateMutex); binaryMessageCallback = callback; }
    void onClientDisconnect(ClientDisconnectCallback callback) { std::lock_guard<std::recursive_mutex> lock(stateMutex); clientDisconnectCallback = callback; }
    void onError(ErrorCallback callback) { std::lock_guard<std::recursive_mutex> lock(stateMutex); errorCallback = callback; }
    void onSubscribe(SubscribeCallback callback) { std::lock_guard<std::recursive_mutex> lock(stateMutex); subscribeCallback = callback; }
    void onUnsubscribe(UnsubscribeCallback callback) { std::lock_guard<std::recursive_mutex> lock(stateMutex); unsubscribeCallback = callback; }
    // Kopie statt ungeschützter Referenz auf gleichzeitig veränderte Map.
    std::map<String, String> getConnectedClientsInfo() const { std::lock_guard<std::recursive_mutex> lock(stateMutex); return connectedClientsInfo; }

    // ---- Connected-Clients API (für UI/Status ohne separaten Zähler) ----
    // Gibt die Anzahl aktuell als "connected" markierter Sessions zurück.
    size_t getConnectedClientCount() const;
    bool setPort(uint16_t newPort);

private:
    mutable std::recursive_mutex stateMutex;
    uint16_t port;
    std::unique_ptr<AsyncServer> server;
    std::map<AsyncClient *, std::shared_ptr<MQTTClient>> clients;
    std::map<String, std::unique_ptr<RetainedMessage>> retainedMessages;
    std::map<String, std::shared_ptr<MQTTClient>> persistentSessions;
    std::deque<IncomingQoS2Message> pendingWills;
    // incomingQoS2Messages liegt jetzt im MQTTClient (per-Client statt global)
    ESPAsyncMQTTBrokerConfig brokerConfig;

    // ---- Auth Cache (einmalig in setConfig() aufbauen) ----
    std::vector<String> allowedUsersLower; // getrimmt + lowercase je User
    bool authAnonMode = true;             // true wenn kein Username konfiguriert
    bool authNeedPassword = false;        // true wenn Passwort konfiguriert
    DebugLevel debugLevel = DEBUG_INFO;  // ← Wird im Konstruktor überschrieben mit BROKER_DEBUG_LEVEL!
    esp_timer_handle_t timeoutTimer = nullptr;
    std::atomic<bool> checkTimeoutsFlag{false};
    std::map<String, String> connectedClientsInfo;
    uint64_t nextSequence = 1;
    uint32_t nextAssignedId = 1;
    uint16_t getNextPacketId(MQTTClient *client);
    size_t storedBytes() const;
    void disconnectClient(AsyncClient *transport);
    void pumpMessages(const std::shared_ptr<MQTTClient>& client);
    bool sendMessage(MQTTClient *client, OutgoingQoSMessage &message, bool duplicate);
    void notifyMessage(const String& clientId, const String& topic, const uint8_t* payload, size_t length);
    bool publishMessage(const char *topic, const uint8_t *payload, size_t payloadLen,
                        bool retained, uint8_t qos, const String &excludeClientId, bool dispatch);
    void flushMessages();

    ClientCallback clientConnectCallback = nullptr;
    ClientDisconnectCallback clientDisconnectCallback = nullptr;
    MessageCallback messageCallback = nullptr;
    BinaryMessageCallback binaryMessageCallback = nullptr;
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
    bool isClientActive(AsyncClient *transport, const MQTTClient *identity) const;
    bool topicMatches(const Subscription &subscription, const String &topic);
    bool topicMatches(const String &subscription, const String &topic);
    void sendRetainedMessages(MQTTClient *client, const Subscription &subscription);
    bool authenticateClient(const String &username, const String &password);
    void onClient(AsyncClient *client, bool ownsTransport = false);
    void checkTimeouts();
    void logMessage(DebugLevel level, const char *format, ...);
    bool isValidPublishTopic(const String &topic);
    bool isValidTopicFilter(const String &filter);
    // BP3-06: isUserAllowed() als toter Code entfernt
};

#endif // ESP_ASYNC_MQTT_BROKER_H
