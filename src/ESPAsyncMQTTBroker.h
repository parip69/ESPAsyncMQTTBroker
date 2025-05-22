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
/** @brief Makro für vereinfachten Zugriff auf die logMessage Funktion. */
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
        : topic(t), qos(q) {
        // Sichere Kopie des Payloads
        if (len > 0 && p != nullptr) {
            if (len <= MQTT_MAX_PAYLOAD_SIZE) {
                this->length = len;
                payload.reset(new uint8_t[this->length]);
                memcpy(payload.get(), p, this->length);
            } else {
                this->length = MQTT_MAX_PAYLOAD_SIZE;
                payload.reset(new uint8_t[this->length]);
                memcpy(payload.get(), p, this->length);
                // Log Message for truncation can be added here if logging is available
                // For now, we assume the main logger of the broker handles this or it's a silent truncation.
            }
        } else {
            this->length = 0;
            // payload bleibt nullptr oder leer, was in Ordnung ist.
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
/** @brief Callback-Typ für Client-Verbindungsereignisse. Wird ausgelöst, wenn ein Client erfolgreich verbindet.
 *  @param clientId Die ID des verbundenen Clients.
 *  @param clientIp Die IP-Adresse des verbundenen Clients.
 */
typedef std::function<void(String clientId, String clientIp)> ClientCallback;

/** @brief Callback-Typ für eingehende Nachrichten. Wird ausgelöst, wenn eine Nachricht von einem Client empfangen wird.
 *  @param clientId Die ID des sendenden Clients.
 *  @param topic Das Topic, auf dem die Nachricht empfangen wurde.
 *  @param message Der Inhalt der empfangenen Nachricht.
 */
typedef std::function<void(String clientId, String topic, String message)> MessageCallback;

/** @brief Callback-Typ für Client-Trennungsereignisse. Wird ausgelöst, wenn ein Client die Verbindung trennt.
 *  @param clientId Die ID des getrennten Clients.
 */
typedef std::function<void(String clientId)> ClientDisconnectCallback;

/** @brief Callback-Typ für Fehlerereignisse. Wird bei clientbezogenen Fehlern ausgelöst.
 *  @param clientId Die ID des Clients, bei dem der Fehler auftrat (kann leer sein).
 *  @param errorCode Der Fehlercode.
 *  @param errorMessage Eine beschreibende Fehlermeldung.
 */
typedef std::function<void(String clientId, int errorCode, const String &errorMessage)> ErrorCallback;

/** @brief Callback-Typ für neue Abonnements. Wird ausgelöst, wenn ein Client ein Topic abonniert.
 *  @param clientId Die ID des abonnierenden Clients.
 *  @param topic Das Topic (oder der Filter), das abonniert wurde.
 */
typedef std::function<void(String clientId, const String &topic)> SubscribeCallback;

/** @brief Callback-Typ für das Beenden von Abonnements. Wird ausgelöst, wenn ein Client ein Abonnement entfernt.
 *  @param clientId Die ID des Clients.
 *  @param topic Das Topic (oder der Filter), dessen Abonnement beendet wurde.
 */
typedef std::function<void(String clientId, const String &topic)> UnsubscribeCallback;

/** @brief Callback-Typ für Log-Nachrichten des Brokers.
 *  @param level Das Debug-Level der Nachricht.
 *  @param message Die Log-Nachricht.
 */
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
    std::unique_ptr<AsyncServer> server; ///< Smart Pointer zum AsyncTCP Serverobjekt.
    uint16_t port;                       ///< Der TCP-Port, auf dem der Broker lauscht.
    
    /** 
     * @brief Map der aktuell verbundenen Clients.
     * Der Schlüssel ist der `AsyncClient*` (Zeiger auf die TCP-Verbindung), 
     * der Wert ist ein `unique_ptr` zum `MQTTClient`-Objekt, das die Zustandsinformationen des Clients hält.
     */
    std::map<AsyncClient*, std::unique_ptr<MQTTClient>> _clients; 
    
    std::vector<std::unique_ptr<RetainedMessage>> retainedMessages; ///< Liste der gespeicherten (retained) Nachrichten.
    
    /**
     * @brief Map der persistenten Client-Sessions.
     * Wird verwendet, wenn Clients mit `cleanSession = false` verbinden.
     * Der Schlüssel ist die `clientId` des Clients.
     */
    std::map<String, std::unique_ptr<MQTTClient>> persistentSessions;
    
    ESPAsyncMQTTBrokerConfig brokerConfig; ///< Aktuelle Konfiguration des Brokers (z.B. Authentifizierung).

    /** 
     * @brief Map zur Speicherung von Informationen über verbundene Clients (Client-ID -> IP-Adresse).
     * Wird für die `getConnectedClientsInfo()` Methode verwendet.
     */
    std::map<String, String> connectedClientsInfo; 

    esp_timer_handle_t timeoutTimer = NULL; ///< Handle für den ESP-Timer, der für Keep-Alive-Timeouts verwendet wird.

    // Callbacks für verschiedene Broker-Ereignisse
    ClientCallback clientConnectCallback = nullptr;         ///< Callback für neue Client-Verbindungen.
    MessageCallback messageCallback = nullptr;              ///< Callback für eingehende Nachrichten.
    ClientDisconnectCallback clientDisconnectCallback = nullptr; ///< Callback für Client-Trennungen.
    ErrorCallback errorCallback = nullptr;                  ///< Callback für Fehlerereignisse.
    SubscribeCallback subscribeCallback = nullptr;          ///< Callback für neue Abonnements.
    UnsubscribeCallback unsubscribeCallback = nullptr;      ///< Callback für das Beenden von Abonnements.
    LoggingCallback loggingCallback = nullptr;              ///< Callback für Log-Nachrichten.

    DebugLevel debugLevel = DEBUG_DEBUG; ///< Aktuelles Debug-Level für die Logging-Ausgabe.

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
};

#endif // ESP_ASYNC_MQTT_BROKER_H
