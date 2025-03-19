#ifndef ESP_ASYNC_MQTT_BROKER_H
#define ESP_ASYNC_MQTT_BROKER_H

#include <Arduino.h>
#include <AsyncTCP.h>
#include <vector>
#include <map>

class ESPAsyncMQTTBroker {
public:
    ESPAsyncMQTTBroker(uint16_t port = 1883);
    void begin();

private:
    uint16_t _port;
    AsyncServer _server;
    std::vector<AsyncClient*> _clients;
    std::map<String, std::vector<AsyncClient*>> _subscriptions;
    std::map<String, String> _retainedMessages;

    static void onNewClient(void* arg, AsyncClient* client);
    static void onClientData(void* arg, AsyncClient* client, void* data, size_t len);
    static void onClientDisconnect(void* arg, AsyncClient* client);
    static void onClientError(void* arg, AsyncClient* client, int8_t error);

    void removeClient(AsyncClient* client);
    void subscribe(AsyncClient* client, const String& topic);
    void publish(AsyncClient* sender, const String& topic, const String& message, bool retain);
};

#endif
