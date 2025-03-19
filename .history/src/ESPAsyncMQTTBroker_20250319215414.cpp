#include "ESPAsyncMQTTBroker.h"

ESPAsyncMQTTBroker::ESPAsyncMQTTBroker(uint16_t port) : _port(port), _server(port) {}

void ESPAsyncMQTTBroker::begin() {
    _server.onClient(&ESPAsyncMQTTBroker::onNewClient, this);
    _server.begin();
    Serial.printf("ESPAsyncMQTTBroker läuft auf Port %d\n", _port);
}

void ESPAsyncMQTTBroker::onNewClient(void* arg, AsyncClient* client) {
    ESPAsyncMQTTBroker* broker = static_cast<ESPAsyncMQTTBroker*>(arg);
    Serial.println("Neuer MQTT-Client verbunden");
    
    // Callbacks registrieren
    client->onData(&ESPAsyncMQTTBroker::onClientData, broker);
    client->onDisconnect(&ESPAsyncMQTTBroker::onClientDisconnect, broker);
    client->onError(&ESPAsyncMQTTBroker::onClientError, broker);
    
    broker->_clients.push_back(client);
}

void ESPAsyncMQTTBroker::onClientData(void* arg, AsyncClient* client, void* data, size_t len) {
    ESPAsyncMQTTBroker* broker = static_cast<ESPAsyncMQTTBroker*>(arg);
    String message = String((char*)data).substring(0, len);
    Serial.printf("Empfangen: %s\n", message.c_str());

    if (message.startsWith("SUBSCRIBE ")) {
        String topic = message.substring(10);
        broker->subscribe(client, topic);
    } else if (message.startsWith("PUBLISH ")) {
        int firstSpace = message.indexOf(' ', 8);
        int secondSpace = message.indexOf(' ', firstSpace + 1);
        if (firstSpace != -1 && secondSpace != -1) {
            String topic = message.substring(8, firstSpace);
            bool retain = message.substring(firstSpace + 1, secondSpace).toInt();
            String payload = message.substring(secondSpace + 1);
            broker->publish(client, topic, payload, retain);
        }
    }
}

void ESPAsyncMQTTBroker::onClientDisconnect(void* arg, AsyncClient* client) {
    ESPAsyncMQTTBroker* broker = static_cast<ESPAsyncMQTTBroker*>(arg);
    Serial.println("MQTT-Client getrennt");
    broker->removeClient(client);
}

void ESPAsyncMQTTBroker::onClientError(void* arg, AsyncClient* client, int8_t error) {
    ESPAsyncMQTTBroker* broker = static_cast<ESPAsyncMQTTBroker*>(arg);
    Serial.printf("MQTT-Client Fehler: %d\n", error);
    broker->removeClient(client);
}

void ESPAsyncMQTTBroker::removeClient(AsyncClient* client) {
    // Entferne Client aus der Liste
    auto it = std::find(_clients.begin(), _clients.end(), client);
    if (it != _clients.end()) {
        _clients.erase(it);
    }
    
    // Entferne Client aus allen Abonnements
    for (auto& subscription : _subscriptions) {
        auto& clients = subscription.second;
        auto clientIt = std::find(clients.begin(), clients.end(), client);
        if (clientIt != clients.end()) {
            clients.erase(clientIt);
        }
    }
}

void ESPAsyncMQTTBroker::subscribe(AsyncClient* client, const String& topic) {
    _subscriptions[topic].push_back(client);
    Serial.printf("Client hat %s abonniert\n", topic.c_str());

    if (_retainedMessages.count(topic) > 0) {
        client->write((uint8_t*)_retainedMessages[topic].c_str(), _retainedMessages[topic].length());
    }
}

void ESPAsyncMQTTBroker::publish(AsyncClient* sender, const String& topic, const String& message, bool retain) {
    Serial.printf("Sende an %s: %s\n", topic.c_str(), message.c_str());

    if (retain) {
        _retainedMessages[topic] = message;
    }

    if (_subscriptions.count(topic) > 0) {
        for (AsyncClient* subscriber : _subscriptions[topic]) {
            subscriber->write((uint8_t*)message.c_str(), message.length());
        }
    }
}
