// @version: 1.5.0 Builddatum 21-05.2025
/**
 * @file ESPAsyncMQTTBroker.h
 * @brief Asynchrone MQTT Broker-Implementierung für ESP32
 * 
 * ESPAsyncMQTTBroker bietet einen vollständigen MQTT Broker für ESP32, basierend
 * auf der AsyncTCP-Bibliothek. Er unterstützt sowohl MQTT 3.1.1 als auch
 * grundlegende MQTT 5.0 Funktionalitäten.
 * 
 * Hauptfunktionen:
 * - Volle MQTT 3.1.1 Unterstützung
 * - Basis-Support für MQTT 5.0 Eigenschaften
 * - QoS 0, 1 und 2 Nachrichtenzustellung
 * - Retained Messages
 * - Persistente Sessions
 * - Callback-basierte Ereignisbehandlung für MQTT-Events
 * - Topic-Filter mit Wildcard-Unterstützung (# und +)
 * - Sichere Speicherverwaltung mit Smart Pointern
 * 
 * @author ESP-Team
 * @date Mai 2025
 */
#ifndef ESP_ASYNC_MQTT_BROKER_H
#define ESP_ASYNC_MQTT_BROKER_H

#include <Arduino.h>
#include <AsyncTCP.h>
#include <vector>
#include <map>
#include <memory>  // Für Smart Pointer
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
#define MQTT_PROTOCOL_LEVEL 4 // MQTT 3.1.1
#define MQTT_PROTOCOL_LEVEL_5 5 // MQTT 5.0
#define MQTT_MAX_PACKET_SIZE 1024
#define MQTT_MAX_TOPIC_SIZE 256   // Maximale Größe für Topic
#define MQTT_MAX_PAYLOAD_SIZE 768 // Maximale Größe für Payload

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
 * @brief Debug-Level für Logging
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
 * @brief Repräsentiert ein MQTT-Abonnement für einen Client
 */
struct Subscription
{
    String filter;    ///< Topic-Filter, mit dem eingehende Nachrichten verglichen werden
    bool noLocal;     ///< MQTT 5.0 noLocal-Flag: Bei true erhält der Client keine selbst veröffentlichten Nachrichten
    // evtl. später noch weitere Flags (retainAsPublished, retainHandling…)
};

/**
 * @brief Repräsentiert einen verbundenen MQTT-Client
 */
struct MQTTClient
{
    AsyncClient *client;               ///< Referenz zur TCP-Verbindung
    String clientId;                   ///< Die vom Client angegebene Client-ID
    bool connected;                    ///< Verbindungsstatus
    uint32_t lastActivity;             ///< Zeitpunkt der letzten Aktivität für Timeout-Überwachung
    uint16_t keepAlive;                ///< Keep-Alive Intervall in Sekunden
    bool cleanSession;                 ///< Clean-Session-Flag (falls false, wird die Session gespeichert)
    std::vector<Subscription> subscriptions; ///< Abonnierte Topics
    uint8_t protocolVersion;           ///< MQTT Protokoll-Version: 4 = MQTT 3.1.1, 5 = MQTT 5.0

    // Neue Member für Last Will and Testament (LWT)
    bool hasWill {false};
    String willTopic {};
    std::unique_ptr<uint8_t[]> willPayload {nullptr};
    size_t willPayloadLen {0};
    uint8_t willQos {0};
    bool willRetain {false};

    // Neues Flag für saubere Trennung
    bool gracefulDisconnect {false};   ///< True, wenn Client ein DISCONNECT Paket gesendet hat
};

/**
 * @brief Datenstruktur für gespeicherte (retained) Nachrichten
 */
struct RetainedMessage
{
    String topic;                      ///< Das Topic der Nachricht
    std::unique_ptr<uint8_t[]> payload;  ///< Smart Pointer zum Nachrichteninhalt
    size_t length;                     ///< Länge des Payloads
    uint8_t qos;                       ///< QoS-Level der Nachricht
    
    /**
     * @brief Konstruktor für eine retained Message
     * @param t Topic der Nachricht
     * @param p Pointer zum Payload-Buffer
     * @param len Länge des Payloads
     * @param q QoS-Level
     */    RetainedMessage(const String& t, const uint8_t* p, size_t len, uint8_t q) 
        : topic(t), length(len), qos(q) {
        // Sichere Kopie des Payloads
        if (len > 0 && p != nullptr) {
            // Anstelle von std::make_unique nutzen wir direkt den Konstruktor
            payload.reset(new uint8_t[len]);
            if (len <= MQTT_MAX_PAYLOAD_SIZE) {
                memcpy(payload.get(), p, len);
            } else {
                memcpy(payload.get(), p, MQTT_MAX_PAYLOAD_SIZE);
                length = MQTT_MAX_PAYLOAD_SIZE;
            }
        }
    }
};

/**
 * @brief Konfigurationsstruktur für den MQTT-Broker
 */
struct ESPAsyncMQTTBrokerConfig
{
    String username = "";              ///< Benutzername für Authentifizierung (wenn leer, ist keine Auth nötig)
    String password = "";              ///< Passwort für Authentifizierung
    bool ignoreLoopDeliver = false;    ///< Bei true werden Nachrichten nicht an den Absender zurückgesendet
};

// Callback-Typdefinitionen für Ereignisbehandlung
typedef std::function<void(String clientId, String clientIp)> ClientCallback;
typedef std::function<void(String clientId, String topic, String message)> MessageCallback;
typedef std::function<void(String clientId)> ClientDisconnectCallback;
typedef std::function<void(String clientId, int errorCode, const String &errorMessage)> ErrorCallback;
typedef std::function<void(String clientId, const String &topic)> SubscribeCallback;
typedef std::function<void(String clientId, const String &topic)> UnsubscribeCallback;
typedef std::function<void(DebugLevel level, const String &message)> LoggingCallback;

/**
 * @brief Asynchroner MQTT Broker für ESP32
 * 
 * Diese Klasse implementiert einen vollständigen MQTT Broker (Server), der auf einem
 * ESP32 laufen kann. Er verwendet die AsyncTCP-Bibliothek für nicht-blockierende 
 * Verarbeitung und bietet eine ereignisbasierte API für einfache Integration.
 * 
 * Hauptmerkmale:
 * - Unterstützt MQTT 3.1.1 und grundlegende MQTT 5.0 Funktionen
 * - QoS 0, 1, und 2 Nachrichten
 * - Retained Messages (gespeicherte Nachrichten)
 * - Persistente Sessions
 * - MQTT-Wildcard-Unterstützung (+ und #)
 * - Optimierte Speichernutzung mit Smart Pointern
 */
class ESPAsyncMQTTBroker
{
public:
    /**
     * @brief Konstruktor für den MQTT-Broker
     * @param port TCP-Port für den MQTT-Dienst (Standard: 1883)
     */
    ESPAsyncMQTTBroker(uint16_t port = 1883);
    
    /**
     * @brief Destruktor, gibt alle Ressourcen frei
     */
    ~ESPAsyncMQTTBroker();

    /**
     * @brief Startet den MQTT-Broker
     * 
     * Öffnet den TCP-Port und beginnt, Verbindungen anzunehmen.
     * Richtet auch den Timer für die Überwachung der Client-Timeouts ein.
     */
    void begin();
    
    /**
     * @brief Stoppt den MQTT-Broker
     * 
     * Beendet den Server und gibt Ressourcen frei.
     */
    void stop();
    
    /**
     * @brief Veröffentlicht eine Nachricht über den Broker
     * 
     * Diese Methode wird verwendet, um eine Nachricht direkt vom Broker 
     * zu veröffentlichen, ohne dass ein Client diese senden muss.
     * 
     * @param topic Das Topic, unter dem die Nachricht veröffentlicht wird
     * @param payload Der Inhalt der Nachricht
     * @param retained Ob die Nachricht als "retained" gespeichert werden soll
     * @param qos Quality of Service (0, 1 oder 2)
     * @return true wenn die Nachricht erfolgreich veröffentlicht wurde
     */
    bool publish(const char* topic, const char* payload, bool retained = false, uint8_t qos = 0);
    
    /**
     * @brief Erweiterte publish-Methode mit Client-Ausschluss
     * 
     * Diese Methode erlaubt es, einen Client anzugeben, der die Nachricht
     * nicht erhalten soll (für noLocal-Unterstützung).
     * 
     * @param topic Das Topic, unter dem die Nachricht veröffentlicht wird
     * @param payload Der Inhalt der Nachricht
     * @param retained Ob die Nachricht als "retained" gespeichert werden soll
     * @param qos Quality of Service (0, 1 oder 2)
     * @param excludeClientId ID eines Clients, der die Nachricht nicht erhalten soll
     * @return true wenn die Nachricht erfolgreich veröffentlicht wurde
     */
    bool publish(const char* topic, const char* payload, bool retained, uint8_t qos, const String& excludeClientId);

    /**
     * @brief Alternative publish-Methode mit anderer Parameter-Reihenfolge
     * 
     * Diese Methode ist für die Kompatibilität mit bestehendem Code.
     * Sie nimmt die Parameter in der Reihenfolge topic, qos, retain, payload.
     * 
     * @param topic Das Topic, unter dem die Nachricht veröffentlicht wird
     * @param qos Quality of Service (0, 1 oder 2)
     * @param retained Ob die Nachricht als "retained" gespeichert werden soll
     * @param payload Der Inhalt der Nachricht
     * @return true wenn die Nachricht erfolgreich veröffentlicht wurde
     */
    bool publish(const char* topic, uint8_t qos, bool retained, const char* payload);

    // Neuer interner Publish-Overload für binären Payload (LWT)
    bool publish(const char* topic, const uint8_t* payload, size_t payloadLen, bool retained, uint8_t qos, const String& excludeClientId = "");

    /**
     * @brief Setzt die Broker-Konfiguration
     * @param config Die neue Konfiguration
     */
    void setConfig(const ESPAsyncMQTTBrokerConfig &config);
    
    /**
     * @brief Setzt das Debug-Level für die Logging-Ausgabe
     * @param level Das gewünschte Debug-Level
     */
    void setDebugLevel(DebugLevel level) { debugLevel = level; }
    
    /**
     * @brief Setzt den Callback für Log-Meldungen
     * 
     * Dieser Callback wird für alle Log-Meldungen aufgerufen, die das aktuelle Debug-Level
     * erfüllen. Damit können Log-Nachrichten an andere Ausgabekanäle weitergeleitet werden.
     * 
     * @param callback Die Callback-Funktion
     */
    void setLoggingCallback(LoggingCallback callback) { loggingCallback = callback; }

    // Callback-Setter
    /**
     * @brief Setzt den Callback für neue Client-Verbindungen
     * @param callback Die Callback-Funktion
     */
    void onClientConnect(ClientCallback callback) { clientConnectCallback = callback; }
    
    /**
     * @brief Setzt den Callback für eingehende Nachrichten
     * @param callback Die Callback-Funktion
     */
    void onMessage(MessageCallback callback) { messageCallback = callback; }
    
    /**
     * @brief Setzt den Callback für Client-Trennungen
     * @param callback Die Callback-Funktion
     */
    void onClientDisconnect(ClientDisconnectCallback callback) { clientDisconnectCallback = callback; }
    
    /**
     * @brief Setzt den Callback für Fehler
     * @param callback Die Callback-Funktion
     */
    void onError(ErrorCallback callback) { errorCallback = callback; }
    
    /**
     * @brief Setzt den Callback für neue Abonnements
     * @param callback Die Callback-Funktion
     */
    void onSubscribe(SubscribeCallback callback) { subscribeCallback = callback; }
    
    /**
     * @brief Setzt den Callback für das Beenden von Abonnements
     * @param callback Die Callback-Funktion
     */
    void onUnsubscribe(UnsubscribeCallback callback) { unsubscribeCallback = callback; }

    /**
     * @brief Gibt Informationen über verbundene Clients zurück
     * @return Map mit Client-IDs und IP-Adressen
     */
    std::map<String, String> getConnectedClientsInfo() const { return connectedClientsInfo; }

private:
    // Struktur zum temporären Speichern von eingehenden QoS 2 Nachrichten
    struct IncomingQoS2Message {
        String topic;
        std::unique_ptr<uint8_t[]> payload;
        size_t payload_len;
        bool retained;
        String originalClientId; // ClientID des ursprünglichen Senders

        // Konstruktor für einfaches Erstellen und Kopieren des Payloads
        IncomingQoS2Message(const String& t, const uint8_t* p, size_t len, bool r, const String& clientId)
            : topic(t), payload_len(len), retained(r), originalClientId(clientId) {
            if (len > 0 && p != nullptr) {
                payload.reset(new uint8_t[len]);
                memcpy(payload.get(), p, len);
            } else {
                payload_len = 0; // Sicherstellen, dass die Länge 0 ist, wenn kein Payload vorhanden ist
            }
        }

        // Standardkonstruktor, um map[]-Zugriff zu ermöglichen (obwohl wir ihn mit find/emplace verwenden werden)
        IncomingQoS2Message() : payload_len(0), retained(false) {}
    };
    std::map<uint16_t, IncomingQoS2Message> incomingQoS2Messages; // Key: Packet ID

    std::unique_ptr<AsyncServer> server;
    uint16_t port;
    std::vector<std::unique_ptr<MQTTClient>> clients;
    std::map<String, std::unique_ptr<RetainedMessage>> retainedMessages; // Geändert von std::vector zu std::map
    std::map<String, std::unique_ptr<MQTTClient>> persistentSessions;
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
    LoggingCallback loggingCallback = nullptr;

    DebugLevel debugLevel = DEBUG_DEBUG;

    /**
     * @brief Verarbeitet ein CONNECT-Paket
     * @param client Der Client, der das Paket gesendet hat
     * @param data Die Paketdaten
     * @param len Die Länge der Daten
     */
    void handleConnect(MQTTClient *client, uint8_t *data, size_t len);
    
    /**
     * @brief Verarbeitet ein PUBLISH-Paket
     * @param client Der Client, der das Paket gesendet hat
     * @param data Die Paketdaten
     * @param len Die Länge der Daten
     * @param header Der MQTT-Header mit QoS und Retain-Flags
     */
    void handlePublish(MQTTClient *client, uint8_t *data, size_t len, uint8_t header);
    
    /**
     * @brief Verarbeitet ein SUBSCRIBE-Paket
     * @param client Der Client, der das Paket gesendet hat
     * @param data Die Paketdaten
     * @param len Die Länge der Daten
     */
    void handleSubscribe(MQTTClient *client, uint8_t *data, size_t len);
    
    /**
     * @brief Verarbeitet ein UNSUBSCRIBE-Paket
     * @param client Der Client, der das Paket gesendet hat
     * @param data Die Paketdaten
     * @param len Die Länge der Daten
     */
    void handleUnsubscribe(MQTTClient *client, uint8_t *data, size_t len);
    
    /**
     * @brief Verarbeitet ein PING-Request-Paket
     * @param client Der Client, der das Paket gesendet hat
     */
    void handlePingReq(MQTTClient *client);
    
    /**
     * @brief Verarbeitet ein DISCONNECT-Paket
     * @param client Der Client, der das Paket gesendet hat
     */
    void handleDisconnect(MQTTClient *client);

    // QoS 2 Handler
    /**
     * @brief Verarbeitet ein PUBREC-Paket (QoS 2)
     * @param client Der Client, der das Paket gesendet hat
     * @param data Die Paketdaten
     * @param len Die Länge der Daten
     */
    void handlePubRec(MQTTClient *client, uint8_t *data, size_t len);
    
    /**
     * @brief Verarbeitet ein PUBREL-Paket (QoS 2)
     * @param client Der Client, der das Paket gesendet hat
     * @param data Die Paketdaten
     * @param len Die Länge der Daten
     */
    void handlePubRel(MQTTClient *client, uint8_t *data, size_t len);
    
    /**
     * @brief Verarbeitet ein PUBCOMP-Paket (QoS 2)
     * @param client Der Client, der das Paket gesendet hat
     * @param data Die Paketdaten
     * @param len Die Länge der Daten
     */    
    void handlePubComp(MQTTClient *client, uint8_t *data, size_t len);
    
    /**
     * @brief Zentrale Paketverarbeitungsfunktion
     * @param client Der Client, der das Paket gesendet hat
     * @param data Die Paketdaten
     * @param len Die Länge der Daten
     */
    void processPacket(MQTTClient *client, uint8_t *data, size_t len);
    
    /**
     * @brief Prüft, ob ein Topic mit dem Filter eines Abonnements übereinstimmt
     * @param subscription Das Abonnement mit dem Filter
     * @param topic Das zu prüfende Topic
     * @return true, wenn das Topic mit dem Filter übereinstimmt
     */
    bool topicMatches(const Subscription &subscription, const String &topic);
    
    /**
     * @brief Prüft, ob ein Topic mit einem Filter übereinstimmt
     * @param subscription Der Filter als String
     * @param topic Das zu prüfende Topic
     * @return true, wenn das Topic mit dem Filter übereinstimmt
     */
    bool topicMatches(const String &subscription, const String &topic);
    
    /**
     * @brief Sendet alle gespeicherten (retained) Nachrichten an einen Client
     * @param client Der Client, der die Nachrichten erhalten soll
     */
    void sendRetainedMessages(MQTTClient *client);

    /**
     * @brief Authentifiziert einen Client anhand von Benutzername und Passwort
     * @param username Der Benutzername
     * @param password Das Passwort
     * @return true, wenn die Authentifizierung erfolgreich war
     */
    bool authenticateClient(const String &username, const String &password);
    
    /**
     * @brief Callback für neue TCP-Verbindungen
     * @param client Der neue TCP-Client
     */
    void onClient(AsyncClient *client);
    
    /**
     * @brief Prüft Timeouts für alle verbundenen Clients
     * 
     * Wird regelmäßig vom Timer aufgerufen, um inaktive Clients zu identifizieren
     * und zu trennen.
     */
    void checkTimeouts();
    
    /**
     * @brief Zentrale Logging-Funktion
     * @param level Das Debug-Level der Nachricht
     * @param format Das Format der Nachricht (printf-style)
     */
    void logMessage(DebugLevel level, const char* format, ...);

    /**
     * @brief Prüft, ob ein Topic-Filter gemäß MQTT-Spezifikation gültig ist.
     * @param filter Der zu prüfende Topic-Filter.
     * @return true, wenn der Filter gültig ist, sonst false.
     */
    bool isValidTopicFilter(const String& filter);

    /**
     * @brief Prüft, ob ein Topic-Name für eine PUBLISH-Nachricht gültig ist.
     * @param topic Der zu prüfende Topic-Name.
     * @return true, wenn der Topic-Name gültig ist, sonst false.
     */
    bool isValidPublishTopic(const String& topic);
};

#endif // ESP_ASYNC_MQTT_BROKER_H
