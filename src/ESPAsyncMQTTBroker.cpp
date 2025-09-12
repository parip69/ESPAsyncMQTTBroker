// @version: 1.5.1
#include "ESPAsyncMQTTBroker.h"
#include <cstdarg>

// Zentrale Logging-Funktion.
void ESPAsyncMQTTBroker::logMessage(DebugLevel level, const char *format, ...)
{
    if (level <= debugLevel)
    {
        char buffer[256];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);

        String message = buffer;

        // Log ausgeben (Serial oder über Callback)
        if (level <= DEBUG_ERROR)
        {
            Serial.print("❌ ");
        }
        else if (level <= DEBUG_INFO)
        {
            Serial.print("ℹ️ ");
        }
        else
        {
            Serial.print("🔍 ");
        }

        Serial.println(message);

        // Wenn verfügbar, auch an Callback weiterleiten
        if (loggingCallback)
        {
            loggingCallback(level, message);
        }
    }
}

ESPAsyncMQTTBroker::ESPAsyncMQTTBroker(uint16_t port) : port(port)
{
}

ESPAsyncMQTTBroker::~ESPAsyncMQTTBroker()
{
    stop();
    clients.clear();
    retainedMessages.clear();
    persistentSessions.clear();
    connectedClientsInfo.clear();
}

void ESPAsyncMQTTBroker::begin()
{
    server.reset(new AsyncServer(port));
    server->onClient([](void *arg, AsyncClient *client)
                     {
        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;
        broker->onClient(client); }, this);
    server->begin();

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
    esp_timer_start_periodic(timeoutTimer, 1000000); // 1 second
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
        server.reset();
    }
}

void ESPAsyncMQTTBroker::checkTimeouts()
{
    uint32_t now = millis();
    for (auto it = clients.begin(); it != clients.end();)
    {
        auto &mqttClient = it->second;
        if (mqttClient->connected && mqttClient->keepAlive > 0)
        {
            if (now - mqttClient->lastActivity > mqttClient->keepAlive * 1500UL)
            {
                logMessage(DEBUG_INFO, "Client ⏰  inactive, disconnecting.", mqttClient->clientId.c_str());
                mqttClient->client->close(); // This will trigger onDisconnect and remove the client
                ++it;
            }
            else
            {
                ++it;
            }
        }
        else
        {
            ++it;
        }
    }
}

void ESPAsyncMQTTBroker::setConfig(const ESPAsyncMQTTBrokerConfig &config)
{
    brokerConfig = config;
    if (brokerConfig.log) {
        setDebugLevel(DEBUG_INFO);
    } else {
        setDebugLevel(DEBUG_NONE);
    }
    logMessage(DEBUG_INFO, "🔧 MQTT-Broker Configuration:");
    logMessage(DEBUG_INFO, "   Username: %s", (brokerConfig.username.isEmpty() ? "[empty]" : brokerConfig.username.c_str()));
    logMessage(DEBUG_INFO, "   Password: %s", (brokerConfig.password.isEmpty() ? "[empty]" : "[set]"));
    logMessage(DEBUG_INFO, "   Auth required: %s", (brokerConfig.username != "" ? "Yes" : "No"));
}

void ESPAsyncMQTTBroker::onClient(AsyncClient *client)
{
    auto mqttClient = std::unique_ptr<MQTTClient>(new MQTTClient());
    mqttClient->client = client;
    mqttClient->connected = false;
    mqttClient->lastActivity = millis();
    mqttClient->keepAlive = 0;
    mqttClient->cleanSession = true;
    mqttClient->hasWill = false;
    mqttClient->gracefulDisconnect = false;
    mqttClient->willQos = 0;
    mqttClient->willRetain = false;
    mqttClient->willPayloadLen = 0;

    client->onData([](void *arg, AsyncClient *client, void *data, size_t len)
                   {
        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;
        auto it = broker->clients.find(client);
        if (it != broker->clients.end()) {
            MQTTClient* mqttClient = it->second.get();
            if (len > MQTT_MAX_PACKET_SIZE) {
                broker->logMessage(DEBUG_ERROR, "Packet size exceeds limit: %u > %u", (unsigned)len, (unsigned)MQTT_MAX_PACKET_SIZE);
                return;
            }
            broker->processPacket(mqttClient, (uint8_t*)data, len);
            mqttClient->lastActivity = millis();
        } }, this);

    client->onDisconnect([](void *arg, AsyncClient *client)
                         {
        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;
        auto it = broker->clients.find(client);
        if (it != broker->clients.end()) {
            auto& target = it->second;

            if (target->hasWill && !target->gracefulDisconnect) {
                broker->logMessage(DEBUG_INFO, "Unclean disconnect from client %s. Publishing LWT: Topic='%s', QoS=%d, Retain=%s",
                                   target->clientId.c_str(), target->willTopic.c_str(), target->willQos, target->willRetain ? "Yes" : "No");
                broker->publish(target->willTopic.c_str(), target->willPayload.get(), target->willPayloadLen, target->willRetain, target->willQos, "");
                target->hasWill = false;
            } else if (target->hasWill && target->gracefulDisconnect) {
                broker->logMessage(DEBUG_DEBUG, "LWT for client %s not sent (clean disconnect already handled).", target->clientId.c_str());
            }

            if (!target->cleanSession) {
                broker->logMessage(DEBUG_INFO, "Client %s disconnected (graceful: %s), session will be kept.",
                                 target->clientId.c_str(), target->gracefulDisconnect ? "Yes" : "No");
                broker->persistentSessions[target->clientId] = std::move(target);
            } else {
                broker->logMessage(DEBUG_INFO, "Client %s disconnected (graceful: %s), Clean Session, removing client.",
                                 target->clientId.c_str(), target->gracefulDisconnect ? "Yes" : "No");
                if (broker->clientDisconnectCallback) {
                    broker->clientDisconnectCallback(target->clientId);
                }
                broker->connectedClientsInfo.erase(target->clientId);
            }
            broker->clients.erase(it);
        } }, this);

    client->onError([](void *arg, AsyncClient *client, int8_t error)
                    {
        ESPAsyncMQTTBroker* broker = (ESPAsyncMQTTBroker*)arg;
        auto it = broker->clients.find(client);
        if (it != broker->clients.end()) {
            MQTTClient* mqttClient = it->second.get();
            if (broker->errorCallback && mqttClient) {
                broker->logMessage(DEBUG_ERROR, "Client %s Error: %d", mqttClient->clientId.c_str(), error);
                broker->errorCallback(mqttClient->clientId, error, "Client Error");
            }
        } }, this);

    clients[client] = std::move(mqttClient);

    logMessage(DEBUG_DEBUG, "New MQTT connection accepted (IP: %s)", client->remoteIP().toString().c_str());
}

void ESPAsyncMQTTBroker::processPacket(MQTTClient *client, uint8_t *data, size_t len)
{
    if (len < 2)
    {
        logMessage(DEBUG_ERROR, "Packet too short for header (len=%d)", len);
        return;
    }

    uint8_t header = data[0];
    uint8_t packetType = (header >> 4) & 0x0F;

    size_t multiplier = 1;
    size_t value = 0;
    uint8_t encodedByte;
    size_t idx = 1;
    do
    {
        if (idx >= len)
        {
            logMessage(DEBUG_ERROR, "Packet too short for full Remaining Length");
            return;
        }
        encodedByte = data[idx++];
        value += (encodedByte & 127) * multiplier;
        multiplier *= 128;
        if (multiplier > 128 * 128 * 128)
        {
            logMessage(DEBUG_ERROR, "Remaining Length has invalid format");
            return;
        }
    } while ((encodedByte & 128) != 0);

    if (len < idx + value)
    {
        logMessage(DEBUG_ERROR, "Packet incomplete or damaged");
        return;
    }

    switch (packetType)
    {
    case MQTT_CONNECT:
        handleConnect(client, data + idx, value);
        break;
    case MQTT_PUBLISH:
        handlePublish(client, data + idx, value, header);
        break;
    case MQTT_PUBACK:
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
        logMessage(DEBUG_DEBUG, "Unknown/unprocessed packet type: %d", packetType);
        break;
    }
}

void ESPAsyncMQTTBroker::handleConnect(MQTTClient *client, uint8_t *data, uint32_t length)
{
    logMessage(DEBUG_DEBUG, "🔍 MQTT CONNECT Paket empfangen:");

    if (length < 10)
    {
        logMessage(DEBUG_ERROR, "❌ Paket zu kurz!");
        return;
    }

    // Protokollnamen-Länge sicherstellen
    uint16_t protocolNameLength = (data[0] << 8) | data[1];
    if (protocolNameLength + 2 > length)
    {
        logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Protokollnamen!");
        return;
    }

    char protocolName[MQTT_MAX_TOPIC_SIZE] = {0};
    if (protocolNameLength < sizeof(protocolName))
    {
        memcpy(protocolName, data + 2, protocolNameLength);
        logMessage(DEBUG_DEBUG, "Protocol: %s", protocolName);
    }
    else
    {
        logMessage(DEBUG_ERROR, "Protocol name too long!");
        return;
    }

    size_t offset = 2 + protocolNameLength;
    if (offset >= length)
    {
        logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Protokoll-Level!");
        return;
    }

    uint8_t protocolLevel = data[offset];
    client->protocolVersion = protocolLevel;

    logMessage(DEBUG_DEBUG, "Protocol-Version: %d (%s)", protocolLevel, protocolLevel == MQTT_PROTOCOL_LEVEL_5 ? "MQTT 5.0" : protocolLevel == MQTT_PROTOCOL_LEVEL ? "MQTT 3.1.1" : "Unknown");

    offset++;
    if (offset >= length)
    {
        logMessage(DEBUG_ERROR, "❌ Paket zu kurz für CONNECT-Flags!");
        return;
    }

    uint8_t connectFlags = data[offset];
    bool cleanSession = (connectFlags & 0x02) != 0;
    bool willFlag = (connectFlags & 0x04) != 0;
    bool passwordFlag = (connectFlags & 0x40) != 0;
    bool usernameFlag = (connectFlags & 0x80) != 0;

    offset++;
    if (offset + 2 > length)
    {
        logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Keep-Alive!");
        return;
    }
    uint16_t keepAlive = (data[offset] << 8) | data[offset + 1];
    offset += 2;

    if (offset + 2 > length)
    {
        logMessage(DEBUG_ERROR, "❌ Paket zu kurz für Client-ID!");
        return;
    }
    uint16_t clientIdLength = (data[offset] << 8) | data[offset + 1];
    offset += 2;

    if (clientIdLength == 0 && !cleanSession)
    {
        logMessage(DEBUG_ERROR, "Client connection rejected: Empty ClientID not allowed for cleanSession=false.");
        uint8_t connack_rejected[] = {0x20, 0x02, 0x00, 0x02};
        client->client->write((const char *)connack_rejected, sizeof(connack_rejected));
        client->client->close();
        return;
    }

    if (offset + clientIdLength > length)
    {
        logMessage(DEBUG_ERROR, "❌ Paket zu kurz für vollständige Client-ID!");
        return;
    }

    char clientIdBuffer[256] = {0};
    if (clientIdLength < sizeof(clientIdBuffer))
    {
        memcpy(clientIdBuffer, data + offset, clientIdLength);
    }
    else
    {
        logMessage(DEBUG_ERROR, "Client-ID too long!");
        return;
    }

    String clientId = String(clientIdBuffer);
    client->clientId = clientId;
    offset += clientIdLength;

    bool sessionActuallyRestored = false;
    auto sessionIt = persistentSessions.find(clientId);
    if (!cleanSession && sessionIt != persistentSessions.end())
    {
        logMessage(DEBUG_INFO, "♻️ Wiederverwende persistente Session für Client: %s", clientId.c_str());
        client->subscriptions = sessionIt->second->subscriptions;
        persistentSessions.erase(sessionIt);
        sessionActuallyRestored = true;
    }

    client->cleanSession = cleanSession;
    client->keepAlive = keepAlive;

    logMessage(DEBUG_DEBUG, "CONNECT Flags: 0x%02X, Clean Session: %s, Keep-Alive: %d seconds", connectFlags, (cleanSession ? "Yes" : "No"), keepAlive);
    logMessage(DEBUG_DEBUG, "Client-ID: '%s'", clientId.c_str());

    if (willFlag)
    {
        client->hasWill = true;
        client->willQos = (connectFlags & 0x18) >> 3;
        client->willRetain = (connectFlags & 0x20) != 0;

        if (offset + 2 > length)
        {
            logMessage(DEBUG_ERROR, "❌ Packet too short for Will-Topic length!");
            client->client->close();
            return;
        }
        uint16_t willTopicLen = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (offset + willTopicLen > length)
        {
            logMessage(DEBUG_ERROR, "❌Packet too short for Will-Topic!");
            client->client->close();
            return;
        }
        if (willTopicLen > MQTT_MAX_TOPIC_SIZE)
        {
            logMessage(DEBUG_ERROR, "Will-Topic too long (%u > %u)! Closing connection.", willTopicLen, MQTT_MAX_TOPIC_SIZE);
            client->client->close();
            return;
        }
        char willTopicBuffer[MQTT_MAX_TOPIC_SIZE + 1] = {0};
        memcpy(willTopicBuffer, data + offset, willTopicLen);
        client->willTopic = String(willTopicBuffer);

        if (!isValidPublishTopic(client->willTopic))
        {
            logMessage(DEBUG_ERROR, "Invalid Will-Topic name: '%s' (contains wildcards). Closing connection.", client->willTopic.c_str());
            client->client->close();
            return;
        }
        offset += willTopicLen;

        if (offset + 2 > length)
        {
            logMessage(DEBUG_ERROR, "❌ Packet too short for Will-Payload length!");
            client->client->close();
            return;
        }
        uint16_t willPayloadActualLen = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (offset + willPayloadActualLen > length)
        {
            logMessage(DEBUG_ERROR, "❌ Packet too short for Will-Payload!");
            client->client->close();
            return;
        }

        client->willPayloadLen = willPayloadActualLen;
        size_t lenToCopy = willPayloadActualLen;

        if (willPayloadActualLen > MQTT_MAX_PAYLOAD_SIZE)
        {
            logMessage(DEBUG_WARNING, "Will-Payload will be truncated to %u (from %u)", MQTT_MAX_PAYLOAD_SIZE, willPayloadActualLen);
            lenToCopy = MQTT_MAX_PAYLOAD_SIZE;
            client->willPayloadLen = MQTT_MAX_PAYLOAD_SIZE;
        }

        if (lenToCopy > 0)
        {
            client->willPayload = std::unique_ptr<uint8_t[]>(new uint8_t[lenToCopy]);
            memcpy(client->willPayload.get(), data + offset, lenToCopy);
        }
        else
        {
            client->willPayload = nullptr;
        }
        offset += willPayloadActualLen;

        logMessage(DEBUG_DEBUG, "LWT registered: Topic='%s', QoS=%d, Retain=%s, PayloadLen=%u", client->willTopic.c_str(), client->willQos, client->willRetain ? "Yes" : "No", client->willPayloadLen);
    }
    else
    {
        client->hasWill = false;
    }

    String username = "";
    String password = "";
    if (usernameFlag)
    {
        if (offset + 2 > length)
        {
            logMessage(DEBUG_ERROR, "❌ Packet too short for Username length!");
            return;
        }
        uint16_t usernameLen = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (offset + usernameLen > length)
        {
            logMessage(DEBUG_ERROR, "❌ Packet too short for Username!");
            return;
        }

        char usernameBuffer[256] = {0};
        if (usernameLen < sizeof(usernameBuffer))
        {
            memcpy(usernameBuffer, data + offset, usernameLen);
            username = String(usernameBuffer);
        }
        else
        {
            logMessage(DEBUG_ERROR, "Username too long!");
            return;
        }
        offset += usernameLen;
    }

    if (passwordFlag)
    {
        if (offset + 2 > length)
        {
            logMessage(DEBUG_ERROR, "❌ Packet too short for Password length!");
            return;
        }
        uint16_t passwordLen = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        if (offset + passwordLen > length)
        {
            logMessage(DEBUG_ERROR, "❌ Packet too short for Password!");
            return;
        }

        char passwordBuffer[256] = {0};
        if (passwordLen < sizeof(passwordBuffer))
        {
            memcpy(passwordBuffer, data + offset, passwordLen);
            password = String(passwordBuffer);
        }
        else
        {
            logMessage(DEBUG_ERROR, "Password too long!");
            return;
        }
    }

    logMessage(DEBUG_DEBUG, "Checking authentication...");

    if (!authenticateClient(username, password))
    {
        logMessage(DEBUG_ERROR, "🚫 Authentication failed - connection rejected");

        uint8_t connack[] = {0x20, 0x02, 0x00, 0x05};
        client->client->write((const char *)connack, 4);
        return;
    }
    else
    {
        logMessage(DEBUG_INFO, "✅ Authentifizierung erfolgreich - Verbindung akzeptiert");
    }

    uint8_t connack[] = {0x20, 0x02, (uint8_t)(cleanSession ? 0x00 : (sessionActuallyRestored ? 0x01 : 0x00)), 0x00};
    client->client->write((const char *)connack, 4);
    client->connected = true;

    if (clientConnectCallback)
    {
        IPAddress ip = client->client->remoteIP();
        String ipStr = String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
        clientConnectCallback(client->clientId, ipStr);
        connectedClientsInfo[client->clientId] = ipStr;
    }

    sendRetainedMessages(client);
}

void ESPAsyncMQTTBroker::handlePublish(MQTTClient *client, uint8_t *data, uint32_t length, uint8_t header)
{
    uint8_t qos = (header & 0x06) >> 1;
    bool retained = (header & 0x01) != 0;

    if (length < 2)
    {
        logMessage(DEBUG_ERROR, "Publish packet too short");
        return;
    }

    uint16_t topicLength = (data[0] << 8) | data[1];
    if (2 + topicLength > length)
    {
        logMessage(DEBUG_ERROR, "Publish packet too short for topic");
        return;
    }
    if (topicLength > MQTT_MAX_TOPIC_SIZE)
    {
        logMessage(DEBUG_ERROR, "Topic too long: %u > %u", topicLength, MQTT_MAX_TOPIC_SIZE);
        return;
    }

    char topicBuffer[MQTT_MAX_TOPIC_SIZE] = {0};
    memcpy(topicBuffer, data + 2, topicLength);
    String topic = String(topicBuffer);

    if (!isValidPublishTopic(topic))
    {
        logMessage(DEBUG_ERROR, "Invalid Topic Name '%s' from client '%s'. Closing connection.", topic.c_str(), client->clientId.c_str());
        if (client->client)
        {
            client->client->close();
        }
        return;
    }

    size_t payloadOffset = 2 + topicLength;
    uint16_t packetId = 0;
    if (qos > 0)
    {
        if (payloadOffset + 2 > length)
        {
            logMessage(DEBUG_ERROR, "Publish packet too short for QoS Packet-ID");
            return;
        }
        packetId = (data[payloadOffset] << 8) | data[payloadOffset + 1];
        payloadOffset += 2;

        if (qos == 1)
        {
            uint8_t puback[] = {0x40, 0x02, (uint8_t)(packetId >> 8), (uint8_t)packetId};
            client->client->write((const char *)puback, 4);
        }
        else if (qos == 2)
        {
            uint32_t payloadLength = length - payloadOffset;
            if (payloadLength > MQTT_MAX_PAYLOAD_SIZE)
            {
                logMessage(DEBUG_WARNING, "QoS 2 Payload will be truncated to %u (from %u)", MQTT_MAX_PAYLOAD_SIZE, payloadLength);
                payloadLength = MQTT_MAX_PAYLOAD_SIZE;
            }

            IncomingQoS2Message qos2Msg(topic, data + payloadOffset, payloadLength, retained, client->clientId);
            incomingQoS2Messages[packetId] = std::move(qos2Msg);

            logMessage(DEBUG_INFO, "QoS 2 Publish received - Topic='%s', PacketID=%u. Sending PUBREC.", topic.c_str(), packetId);

            uint8_t pubrec[] = {(MQTT_PUBREC << 4), 0x02, (uint8_t)(packetId >> 8), (uint8_t)packetId};
            client->client->write((const char *)pubrec, 4);
            return;
        }
    }

    if (qos == 0)
    {
        uint32_t payloadLength = length - payloadOffset;
        if (payloadLength > 0)
        {
            if (payloadLength > MQTT_MAX_PAYLOAD_SIZE)
            {
                logMessage(DEBUG_WARNING, "Payload will be truncated to %u (from %u)", MQTT_MAX_PAYLOAD_SIZE, payloadLength);
                payloadLength = MQTT_MAX_PAYLOAD_SIZE;
            }

            char payloadBuffer[MQTT_MAX_PAYLOAD_SIZE + 1] = {0};
            memcpy(payloadBuffer, data + payloadOffset, payloadLength);
            payloadBuffer[payloadLength] = 0;

            String payloadStr = String(payloadBuffer);
            logMessage(DEBUG_INFO, "🔔 Publish (QoS 0) - Topic='%s', Payload='%s'", topic.c_str(), payloadStr.c_str());

            if (messageCallback)
            {
                messageCallback(client->clientId, topic, payloadStr);
            }

            publish(topic.c_str(), payloadStr.c_str(), retained, qos, client->clientId);
        }
        else if (retained)
        {
            logMessage(DEBUG_INFO, "Publish (QoS 0, empty Retained) - Topic='%s'", topic.c_str());
            if (messageCallback)
            {
                messageCallback(client->clientId, topic, "");
            }
            publish(topic.c_str(), "", retained, qos, client->clientId);
        }
    }
}

void ESPAsyncMQTTBroker::handleSubscribe(MQTTClient *client, uint8_t *data, uint32_t length)
{
    if (length < 2)
    {
        logMessage(DEBUG_ERROR, "Subscribe packet too short");
        return;
    }

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

        if (topicLength > MQTT_MAX_TOPIC_SIZE)
        {
            logMessage(DEBUG_ERROR, "Subscribe topic too long: %u > %u", topicLength, MQTT_MAX_TOPIC_SIZE);
            break;
        }

        char topicBuffer[MQTT_MAX_TOPIC_SIZE] = {0};
        memcpy(topicBuffer, data + index, topicLength);
        String topic = String(topicBuffer);
        index += topicLength;

        if (index >= length)
            break;

        uint8_t options = data[index++];
        uint8_t requestedQoS = options & 0x03;
        bool noLocal = (options & 0x04) != 0;

        logMessage(DEBUG_DEBUG, "Subscribe: Topic '%s', QoS %d, noLocal: %s", topicBuffer, requestedQoS, noLocal ? "true" : "false");

        if (isValidTopicFilter(topic))
        {
            Subscription sub;
            sub.filter = topic;
            sub.noLocal = noLocal;
            client->subscriptions.push_back(sub);
            returnCodes.push_back(requestedQoS);
            logMessage(DEBUG_INFO, "Subscription for client '%s' to topic filter '%s' added (QoS %d, noLocal %s).", client->clientId.c_str(), topic.c_str(), requestedQoS, noLocal ? "Yes" : "No");

            if (subscribeCallback)
            {
                subscribeCallback(client->clientId, topic);
            }
        }
        else
        {
            logMessage(DEBUG_WARNING, "Subscription for client '%s' to invalid topic filter '%s' rejected.", client->clientId.c_str(), topic.c_str());
            if (client->protocolVersion == MQTT_PROTOCOL_LEVEL_5)
            {
                returnCodes.push_back(0x8F);
            }
            else
            {
                returnCodes.push_back(0x80);
            }
        }
    }

    if (returnCodes.empty())
    {
        logMessage(DEBUG_ERROR, "No valid subscriptions in SUBSCRIBE packet");
        return;
    }
    size_t subackLength = 2 + returnCodes.size();
    std::unique_ptr<uint8_t[]> suback(new uint8_t[2 + subackLength]);

    suback[0] = MQTT_SUBACK << 4;
    suback[1] = subackLength;
    suback[2] = packetId >> 8;
    suback[3] = packetId & 0xFF;

    for (size_t i = 0; i < returnCodes.size(); i++)
    {
        suback[4 + i] = returnCodes[i];
    }

    client->client->write((const char *)suback.get(), 2 + subackLength);

    sendRetainedMessages(client);
}

void ESPAsyncMQTTBroker::handleUnsubscribe(MQTTClient *client, uint8_t *data, uint32_t length)
{
    if (length < 2)
    {
        logMessage(DEBUG_ERROR, "Unsubscribe packet too short");
        return;
    }

    uint16_t packetId = (data[0] << 8) | data[1];

    uint8_t unsuback[4] = {0xB0, 0x02, (uint8_t)(packetId >> 8), (uint8_t)packetId};
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

        if (topicLength > MQTT_MAX_TOPIC_SIZE)
        {
            logMessage(DEBUG_ERROR, "Unsubscribe topic too long: %u > %u", topicLength, MQTT_MAX_TOPIC_SIZE);
            break;
        }

        char topicBuffer[MQTT_MAX_TOPIC_SIZE] = {0};
        memcpy(topicBuffer, data + index, topicLength);
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
    logMessage(DEBUG_DEBUG, "PING from %s answered", client->clientId.c_str());
}

void ESPAsyncMQTTBroker::handleDisconnect(MQTTClient *client)
{
    logMessage(DEBUG_INFO, "Clean disconnect from client %s (DISCONNECT packet received).", client->clientId.c_str());
    client->connected = false;
    client->gracefulDisconnect = true;

    if (client->hasWill)
    {
        logMessage(DEBUG_DEBUG, "LWT for client %s is discarded (clean disconnect).", client->clientId.c_str());
        client->hasWill = false;
        client->willTopic = "";
        client->willPayload.reset();
        client->willPayloadLen = 0;
    }

    if (client->cleanSession && client->client)
    {
        client->client->close();
    }
}

void ESPAsyncMQTTBroker::handlePubRec(MQTTClient *client, uint8_t *data, size_t len)
{
    if (len < 2)
    {
        logMessage(DEBUG_ERROR, "PubRec packet too short");
        return;
    }
    uint16_t packetId = (data[0] << 8) | data[1];
    uint8_t pubrel[] = {0x62, 0x02, (uint8_t)(packetId >> 8), (uint8_t)packetId};
    client->client->write((const char *)pubrel, sizeof(pubrel));
    logMessage(DEBUG_DEBUG, "PUBREC for packet ID %u processed", packetId);
}

void ESPAsyncMQTTBroker::handlePubRel(MQTTClient *client, uint8_t *data, size_t len)
{
    if (len < 2)
    {
        logMessage(DEBUG_ERROR, "PubRel packet too short");
        return;
    }
    uint16_t packetId = (data[0] << 8) | data[1];

    auto it = incomingQoS2Messages.find(packetId);
    if (it != incomingQoS2Messages.end())
    {
        IncomingQoS2Message &msg = it->second;

        String payloadStr;
        if (msg.payload_len > 0 && msg.payload)
        {
            char tempPayload[msg.payload_len + 1];
            memcpy(tempPayload, msg.payload.get(), msg.payload_len);
            tempPayload[msg.payload_len] = '\0';
            payloadStr = String(tempPayload);
        }
        else
        {
            payloadStr = "";
        }

        logMessage(DEBUG_INFO, "PUBREL for packet ID %u received. Publishing QoS 2 message: Topic='%s'", packetId, msg.topic.c_str());

        publish(msg.topic.c_str(), payloadStr.c_str(), msg.retained, MQTT_QOS2, msg.originalClientId);

        incomingQoS2Messages.erase(it);
    }
    else
    {
        logMessage(DEBUG_WARNING, "PUBREL for unknown packet ID %u received.", packetId);
    }

    uint8_t pubcomp[] = {(MQTT_PUBCOMP << 4), 0x02, (uint8_t)(packetId >> 8), (uint8_t)packetId};
    client->client->write((const char *)pubcomp, sizeof(pubcomp));
    logMessage(DEBUG_DEBUG, "PUBCOMP for packet ID %u sent.", packetId);
}

void ESPAsyncMQTTBroker::handlePubComp(MQTTClient *client, uint8_t *data, size_t len)
{
    if (len >= 2)
    {
        uint16_t packetId = (data[0] << 8) | data[1];
        logMessage(DEBUG_DEBUG, "PUBCOMP for packet ID %u received", packetId);
    }
}

bool ESPAsyncMQTTBroker::topicMatches(const Subscription &subscription, const String &topic)
{
    return topicMatches(subscription.filter, topic);
}

bool ESPAsyncMQTTBroker::topicMatches(const String &filter, const String &topic) {
    const char *f = filter.c_str();
    const char *t = topic.c_str();

    while (*f && *t) {
        const char *f_end = strchr(f, '/');
        const char *t_end = strchr(t, '/');

        size_t f_len = f_end ? (size_t)(f_end - f) : strlen(f);

        if (f_len == 1 && *f == '#') {
            return true;
        }

        if (f_len == 1 && *f == '+') {
            f = f_end ? f_end + 1 : f + f_len;
            t = t_end ? t_end + 1 : t + strlen(t);
            continue;
        }

        size_t t_len = t_end ? (size_t)(t_end - t) : strlen(t);
        if (f_len != t_len || strncmp(f, t, f_len) != 0) {
            return false;
        }

        f = f_end ? f_end + 1 : f + f_len;
        t = t_end ? t_end + 1 : t + t_len;
    }

    if (*f && strcmp(f, "/#") == 0) {
        return true;
    }

    return *f == *t;
}

void ESPAsyncMQTTBroker::sendRetainedMessages(MQTTClient *client)
{
    for (auto const &entry : retainedMessages)
    {
        auto const &msg = entry.second;

        if (!msg)
        {
            logMessage(DEBUG_ERROR, "Error: Invalid unique_ptr in retainedMessages Map found.");
            continue;
        }

        for (auto &sub : client->subscriptions)
        {
            if (topicMatches(sub, msg->topic))
            {
                size_t topicLength = msg->topic.length();

                if (topicLength > MQTT_MAX_TOPIC_SIZE)
                {
                    logMessage(DEBUG_ERROR, "Retained Topic too long: %u > %u", (unsigned)topicLength, MQTT_MAX_TOPIC_SIZE);
                    continue;
                }

                size_t actualPayloadLength = msg->length;
                if (msg->length > MQTT_MAX_PAYLOAD_SIZE)
                {
                    logMessage(DEBUG_WARNING, "Retained Payload for Topic '%s' will be truncated: %u > %u", msg->topic.c_str(), (unsigned)msg->length, MQTT_MAX_PAYLOAD_SIZE);
                    actualPayloadLength = MQTT_MAX_PAYLOAD_SIZE;
                }

                size_t remainingLengthField = 2 + topicLength + actualPayloadLength;
                size_t totalPacketLength = 1 + 1 + remainingLengthField;

                if (remainingLengthField > 127)
                {
                    logMessage(DEBUG_ERROR, "Retained Message (Topic: %s) too large for simple Remaining Length encoding: %u.", msg->topic.c_str(), (unsigned)remainingLengthField);
                    continue;
                }

                if (totalPacketLength > MQTT_MAX_PACKET_SIZE)
                {
                    logMessage(DEBUG_ERROR, "Retained Message (Topic: %s) exceeds MQTT_MAX_PACKET_SIZE: %u > %u.", msg->topic.c_str(), (unsigned)totalPacketLength, MQTT_MAX_PACKET_SIZE);
                    continue;
                }

                std::unique_ptr<uint8_t[]> packet(new uint8_t[totalPacketLength]);
                packet[0] = (MQTT_PUBLISH << 4) | (msg->qos << 1) | 0x01;
                packet[1] = (uint8_t)remainingLengthField;
                packet[2] = topicLength >> 8;
                packet[3] = topicLength & 0xFF;
                memcpy(packet.get() + 4, msg->topic.c_str(), topicLength);

                if (actualPayloadLength > 0 && msg->payload)
                {
                    memcpy(packet.get() + 4 + topicLength, msg->payload.get(), actualPayloadLength);
                }

                client->client->write((const char *)packet.get(), totalPacketLength);

                logMessage(DEBUG_DEBUG, "Retained Message sent: Topic='%s', Payload-length=%u, QoS=%d", msg->topic.c_str(), (unsigned)actualPayloadLength, msg->qos);

                break;
            }
        }
    }
}

bool ESPAsyncMQTTBroker::authenticateClient(const String &username, const String &password)
{
    logMessage(DEBUG_DEBUG, "🔐 Authentication attempt:");
    logMessage(DEBUG_DEBUG, "   - Received Username: '%s'", username.c_str());
    logMessage(DEBUG_DEBUG, "   - Password: %s", (password.isEmpty() ? "[empty]" : "********"));
    logMessage(DEBUG_DEBUG, "   - Configured Username: '%s'", brokerConfig.username.c_str());
    logMessage(DEBUG_DEBUG, "   - Configured Password: %s", (brokerConfig.password.isEmpty() ? "[empty]" : "********"));

    if (brokerConfig.username.isEmpty())
    {
        logMessage(DEBUG_INFO, "✅ Anonymous access allowed (no username configured)");
        return true;
    }

    if (username.isEmpty())
    {
        logMessage(DEBUG_ERROR, "❌ Connection without username not allowed");
        return false;
    }

    if (username != brokerConfig.username)
    {
        logMessage(DEBUG_ERROR, "❌ Wrong username");
        return false;
    }

    if (brokerConfig.password.isEmpty())
    {
        logMessage(DEBUG_INFO, "✅ Username correct, no password required");
        return true;
    }

    if (password != brokerConfig.password)
    {
        logMessage(DEBUG_ERROR, "❌ Wrong password");
        return false;
    }

    logMessage(DEBUG_INFO, "✅ Username and password correct");
    return true;
}

bool ESPAsyncMQTTBroker::isValidPublishTopic(const String &topic)
{
    if (topic.isEmpty())
    {
        logMessage(DEBUG_WARNING, "Invalid publish topic: Topic is empty.");
        return false;
    }

    if (topic.length() > MQTT_MAX_TOPIC_SIZE)
    {
        logMessage(DEBUG_WARNING, "Invalid publish topic: Topic '%s' exceeds max length of %d.", topic.c_str(), MQTT_MAX_TOPIC_SIZE);
        return false;
    }

    if (topic.indexOf('#') != -1)
    {
        logMessage(DEBUG_WARNING, "Invalid publish topic: Topic '%s' contains multi-level wildcard '#'.", topic.c_str());
        return false;
    }
    if (topic.indexOf('+') != -1)
    {
        logMessage(DEBUG_WARNING, "Invalid publish topic: Topic '%s' contains single-level wildcard '+'.", topic.c_str());
        return false;
    }

    return true;
}

bool ESPAsyncMQTTBroker::publish(const char *topic, const char *payload, bool retained, uint8_t qos)
{
    return publish(topic, payload, retained, qos, "");
}

bool ESPAsyncMQTTBroker::publish(const char *topic, const char *payload, bool retained, uint8_t qos, const String &excludeClientId)
{
    if (!topic)
    {
        logMessage(DEBUG_ERROR, "Null pointer as topic for C-String Publish");
        return false;
    }
    if (!payload)
    {
        logMessage(DEBUG_DEBUG, "Null pointer as payload for C-String Publish, treating as empty string.");
        return publish(topic, (const uint8_t *)"", 0, retained, qos, excludeClientId);
    }

    return publish(topic, (const uint8_t *)payload, strlen(payload), retained, qos, excludeClientId);
}

bool ESPAsyncMQTTBroker::publish(const char *topic, uint8_t qos, bool retained, const char *payload)
{
    return publish(topic, payload, retained, qos);
}

bool ESPAsyncMQTTBroker::publish(const char *topic, const uint8_t *payload, size_t payloadLen, bool retained, uint8_t qos, const String &excludeClientId)
{
    if (!topic)
    {
        logMessage(DEBUG_ERROR, "Null pointer as topic for Publish");
        return false;
    }
    if (payloadLen > 0 && !payload)
    {
        logMessage(DEBUG_ERROR, "Null pointer as payload with payloadLen > 0 for Publish");
        return false;
    }

    size_t topicLen = strlen(topic);
    if (topicLen > MQTT_MAX_TOPIC_SIZE)
    {
        logMessage(DEBUG_ERROR, "Topic too long: %u > %u", (unsigned)topicLen, MQTT_MAX_TOPIC_SIZE);
        return false;
    }

    if (payloadLen > MQTT_MAX_PAYLOAD_SIZE)
    {
        logMessage(DEBUG_WARNING, "Payload will be truncated: %u > %u", (unsigned)payloadLen, MQTT_MAX_PAYLOAD_SIZE);
        payloadLen = MQTT_MAX_PAYLOAD_SIZE;
    }

    logMessage(DEBUG_INFO, "📤 Broker is publishing on topic '%s' (Length: %u, QoS: %d, Retained: %s)", topic, (unsigned)payloadLen, qos, retained ? "Yes" : "No");
    if (!excludeClientId.isEmpty())
    {
        logMessage(DEBUG_INFO, "   - Excluded client: %s", excludeClientId.c_str());
    }

    String topicStr = String(topic);

    if (retained)
    {
        retainedMessages.erase(topicStr);

        if (payloadLen > 0)
        {
            auto msg = std::unique_ptr<RetainedMessage>(new RetainedMessage(topicStr, payload, payloadLen, qos));
            retainedMessages[topicStr] = std::move(msg);
        }
    }

    uint8_t header = (MQTT_PUBLISH << 4) | (qos << 1) | (retained ? 1 : 0);

    bool messageSent = false;
    int clientCount = 0;
    int sentCount = 0;

    size_t currentTopicLen = topicStr.length();
    size_t remainingLength = 2 + currentTopicLen + payloadLen;

    if (remainingLength > 127)
    {
        logMessage(DEBUG_ERROR, "Message too large for simple Remaining Length encoding: %u. Topic: %s", (unsigned)remainingLength, topicStr.c_str());
        return false;
    }

    size_t packetSize = 1 + 1 + remainingLength;
    auto packet = std::unique_ptr<uint8_t[]>(new uint8_t[packetSize]);
    packet[0] = header;
    packet[1] = remainingLength;
    packet[2] = currentTopicLen >> 8;
    packet[3] = currentTopicLen & 0xFF;
    memcpy(packet.get() + 4, topicStr.c_str(), currentTopicLen);
    if (payloadLen > 0)
    {
        memcpy(packet.get() + 4 + currentTopicLen, payload, payloadLen);
    }

    for (auto it = clients.begin(); it != clients.end(); ++it)
    {
        auto& c = it->second;
        if (!c->connected)
            continue;

        clientCount++;
        bool sentToThisClient = false;

        bool isOriginalPublisherAndShouldBeSkipped = false;
        if (!excludeClientId.isEmpty() && c->clientId == excludeClientId)
        {
            bool foundNonNoLocalMatch = false;
            bool hasAnyMatchingSubscription = false;
            for (auto &subCheck : c->subscriptions)
            {
                if (topicMatches(subCheck.filter, topicStr))
                {
                    hasAnyMatchingSubscription = true;
                    if (!subCheck.noLocal)
                    {
                        foundNonNoLocalMatch = true;
                        break;
                    }
                }
            }
            if (hasAnyMatchingSubscription && !foundNonNoLocalMatch)
            {
                isOriginalPublisherAndShouldBeSkipped = true;
                logMessage(DEBUG_DEBUG, "  - Client %s (Original Publisher) will be skipped (all matching subscriptions have noLocal=true)", c->clientId.c_str());
            }
        }

        if (isOriginalPublisherAndShouldBeSkipped)
        {
            continue;
        }

        for (auto &sub : c->subscriptions)
        {
            logMessage(DEBUG_DEBUG, "Checking topic match for client %s: Subscription='%s', Incoming='%s'", c->clientId.c_str(), sub.filter.c_str(), topicStr.c_str());

            bool matched = topicMatches(sub.filter, topicStr);
            logMessage(DEBUG_DEBUG, "  - Match: %s", matched ? "Yes" : "No");

            if (matched)
            {
                logMessage(DEBUG_DEBUG, "Sending packet to Client ID: %s (Length: %u)", c->clientId.c_str(), (unsigned)packetSize);

                bool writeSuccess = c->client->write((const char *)packet.get(), packetSize);

                if (writeSuccess)
                {
                    sentCount++;
                    sentToThisClient = true;
                    messageSent = true;
                }

                logMessage(DEBUG_DEBUG, "  - Send %s", writeSuccess ? "successful" : "failed");

                break;
            }
        }

        if (!sentToThisClient && !isOriginalPublisherAndShouldBeSkipped)
        {
            logMessage(DEBUG_DEBUG, "  - Client %s has no matching subscriptions for topic %s", c->clientId.c_str(), topicStr.c_str());
        }
    }

    logMessage(DEBUG_INFO, "📊 Message sent to %d of %d connected clients", sentCount, clientCount);

    return messageSent;
}

bool ESPAsyncMQTTBroker::isValidTopicFilter(const String &filter)
{
    if (filter.isEmpty())
    {
        logMessage(DEBUG_WARNING, "Invalid topic filter: Filter is empty.");
        return false;
    }

    if (filter.length() > 65535)
    {
        logMessage(DEBUG_WARNING, "Invalid topic filter: Filter exceeds 65535 bytes.");
        return false;
    }

    std::vector<String> levels;
    int start = 0;
    int pos;
    while ((pos = filter.indexOf('/', start)) != -1)
    {
        levels.push_back(filter.substring(start, pos));
        start = pos + 1;
    }
    levels.push_back(filter.substring(start));

    if (levels.empty() && !filter.isEmpty())
    {
    }
    else if (levels.empty() && filter.length() > 0)
    {
        logMessage(DEBUG_WARNING, "Invalid topic filter: Could not split levels for non-empty filter '%s'.", filter.c_str());
        return false;
    }

    for (size_t i = 0; i < levels.size(); ++i)
    {
        const String &level = levels[i];

        if (level.indexOf('#') != -1)
        {
            if (level.length() > 1)
            {
                logMessage(DEBUG_WARNING, "Invalid topic filter: '#' cannot be part of a level (Level: '%s', Filter: '%s').", level.c_str(), filter.c_str());
                return false;
            }
            if (i != levels.size() - 1)
            {
                logMessage(DEBUG_WARNING, "Invalid topic filter: '#' must be the last level (Filter: '%s').", filter.c_str());
                return false;
            }
        }
        else if (level.indexOf('+') != -1)
        {
            if (level.length() > 1)
            {
                logMessage(DEBUG_WARNING, "Invalid topic filter: '+' cannot be part of a level (Level: '%s', Filter: '%s').", level.c_str(), filter.c_str());
                return false;
            }
        }
    }

    return true;
}
