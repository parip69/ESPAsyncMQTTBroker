// @version: 1.4.150 Builddatum 18:53:45 02-04.2025
#include "ESPAsyncMQTTBroker.h"

// Implementierung der einfacheren publish-Methode, die die vollständigere Variante aufruft
bool ESPAsyncMQTTBroker::publish(const char *topic, uint8_t qos, bool retained, const char *payload)
{
    return publish(topic, qos, retained, payload, "");
}

ESPAsyncMQTTBroker::ESPAsyncMQTTBroker(uint16_t port) : server(NULL), port(port)
{
}

ESPAsyncMQTTBroker::~ESPAsyncMQTTBroker()
{
    stop();

    for (auto client : clients)
    {
        delete client;
    }
    clients.clear();

    for (auto msg : retainedMessages)
    {
        delete[] msg->payload;
        delete msg;
    }
    retainedMessages.clear();

    for (auto &pair : persistentSessions)
    {
        delete pair.second;
    }
    persistentSessions.clear();

    connectedClientsInfo.clear();
}

void ESPAsyncMQTTBroker::begin()
{
    server = new AsyncServer(port);
    server->onClient([](void *arg, AsyncClient *client)
                     {
        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;
        broker->onClient(client); }, this);
    server->begin();

    // Asynchroner Timer: prüft alle 1 Sekunde die Timeouts
    esp_timer_create_args_t timer_args;
    timer_args.callback = [](void *arg)
    {
        ESPAsyncMQTTBroker *broker = (ESPAsyncMQTTBroker *)arg;
        broker->checkTimeouts();
    };
    timer_args.arg = this;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "mqtt_timeout_timer";
    esp_timer_create(&timer_args, &timeoutTimer);
    esp_timer_start_periodic(timeoutTimer, 1000000); // 1 Sekunde
}

void ESPAsyncMQTTBroker::stop()
{
    if (timeoutTimer)
    {
        esp_timer_stop(timeoutTimer);
        esp_timer_delete(timeoutTimer);
        timeoutTimer = NULL;
    }

    if (server)
    {
        server->end();
        delete server;
        server = NULL;
    }
}

void ESPAsyncMQTTBroker::checkTimeouts()
{
    uint32_t now = millis();
    for (auto it = clients.begin(); it != clients.end(); ++it)
    {
        MQTTClient *mqttClient = *it;
        if (mqttClient->connected && mqttClient->keepAlive > 0)
        {
            // Timeout: 1,5 × KeepAlive (in Millisekunden)
            if (now - mqttClient->lastActivity > mqttClient->keepAlive * 1500UL)
            {
                if (debugLevel >= DEBUG_INFO)
                {
                    Serial.println(String("⏰ Client ") + mqttClient->clientId + " inaktiv, trenne Verbindung.");
                }
                mqttClient->client->close();
            }
        }
    }
}

void ESPAsyncMQTTBroker::setConfig(const ESPAsyncMQTTBrokerConfig &config)
{
    brokerConfig = config;
    if (debugLevel >= DEBUG_INFO)
    {
        Serial.println(String("🔧 MQTT-Broker Konfiguration:"));
        Serial.println(String("   Username: ") + (brokerConfig.username.isEmpty() ? "[leer]" : brokerConfig.username));
        Serial.println(String("   Passwort: ") + (brokerConfig.password.isEmpty() ? "[leer]" : "[gesetzt]"));
        Serial.println(String("   Auth erforderlich: ") + (brokerConfig.username != "" ? "Ja" : "Nein"));
    }
}

void ESPAsyncMQTTBroker::onClient(AsyncClient *client)
{
    MQTTClient *mqttClient = new MQTTClient();
    mqttClient->client = client;
    mqttClient->connected = false;
    mqttClient->lastActivity = millis();
    mqttClient->keepAlive = 0; // Wird im CONNECT gesetzt
    mqttClient->cleanSession = true;

    client->onData([](void *arg, AsyncClient *client, void *data, size_t len)
                   {
        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;
        MQTTClient* mqttClient = nullptr;
        for (auto c : broker->clients) {
            if (c->client == client) {
                mqttClient = c;
                break;
            }
        }
        if (mqttClient) {
            broker->processPacket(mqttClient, (uint8_t*)data, len);
            mqttClient->lastActivity = millis();
        } }, this);

    client->onDisconnect([](void *arg, AsyncClient *client)
                         {
        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;
        MQTTClient* target = nullptr;
        for (auto it = broker->clients.begin(); it != broker->clients.end(); ++it) {
            if ((*it)->client == client) {
                target = *it;
                if (!target->cleanSession) {
                    broker->persistentSessions[target->clientId] = target;
                    target->client = nullptr;
                } else {
                    if (broker->clientDisconnectCallback) {
                        broker->clientDisconnectCallback(target->clientId);
                    }
                    // Entferne Client aus der Client-Informationen-Map
                    broker->connectedClientsInfo.erase(target->clientId);
                    delete target;
                    broker->clients.erase(it);
                }
                break;
            }
        } }, this);

    client->onError([](void *arg, AsyncClient *client, int8_t error)
                    {
        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;
        MQTTClient* mqttClient = nullptr;
        for (auto c : broker->clients) {
            if (c->client == client) {
                mqttClient = c;
                break;
            }
        }
        if (broker->errorCallback && mqttClient) {
            broker->errorCallback(mqttClient->clientId, error, "Client Error");
        } }, this);

    // Den onTimeOut-Callback entfernen, da der Timer den Timeout prüft.

    clients.push_back(mqttClient);
}

void ESPAsyncMQTTBroker::processPacket(MQTTClient *client, uint8_t *data, size_t len)
{
    if (len < 2)
        return;
    uint8_t header = data[0];
    uint8_t packetType = (header >> 4) & 0x0F;

    // Remaining Length decodieren
    size_t multiplier = 1;
    size_t value = 0;
    uint8_t encodedByte;
    size_t idx = 1;
    do
    {
        if (idx >= len)
            return;
        encodedByte = data[idx++];
        value += (encodedByte & 127) * multiplier;
        multiplier *= 128;
        if (multiplier > 128 * 128 * 128)
            return;
    } while ((encodedByte & 128) != 0);

    if (len < idx + value)
        return;

    switch (packetType)
    {
    case MQTT_CONNECT:
        handleConnect(client, data + idx, value);
        break;

    case MQTT_PUBLISH:
        handlePublish(client, data + idx, value, header);
        break;

    case MQTT_PUBACK:
        // 🚫 QoS 1-Ack vom Client – wir ignorieren es einfach
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

    case MQTT_PUBREC:
        handlePubRec(client, data + idx, value);
        break;

    case MQTT_PUBREL:
        handlePubRel(client, data + idx, value);
        break;

    case MQTT_PUBCOMP:
        handlePubComp(client, data + idx, value);
        break;

    default:
        // Alle anderen Typen, die wir nicht brauchen, ignorieren
        // (keine Fehlermeldung mehr für PacketType 4)
        break;
    }
}

void ESPAsyncMQTTBroker::handleConnect(MQTTClient *client, uint8_t *data, uint32_t length)
{
    if (debugLevel >= DEBUG_DEBUG)
    {
        Serial.println("\n🔍 MQTT CONNECT Paket empfangen:");
    }
    if (length < 10)
    {
        if (debugLevel >= DEBUG_ERROR)
        {
            Serial.println("❌ Paket zu kurz!");
        }
        return;
    }

    // Protokoll-Version überprüfen
    uint8_t protocolLevel = data[2 + (data[0] << 8 | data[1])];
    client->protocolVersion = protocolLevel;

    if (debugLevel >= DEBUG_DEBUG)
    {
        Serial.printf("Protokoll-Version: %d (%s)\n",
                      protocolLevel,
                      protocolLevel == MQTT_PROTOCOL_LEVEL_5 ? "MQTT 5.0" : protocolLevel == MQTT_PROTOCOL_LEVEL ? "MQTT 3.1.1"
                                                                                                                 : "Unbekannt");
    }

    uint16_t protocolNameLength = (data[0] << 8) | data[1];
    if (protocolNameLength + 2 > length)
    {
        if (debugLevel >= DEBUG_ERROR)
        {
            Serial.println("❌ Paket zu kurz für Protokollnamen!");
        }
        return;
    }

    char protocolName[50] = {0};
    if (protocolNameLength < 50)
    {
        memcpy(protocolName, data + 2, protocolNameLength);
        if (debugLevel >= DEBUG_DEBUG)
        {
            Serial.printf("Protokoll: %s\n", protocolName);
        }
    }

    size_t offset = 2 + protocolNameLength + 1; // Protokoll-Level
    if (offset >= length)
    {
        if (debugLevel >= DEBUG_ERROR)
        {
            Serial.println("❌ Paket zu kurz für CONNECT-Flags!");
        }
        return;
    }

    uint8_t connectFlags = data[offset];
    bool cleanSession = (connectFlags & 0x02) != 0;
    bool willFlag = (connectFlags & 0x04) != 0;
    // willQoS und willRetain nicht benötigt – entfernen
    bool passwordFlag = (connectFlags & 0x40) != 0;
    bool usernameFlag = (connectFlags & 0x80) != 0;

    offset++;
    if (offset + 2 > length)
    {
        if (debugLevel >= DEBUG_ERROR)
        {
            Serial.println("❌ Paket zu kurz für Keep-Alive!");
        }
        return;
    }
    uint16_t keepAlive = (data[offset] << 8) | data[offset + 1];
    offset += 2;

    if (offset + 2 > length)
    {
        if (debugLevel >= DEBUG_ERROR)
        {
            Serial.println("❌ Paket zu kurz für Client-ID!");
        }
        return;
    }
    uint16_t clientIdLength = (data[offset] << 8) | data[offset + 1];
    offset += 2;

    if (offset + clientIdLength > length)
    {
        if (debugLevel >= DEBUG_ERROR)
        {
            Serial.println("❌ Paket zu kurz für vollständige Client-ID!");
        }
        return;
    }

    char clientIdBuffer[256];
    memcpy(clientIdBuffer, data + offset, clientIdLength);
    clientIdBuffer[clientIdLength] = 0;
    String clientId = String(clientIdBuffer);
    client->clientId = clientId;
    offset += clientIdLength;

    if (!cleanSession && persistentSessions.find(clientId) != persistentSessions.end())
    {
        if (debugLevel >= DEBUG_INFO)
        {
            Serial.println(String("♻️ Wiederverwende persistente Session für Client: ") + clientId);
        }
        MQTTClient *persistent = persistentSessions[clientId];
        client->subscriptions = persistent->subscriptions;
        persistentSessions.erase(clientId);
    }
    client->cleanSession = cleanSession;
    client->keepAlive = keepAlive;

    if (debugLevel >= DEBUG_DEBUG)
    {
        Serial.printf("CONNECT Flags: 0x%02X, Clean Session: %s, Keep-Alive: %d Sekunden\n",
                      connectFlags, (cleanSession ? "Ja" : "Nein"), keepAlive);
        Serial.println(String("Client-ID: '") + clientId + "'");
    }

    if (willFlag)
    {
        if (offset + 2 > length)
        {
            if (debugLevel >= DEBUG_ERROR)
            {
                Serial.println("❌ Paket zu kurz für Will-Topic-Länge!");
            }
            return;
        }
        uint16_t willTopicLen = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (offset + willTopicLen > length)
        {
            if (debugLevel >= DEBUG_ERROR)
            {
                Serial.println("❌ Paket zu kurz für Will-Topic!");
            }
            return;
        }
        char willTopicBuffer[256] = {0};
        memcpy(willTopicBuffer, data + offset, willTopicLen);
        offset += willTopicLen;

        if (offset + 2 > length)
        {
            if (debugLevel >= DEBUG_ERROR)
            {
                Serial.println("❌ Paket zu kurz für Will-Message-Länge!");
            }
            return;
        }
        uint16_t willMsgLen = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (offset + willMsgLen > length)
        {
            if (debugLevel >= DEBUG_ERROR)
            {
                Serial.println("❌ Paket zu kurz für Will-Message!");
            }
            return;
        }
        char willMsgBuffer[256] = {0};
        memcpy(willMsgBuffer, data + offset, willMsgLen);
        offset += willMsgLen;
        if (debugLevel >= DEBUG_DEBUG)
        {
            Serial.printf("Will Topic: '%s', Will Message: '%s'\n", willTopicBuffer, willMsgBuffer);
        }
    }

    String username = "";
    String password = "";
    if (usernameFlag)
    {
        if (offset + 2 > length)
        {
            if (debugLevel >= DEBUG_ERROR)
            {
                Serial.println("❌ Paket zu kurz für Username-Länge!");
            }
            return;
        }
        uint16_t usernameLen = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (offset + usernameLen > length)
        {
            if (debugLevel >= DEBUG_ERROR)
            {
                Serial.println("❌ Paket zu kurz für Username!");
            }
            return;
        }
        char usernameBuffer[256];
        memcpy(usernameBuffer, data + offset, usernameLen);
        usernameBuffer[usernameLen] = 0;
        username = String(usernameBuffer);
        offset += usernameLen;
    }

    if (passwordFlag)
    {
        if (offset + 2 > length)
        {
            if (debugLevel >= DEBUG_ERROR)
            {
                Serial.println("❌ Paket zu kurz für Password-Länge!");
            }
            return;
        }
        uint16_t passwordLen = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (offset + passwordLen > length)
        {
            if (debugLevel >= DEBUG_ERROR)
            {
                Serial.println("❌ Paket zu kurz für Password!");
            }
            return;
        }
        char passwordBuffer[256];
        memcpy(passwordBuffer, data + offset, passwordLen);
        passwordBuffer[passwordLen] = 0;
        password = String(passwordBuffer);
    }

    if (debugLevel >= DEBUG_DEBUG)
    {
        Serial.println("Authentifizierung wird überprüft...");
    }

    if (!authenticateClient(username, password))
    {
        if (debugLevel >= DEBUG_ERROR)
        {
            Serial.println(String("🚫 Authentifizierung fehlgeschlagen - Verbindung abgelehnt"));
        }
        uint8_t connack[] = {0x20, 0x02, 0x00, 0x05};
        client->client->write((const char *)connack, 4);
        return;
    }
    else
    {
        if (debugLevel >= DEBUG_INFO)
        {
            Serial.println(String("✅ Authentifizierung erfolgreich - Verbindung akzeptiert"));
        }
    }

    uint8_t connack[] = {
        0x20,
        0x02,
        (uint8_t)(cleanSession ? 0x00 : 0x01),
        0x00};
    client->client->write((const char *)connack, 4);
    client->connected = true;

    if (clientConnectCallback)
    {
        IPAddress ip = client->client->remoteIP();
        String ipStr = String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
        clientConnectCallback(client->clientId, ipStr);

        // Speichere Client-Informationen
        connectedClientsInfo[client->clientId] = ipStr;
    }

    sendRetainedMessages(client);
}

void ESPAsyncMQTTBroker::handlePublish(MQTTClient *client, uint8_t *data, uint32_t length, uint8_t header)
{
    uint8_t qos = (header & 0x06) >> 1;
    bool retained = (header & 0x01) != 0;

    if (length < 2)
        return;
    uint16_t topicLength = (data[0] << 8) | data[1];
    if (2 + topicLength > length)
        return;

    char topicBuffer[256];
    memcpy(topicBuffer, data + 2, topicLength);
    topicBuffer[topicLength] = 0;
    String topic = String(topicBuffer);

    size_t payloadOffset = 2 + topicLength;
    uint16_t packetId = 0;
    if (qos > 0)
    {
        if (payloadOffset + 2 > length)
            return;
        packetId = (data[payloadOffset] << 8) | data[payloadOffset + 1];
        payloadOffset += 2;
        if (qos == 1)
        {
            uint8_t puback[] = {
                0x40, 0x02,
                (uint8_t)(packetId >> 8),
                (uint8_t)packetId};
            client->client->write((const char *)puback, 4);
        }
    }

    uint32_t payloadLength = length - payloadOffset;
    if (payloadLength > 0)
    {
        uint8_t payloadBuffer[1024];
        size_t copyLength = min(payloadLength, (uint32_t)1023);
        memcpy(payloadBuffer, data + payloadOffset, copyLength);
        payloadBuffer[copyLength] = 0;
        String payloadStr = String((char *)payloadBuffer);
        // DEBUG: eingehende Nachricht
        Serial.printf("🔔 handlePublish – Topic='%s', Payload='%s'\n", topic.c_str(), payloadStr.c_str());

        if (messageCallback)
        {
            messageCallback(client->clientId, topic, payloadStr);
        } // Verwende die erweiterte publish-Methode mit noLocal-Unterstützung
        // Die Nachricht weiterleiten, aber den absendenden Client ausschließen
        publish(topic.c_str(), qos, retained, payloadStr.c_str(), client->clientId);
    }
}

void ESPAsyncMQTTBroker::handleSubscribe(MQTTClient *client, uint8_t *data, uint32_t length)
{
    if (length < 2)
        return;

    uint16_t packetId = (data[0] << 8) | data[1];
    size_t index = 2;
    std::vector<uint8_t> returnCodes;

    while (index < length)
    {
        if (index + 2 > length)
            break;
        uint16_t topicLength = (data[index] << 8) | data[index + 1];
        index += 2;
        if (index + topicLength > length)
            break;
        char topicBuffer[256];
        memcpy(topicBuffer, data + index, topicLength);
        topicBuffer[topicLength] = 0;
        String topic = String(topicBuffer);
        index += topicLength;
        if (index >= length)
            break;

        // Options-Byte lesen (bei MQTT 5.0 Subscription Options)
        uint8_t options = data[index++];
        uint8_t requestedQoS = options & 0x03;
        bool noLocal = (options & 0x04) != 0; // Bit 2 = noLocal

        if (debugLevel >= DEBUG_DEBUG)
        {
            Serial.printf("Subscribe: Topic '%s', QoS %d, noLocal: %s\n",
                          topicBuffer, requestedQoS, noLocal ? "true" : "false");
        }

        // Neue Subscription-Struktur verwenden
        Subscription sub;
        sub.filter = topic;
        sub.noLocal = noLocal;
        client->subscriptions.push_back(sub);

        returnCodes.push_back(requestedQoS);

        if (subscribeCallback)
        {
            subscribeCallback(client->clientId, topic);
        }
    }

    size_t subackLength = 2 + returnCodes.size();
    uint8_t *suback = new uint8_t[2 + subackLength];
    suback[0] = MQTT_SUBACK << 4;
    suback[1] = subackLength;
    suback[2] = packetId >> 8;
    suback[3] = packetId & 0xFF;

    for (size_t i = 0; i < returnCodes.size(); i++)
    {
        suback[4 + i] = returnCodes[i];
    }

    client->client->write((const char *)suback, 2 + subackLength);
    delete[] suback;

    sendRetainedMessages(client);
}

void ESPAsyncMQTTBroker::handleUnsubscribe(MQTTClient *client, uint8_t *data, uint32_t length)
{
    uint16_t packetId = (data[0] << 8) | data[1];

    uint8_t unsuback[4] = {
        0xB0,
        0x02,
        (uint8_t)(packetId >> 8),
        (uint8_t)packetId};
    client->client->write((const char *)unsuback, 4);

    size_t index = 2;
    while (index < length)
    {
        if (index + 2 > length)
            break;
        uint16_t topicLength = (data[index] << 8) | data[index + 1];
        index += 2;
        if (index + topicLength > length)
            break;
        char topicBuffer[256];
        memcpy(topicBuffer, data + index, topicLength);
        topicBuffer[topicLength] = 0;
        String topic = String(topicBuffer);
        index += topicLength;
        for (auto it = client->subscriptions.begin(); it != client->subscriptions.end();)
        {
            if (it->filter == topic)
            {
                if (unsubscribeCallback)
                {
                    unsubscribeCallback(client->clientId, topic);
                }
                it = client->subscriptions.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}

void ESPAsyncMQTTBroker::handlePingReq(MQTTClient *client)
{
    uint8_t pingresp[] = {0xD0, 0x00};
    client->client->write((const char *)pingresp, 2);
}

void ESPAsyncMQTTBroker::handleDisconnect(MQTTClient *client)
{
    client->connected = false;
    if (client->cleanSession && client->client)
    {
        client->client->close();
    }
}

void ESPAsyncMQTTBroker::handlePubRec(MQTTClient *client, uint8_t *data, size_t len)
{
    if (len < 2)
        return;
    uint16_t packetId = (data[0] << 8) | data[1];
    uint8_t pubrel[] = {
        0x62, 0x02,
        (uint8_t)(packetId >> 8),
        (uint8_t)packetId};
    client->client->write((const char *)pubrel, sizeof(pubrel));
}

void ESPAsyncMQTTBroker::handlePubRel(MQTTClient *client, uint8_t *data, size_t len)
{
    if (len < 2)
        return;
    uint16_t packetId = (data[0] << 8) | data[1];
    uint8_t pubcomp[] = {
        0x70, 0x02,
        (uint8_t)(packetId >> 8),
        (uint8_t)packetId};
    client->client->write((const char *)pubcomp, sizeof(pubcomp));
}

void ESPAsyncMQTTBroker::handlePubComp(MQTTClient *client, uint8_t *data, size_t len)
{
    // Für QoS 2 Abschluss keine weitere Aktion erforderlich
}

bool ESPAsyncMQTTBroker::topicMatches(const Subscription &subscription, const String &topic)
{
    return topicMatches(subscription.filter, topic);
}

bool ESPAsyncMQTTBroker::topicMatches(const String &subscription, const String &topic)
{
    // Teile die Subscription in Ebenen auf
    std::vector<String> subLevels;
    int start = 0;
    int pos = 0;
    while ((pos = subscription.indexOf('/', start)) != -1)
    {
        subLevels.push_back(subscription.substring(start, pos));
        start = pos + 1;
    }
    subLevels.push_back(subscription.substring(start));

    // Teile das Topic in Ebenen auf
    std::vector<String> topicLevels;
    start = 0;
    while ((pos = topic.indexOf('/', start)) != -1)
    {
        topicLevels.push_back(topic.substring(start, pos));
        start = pos + 1;
    }
    topicLevels.push_back(topic.substring(start));

    // Vergleiche die Ebenen
    int i = 0;
    for (; i < subLevels.size(); i++)
    {
        String subPart = subLevels[i];
        // Der Mehr-Ebenen-Wildcard '#' muss als letztes Element stehen und matcht alle restlichen Ebenen
        if (subPart == "#")
        {
            return true;
        }
        // Die Ein-Ebenen-Wildcard '+' matcht genau eine Ebene
        if (subPart == "+")
        {
            // Es muss eine entsprechende Topic-Ebene vorhanden sein
            if (i >= topicLevels.size())
                return false;
            // Gehe zur nächsten Ebene
            continue;
        }
        // Für alle anderen Fälle muss die Ebene exakt übereinstimmen
        if (i >= topicLevels.size() || subPart != topicLevels[i])
        {
            return false;
        }
    }
    // Nach Vergleich aller Subscription-Ebenen darf es keine zusätzlichen Topic-Ebenen geben
    return (i == topicLevels.size());
}

void ESPAsyncMQTTBroker::sendRetainedMessages(MQTTClient *client)
{
    for (auto &msg : retainedMessages)
    {
        for (auto &sub : client->subscriptions)
        {
            if (topicMatches(sub, msg->topic))
            {
                size_t topicLength = msg->topic.length();
                size_t totalLength = 1 + 1 + 2 + topicLength + msg->length;
                if (totalLength <= MQTT_MAX_PACKET_SIZE)
                {
                    uint8_t packet[MQTT_MAX_PACKET_SIZE];
                    packet[0] = (MQTT_PUBLISH << 4) | 0x01;
                    packet[1] = totalLength - 2;
                    packet[2] = topicLength >> 8;
                    packet[3] = topicLength & 0xFF;
                    memcpy(packet + 4, msg->topic.c_str(), topicLength);
                    memcpy(packet + 4 + topicLength, msg->payload, msg->length);
                    client->client->write((const char *)packet, totalLength);
                }
                else
                {
                    uint8_t *packet = new uint8_t[totalLength];
                    packet[0] = (MQTT_PUBLISH << 4) | 0x01;
                    packet[1] = totalLength - 2;
                    packet[2] = topicLength >> 8;
                    packet[3] = topicLength & 0xFF;
                    memcpy(packet + 4, msg->topic.c_str(), topicLength);
                    memcpy(packet + 4 + topicLength, msg->payload, msg->length);
                    client->client->write((const char *)packet, totalLength);
                    delete[] packet;
                }
                break;
            }
        }
    }
}

bool ESPAsyncMQTTBroker::authenticateClient(const String &username, const String &password)
{
    if (debugLevel >= DEBUG_DEBUG)
    {
        Serial.println("🔐 Authentifizierungsversuch:");
        Serial.println(String("   • Empfangener Username: '") + username + "'");
        Serial.println(String("   • Passwort: ") + (password.isEmpty() ? "[leer]" : "********"));
        Serial.println(String("   • Konfigurierter Username: '") + brokerConfig.username + "'");
        Serial.println(String("   • Konfiguriertes Passwort: ") + (brokerConfig.password.isEmpty() ? "[leer]" : "********"));
    }

    // 1. Wenn kein Username konfiguriert ist → anonyme Verbindung erlauben
    if (brokerConfig.username.isEmpty())
    {
        if (debugLevel >= DEBUG_INFO)
        {
            Serial.println("✅ Anonymer Zugriff erlaubt (kein Benutzername konfiguriert)");
        }
        return true;
    }

    // 2. Wenn konfigurierter Username vorhanden, aber Client sendet keinen
    if (username.isEmpty())
    {
        if (debugLevel >= DEBUG_ERROR)
        {
            Serial.println("❌ Verbindung ohne Benutzername nicht erlaubt");
        }
        return false;
    }

    // 3. Username muss übereinstimmen
    if (username != brokerConfig.username)
    {
        if (debugLevel >= DEBUG_ERROR)
        {
            Serial.println("❌ Falscher Benutzername");
        }
        return false;
    }

    // 4. Wenn Passwort nicht konfiguriert ist → Username reicht aus
    if (brokerConfig.password.isEmpty())
    {
        if (debugLevel >= DEBUG_INFO)
        {
            Serial.println("✅ Benutzername korrekt, kein Passwort erforderlich");
        }
        return true;
    }

    // 5. Wenn Passwort konfiguriert ist → muss exakt stimmen
    if (password != brokerConfig.password)
    {
        if (debugLevel >= DEBUG_ERROR)
        {
            Serial.println("❌ Falsches Passwort");
        }
        return false;
    }

    if (debugLevel >= DEBUG_INFO)
    {
        Serial.println("✅ Benutzername und Passwort korrekt");
    }
    return true;
}

bool ESPAsyncMQTTBroker::publish(const char *topic,
                                 uint8_t qos,
                                 bool retained,
                                 const char *payload,
                                 const String &excludeClientId)
{
    // INFO-Log
    if (debugLevel >= DEBUG_INFO)
    {
        Serial.printf("📤 Broker veröffentlicht auf Topic '%s': %s\n", topic, payload);
        if (!excludeClientId.isEmpty())
        {
            Serial.printf("   - Ausgeschlossener Client: %s\n", excludeClientId.c_str());
        }
    }

    String topicStr = String(topic);
    size_t payloadLen = strlen(payload);

    // Retained-Nachrichten verwalten
    if (retained)
    {
        for (auto it = retainedMessages.begin(); it != retainedMessages.end();)
        {
            if ((*it)->topic == topicStr)
            {
                delete[] (*it)->payload;
                delete *it;
                it = retainedMessages.erase(it);
            }
            else
            {
                ++it;
            }
        }
        if (payloadLen > 0)
        {
            RetainedMessage *msg = new RetainedMessage();
            msg->topic = topicStr;
            msg->payload = new uint8_t[payloadLen];
            memcpy(msg->payload, payload, payloadLen);
            msg->length = payloadLen;
            msg->qos = qos;
            retainedMessages.push_back(msg);
        }
    }

    // --- PATCH: Header mit QoS- und Retain-Bits ---
    uint8_t header = (MQTT_PUBLISH << 4) | (qos << 1) | (retained ? 1 : 0);

    bool messageSent = false;
    int clientCount = 0;
    int sentCount = 0;

    for (auto c : clients)
    {
        if (!c->connected)
            continue;

        clientCount++;
        bool sentToThisClient = false;

        // --- PATCH: Sender ausschließen, ganz unabhängig von ignoreLoopDeliver ---
        if (!excludeClientId.isEmpty() && c->clientId == excludeClientId)
        {
            continue;
        }

        // Durch alle Subscriptions des Clients iterieren
        for (auto &sub : c->subscriptions)
        {
            if (debugLevel >= DEBUG_DEBUG)
            {
                Serial.printf("🔍 Prüfe Topic-Match für Client %s: Abo='%s', Eingang='%s'\n",
                              c->clientId.c_str(), sub.filter.c_str(), topicStr.c_str());
            }

            bool matched = topicMatches(sub.filter, topicStr);
            if (debugLevel >= DEBUG_DEBUG)
            {
                Serial.printf("  - Match: %s\n", matched ? "✅ JA" : "❌ NEIN");
            }

            if (matched)
            {
                // noLocal-Flag: falls gesetzt, nochmals sicherstellen, dass der Sender nicht bekommt
                if (sub.noLocal && c->clientId == excludeClientId)
                {
                    if (debugLevel >= DEBUG_DEBUG)
                    {
                        Serial.printf("  - Client %s wird übersprungen (noLocal)\n", c->clientId.c_str());
                    }
                    break;
                }

                size_t topicLen = topicStr.length();
                size_t remainingLength = 2 + topicLen + payloadLen;

                // Paket zusammenbauen
                uint8_t *packet = new uint8_t[1 + 1 + remainingLength];
                packet[0] = header;
                packet[1] = remainingLength; // bleibt < 128 Bytes
                packet[2] = topicLen >> 8;
                packet[3] = topicLen & 0xFF;
                memcpy(packet + 4, topicStr.c_str(), topicLen);
                memcpy(packet + 4 + topicLen, payload, payloadLen);

                if (debugLevel >= DEBUG_DEBUG)
                {
                    Serial.printf("📦 Sende Paket an Client ID: %s (Länge: %d)\n",
                                  c->clientId.c_str(), 1 + 1 + remainingLength);
                }

                bool writeSuccess = c->client->write((const char *)packet, 1 + 1 + remainingLength);
                delete[] packet;

                if (writeSuccess)
                {
                    sentCount++;
                    sentToThisClient = true;
                    messageSent = true;
                }

                if (debugLevel >= DEBUG_DEBUG)
                {
                    Serial.printf("  - Senden %s\n", writeSuccess ? "erfolgreich" : "fehlgeschlagen");
                }

                break; // pro Client nur einmal senden
            }
        }

        if (debugLevel >= DEBUG_DEBUG && !sentToThisClient)
        {
            Serial.printf("  - Client %s hat keine passenden Subscriptions für Topic %s\n",
                          c->clientId.c_str(), topicStr.c_str());
        }
    }

    if (debugLevel >= DEBUG_INFO)
    {
        Serial.printf("📊 Nachricht gesendet an %d von %d verbundenen Clients\n", sentCount, clientCount);
    }

    return messageSent;
}
