#include "ESPAsyncMQTTBroker.h"

ESPAsyncMQTTBroker::ESPAsyncMQTTBroker(uint16_t port) : server(NULL), port(port) {
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

// Implementierungen der individuellen Handler-Methoden
void ESPAsyncMQTTBroker::handleConnect(MQTTClient* client, uint8_t* data, uint32_t length) {
    // MQTT CONNECT Paket verarbeiten
    // Später mit vollständiger Connect-Logik implementieren
    
    // Einfaches CONNACK senden (Code 0 = Verbindung akzeptiert)
    uint8_t connack[] = {
        0x20, // CONNACK Paket
        0x02, // Restlänge
        0x00, // Keine Session vorhanden
        0x00  // Verbindung akzeptiert
    };
    client->client->write((const char*)connack, 4);
    client->connected = true;
}

void ESPAsyncMQTTBroker::handlePublish(MQTTClient* client, uint8_t* data, uint32_t length, uint8_t header) {
    // MQTT PUBLISH Paket verarbeiten
    // Später mit vollständiger Publish-Logik implementieren
    
    // QoS-Level extrahieren
    uint8_t qos = (header & 0x06) >> 1;
    // Retained-Flag extrahieren
    bool retained = (header & 0x01) != 0;
    
    // Hier Logik für das Weiterleiten der Nachrichten an Subscriber implementieren
    // und retained Messages speichern, falls das Flag gesetzt ist
}

void ESPAsyncMQTTBroker::handleSubscribe(MQTTClient* client, uint8_t* data, uint32_t length) {
    // MQTT SUBSCRIBE Paket verarbeiten
    // Später mit vollständiger Subscribe-Logik implementieren
    
    // Packet Identifier extrahieren (ersten 2 Bytes)
    uint16_t packetId = (data[0] << 8) | data[1];
    
    // Einfaches SUBACK senden
    uint8_t suback[3] = {
        0x90, // SUBACK packet
        0x03, // Restlänge: 3 Bytes
        (uint8_t)(packetId >> 8), // Packet ID MSB
        (uint8_t)packetId,       // Packet ID LSB
        0x00  // QoS 0 gewährt
    };
    client->client->write((const char*)suback, 5);
    
    // Hier Logik für das Speichern von Subscriptions implementieren
}

void ESPAsyncMQTTBroker::handleUnsubscribe(MQTTClient* client, uint8_t* data, uint32_t length) {
    // MQTT UNSUBSCRIBE Paket verarbeiten
    // Später mit vollständiger Unsubscribe-Logik implementieren
    
    // Packet Identifier extrahieren (ersten 2 Bytes)
    uint16_t packetId = (data[0] << 8) | data[1];
    
    // Einfaches UNSUBACK senden
    uint8_t unsuback[4] = {
        0xB0, // UNSUBACK packet
        0x02, // Restlänge: 2 Bytes
        (uint8_t)(packetId >> 8), // Packet ID MSB
        (uint8_t)packetId        // Packet ID LSB
    };
    client->client->write((const char*)unsuback, 4);
    
    // Hier Logik für das Entfernen von Subscriptions implementieren
}

void ESPAsyncMQTTBroker::handlePingReq(MQTTClient* client) {
    // MQTT PINGREQ Paket verarbeiten
    // Mit PINGRESP antworten
    uint8_t pingresp[] = {
        0xD0, // PINGRESP packet
        0x00  // Restlänge: 0 Bytes
    };
    client->client->write((const char*)pingresp, 2);
}

void ESPAsyncMQTTBroker::handleDisconnect(MQTTClient* client) {
    // MQTT DISCONNECT Paket verarbeiten
    client->connected = false;
    
    // Optional: Verbindung schließen
    // client->client->close();
}