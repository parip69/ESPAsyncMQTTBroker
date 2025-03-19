#ifndef ESP_ASYNC_MQTT_BROKER_H
#define ESP_ASYNC_MQTT_BROKER_H

#include <Arduino.h>
#include <AsyncTCP.h>
#include <vector>
#include <map>

// MQTT Pakettypen
#define MQTT_CONNECT     1
#define MQTT_CONNACK     2
#define MQTT_PUBLISH     3
#define MQTT_PUBACK      4
#define MQTT_PUBREC      5
#define MQTT_PUBREL      6
#define MQTT_PUBCOMP     7
#define MQTT_SUBSCRIBE   8
#define MQTT_SUBACK      9
#define MQTT_UNSUBSCRIBE 10
#define MQTT_UNSUBACK    11
#define MQTT_PINGREQ     12
#define MQTT_PINGRESP    13
#define MQTT_DISCONNECT  14

// QoS Level
#define MQTT_QOS0        0
#define MQTT_QOS1        1
#define MQTT_QOS2        2

// Andere Konstanten
#define MQTT_PROTOCOL_LEVEL 4  // MQTT 3.1.1
#define MQTT_MAX_PACKET_SIZE 1024

// Client-Struktur
struct MQTTClient {
    AsyncClient* client;
    String clientId;
    bool connected;
    uint32_t lastActivity;
    std::vector<String> subscriptions;
};

// Datenstruktur für gespeicherte Nachrichten (retained)
struct RetainedMessage {
    String topic;
    uint8_t* payload;
    size_t length;
    uint8_t qos;
};

class ESPAsyncMQTTBroker {
public:
    ESPAsyncMQTTBroker(uint16_t port = 1883);
    ~ESPAsyncMQTTBroker();
    
    void begin();
    void stop();

private:
    AsyncServer* server;
    uint16_t port;
    std::vector<MQTTClient*> clients;
    std::vector<RetainedMessage*> retainedMessages;
    
    // MQTT Paket-Verarbeitung
    void handleConnect(MQTTClient* client, uint8_t* data, size_t len);
    void handlePublish(MQTTClient* client, uint8_t* data, size_t len, uint8_t header);
    void handleSubscribe(MQTTClient* client, uint8_t* data, size_t len);
    void handleUnsubscribe(MQTTClient* client, uint8_t* data, size_t len);
    void handlePingReq(MQTTClient* client);
    void handleDisconnect(MQTTClient* client);
    
    // Hilfsfunktionen
    void processPacket(MQTTClient* client, uint8_t* data, size_t len);
    void distributeMessage(const String& topic, uint8_t* payload, size_t length, uint8_t qos, bool retain);
    bool topicMatches(const String& subscription, const String& topic);
    void storeRetainedMessage(const String& topic, uint8_t* payload, size_t length, uint8_t qos);
    void sendRetainedMessages(MQTTClient* client);
    
    // Callback für AsyncServer
    void onClient(AsyncClient* client);
    static void handleData(void* arg, AsyncClient* client, void* data, size_t len);
    static void handleDisconnect(void* arg, AsyncClient* client);
    static void handleError(void* arg, AsyncClient* client, int8_t error);
    static void handleTimeOut(void* arg, AsyncClient* client, uint32_t time);
};

#endif // ESP_ASYNC_MQTT_BROKER_H