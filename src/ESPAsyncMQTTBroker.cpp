// @version: 1.5.1
#include "ESPAsyncMQTTBroker.h"
#include <cstdarg>
#include <algorithm>

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

        Serial.println(buffer);

        // Wenn verfügbar, auch an Callback weiterleiten
        if (loggingCallback)
        {
            loggingCallback(level, buffer);
        }
    }
}

ESPAsyncMQTTBroker::ESPAsyncMQTTBroker(uint16_t port) : port(port)
{
}

ESPAsyncMQTTBroker::~ESPAsyncMQTTBroker()
{
    stop();
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
    for (auto it = clients.begin(); it != clients.end(); ++it)
    {
        auto &mqttClient = it->second;
        if (mqttClient->connected && mqttClient->keepAlive > 0)
        {
            if (now - mqttClient->lastActivity > mqttClient->keepAlive * 1500UL)
            {
                logMessage(DEBUG_INFO, "Client '%s' ⏰  inactive, disconnecting.", mqttClient->clientId);
                mqttClient->client->close(); // This will trigger onDisconnect and remove the client
            }
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
    logMessage(DEBUG_INFO, "   Username: %s", (brokerConfig.username[0] == '\0' ? "[empty]" : brokerConfig.username));
    logMessage(DEBUG_INFO, "   Password: %s", (brokerConfig.password[0] == '\0' ? "[empty]" : "[set]"));
    logMessage(DEBUG_INFO, "   Auth required: %s", (brokerConfig.username[0] != '\0' ? "Yes" : "No"));
}

void ESPAsyncMQTTBroker::onClient(AsyncClient *client)
{
    auto mqttClient = std::make_unique<MQTTClient>();
    mqttClient->client = client;
    mqttClient->connected = false;
    mqttClient->lastActivity = millis();
    mqttClient->keepAlive = 0;
    mqttClient->cleanSession = true;
    mqttClient->clientId[0] = '\0';
    mqttClient->hasWill = false;
    mqttClient->gracefulDisconnect = false;
    mqttClient->willTopic[0] = '\0';
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
                                   target->clientId, target->willTopic, target->willQos, target->willRetain ? "Yes" : "No");
                broker->publish(target->willTopic, target->willPayload.get(), target->willPayloadLen, target->willRetain, target->willQos, nullptr);
                target->hasWill = false;
            } else if (target->hasWill && target->gracefulDisconnect) {
                broker->logMessage(DEBUG_DEBUG, "LWT for client %s not sent (clean disconnect already handled).", target->clientId);
            }

            if (!target->cleanSession) {
                broker->logMessage(DEBUG_INFO, "Client %s disconnected (graceful: %s), session will be kept.",
                                 target->clientId, target->gracefulDisconnect ? "Yes" : "No");
                broker->persistentSessions.push_back(std::move(target));
            } else {
                broker->logMessage(DEBUG_INFO, "Client %s disconnected (graceful: %s), Clean Session, removing client.",
                                 target->clientId, target->gracefulDisconnect ? "Yes" : "No");
                if (broker->clientDisconnectCallback) {
                    broker->clientDisconnectCallback(target->clientId);
                }
                auto& vec = broker->connectedClientsInfo;
                vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const ClientInfo& ci) {
                    return strcmp(ci.clientId, target->clientId) == 0;
                }), vec.end());
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
                broker->logMessage(DEBUG_ERROR, "Client %s Error: %d", mqttClient->clientId, error);
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

    if (clientIdLength > MQTT_MAX_CLIENT_ID_SIZE) {
        logMessage(DEBUG_ERROR, "Client-ID too long!");
        return;
    }
    memcpy(client->clientId, data + offset, clientIdLength);
    client->clientId[clientIdLength] = '\0';
    offset += clientIdLength;

    bool sessionActuallyRestored = false;
    auto& sessions = persistentSessions;
    auto sessionIt = std::find_if(sessions.begin(), sessions.end(), [&](const auto& s) {
        return strcmp(s->clientId, client->clientId) == 0;
    });

    if (!cleanSession && sessionIt != sessions.end())
    {
        logMessage(DEBUG_INFO, "♻️ Wiederverwende persistente Session für Client: %s", client->clientId);
        client->subscriptions = (*sessionIt)->subscriptions;
        sessions.erase(sessionIt);
        sessionActuallyRestored = true;
    }

    client->cleanSession = cleanSession;
    client->keepAlive = keepAlive;

    logMessage(DEBUG_DEBUG, "CONNECT Flags: 0x%02X, Clean Session: %s, Keep-Alive: %d seconds", connectFlags, (cleanSession ? "Yes" : "No"), keepAlive);
    logMessage(DEBUG_DEBUG, "Client-ID: '%s'", client->clientId);

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
        memcpy(client->willTopic, data + offset, willTopicLen);
        client->willTopic[willTopicLen] = '\0';

        if (!isValidPublishTopic(client->willTopic))
        {
            logMessage(DEBUG_ERROR, "Invalid Will-Topic name: '%s' (contains wildcards). Closing connection.", client->willTopic);
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

        logMessage(DEBUG_DEBUG, "LWT registered: Topic='%s', QoS=%d, Retain=%s, PayloadLen=%u", client->willTopic, client->willQos, client->willRetain ? "Yes" : "No", client->willPayloadLen);
    }
    else
    {
        client->hasWill = false;
    }

    char username[MQTT_MAX_USERNAME_SIZE + 1] = {0};
    char password[MQTT_MAX_PASSWORD_SIZE + 1] = {0};
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

        if (usernameLen < sizeof(username))
        {
            memcpy(username, data + offset, usernameLen);
            username[usernameLen] = '\0';
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

        if (passwordLen < sizeof(password))
        {
            memcpy(password, data + offset, passwordLen);
            password[passwordLen] = '\0';
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
        char ipStr[16];
        snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
        clientConnectCallback(client->clientId, ipStr);

        ClientInfo info;
        strncpy(info.clientId, client->clientId, MQTT_MAX_CLIENT_ID_SIZE);
        info.clientId[MQTT_MAX_CLIENT_ID_SIZE] = '\0';
        strncpy(info.ip, ipStr, sizeof(info.ip) - 1);
        info.ip[sizeof(info.ip)-1] = '\0';
        connectedClientsInfo.push_back(info);
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

    char topicBuffer[MQTT_MAX_TOPIC_SIZE + 1] = {0};
    memcpy(topicBuffer, data + 2, topicLength);

    if (!isValidPublishTopic(topicBuffer))
    {
        logMessage(DEBUG_ERROR, "Invalid Topic Name '%s' from client '%s'. Closing connection.", topicBuffer, client->clientId);
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
            // Original implementation did not publish here for QoS 1, only for QoS 0.
            // Reverting to that behavior to be safe during refactoring.
        }
        else if (qos == 2)
        {
            uint32_t payloadLength = length - payloadOffset;
            if (payloadLength > MQTT_MAX_PAYLOAD_SIZE)
            {
                logMessage(DEBUG_WARNING, "QoS 2 Payload will be truncated to %u (from %u)", MQTT_MAX_PAYLOAD_SIZE, payloadLength);
                payloadLength = MQTT_MAX_PAYLOAD_SIZE;
            }

            IncomingQoS2Message qos2Msg(topicBuffer, data + payloadOffset, payloadLength, retained, client->clientId);
            incomingQoS2Messages[packetId] = std::move(qos2Msg);

            logMessage(DEBUG_INFO, "QoS 2 Publish received - Topic='%s', PacketID=%u. Sending PUBREC.", topicBuffer, packetId);

            uint8_t pubrec[] = {(MQTT_PUBREC << 4), 0x02, (uint8_t)(packetId >> 8), (uint8_t)packetId};
            client->client->write((const char *)pubrec, 4);
            return;
        }
    }

    if (qos == 0) {
        uint32_t payloadLength = length - payloadOffset;
        const uint8_t* payload = data + payloadOffset;

        if (payloadLength > MQTT_MAX_PAYLOAD_SIZE)
        {
            logMessage(DEBUG_WARNING, "Payload will be truncated to %u (from %u)", MQTT_MAX_PAYLOAD_SIZE, payloadLength);
            payloadLength = MQTT_MAX_PAYLOAD_SIZE;
        }

        logMessage(DEBUG_INFO, "🔔 Publish (QoS 0) - Topic='%s'", topicBuffer);

        if (messageCallback)
        {
            messageCallback(client->clientId, topicBuffer, payload, payloadLength);
        }

        publish(topicBuffer, payload, payloadLength, retained, qos, client->clientId);
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

        char topicBuffer[MQTT_MAX_TOPIC_SIZE + 1] = {0};
        memcpy(topicBuffer, data + index, topicLength);
        index += topicLength;

        if (index >= length)
            break;

        uint8_t options = data[index++];
        uint8_t requestedQoS = options & 0x03;
        bool noLocal = (options & 0x04) != 0;

        logMessage(DEBUG_DEBUG, "Subscribe: Topic '%s', QoS %d, noLocal: %s", topicBuffer, requestedQoS, noLocal ? "true" : "false");

        if (isValidTopicFilter(topicBuffer))
        {
            Subscription sub;
            strncpy(sub.filter, topicBuffer, MQTT_MAX_TOPIC_SIZE);
            sub.filter[MQTT_MAX_TOPIC_SIZE] = '\0';
            sub.noLocal = noLocal;
            client->subscriptions.push_back(sub);
            returnCodes.push_back(requestedQoS);
            logMessage(DEBUG_INFO, "Subscription for client '%s' to topic filter '%s' added (QoS %d, noLocal %s).", client->clientId, topicBuffer, requestedQoS, noLocal ? "Yes" : "No");

            if (subscribeCallback)
            {
                subscribeCallback(client->clientId, topicBuffer);
            }
        }
        else
        {
            logMessage(DEBUG_WARNING, "Subscription for client '%s' to invalid topic filter '%s' rejected.", client->clientId, topicBuffer);
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
    size_t subackPayloadLength = returnCodes.size();
    size_t subackPacketLength = 4 + subackPayloadLength;
    if (subackPacketLength > MQTT_MAX_PACKET_SIZE) {
        logMessage(DEBUG_ERROR, "SUBACK packet too large: %d", subackPacketLength);
        return;
    }

    _packet_buffer[0] = MQTT_SUBACK << 4;
    _packet_buffer[1] = 2 + subackPayloadLength; // remaining length
    _packet_buffer[2] = packetId >> 8;
    _packet_buffer[3] = packetId & 0xFF;

    for (size_t i = 0; i < returnCodes.size(); i++)
    {
        _packet_buffer[4 + i] = returnCodes[i];
    }

    client->client->write((const char *)_packet_buffer, subackPacketLength);

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

        char topicBuffer[MQTT_MAX_TOPIC_SIZE + 1] = {0};
        memcpy(topicBuffer, data + index, topicLength);
        index += topicLength;

        auto& subs = client->subscriptions;
        subs.erase(std::remove_if(subs.begin(), subs.end(), [&](const Subscription& s) {
            bool match = (strcmp(s.filter, topicBuffer) == 0);
            if (match && unsubscribeCallback) {
                unsubscribeCallback(client->clientId, topicBuffer);
            }
            return match;
        }), subs.end());
    }
}

void ESPAsyncMQTTBroker::handlePingReq(MQTTClient *client)
{
    uint8_t pingresp[] = {0xD0, 0x00};
    client->client->write((const char *)pingresp, 2);
    logMessage(DEBUG_DEBUG, "PING from %s answered", client->clientId);
}

void ESPAsyncMQTTBroker::handleDisconnect(MQTTClient *client)
{
    logMessage(DEBUG_INFO, "Clean disconnect from client %s (DISCONNECT packet received).", client->clientId);
    client->connected = false;
    client->gracefulDisconnect = true;

    if (client->hasWill)
    {
        logMessage(DEBUG_DEBUG, "LWT for client %s is discarded (clean disconnect).", client->clientId);
        client->hasWill = false;
        client->willTopic[0] = '\0';
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

        logMessage(DEBUG_INFO, "PUBREL for packet ID %u received. Publishing QoS 2 message: Topic='%s'", packetId, msg.topic);

        publish(msg.topic, msg.payload.get(), msg.payload_len, msg.retained, MQTT_QOS2, msg.originalClientId);

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

bool ESPAsyncMQTTBroker::topicMatches(const char* filter, const char* topic) {
    const char *f = filter;
    const char *t = topic;

    while (*f && *t) {
        // Find end of current level
        const char *f_end = strchr(f, '/');
        const char *t_end = strchr(t, '/');

        size_t f_len = f_end ? (size_t)(f_end - f) : strlen(f);

        // Check for '#' wildcard
        if (f_len == 1 && *f == '#') {
            // '#' must be the last character in the filter
            return (f_end == nullptr);
        }

        // Check for '+' wildcard
        if (f_len == 1 && *f == '+') {
            // '+' matches one level, so we just advance pointers
            f = f_end ? f_end + 1 : f + f_len;
            t = t_end ? t_end + 1 : t + strlen(t);
            continue;
        }

        // Compare levels
        size_t t_len = t_end ? (size_t)(t_end - t) : strlen(t);
        if (f_len != t_len || strncmp(f, t, f_len) != 0) {
            return false;
        }

        // Advance to next level
        f = f_end ? f_end + 1 : f + f_len;
        t = t_end ? t_end + 1 : t + t_len;
    }

    // Handle case where filter ends with '/#'
    if (strcmp(f, "/#") == 0) {
        return true;
    }

    // If both strings ended at the same time, it's a match
    return *f == *t;
}

void ESPAsyncMQTTBroker::sendRetainedMessages(MQTTClient *client)
{
    for (const auto& msg : retainedMessages)
    {
        if (!msg)
        {
            logMessage(DEBUG_ERROR, "Error: Invalid unique_ptr in retainedMessages vector found.");
            continue;
        }

        for (const auto& sub : client->subscriptions)
        {
            if (topicMatches(sub.filter, msg->topic))
            {
                size_t topicLength = strlen(msg->topic);

                size_t actualPayloadLength = msg->length;
                if (msg->length > MQTT_MAX_PAYLOAD_SIZE)
                {
                    logMessage(DEBUG_WARNING, "Retained Payload for Topic '%s' will be truncated: %u > %u", msg->topic, (unsigned)msg->length, MQTT_MAX_PAYLOAD_SIZE);
                    actualPayloadLength = MQTT_MAX_PAYLOAD_SIZE;
                }

                size_t remainingLengthField = 2 + topicLength + actualPayloadLength;
                if (remainingLengthField > 127) // Simplified Remaining Length encoding check
                {
                    logMessage(DEBUG_ERROR, "Retained Message (Topic: %s) too large for simple Remaining Length encoding: %u.", msg->topic, (unsigned)remainingLengthField);
                    continue;
                }

                size_t totalPacketLength = 1 + 1 + remainingLengthField;
                if (totalPacketLength > MQTT_MAX_PACKET_SIZE)
                {
                    logMessage(DEBUG_ERROR, "Retained Message (Topic: %s) exceeds MQTT_MAX_PACKET_SIZE: %u > %u.", msg->topic, (unsigned)totalPacketLength, MQTT_MAX_PACKET_SIZE);
                    continue;
                }

                _packet_buffer[0] = (MQTT_PUBLISH << 4) | (msg->qos << 1) | 0x01; // Retain = 1
                _packet_buffer[1] = (uint8_t)remainingLengthField;
                _packet_buffer[2] = topicLength >> 8;
                _packet_buffer[3] = topicLength & 0xFF;
                memcpy(_packet_buffer + 4, msg->topic, topicLength);

                if (actualPayloadLength > 0 && msg->payload)
                {
                    memcpy(_packet_buffer + 4 + topicLength, msg->payload.get(), actualPayloadLength);
                }

                client->client->write((const char *)_packet_buffer, totalPacketLength);

                logMessage(DEBUG_DEBUG, "Retained Message sent: Topic='%s', Payload-length=%u, QoS=%d", msg->topic, (unsigned)actualPayloadLength, msg->qos);

                break; // A given retained message is sent only once per client on subscribe
            }
        }
    }
}

bool ESPAsyncMQTTBroker::authenticateClient(const char* username, const char* password)
{
    logMessage(DEBUG_DEBUG, "🔐 Authentication attempt:");
    logMessage(DEBUG_DEBUG, "   - Received Username: '%s'", username);
    logMessage(DEBUG_DEBUG, "   - Password: %s", (password[0] == '\0' ? "[empty]" : "********"));
    logMessage(DEBUG_DEBUG, "   - Configured Username: '%s'", brokerConfig.username);
    logMessage(DEBUG_DEBUG, "   - Configured Password: %s", (brokerConfig.password[0] == '\0' ? "[empty]" : "********"));

    if (brokerConfig.username[0] == '\0')
    {
        logMessage(DEBUG_INFO, "✅ Anonymous access allowed (no username configured)");
        return true;
    }

    if (username[0] == '\0')
    {
        logMessage(DEBUG_ERROR, "❌ Connection without username not allowed");
        return false;
    }

    if (strcmp(username, brokerConfig.username) != 0)
    {
        logMessage(DEBUG_ERROR, "❌ Wrong username");
        return false;
    }

    if (brokerConfig.password[0] == '\0')
    {
        logMessage(DEBUG_INFO, "✅ Username correct, no password required");
        return true;
    }

    if (strcmp(password, brokerConfig.password) != 0)
    {
        logMessage(DEBUG_ERROR, "❌ Wrong password");
        return false;
    }

    logMessage(DEBUG_INFO, "✅ Username and password correct");
    return true;
}

bool ESPAsyncMQTTBroker::isValidPublishTopic(const char* topic)
{
    if (topic == nullptr || topic[0] == '\0')
    {
        logMessage(DEBUG_WARNING, "Invalid publish topic: Topic is empty.");
        return false;
    }

    if (strlen(topic) > MQTT_MAX_TOPIC_SIZE)
    {
        logMessage(DEBUG_WARNING, "Invalid publish topic: Topic '%s' exceeds max length of %d.", topic, MQTT_MAX_TOPIC_SIZE);
        return false;
    }

    if (strchr(topic, '#') != nullptr)
    {
        logMessage(DEBUG_WARNING, "Invalid publish topic: Topic '%s' contains multi-level wildcard '#'.", topic);
        return false;
    }
    if (strchr(topic, '+') != nullptr)
    {
        logMessage(DEBUG_WARNING, "Invalid publish topic: Topic '%s' contains single-level wildcard '+'.", topic);
        return false;
    }

    return true;
}

bool ESPAsyncMQTTBroker::publish(const char* topic, const uint8_t* payload, size_t len, bool retained, uint8_t qos, const char* excludeClientId)
{
    if (!topic)
    {
        logMessage(DEBUG_ERROR, "Null pointer as topic for Publish");
        return false;
    }
    if (len > 0 && !payload)
    {
        logMessage(DEBUG_ERROR, "Null pointer as payload with len > 0 for Publish");
        return false;
    }

    size_t topicLen = strlen(topic);
    if (topicLen > MQTT_MAX_TOPIC_SIZE)
    {
        logMessage(DEBUG_ERROR, "Topic too long: %u > %u", (unsigned)topicLen, MQTT_MAX_TOPIC_SIZE);
        return false;
    }

    if (len > MQTT_MAX_PAYLOAD_SIZE)
    {
        logMessage(DEBUG_WARNING, "Payload will be truncated: %u > %u", (unsigned)len, MQTT_MAX_PAYLOAD_SIZE);
        len = MQTT_MAX_PAYLOAD_SIZE;
    }

    logMessage(DEBUG_INFO, "📤 Broker is publishing on topic '%s' (Length: %u, QoS: %d, Retained: %s)", topic, (unsigned)len, qos, retained ? "Yes" : "No");
    if (excludeClientId)
    {
        logMessage(DEBUG_INFO, "   - Excluded client: %s", excludeClientId);
    }

    if (retained)
    {
        auto& vec = retainedMessages;
        vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const auto& msg) {
            return strcmp(msg->topic, topic) == 0;
        }), vec.end());

        if (len > 0)
        {
            auto msg = std::make_unique<RetainedMessage>(topic, payload, len, qos);
            retainedMessages.push_back(std::move(msg));
        }
    }

    uint8_t header = (MQTT_PUBLISH << 4) | (qos << 1) | (retained ? 1 : 0);

    size_t remainingLength = 2 + topicLen + len;
    if (remainingLength > 127)
    {
        logMessage(DEBUG_ERROR, "Message too large for simple Remaining Length encoding: %u. Topic: %s", (unsigned)remainingLength, topic);
        return false;
    }

    size_t packetSize = 1 + 1 + remainingLength;
    if (packetSize > MQTT_MAX_PACKET_SIZE) {
        logMessage(DEBUG_ERROR, "Publish packet too large: %d", packetSize);
        return false;
    }
    _packet_buffer[0] = header;
    _packet_buffer[1] = remainingLength;
    _packet_buffer[2] = topicLen >> 8;
    _packet_buffer[3] = topicLen & 0xFF;
    memcpy(_packet_buffer + 4, topic, topicLen);
    if (len > 0)
    {
        memcpy(_packet_buffer + 4 + topicLen, payload, len);
    }

    bool messageSent = false;
    int clientCount = 0;
    int sentCount = 0;

    for (auto const& [asyncClient, mqttClient] : clients)
    {
        if (!mqttClient->connected) continue;

        clientCount++;

        // Check if the client is the original publisher and all its matching subscriptions are "noLocal"
        if (excludeClientId && strcmp(mqttClient->clientId, excludeClientId) == 0) {
            bool shouldSkip = true;
            for (const auto& sub : mqttClient->subscriptions) {
                if (topicMatches(sub.filter, topic) && !sub.noLocal) {
                    shouldSkip = false;
                    break;
                }
            }
            if (shouldSkip) {
                logMessage(DEBUG_DEBUG, "  - Client %s (Original Publisher) will be skipped (all matching subscriptions have noLocal=true)", mqttClient->clientId);
                continue;
            }
        }

        for (const auto& sub : mqttClient->subscriptions)
        {
            if (topicMatches(sub.filter, topic))
            {
                logMessage(DEBUG_DEBUG, "Sending packet to Client ID: %s (Length: %u)", mqttClient->clientId, (unsigned)packetSize);
                if (mqttClient->client->write((const char*)_packet_buffer, packetSize))
                {
                    sentCount++;
                    messageSent = true;
                }
                else
                {
                    logMessage(DEBUG_WARNING, "  - Send failed to client %s", mqttClient->clientId);
                }
                break; // Message sent, move to next client
            }
        }
    }

    logMessage(DEBUG_INFO, "📊 Message sent to %d of %d connected clients", sentCount, clientCount);
    return messageSent;
}

bool ESPAsyncMQTTBroker::isValidTopicFilter(const char* filter)
{
    if (filter == nullptr || filter[0] == '\0')
    {
        logMessage(DEBUG_WARNING, "Invalid topic filter: Filter is empty.");
        return false;
    }

    if (strlen(filter) > 65535)
    {
        logMessage(DEBUG_WARNING, "Invalid topic filter: Filter exceeds 65535 bytes.");
        return false;
    }

    const char* current = filter;
    while (*current) {
        const char* next_slash = strchr(current, '/');
        const char* level_end = next_slash ? next_slash : current + strlen(current);

        if (strchr(current, '#') != nullptr) {
            if (strlen(current) > 1 || *(current+1) != '\0') {
                 logMessage(DEBUG_WARNING, "Invalid topic filter: '#' must be the last level (Filter: '%s').", filter);
                 return false;
            }
        }
        if (strchr(current, '+') != nullptr) {
             if (level_end - current > 1) {
                logMessage(DEBUG_WARNING, "Invalid topic filter: '+' cannot be part of a level (Filter: '%s').", filter);
                return false;
             }
        }

        if (!next_slash) break;
        current = next_slash + 1;
    }

    return true;
}
