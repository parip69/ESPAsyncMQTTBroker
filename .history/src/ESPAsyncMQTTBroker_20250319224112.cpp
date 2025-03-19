#include "ESPAsyncMQTTBroker.h"

ESPAsyncMQTTBroker::ESPAsyncMQTTBroker(uint16_t port) : port(port), server(NULL) {
}

ESPAsyncMQTTBroker::~ESPAsyncMQTTBroker() {
    stop();
    
    // Aufräumen
    for (auto client : clients) {
        delete client;
    }
    clients.clear();
    
    for (auto msg : retainedMessages) {
        delete[] msg->payload;
        delete msg;
    }
    retainedMessages.clear();
}

void ESPAsyncMQTTBroker::begin() {
    server = new AsyncServer(port);
    server->onClient([this](void* arg, AsyncClient* client) {
        this->onClient(client);
    }, NULL);
    server->begin();
}

void ESPAsyncMQTTBroker::stop() {
    if (server) {
        server->end();
        delete server;
        server = NULL;
    }
}

void ESPAsyncMQTTBroker::onClient(AsyncClient* client) {
    MQTTClient* mqttClient = new MQTTClient();
    mqttClient->client = client;
    mqttClient->connected = false;
    mqttClient->lastActivity = millis();
    
    client->onData([this](void* arg, AsyncClient* client, void* data, size_t len) {
        MQTTClient* mqttClient = NULL;
        for (auto c : this->clients) {
            if (c->client == client) {
                mqttClient = c;
                break;
            }
        }
        
        if (mqttClient) {
            this->processPacket(mqttClient, (uint8_t*)data, len);
            mqttClient->lastActivity = millis();
        }
    }, this);
    
    client->onDisconnect([this](void* arg, AsyncClient* client) {
        for (auto it = this->clients.begin(); it != this->clients.end(); ++it) {
            if ((*it)->client == client) {
                delete *it;
                this->clients.erase(it);
                break;
            }
        }
    }, this);
    
    clients.push_back(mqttClient);
}

void ESPAsyncMQTTBroker::processPacket(MQTTClient* client, uint8_t* data, size_t len) {
    if (len < 2) return; // Zu kleine Nachricht
    
    uint8_t header = data[0];
    uint8_t packetType = (header >> 4) & 0x0F;
    
    // Variable Length Berechnung (MQTT-Protokoll)
    size_t multiplier = 1;
    size_t value = 0;
    uint8_t encodedByte;
    size_t idx = 1;
    
    do {
        if (idx >= len) return; // Unvollständiges Paket
        encodedByte = data[idx++];
        value += (encodedByte & 127) * multiplier;
        multiplier *= 128;
        if (multiplier > 128*128*128) return; // Malformed length
    } while ((encodedByte & 128) != 0);
    
    if (len < idx + value) return; // Unvollständiges Paket
    
    // Paket-Typ verarbeiten
    switch (packetType) {
        case MQTT_CONNECT:
            handleConnect(client, data + idx, value);
            break;
        case MQTT_PUBLISH:
            handlePublish(client, data + idx, value, header);
            break;
        case MQTT_SUBSCRIBE:
            handleSubscribe(client, data + idx, value);
            break;
        case MQTT_UNSUBSCRIBE:
            handleUnsubscribe(client, data + idx, value);
            break;
        case MQTT_PINGREQ:
            handlePingReq(client);
            break;
        case MQTT_DISCONNECT:
            handleDisconnect(client);
            break;
        // Weitere Pakettypen nach Bedarf implementieren
    }
}

// Hier folgen die Implementierungen der individuellen Handler-Methoden
// Dies ist eine Grundstruktur, die du basierend auf deinen spezifischen Anforderungen erweitern solltest